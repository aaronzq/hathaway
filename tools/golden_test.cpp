// ---------------------------------------------------------------------------
// Protocol test.
//
// OUTBOUND: the wire format changed deliberately, so this is a spec test --
//   every line the firmware can emit is checked against its exact expected
//   bytes, then handed to ingest.py (by run_golden.sh) to confirm it survives
//   the round trip with the same type / channel / value / device time.
//
// INBOUND: the command format did NOT change, so these are still compared
//   against the real pre-refactor handleCommandLine(), sliced out of git by
//   extract.py. That side must stay byte-identical.
// ---------------------------------------------------------------------------
#include <Arduino.h>   // -Ishim
#include <vector>
#include <string>
#include <cstdio>

std::string g_serialOut;
SerialShim  Serial;
CapturedCmd g_lastCmd = {0, 0.0f, false};

// ======================= OLD firmware (inbound only) =======================
namespace Old {
const int RIG_ID = 1;
unsigned long REWARD_DURATION1 = 50;
unsigned long REWARD_DURATION2 = 50;
unsigned long REWARD_INTERVAL1 = 3000;
unsigned long REWARD_INTERVAL2 = 3000;
unsigned long MAG_FIX_DURATION = 5000;
float SCALE_HIGH_THRESH = 40.0;
float SCALE_LOW_THRESH  = 10.0;
QueueHandle_t cmdQueue = (QueueHandle_t)1;   // non-null so sends "succeed"

#include "old/protocol_old.h"
#include "old/old_extracted.inc"
}  // namespace Old

// ============================ NEW firmware =================================
namespace New {
#include "../protocol.h"
// Tunable globals and apply-function stubs, generated from CMD_TABLE and
// behavior_task.h by extract.py -- so adding a parameter or an action to the
// sketch needs no edit here.
#include "new_stubs.inc"
#include "new_extracted.inc"

inline int slotOf(const char *name) {
  return protoFindCmd(CMD_TABLE, CMD_COUNT, name);
}

inline std::string line(uint8_t type, uint8_t ch, float v, uint32_t ms) {
  char b[128];
  TelemRec r = {type, ch, v, ms};
  int n = protoFormat(b, sizeof(b), 1, TELEM_TABLE, TELEM_COUNT,
                      CMD_TABLE, CMD_COUNT, r);
  return std::string(b, n > 0 ? n : 0);
}
}  // namespace New

// ================================ harness ==================================
static int failures = 0, checks = 0;
static std::vector<std::string> emitted;   // "<expect>\t<line>" for ingest.py

static void expectSame(const char *what, const std::string &got,
                       const std::string &want) {
  checks++;
  if (got != want) {
    failures++;
    printf("  FAIL %-26s got=%-30s want=%s\n", what,
           ("[" + got + "]").c_str(), ("[" + want + "]").c_str());
  }
}

// Check the exact bytes, and queue the line for the ingest.py round trip.
static void checkWire(uint8_t type, const char *tname, uint8_t ch, float v,
                      uint32_t ms, const char *want) {
  std::string got = New::line(type, ch, v, ms);
  expectSame(tname, got, std::string(want) + "\n");
  if (got.empty()) return;
  got.pop_back();                       // drop the newline
  char meta[192];
  snprintf(meta, sizeof(meta), "%s|%u|%g|%lu\t%s", tname, (unsigned)ch,
           (double)v, (unsigned long)ms, got.c_str());
  emitted.push_back(meta);
}

// Old ParamId -> new CMD_TABLE slot, resolved by NAME so that reordering,
// adding or removing table rows cannot invalidate the mapping.
static int oldIdToSlot(int id) {
  if (id == Old::PARAM_DUMP) return CMD_SLOT_DUMP;
  if (id == Old::PARAM_TARE) return New::slotOf("TARE");
  return New::slotOf(Old::paramName(id));
}

static void checkCommand(const char *line, bool expectDivergence = false) {
  g_serialOut.clear();
  g_lastCmd = {0, 0.0f, false};
  Old::handleCommandLine(line);
  std::string oldReply = g_serialOut;
  CapturedCmd oldCmd = g_lastCmd;

  New::CmdMsg m;
  char err[80] = "";
  bool ok = New::protoParseCommand(New::CMD_TABLE, New::CMD_COUNT, line, &m,
                                   err, sizeof(err)) == New::PARSE_OK;
  std::string newReply;
  if (!ok) { char b[128]; snprintf(b, sizeof(b), "1|%s\n", err); newReply = b; }

  char label[64];
  snprintf(label, sizeof(label), "cmd %.40s", line);

  if (expectDivergence) {
    checks++;
    printf("  NOTE %-26s old=%-30s new=%s\n", label,
           ("[" + oldReply + "]").c_str(),
           (ok ? "accepted" : "[" + newReply + "]").c_str());
    return;
  }
  expectSame(label, newReply, oldReply);
  checks++;
  bool same = (oldCmd.valid == ok) &&
              (!ok || (oldIdToSlot(oldCmd.id) == (int)m.slot &&
                       oldCmd.value == m.value));
  if (!same) {
    failures++;
    printf("  FAIL %-26s old id=%d v=%g valid=%d | new slot=%u v=%g ok=%d\n",
           label, oldCmd.id, oldCmd.value, oldCmd.valid, m.slot, m.value, ok);
  }
}

int main() {
  printf("\n--- outbound: one shape, NAME:<ch>,<val>,<ms> ---\n");
  checkWire(New::TELEM_WEIGHT,   "WEIGHT",   1, 20.14f, 12345, "1|WEIGHT:1,20.14,12345");
  checkWire(New::TELEM_WEIGHT,   "WEIGHT",   1, -3.5f,  12346, "1|WEIGHT:1,-3.5,12346");
  checkWire(New::TELEM_WEIGHT,   "WEIGHT",   1, 0.0f,   12347, "1|WEIGHT:1,0,12347");
  checkWire(New::TELEM_POSITION, "POSITION", 1, 1.0f,   12348, "1|POSITION:1,1,12348");
  checkWire(New::TELEM_POSITION, "POSITION", 1, 0.0f,   12349, "1|POSITION:1,0,12349");
  checkWire(New::TELEM_MAGNET,   "MAGNET",   1, 1.0f,   999,   "1|MAGNET:1,1,999");
  checkWire(New::TELEM_MAGNET,   "MAGNET",   1, 0.0f,   4294967295u, "1|MAGNET:1,0,4294967295");
  checkWire(New::TELEM_LICK,     "LICK",     1, 1.0f,   12350, "1|LICK:1,1,12350");
  checkWire(New::TELEM_LICK,     "LICK",     2, 1.0f,   12351, "1|LICK:2,1,12351");
  checkWire(New::TELEM_REWARD,   "REWARD",   1, 7.0f,   12352, "1|REWARD:1,7,12352");
  checkWire(New::TELEM_REWARD,   "REWARD",   2, 253.0f, 12353, "1|REWARD:2,253,12353");

  printf("\n--- outbound: every table row renders ---\n");
  for (size_t i = 0; i < New::TELEM_COUNT; i++) {
    checks++;
    std::string s = New::line(New::TELEM_TABLE[i].id, 1, 1.0f, 42);
    char want[64];
    snprintf(want, sizeof(want), "1|%s:1,1,42\n", New::TELEM_TABLE[i].name);
    if (s != want) {
      failures++;
      printf("  FAIL row %zu: got [%s] want [%s]\n", i, s.c_str(), want);
    }
  }

  printf("\n--- outbound: startup schema announcement ---\n");
  for (size_t i = 0; i < New::TELEM_COUNT; i++) {
    char want[96];
    snprintf(want, sizeof(want), "1|#DEF %s,%c\n", New::TELEM_TABLE[i].name,
             (char)New::TELEM_TABLE[i].kind);
    std::string l = New::line(New::TELEM_INTERNAL_DEF, (uint8_t)i, 0, 0);
    expectSame("#DEF", l, want);
    if (l.empty()) continue;
    l.pop_back();
    emitted.push_back("#DEF\t" + l);   // ingest must learn from these
  }
  // Every row must have declared a kind.
  for (size_t i = 0; i < New::TELEM_COUNT; i++) {
    checks++;
    uint8_t k = New::TELEM_TABLE[i].kind;
    if (k != New::TELEM_SAMPLE && k != New::TELEM_EVENT) {
      failures++;
      printf("  FAIL %s has no valid kind (%u)\n", New::TELEM_TABLE[i].name, k);
    }
  }

  printf("\n--- outbound: control plane (unchanged) ---\n");
  expectSame("PARAM int",
             New::line(New::TELEM_INTERNAL_PARAM,
                       New::slotOf("REWARD_DURATION1"), 50.0f, 5),
             "1|PARAM:REWARD_DURATION1,50\n");
  expectSame("PARAM float",
             New::line(New::TELEM_INTERNAL_PARAM,
                       New::slotOf("SCALE_HIGH_THRESH"), 12.5f, 5),
             "1|PARAM:SCALE_HIGH_THRESH,12.5\n");
  expectSame("TARE ack",
             New::line(New::TELEM_INTERNAL_ACK, New::slotOf("TARE"), 0.0f, 5),
             "1|#TARE ok\n");
  for (size_t i = 0; i < New::CMD_COUNT; i++)
    if (New::CMD_TABLE[i].kind == New::CMD_PARAM) {
      std::string l = New::line(New::TELEM_INTERNAL_PARAM, (uint8_t)i,
                                New::protoLoad(New::CMD_TABLE[i]), 5);
      l.pop_back();
      emitted.push_back("#IGNORE\t" + l);   // must NOT become a data record
    }

  printf("\n--- inbound: byte-identical to the old firmware ---\n");
  checkCommand("SET REWARD_DURATION1 75");
  checkCommand("SET REWARD_DURATION2 0");
  checkCommand("SET REWARD_INTERVAL1 2500");
  checkCommand("SET REWARD_INTERVAL2 60000");
  checkCommand("SET MAG_FIX_DURATION 1000");
  checkCommand("SET SCALE_HIGH_THRESH 12.5");
  checkCommand("SET SCALE_LOW_THRESH -49.9");
  checkCommand("SET REWARD_DURATION1 1001");     // above range
  checkCommand("SET SCALE_LOW_THRESH -50.1");    // below range
  checkCommand("SET NOT_A_PARAM 5");             // unknown
  checkCommand("SET REWARD_DURATION1");          // malformed
  checkCommand("gibberish");                     // malformed
  checkCommand("TARE");
  checkCommand("DUMP");
  checkCommand("GET");

  printf("\n--- inbound: intentional divergences (new is stricter) ---\n");
  checkCommand("TAREXYZ", true);
  checkCommand("DUMPSTER", true);

  printf("\n--- lines handed to ingest.py ---\n");
  for (auto &l : emitted) printf("WIRE %s\n", l.c_str());

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
