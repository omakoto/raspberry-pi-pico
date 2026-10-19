// Runs every TEST registered by the test_*.cpp files.

#include <cstdio>

#include "minitest.hpp"

int main() {
  int before = 0;
  for (const minitest::Case& test : minitest::cases()) {
    test.fn();
    const bool failed = minitest::failures() != before;
    std::printf("%s  %s\n", failed ? "FAIL" : "ok  ", test.name);
    before = minitest::failures();
  }
  std::printf("\n%zu tests, %d failed checks\n", minitest::cases().size(), minitest::failures());
  return minitest::failures() == 0 ? 0 : 1;
}
