// ntest.h — a 100-line test framework. gtest is not downloadable in this sandbox
// (see ADR-001), and a vendored copy would be a licence/size liability for a header.
#ifndef NI_NTEST_H
#define NI_NTEST_H
#include <math.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace ntest {
inline int& fails() {
  static int f = 0;
  return f;
}
inline int& checks() {
  static int c = 0;
  return c;
}
inline std::vector<std::string>& notes() {
  static std::vector<std::string> n;
  return n;
}
inline void note(const std::string& s) {
  notes().push_back(s);
  printf("  note: %s\n", s.c_str());
}
inline void fail(const char* file, int line, const std::string& msg) {
  fails()++;
  printf("  FAIL %s:%d  %s\n", file, line, msg.c_str());
}
inline int summary(const char* name) {
  printf("[%s] %d checks, %d failures\n", name, checks(), fails());
  return fails() ? 1 : 0;
}
}  // namespace ntest

#define NT_CHECK(cond)                                              \
  do {                                                              \
    ntest::checks()++;                                              \
    if (!(cond)) ntest::fail(__FILE__, __LINE__, "check: " #cond);  \
  } while (0)
#define NT_CHECK_MSG(cond, msg)                                     \
  do {                                                              \
    ntest::checks()++;                                              \
    if (!(cond)) ntest::fail(__FILE__, __LINE__, std::string(msg)); \
  } while (0)
#define NT_EQ(a, b)                                                    \
  do {                                                                 \
    ntest::checks()++;                                                 \
    auto _va = (a);                                                    \
    auto _vb = (b);                                                    \
    if (!(_va == _vb)) {                                               \
      char buf[512];                                                   \
      snprintf(buf, sizeof buf, "%s != %s  (%lld vs %lld)", #a, #b,   \
               (long long)_va, (long long)_vb);                        \
      ntest::fail(__FILE__, __LINE__, buf);                            \
    }                                                                  \
  } while (0)
#define NT_NEAR(a, b, tol)                                                   \
  do {                                                                       \
    ntest::checks()++;                                                         \
    double _va = (double)(a), _vb = (double)(b);                              \
    if (fabs(_va - _vb) > (double)(tol)) {                                    \
      char buf[512];                                                          \
      snprintf(buf, sizeof buf, "%s=%.9g not within %g of %s=%.9g", #a, _va,  \
               (double)(tol), #b, _vb);                                       \
      ntest::fail(__FILE__, __LINE__, buf);                                   \
    }                                                                         \
  } while (0)
#define NT_STR_EQ(a, b)                                                      \
  do {                                                                       \
    ntest::checks()++;                                                         \
    std::string _va = (a), _vb = (b);                                        \
    if (_va != _vb) {                                                        \
      ntest::fail(__FILE__, __LINE__, std::string(#a) + "=\"" + _va + "\" != \"" + _vb + "\""); \
    }                                                                        \
  } while (0)
#endif
