#include "../lick.h"

#include <cstdio>
#include <string>

unsigned long LICK_DEBOUNCE_TIME = 20;

static int g_failures = 0;

static void check(bool ok, const std::string &what) {
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) g_failures++;
}

static void setPin(int pin, int value, uint32_t now) {
  g_pinValues[pin] = value;
  g_arduinoMillis = now;
}

static void test_debounce_time_can_be_changed_after_construction() {
  printf("lick: debounce time can be changed after construction\n");

  const int pin = 5;
  setPin(pin, LOW, 1000);
  LickHandler lick(pin, 20);

  setPin(pin, HIGH, 1001);
  check(!lick.update(), "first high sample starts the original 20 ms debounce");
  setPin(pin, HIGH, 1020);
  check(!lick.update(), "19 ms is still inside the original debounce");
  setPin(pin, HIGH, 1021);
  check(lick.update(), "20 ms accepts the lick");

  setPin(pin, LOW, 1022);
  check(!lick.update(), "falling edge starts reset debounce");
  setPin(pin, LOW, 1042);
  check(!lick.update(), "falling edge resets without reporting a lick");

  lick.setDebounceTime(5);

  setPin(pin, HIGH, 1043);
  check(!lick.update(), "first high sample starts the updated 5 ms debounce");
  setPin(pin, HIGH, 1047);
  check(!lick.update(), "4 ms is still inside the updated debounce");
  setPin(pin, HIGH, 1048);
  check(lick.update(), "5 ms accepts the lick");
}

int main() {
  test_debounce_time_can_be_changed_after_construction();

  if (g_failures) {
    printf("\n%d failure(s)\n", g_failures);
    return 1;
  }
  printf("\nall lick tests passed\n");
  return 0;
}
