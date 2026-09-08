#pragma once
#include <Arduino.h>
#include "protocol.h"

// ---------------------------------------------------------------------------
// comms.h -- the communication facility.
//
// Owns the dual-core split described in comms_architecture_plan.md:
//   core 0 (comms task)  owns Serial: formats and writes telemetry, reads and
//                        validates inbound command lines.
//   core 1 (loop)        runs the task. Never touches Serial. Enqueues
//                        telemetry with emit(); applies commands in service().
//
// The engine is driven entirely by the two tables you pass to begin(), so
// adding a message or a command never requires editing comms.cpp.
//
// Usage from the sketch:
//     Comms::begin(RIG_ID, TELEM_TABLE, TELEM_COUNT, CMD_TABLE, CMD_COUNT);
//     Comms::dumpParams();                       // in setup(), after begin()
//     Comms::service();                          // first line of loop()
//     Comms::emit(TELEM_LICK, 1, 1.0f, millis());
// ---------------------------------------------------------------------------

namespace Comms {

// Start Serial, create the queues, and launch the comms task on `core`.
// The tables must have static storage duration (they are kept by pointer).
void begin(int rigId,
           const TelemSpec *telemTable, size_t telemCount,
           const CmdSpec   *cmdTable,   size_t cmdCount,
           uint32_t baud = 115200, int core = 0);

// Enqueue one telemetry record. Safe to call from the control loop: never
// blocks. If the queue is full the record is dropped and dropped() increments.
void emit(uint8_t type, uint8_t channel, float value, uint32_t dev_ms);

// Drain and apply any validated commands. Call once at the top of loop().
// Runs on the control core, so parameter writes and device setters have a
// single writer -- no cross-core races.
void service();

// Emit every CMD_PARAM row as a "PARAM:<name>,<value>" line, so the host learns
// the full parameter state. Called at startup and by the "DUMP"/"GET" command.
void dumpParams();

// Emit "#DEF <NAME>,<S|E>" for every telemetry row, so the host learns the
// message set from the device instead of hard-coding it. Called at startup and
// by the "DUMP"/"GET" command.
void dumpSchema();

// Register a function to run at the end of a "DUMP"/"GET", after dumpParams().
//
// dumpParams() can only report CMD_PARAM rows, because a parameter is a value
// the engine holds. Some state the host wants on reconnect is not a parameter
// at all -- rail position, for one: the rig owns it, no SET can write it, and it
// changes only when a command moves the hardware. Without a hook, such a value
// would only reach a host that happened to be listening when it last changed.
//
// The hook runs on the control core, inside service(), so it may read device
// objects directly and should do nothing but emit(). Optional; unset by default.
void setDumpHook(void (*fn)());

// Telemetry records dropped because the queue was full (health metric).
uint32_t dropped();

}  // namespace Comms
