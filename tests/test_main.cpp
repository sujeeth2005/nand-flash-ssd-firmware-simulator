#include <chrono>
#include <cstring>

#include "test_framework.hpp"

namespace nt {
std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
static int g_failures_in_current = 0;
void report_failure(const char* file, int line, const std::string& msg) {
    ++g_failures_in_current;
    std::cout << "    FAILED " << file << ":" << line << ": " << msg << "\n";
}
}  // namespace nt

int main(int argc, char** argv) {
    std::string filter;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
        else if (!std::strcmp(argv[i], "--list")) list = true;
    }
    int ran = 0, failed = 0;
    for (const auto& t : nt::registry()) {
        if (!filter.empty() && t.name.find(filter) == std::string::npos) continue;
        if (list) { std::cout << t.name << "\n"; continue; }
        std::cout << "[ RUN  ] " << t.name << std::endl;
        nt::g_failures_in_current = 0;
        auto t0 = std::chrono::steady_clock::now();
        try {
            t.fn();
        } catch (const nt::Abort&) {
        } catch (const std::exception& e) {
            nt::report_failure("(exception)", 0, e.what());
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++ran;
        if (nt::g_failures_in_current) { ++failed; std::cout << "[ FAIL ] " << t.name << " (" << ms << " ms)\n"; }
        else std::cout << "[  OK  ] " << t.name << " (" << ms << " ms)\n";
    }
    if (!list) std::cout << "\n" << (ran - failed) << "/" << ran << " tests passed\n";
    return failed ? 1 : 0;
}
