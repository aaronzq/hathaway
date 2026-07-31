#include "comms.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ---------------------------------------------------------------------------
// Deliberately includes nothing but protocol.h and Arduino.h.
//
// In particular it must NOT include behavior_task.h: that header *defines*
// mutable globals (unsigned long REWARD_DURATION1 = 50;) with external linkage,
// so a second translation unit including it would fail to link. Rig identity
// arrives through begin(); rig behaviour arrives through the tables.
// ---------------------------------------------------------------------------

namespace {

// Wiring supplied by begin(). Written once at startup, read-only thereafter.
int              g_rigId = 0;
const TelemSpec *g_tt    = nullptr;
size_t           g_ttN   = 0;
const CmdSpec   *g_ct    = nullptr;
size_t           g_ctN   = 0;

const int     TELEM_QUEUE_LEN = 128;
const int     CMD_QUEUE_LEN   = 8;
const size_t  LINE_CAP        = 96;   // rig prefix + longest wire line
const size_t  RX_CAP          = 64;

QueueHandle_t     g_telemQueue = nullptr;   // control core -> comms core
QueueHandle_t     g_cmdQueue   = nullptr;   // comms core  -> control core
TaskHandle_t      g_commsTask  = nullptr;
volatile uint32_t g_dropped    = 0;

// --- comms core: inbound ---------------------------------------------------

// Validate one inbound line (protoParseCommand does the pure work) and hand it
// to the control core. Runs on the comms task, which owns Serial, so rejections
// are written here. Success acks are emitted by the control core once the
// change has actually landed.
void handleCommandLine(const char *s) {
  CmdMsg m;
  char   err[80];
  if (protoParseCommand(g_ct, g_ctN, s, &m, err, sizeof(err)) != PARSE_OK) {
    Serial.printf("%d|%s\n", g_rigId, err);
    return;
  }
  if (g_cmdQueue == nullptr || xQueueSend(g_cmdQueue, &m, 0) != pdTRUE) {
    const char *what = (m.slot < g_ctN) ? g_ct[m.slot].name : "DUMP";
    Serial.printf("%d|#ERR busy: %s\n", g_rigId, what);
  }
}

// --- comms core: the task itself -------------------------------------------

void commsTask(void *pv) {
  (void)pv;
  TelemRec r;
  char     line[LINE_CAP];
  char     rx[RX_CAP];
  size_t   rxlen = 0;

  for (;;) {
    // 1) Drain outgoing telemetry (short wait so input is still polled).
    if (xQueueReceive(g_telemQueue, &r, pdMS_TO_TICKS(5)) == pdTRUE) {
      do {
        int n = protoFormat(line, sizeof(line), g_rigId,
                            g_tt, g_ttN, g_ct, g_ctN, r);
        if (n > 0) Serial.write((const uint8_t *)line, (size_t)n);
      } while (xQueueReceive(g_telemQueue, &r, 0) == pdTRUE);
    }
    // 2) Read inbound bytes; dispatch on each complete line.
    while (Serial.available() > 0) {
      char c = (char)Serial.read();
      if (c == '\n' || c == '\r') {
        if (rxlen > 0) { rx[rxlen] = '\0'; handleCommandLine(rx); rxlen = 0; }
      } else if (rxlen < sizeof(rx) - 1) {
        rx[rxlen++] = c;
      } else {
        rxlen = 0;   // overflow; drop the line
      }
    }
  }
}

}  // namespace

// --- public API -------------------------------------------------------------

namespace Comms {

void begin(int rigId,
           const TelemSpec *telemTable, size_t telemCount,
           const CmdSpec   *cmdTable,   size_t cmdCount,
           uint32_t baud, int core) {
  g_rigId = rigId;
  g_tt = telemTable; g_ttN = telemCount;
  g_ct = cmdTable;   g_ctN = cmdCount;

  Serial.begin(baud);
  g_telemQueue = xQueueCreate(TELEM_QUEUE_LEN, sizeof(TelemRec));
  g_cmdQueue   = xQueueCreate(CMD_QUEUE_LEN,   sizeof(CmdMsg));
  xTaskCreatePinnedToCore(commsTask, "comms", 4096, nullptr, 1,
                          &g_commsTask, core);   // that core owns Serial
}

void emit(uint8_t type, uint8_t channel, float value, uint32_t dev_ms) {
  TelemRec r = { type, channel, value, dev_ms };
  if (g_telemQueue == nullptr || xQueueSend(g_telemQueue, &r, 0) != pdTRUE)
    g_dropped++;   // drop rather than stall the control loop
}

void dumpParams() {
  uint32_t now = millis();
  for (size_t i = 0; i < g_ctN; i++)
    if (g_ct[i].kind == CMD_PARAM)
      emit(TELEM_INTERNAL_PARAM, (uint8_t)i, protoLoad(g_ct[i]), now);
}

void dumpSchema() {
  uint32_t now = millis();
  for (size_t i = 0; i < g_ttN; i++)
    emit(TELEM_INTERNAL_DEF, (uint8_t)i, 0.0f, now);
}

void service() {
  CmdMsg m;
  while (g_cmdQueue && xQueueReceive(g_cmdQueue, &m, 0) == pdTRUE) {
    if (m.slot == CMD_SLOT_DUMP) { dumpSchema(); dumpParams(); continue; }
    if (m.slot >= g_ctN) continue;
    const CmdSpec &c = g_ct[m.slot];

    if (c.kind == CMD_PARAM) {
      protoStore(c, m.value);              // write the tunable global
      if (c.apply) c.apply(m.value);       // then poke the device object
      if (c.ack)                           // ack the value actually stored
        emit(TELEM_INTERNAL_PARAM, m.slot, protoLoad(c), millis());
    } else {
      if (c.apply) c.apply(m.value);       // one-shot action
      if (c.ack) emit(TELEM_INTERNAL_ACK, m.slot, 0.0f, millis());
    }
  }
}

uint32_t dropped() { return g_dropped; }

}  // namespace Comms
