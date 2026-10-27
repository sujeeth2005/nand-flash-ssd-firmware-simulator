#pragma once
// Minimal dependency-free test harness (no network / GoogleTest needed).
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace nt {

struct TestCase {
    std::string name;
    void (*fn)();
};
std::vector<TestCase>& registry();
void report_failure(const char* file, int line, const std::string& msg);

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};
struct Abort {};  // thrown by REQUIRE

template <class A, class B>
std::string cmp_msg(const char* ea, const char* eb, const A& a, const B& b) {
    std::ostringstream os;
    os << ea << " == " << eb << "   (" << a << " vs " << b << ")";
    return os.str();
}

}  // namespace nt

#define TEST(suite, name)                                                         \
    static void suite##_##name();                                                 \
    static ::nt::Registrar reg_##suite##_##name(#suite "." #name, &suite##_##name); \
    static void suite##_##name()

#define CHECK(cond) do { if (!(cond)) ::nt::report_failure(__FILE__, __LINE__, #cond); } while (0)
#define CHECK_EQ(a, b) do { auto&& _a = (a); auto&& _b = (b); \
    if (!(_a == _b)) ::nt::report_failure(__FILE__, __LINE__, ::nt::cmp_msg(#a, #b, _a, _b)); } while (0)
#define CHECK_MSG(cond, msg) do { if (!(cond)) { std::ostringstream _os; _os << #cond << " : " << msg; \
    ::nt::report_failure(__FILE__, __LINE__, _os.str()); } } while (0)
#define REQUIRE(cond) do { if (!(cond)) { ::nt::report_failure(__FILE__, __LINE__, #cond); throw ::nt::Abort(); } } while (0)
#define REQUIRE_EQ(a, b) do { auto&& _a = (a); auto&& _b = (b); \
    if (!(_a == _b)) { ::nt::report_failure(__FILE__, __LINE__, ::nt::cmp_msg(#a, #b, _a, _b)); throw ::nt::Abort(); } } while (0)
