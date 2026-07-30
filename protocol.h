#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Serial protocol / telemetry types
// These live in a header (not the .ino) so they are defined before the Arduino
// IDE's auto-generated function prototypes, which are inserted right after the
// #includes. Functions that take these types (e.g. formatTelem(const TelemRec&),
// applyParam(const ParamCmd&)) would otherwise fail with "does not name a type".
// ---------------------------------------------------------------------------

// Telemetry record kinds -> wire lines (each line is prefixed with "<RIG_ID>|").
enum TelemType : uint8_t {
  TELEM_WEIGHT,     // "HX711 reading: <float>"   (no device timestamp)
  TELEM_POSITION,   // "POSITION:<0|1>,<ms>"
  TELEM_MAGNET,     // "MAGNET:<0|1>,<ms>"
  TELEM_LICK,       // "LICK<ch>,<ms>"
  TELEM_REWARD,     // "REWARD:<ch>,<count>,<ms>"
  TELEM_PARAM,      // "PARAM:<name>,<value>"  (ack of an applied SET)
  TELEM_TARE,       // "#TARE ok"  (confirmation that scale.tare() ran)
};

struct TelemRec {
  uint8_t  type;      // TelemType
  uint8_t  channel;   // LICK spout (1/2); unused otherwise
  float    value;     // weight, 0/1 state, or reward count
  uint32_t dev_ms;    // millis() captured at the event
};

// Tunable-parameter identifiers and the host command record.
enum ParamId : uint8_t {
  PARAM_REWARD_DURATION1,
  PARAM_REWARD_DURATION2,
  PARAM_REWARD_INTERVAL1,
  PARAM_REWARD_INTERVAL2,
  PARAM_MAG_FIX_DURATION,
  PARAM_SCALE_HIGH,
  PARAM_SCALE_LOW,
  PARAM_DUMP = 200,   // not a stored param: request a full parameter dump
  PARAM_TARE = 201,   // not a stored param: run scale.tare() once
};

struct ParamCmd {
  uint8_t id;      // ParamId
  float   value;   // new value (cast on apply)
};

struct ParamSpec { const char *name; uint8_t id; float lo; float hi; };
