// Protocol parser regression tests.
//
// These tests protect both the old command forms and action commands with one
// numeric argument. Keep them small: the parser is pure and should stay boring.
#include "../protocol.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
static unsigned long PARAM_A = 10;
static float PARAM_B = 0.5f;

static void actionNoArg(float) {}
static void actionValue(float) {}

static const CmdSpec CMDS[] = {
  PARAM_U32(PARAM_A, 0, 100, nullptr),
  PARAM_F32(PARAM_B, -1, 1, nullptr),
  ACTION(NO_ARG, actionNoArg),
  ACTION_F32(VALUE_MM, -2, 2, actionValue),
  ACTION_I32(VALUE_PULSES, -6000, 6000, actionValue),
};
static const size_t CMD_N = sizeof(CMDS) / sizeof(CMDS[0]);

static void check(bool ok, const char *what) {
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) g_failures++;
}

static bool parse(const char *line, CmdMsg &m, char *err, size_t errcap) {
  return protoParseCommand(CMDS, CMD_N, line, &m, err, errcap) == PARSE_OK;
}

static void test_existing_commands_still_parse() {
  printf("protocol: existing commands still parse\n");
  CmdMsg m;
  char err[80];

  check(parse("DUMP", m, err, sizeof(err)) && m.slot == CMD_SLOT_DUMP,
        "DUMP still requests a dump");
  check(parse("GET", m, err, sizeof(err)) && m.slot == CMD_SLOT_DUMP,
        "GET still requests a dump");
  check(parse("NO_ARG", m, err, sizeof(err)) && m.slot == 2 && m.value == 0.0f,
        "bare action still parses with value zero");
  check(parse("SET PARAM_A 42", m, err, sizeof(err)) && m.slot == 0 &&
          std::fabs(m.value - 42.0f) < 0.001f,
        "SET integer parameter still parses");
  check(parse("SET PARAM_B -0.25", m, err, sizeof(err)) && m.slot == 1 &&
          std::fabs(m.value + 0.25f) < 0.001f,
        "SET float parameter still parses");
}

static void test_action_values_parse() {
  printf("protocol: ranged action values parse\n");
  CmdMsg m;
  char err[80];

  check(parse("VALUE_MM 1.25", m, err, sizeof(err)) && m.slot == 3 &&
          std::fabs(m.value - 1.25f) < 0.001f,
        "float action accepts a value inside range");
  check(parse("VALUE_MM -2", m, err, sizeof(err)) && m.slot == 3 &&
          std::fabs(m.value + 2.0f) < 0.001f,
        "float action accepts lower boundary");
  check(parse("VALUE_PULSES -6000", m, err, sizeof(err)) && m.slot == 4 &&
          std::fabs(m.value + 6000.0f) < 0.001f,
        "integer action accepts pulse lower boundary");
  check(parse("VALUE_PULSES 6000", m, err, sizeof(err)) && m.slot == 4 &&
          std::fabs(m.value - 6000.0f) < 0.001f,
        "integer action accepts pulse upper boundary");
}

static void test_bad_lines_are_rejected() {
  printf("protocol: bad command lines are rejected\n");
  CmdMsg m;
  char err[80];

  check(!parse("VALUE_MM 2.01", m, err, sizeof(err)) &&
          std::strcmp(err, "#ERR range: VALUE_MM=2.01") == 0,
        "float action rejects high out of range");
  check(!parse("VALUE_PULSES -6001", m, err, sizeof(err)) &&
          std::strcmp(err, "#ERR range: VALUE_PULSES=-6001") == 0,
        "integer action rejects low out of range");
  check(!parse("VALUE_MM", m, err, sizeof(err)) &&
          std::strcmp(err, "#ERR parse: VALUE_MM") == 0,
        "valued action rejects missing value");
  check(!parse("NO_ARG 1", m, err, sizeof(err)) &&
          std::strcmp(err, "#ERR parse: NO_ARG 1") == 0,
        "bare action rejects accidental value");
  check(!parse("SET VALUE_MM 1", m, err, sizeof(err)) &&
          std::strcmp(err, "#ERR unknown: VALUE_MM") == 0,
        "valued action is not accepted through SET");
  check(!parse("SET PARAM_A 101", m, err, sizeof(err)) &&
          std::strcmp(err, "#ERR range: PARAM_A=101") == 0,
        "SET parameter range checks still work");
}

// A range check cannot catch NaN: every comparison against it is false, so
// "nan" sails through `val < lo || val > hi` and is then cast to an integer,
// which is undefined. sscanf accepts the spelling, so it is reachable from the
// wire -- and on a valued action it would reach the hardware as a step count
// nobody chose. Rejected before the range test instead.
static void test_nan_is_rejected() {
  printf("protocol: NaN is rejected everywhere a number is read\n");
  CmdMsg m;
  char err[80];

  check(!parse("VALUE_MM nan", m, err, sizeof(err)) &&
          std::strcmp(err, "#ERR parse: VALUE_MM nan") == 0,
        "float action rejects NaN");
  check(!parse("VALUE_PULSES nan", m, err, sizeof(err)) &&
          std::strcmp(err, "#ERR parse: VALUE_PULSES nan") == 0,
        "integer action rejects NaN");
  check(!parse("SET PARAM_B nan", m, err, sizeof(err)) &&
          std::strcmp(err, "#ERR parse: SET PARAM_B nan") == 0,
        "SET rejects NaN");
  // Infinities need no special case: they fail the range test like any other
  // out-of-range value. Checked so that stays true.
  check(!parse("VALUE_MM inf", m, err, sizeof(err)) &&
          std::strcmp(err, "#ERR range: VALUE_MM=inf") == 0,
        "float action rejects infinity as out of range");
}

int main() {
  test_existing_commands_still_parse();
  test_action_values_parse();
  test_bad_lines_are_rejected();
  test_nan_is_rejected();

  printf("\n%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
