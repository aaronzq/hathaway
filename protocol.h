#pragma once
#include <Arduino.h>
#include <math.h>
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
  // "<NAME> <VALUE>" -- a one-shot handler that takes one argument, range
  // checked like a parameter but stored nowhere. For a command that DOES
  // something with a number rather than remembering one: a rail move is an
  // instruction to travel 0.4 mm now, not a "distance" the rig holds. Because
  // nothing is stored, such a command never appears in a DUMP and so can never
  // be replayed by a reconnect -- no action, no effect.
  //
  // _F32 takes any value in range; _I32 additionally requires a whole number,
  // so a step count is never silently rounded.
  CMD_ACTION_F32,
  CMD_ACTION_I32,
};

// True for every kind that runs a handler rather than writing a global.
inline bool protoIsAction(uint8_t kind) {
  return kind == CMD_ACTION || kind == CMD_ACTION_F32 || kind == CMD_ACTION_I32;
}

// True for the kinds that expect a value token after the name.
inline bool protoActionTakesValue(uint8_t kind) {
  return kind == CMD_ACTION_F32 || kind == CMD_ACTION_I32;
}

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
//   ACTION_F32(RAIL_MOVE_MM, -2, 2, doRailMoveMm)
//   ACTION_I32(RAIL_MOVE_PULSES, -6000, 6000, doRailMovePulses)
//
#define PARAM_U32(VAR, LO, HI, FN)                                            \
  { #VAR, CMD_PARAM, STORE_U32, (void *)&VAR, (float)(LO), (float)(HI), FN, true }
#define PARAM_F32(VAR, LO, HI, FN)                                            \
  { #VAR, CMD_PARAM, STORE_F32, (void *)&VAR, (float)(LO), (float)(HI), FN, true }
#define ACTION(NAME, FN)                                                      \
  { #NAME, CMD_ACTION, STORE_NONE, nullptr, 0.0f, 0.0f, FN, true }
#define ACTION_QUIET(NAME, FN)                                                \
  { #NAME, CMD_ACTION, STORE_NONE, nullptr, 0.0f, 0.0f, FN, false }
#define ACTION_F32(NAME, LO, HI, FN)                                          \
  { #NAME, CMD_ACTION_F32, STORE_NONE, nullptr, (float)(LO), (float)(HI), FN, true }
#define ACTION_I32(NAME, LO, HI, FN)                                          \
  { #NAME, CMD_ACTION_I32, STORE_NONE, nullptr, (float)(LO), (float)(HI), FN, true }

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
    // Carries device_ms like every data line does. Without it the host has no
    // idea *when* a parameter took effect, so it cannot say which settings were
    // in force for a given trial. The record already held the timestamp; it just
    // was not being rendered.
    const char *n = (r.channel < ctN) ? ct[r.channel].name : "?";
    m = snprintf(b, c, "PARAM:%s,%g,%lu\n", n, (double)r.value,
                 (unsigned long)r.dev_ms);
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

// Step over the first token and any blanks after it. Returns a pointer to
// whatever follows -- "" if the line held only that one token.
inline const char *protoAfterFirstToken(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  while (*s && *s != ' ' && *s != '\t') s++;
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

// Parse one inbound line into a CmdMsg. Pure function: no Serial, no RTOS, no
// globals -- host-testable. Accepts:
//     "DUMP" / "GET"          -> CMD_SLOT_DUMP
//     "<ACTION>"              -> that action's slot
//     "<ACTION> <VALUE>"      -> that action's slot, range-checked
//     "SET <NAME> <VALUE>"    -> that parameter's slot, range-checked
//
// Anything else is rejected, including the near-misses: an action given a value
// it does not take, an action denied the value it does take, a whole-number
// action handed a fraction, and a valued action smuggled in through SET. The
// strictness is deliberate. These commands move hardware, and a line that is
// silently reinterpreted rather than refused is how a typo becomes a movement.
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
  if (slot >= 0 && protoIsAction(ct[slot].kind)) {
    const char *rest = protoAfterFirstToken(s);

    if (!protoActionTakesValue(ct[slot].kind)) {
      if (*rest != '\0') {           // "TARE 5" is a mistake, not a tare
        snprintf(err, errcap, "#ERR parse: %s", s);
        return PARSE_ERR;
      }
      out->slot = (uint8_t)slot; out->value = 0.0f;
      return PARSE_OK;
    }

    // One number and nothing else: sscanf returning 1 means it read the value
    // and found no trailing token, so "RAIL_MOVE_MM 1 2" and "RAIL_MOVE_MM abc"
    // are both refused rather than half-read.
    double val;
    char   extra[4];
    if (sscanf(rest, "%lf %3s", &val, extra) != 1 || isnan(val)) {
      // NaN is checked here and not left to the range test below, because every
      // comparison against NaN is false: "nan" would pass a range check, then
      // be cast to an integer (undefined), and reach the hardware as a step
      // count nobody chose. sscanf accepts the spelling, so it is reachable
      // from the wire. Infinities need no special case -- they fail the range.
      snprintf(err, errcap, "#ERR parse: %s", s);
      return PARSE_ERR;
    }
    // Range before the whole-number test, so a wild value is reported as out of
    // range and never reaches the integer cast below.
    if (val < ct[slot].lo || val > ct[slot].hi) {
      snprintf(err, errcap, "#ERR range: %s=%g", ct[slot].name, val);
      return PARSE_ERR;
    }
    if (ct[slot].kind == CMD_ACTION_I32 && val != (double)(long)val) {
      snprintf(err, errcap, "#ERR parse: %s", s);   // no silent rounding
      return PARSE_ERR;
    }
    out->slot = (uint8_t)slot; out->value = (float)val;
    return PARSE_OK;
  }

  char   name[32];
  double val;
  if (sscanf(s, "SET %31s %lf", name, &val) != 2 || isnan(val)) {
    snprintf(err, errcap, "#ERR parse: %s", s);   // NaN: see the note above
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
