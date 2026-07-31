#pragma once
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// protocol.h -- WIRE MECHANISM ONLY.
//
// This header defines *how* messages are described and rendered. It knows
// nothing about gratings, spouts, scales or magnets. Rig-specific content lives
// in the two tables at the top of hathaway.ino; the engine that pumps them
// lives in comms.h/comms.cpp.
//
// Nothing here touches FreeRTOS or Serial, so the whole formatting path can be
// compiled and unit-tested on a host machine (see tools/golden_test.cpp).
//
// You should rarely need to edit this file. To add a message or a command, add
// a row to TELEM_TABLE or CMD_TABLE in hathaway.ino.
// ---------------------------------------------------------------------------


// =========================== OUTBOUND (telemetry) ==========================
//
// ONE wire shape for every data line:
//
//     <RIG_ID>|NAME:<channel>,<value>,<device_ms>
//
// Single-channel devices report channel 1. `value` is rendered with "%g", so
// integers stay integers ("0", "1", "253") and floats stay readable ("20.14").
//
// Because every line has the same field layout, the host parser needs no
// per-message knowledge: split on ':' then ',' and you are done.
//
// Control-plane lines (PARAM: acks, #DEF, #ERR, #NAME ok) are a separate,
// smaller vocabulary handled by the engine -- see below.

// One telemetry event, enqueued by the control core and rendered on the comms
// core. Fixed size so it can travel through a FreeRTOS queue by value.
struct TelemRec {
  uint8_t  type;      // a TELEM_* id from hathaway.ino, or a TELEM_INTERNAL_*
  uint8_t  channel;   // 1-based; command/table slot for internal messages
  float    value;     // weight, 0/1 state, count...
  uint32_t dev_ms;    // millis() captured at the moment of the event
};

// A message is either a state SAMPLE (a signal that holds a value between
// updates: weight, position, magnet) or an EVENT (an instant: lick, reward).
// The rig announces this per message at startup as "#DEF <NAME>,<S|E>", so the
// host never needs a hard-coded list of message types.
enum TelemKind : uint8_t {
  TELEM_SAMPLE = 'S',
  TELEM_EVENT  = 'E',
};

// Escape hatch: a custom renderer, should some future message not fit the
// standard shape. Leave it out of the table row to get the standard shape.
typedef int (*TelemFmt)(char *buf, size_t cap, const char *name,
                        const TelemRec &r);

struct TelemSpec {
  uint8_t     id;     // TELEM_* value used at the call site
  const char *name;   // wire name, e.g. "LICK"
  uint8_t     kind;   // TELEM_SAMPLE or TELEM_EVENT
  TelemFmt    fmt;    // omit for the standard shape
};

// The standard shape: "NAME:<ch>,<val>,<ms>".
inline int FMT_STD(char *b, size_t c, const char *name, const TelemRec &r) {
  return snprintf(b, c, "%s:%u,%g,%lu\n", name, (unsigned)r.channel,
                  (double)r.value, (unsigned long)r.dev_ms);
}

// Telemetry ids reserved by the engine. Keep your TELEM_* ids below 240.
enum : uint8_t {
  TELEM_INTERNAL_PARAM = 250,   // "PARAM:<name>,<value>"  ack of an applied SET
  TELEM_INTERNAL_ACK   = 251,   // "#<NAME> ok"            ack of an action
  TELEM_INTERNAL_DEF   = 252,   // "#DEF <NAME>,<S|E>"     schema announcement
};
#define TELEM_USER_ID_MAX 239


// ============================ INBOUND (commands) ===========================

enum CmdKind : uint8_t {
  CMD_PARAM,    // "SET <NAME> <VALUE>" -- writes a tunable global
  CMD_ACTION,   // "<NAME>"             -- runs a one-shot handler
};

enum StoreType : uint8_t {
  STORE_NONE,
  STORE_U32,    // storage points at an `unsigned long`
  STORE_F32,    // storage points at a `float`
};

// One row of CMD_TABLE. A parameter row carries a pointer to the variable it
// controls, which is what lets the engine read and write it generically -- no
// per-parameter switch statements anywhere.
struct CmdSpec {
  const char *name;              // wire token; for params, == the C identifier
  uint8_t     kind;              // CmdKind
  uint8_t     stype;             // StoreType
  void       *storage;           // -> the tunable global (control core owns it)
  float       lo, hi;            // inclusive accepted range (params only)
  void      (*apply)(float v);   // optional; runs on the CONTROL core
  bool        ack;               // emit an ack line after applying
};

// Table macros. The parameter macros stringify the variable name, so the wire
// name can never drift from the C identifier.
//
//   PARAM_U32(REWARD_DURATION1, 0, 1000, applyRewardDuration1)
//   PARAM_F32(SCALE_HIGH_THRESH, -50, 50, nullptr)
//   ACTION(TARE, doTare)
//
#define PARAM_U32(VAR, LO, HI, FN)                                            \
  { #VAR, CMD_PARAM, STORE_U32, (void *)&VAR, (float)(LO), (float)(HI), FN, true }
#define PARAM_F32(VAR, LO, HI, FN)                                            \
  { #VAR, CMD_PARAM, STORE_F32, (void *)&VAR, (float)(LO), (float)(HI), FN, true }
#define ACTION(NAME, FN)                                                      \
  { #NAME, CMD_ACTION, STORE_NONE, nullptr, 0.0f, 0.0f, FN, true }
#define ACTION_QUIET(NAME, FN)                                                \
  { #NAME, CMD_ACTION, STORE_NONE, nullptr, 0.0f, 0.0f, FN, false }

// Sent from the comms core to the control core once a line has been validated.
struct CmdMsg {
  uint8_t slot;    // index into CMD_TABLE, or CMD_SLOT_DUMP
  float   value;
};
#define CMD_SLOT_DUMP 0xFF   // built-in: re-emit every parameter


// ===================== generic storage access (control core) ===============

inline float protoLoad(const CmdSpec &c) {
  switch (c.stype) {
    case STORE_U32: return (float)(*(unsigned long *)c.storage);
    case STORE_F32: return *(float *)c.storage;
    default:        return 0.0f;
  }
}

inline void protoStore(const CmdSpec &c, float v) {
  switch (c.stype) {
    case STORE_U32: *(unsigned long *)c.storage = (unsigned long)v; break;
    case STORE_F32: *(float *)c.storage = v;                        break;
    default: break;
  }
}


// ============================ line rendering ===============================

// Render one record as a complete wire line, including the "<rigId>|" prefix.
// Pure function: no Serial, no RTOS, no globals -- host-testable.
// Returns bytes written (0 if the record is unknown or the buffer is too small).
inline int protoFormat(char *line, size_t cap, int rigId,
                       const TelemSpec *tt, size_t ttN,
                       const CmdSpec *ct, size_t ctN,
                       const TelemRec &r) {
  int p = snprintf(line, cap, "%d|", rigId);
  if (p < 0 || (size_t)p >= cap) return 0;
  char  *b = line + p;
  size_t c = cap - p;
  int    m = 0;

  if (r.type == TELEM_INTERNAL_PARAM) {
    const char *n = (r.channel < ctN) ? ct[r.channel].name : "?";
    m = snprintf(b, c, "PARAM:%s,%g\n", n, (double)r.value);
  } else if (r.type == TELEM_INTERNAL_ACK) {
    const char *n = (r.channel < ctN) ? ct[r.channel].name : "?";
    m = snprintf(b, c, "#%s ok\n", n);
  } else if (r.type == TELEM_INTERNAL_DEF) {
    if (r.channel >= ttN) return 0;
    m = snprintf(b, c, "#DEF %s,%c\n", tt[r.channel].name,
                 (char)tt[r.channel].kind);
  } else {
    const TelemSpec *s = nullptr;
    for (size_t i = 0; i < ttN; i++)
      if (tt[i].id == r.type) { s = &tt[i]; break; }
    if (s == nullptr) return 0;
    m = (s->fmt ? s->fmt : FMT_STD)(b, c, s->name, r);
  }
  if (m <= 0 || (size_t)m >= c) return 0;   // truncated -> drop, never emit half
  return p + m;
}

// Find a command by wire name. Returns its slot, or -1.
inline int protoFindCmd(const CmdSpec *ct, size_t ctN, const char *name) {
  for (size_t i = 0; i < ctN; i++)
    if (strcmp(name, ct[i].name) == 0) return (int)i;
  return -1;
}


// ============================ line parsing =================================

enum ParseResult : uint8_t {
  PARSE_OK,      // `out` is filled and should be handed to the control core
  PARSE_ERR,     // `err` holds the reply body; the caller adds the rig prefix
};

// Copy the first whitespace-delimited token of `s` into `out`.
inline void protoFirstToken(const char *s, char *out, size_t cap) {
  size_t n = 0;
  while (*s == ' ' || *s == '\t') s++;
  while (*s && *s != ' ' && *s != '\t' && n + 1 < cap) out[n++] = *s++;
  out[n] = '\0';
}

// Parse one inbound line into a CmdMsg. Pure function: no Serial, no RTOS, no
// globals -- host-testable. Accepts:
//     "DUMP" / "GET"        -> CMD_SLOT_DUMP
//     "<ACTION>"            -> that action's slot
//     "SET <NAME> <VALUE>"  -> that parameter's slot, range-checked
inline ParseResult protoParseCommand(const CmdSpec *ct, size_t ctN,
                                     const char *s, CmdMsg *out,
                                     char *err, size_t errcap) {
  char tok[32];
  protoFirstToken(s, tok, sizeof(tok));

  if (strcmp(tok, "DUMP") == 0 || strcmp(tok, "GET") == 0) {
    out->slot = CMD_SLOT_DUMP; out->value = 0.0f;
    return PARSE_OK;
  }

  int slot = protoFindCmd(ct, ctN, tok);
  if (slot >= 0 && ct[slot].kind == CMD_ACTION) {
    out->slot = (uint8_t)slot; out->value = 0.0f;
    return PARSE_OK;
  }

  char   name[32];
  double val;
  if (sscanf(s, "SET %31s %lf", name, &val) != 2) {
    snprintf(err, errcap, "#ERR parse: %s", s);
    return PARSE_ERR;
  }
  slot = protoFindCmd(ct, ctN, name);
  if (slot < 0 || ct[slot].kind != CMD_PARAM) {
    snprintf(err, errcap, "#ERR unknown: %s", name);
    return PARSE_ERR;
  }
  if (val < ct[slot].lo || val > ct[slot].hi) {
    snprintf(err, errcap, "#ERR range: %s=%g", name, val);
    return PARSE_ERR;
  }
  out->slot = (uint8_t)slot; out->value = (float)val;
  return PARSE_OK;
}
