// A tiny unit-test framework for the host tests (no dependencies, no exceptions).
//
//   TEST(name) { CHECK(cond); CHECK_EQ(actual, expected); }
//
// Tests register themselves; test_main.cpp runs them all and returns non-zero if any CHECK failed.

#pragma once

#include <cstdio>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace minitest {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& cases() {
  static std::vector<Case> all;
  return all;
}

inline int& failures() {
  static int count = 0;
  return count;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { cases().push_back({name, std::move(fn)}); }
};

template <typename T>
std::string show(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}
inline std::string show(const std::string& value) { return "\"" + value + "\""; }
inline std::string show(bool value) { return value ? "true" : "false"; }
// These are declared before the templates that call them, because the call is found by name lookup
// at the point of definition (argument-dependent lookup would only search namespace std).
template <typename T>
std::string show(const std::vector<T>& values);
template <typename T>
std::string show(const std::optional<T>& value);

template <typename T>
std::string show(const std::vector<T>& values) {
  std::string out = "{";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += ", ";
    out += show(values[i]);
  }
  return out + "}";
}
template <typename T>
std::string show(const std::optional<T>& value) { return value ? "some(" + show(*value) + ")" : "none"; }

}  // namespace minitest

#define TEST(name)                                                    \
  static void test_##name();                                          \
  static minitest::Registrar registrar_##name(#name, test_##name);    \
  static void test_##name()

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++minitest::failures();                                                    \
    }                                                                            \
  } while (0)

// The values are copied, not bound by reference: an argument like `status().candidate->mac` is a member
// of a temporary that would be destroyed before the comparison.
#define CHECK_EQ(actual, expected)                                                         \
  do {                                                                                     \
    const auto a_ = (actual);                                                              \
    const auto e_ = (expected);                                                            \
    if (!(a_ == e_)) {                                                                     \
      std::printf("  FAIL %s:%d: %s == %s\n    actual:   %s\n    expected: %s\n", __FILE__, \
                  __LINE__, #actual, #expected, minitest::show(a_).c_str(),                \
                  minitest::show(e_).c_str());                                             \
      ++minitest::failures();                                                              \
    }                                                                                      \
  } while (0)
