// ============================================================================
// Serial logging test  -  FAKE DATA GENERATOR (30 Hz)
// Target: Adafruit Metro ESP32-S3  (Arduino core)
// ----------------------------------------------------------------------------
// Purpose
//   Stand-in firmware to build and test the PC-side pipeline (ingest +
//   database + dashboard) over USB serial, before any real sensors or WiFi.
//
//   It demonstrates the production logging architecture:
//     * Core 1 = ACQUISITION. A 30 Hz hardware timer produces records and
//                timestamps them with esp_timer_get_time() (microseconds).
//                It only pushes into a FreeRTOS queue -- it never touches
//                Serial, so its timing can never be blocked by USB I/O.
//     * Core 0 = LOGGING. A task drains the queue and writes lines to USB
//                serial in batches. If the USB host stalls, only this core
//                waits; Core 1 keeps stamping on time.
//
//   Fake data is intentionally heterogeneous to exercise the pipeline:
//     * LOADCELL : one continuous sample every tick (30 Hz)  -> "samples"
//     * LICK     : random sparse events                      -> "events"
//     * REWARD   : rare random events                        -> "events"
//
// This is a standalone test sketch. It does NOT include or modify the
// hathaway project.
// ----------------------------------------------------------------------------
// LINE PROTOCOL (one record per line, '\n' terminated, ASCII)
//   Header, printed once at boot:
//     #HATHAWAY_SERIAL v1
//     #RIG <rig_id>
//     #COLUMNS seq,t_us,kind,type,channel,value
//   Data lines:
//     <seq>,<t_us>,<kind>,<type>,<channel>,<value>
//       seq     : uint32, per-rig monotonic counter (gap/dup detection)
//       t_us    : uint64, device clock in microseconds (esp_timer)
//       kind    : 'S' = continuous sample, 'E' = discrete event
//       type    : LOADCELL | LICK | REWARD  (extend freely)
//       channel : int  (e.g. spout/sensor index)
//       value   : float
//   Lines beginning with '#' are comments/metadata and must be ignored by
//   the ingest parser (except the header lines it may choose to read).
// ============================================================================

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_timer.h"

// ---- configuration ---------------------------------------------------------
static const uint32_t RIG_ID        = 1;
static const uint32_t SAMPLE_HZ      = 30;         // continuous sample rate
static const uint32_t SERIAL_BAUD    = 921600;     // fast USB-CDC; match ingest
static const size_t   QUEUE_LEN      = 512;        // records buffered Core1->Core0
static const uint32_t LICK_PPM       = 4000;       // ~lick prob per tick (ppm)
static const uint32_t REWARD_PPM     = 300;        // ~reward prob per tick (ppm)

// ---- record passed through the queue ---------------------------------------
struct Record {
  uint32_t seq;
  uint64_t t_us;
  char     kind;      // 'S' or 'E'
  uint8_t  type;      // enum below
  int16_t  channel;
  float    value;
};

enum : uint8_t { T_LOADCELL = 0, T_LICK = 1, T_REWARD = 2 };
static const char* TYPE_NAME[] = { "LOADCELL", "LICK", "REWARD" };

// ---- globals ---------------------------------------------------------------
static QueueHandle_t   q;
static esp_timer_handle_t sampleTimer;
static volatile uint32_t g_seq = 0;
static volatile uint32_t g_dropped = 0;   // records lost to a full queue

// ---------------------------------------------------------------------------
// ACQUISITION (Core 1) -- runs in the esp_timer callback at SAMPLE_HZ.
// Keep this short and non-blocking. Timestamp first, enqueue, return.
// ---------------------------------------------------------------------------
static inline void pushRecord(char kind, uint8_t type, int16_t ch, float val,
                              uint64_t t_us) {
  Record r{ ++g_seq, t_us, kind, type, ch, val };
  // Non-blocking send: if the consumer is behind and the queue is full we
  // drop and count it rather than stall acquisition timing.
  if (xQueueSend(q, &r, 0) != pdTRUE) {
    g_dropped++;
  }
}

static void IRAM_ATTR onSampleTimer(void* /*arg*/) {
  const uint64_t now = (uint64_t)esp_timer_get_time();  // microseconds

  // 1) continuous load-cell-like sample every tick
  float noise = (float)(esp_random() & 0xFFFF) / 65535.0f;   // 0..1
  float weight = 20.0f + 2.0f * noise;                        // fake grams
  pushRecord('S', T_LOADCELL, 0, weight, now);

  // 2) sparse random lick events
  if ((esp_random() % 1000000u) < LICK_PPM) {
    int16_t spout = (esp_random() & 1) ? 1 : 2;
    pushRecord('E', T_LICK, spout, 1.0f, now);
  }

  // 3) rare random reward events
  if ((esp_random() % 1000000u) < REWARD_PPM) {
    pushRecord('E', T_REWARD, 1, 1.0f, now);
  }
}

// ---------------------------------------------------------------------------
// LOGGING (Core 0) -- drain the queue and write to USB serial in batches.
// Allowed to block on Serial; Core 1 is unaffected.
// ---------------------------------------------------------------------------
static void loggerTask(void* /*arg*/) {
  Record r;
  char line[96];
  uint32_t sinceFlush = 0;
  for (;;) {
    // Wait up to 5 ms for a record, then flush what we have.
    if (xQueueReceive(q, &r, pdMS_TO_TICKS(5)) == pdTRUE) {
      int n = snprintf(line, sizeof(line), "%lu,%llu,%c,%s,%d,%.4f\n",
                       (unsigned long)r.seq, (unsigned long long)r.t_us,
                       r.kind, TYPE_NAME[r.type], (int)r.channel, r.value);
      Serial.write((const uint8_t*)line, n);
      if (++sinceFlush >= 32) { Serial.flush(); sinceFlush = 0; }
    } else {
      Serial.flush();
      sinceFlush = 0;
      // periodic health line so the PC can see drops
      static uint32_t lastDrop = 0;
      uint32_t d = g_dropped;
      if (d != lastDrop) {
        int n = snprintf(line, sizeof(line), "#DROPPED %lu\n",
                         (unsigned long)d);
        Serial.write((const uint8_t*)line, n);
        lastDrop = d;
      }
    }
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);

  q = xQueueCreate(QUEUE_LEN, sizeof(Record));

  // header
  Serial.printf("#HATHAWAY_SERIAL v1\n");
  Serial.printf("#RIG %lu\n", (unsigned long)RIG_ID);
  Serial.printf("#COLUMNS seq,t_us,kind,type,channel,value\n");
  Serial.flush();

  // Logging task on Core 0 (WiFi core in production; free here).
  xTaskCreatePinnedToCore(loggerTask, "logger", 4096, nullptr, 1, nullptr, 0);

  // 30 Hz acquisition timer. esp_timer callbacks run at high priority and
  // are pinned by the scheduler; the heavy display/task code would live in
  // loop() on Core 1 in the real firmware.
  const esp_timer_create_args_t targs = {
    .callback = &onSampleTimer,
    .arg = nullptr,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "sample",
    .skip_unhandled_events = true,
  };
  esp_timer_create(&targs, &sampleTimer);
  esp_timer_start_periodic(sampleTimer, 1000000ULL / SAMPLE_HZ);  // µs period
}

void loop() {
  // Core 1. In the real firmware this runs the grating/task state machine.
  // Here it stays idle; acquisition is timer-driven, logging is on Core 0.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
