#pragma once
// Minimal Arduino/FreeRTOS shim so the pure protocol layer can be compiled and
// tested with g++ on a normal computer -- no ESP32, no upload cycle.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>

typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
#define pdTRUE 1

inline uint32_t millis() { return 12345; }

// Serial: captures everything written so the test can compare it.
extern std::string g_serialOut;
struct SerialShim {
  template <class... A>
  void printf(const char *f, A... a) {
    char b[256];
    snprintf(b, sizeof(b), f, a...);
    g_serialOut += b;
  }
};
extern SerialShim Serial;

// xQueueSend: captures the record the old firmware would have enqueued.
struct CapturedCmd { int id; float value; bool valid; };
extern CapturedCmd g_lastCmd;

template <class T>
inline int xQueueSend(QueueHandle_t, const T *c, int) {
  g_lastCmd.id = (int)c->id;
  g_lastCmd.value = c->value;
  g_lastCmd.valid = true;
  return pdTRUE;
}
