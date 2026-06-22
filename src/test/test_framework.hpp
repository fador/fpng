#pragma once

#include <iostream>
#include <string>

namespace fpng {
namespace test {

extern int tests_passed;
extern int tests_failed;

inline void run_test(const std::string& name, bool result) {
    if (result) {
        tests_passed++;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        tests_failed++;
        std::cout << "  [FAIL] " << name << "\n";
    }
}

int summary();

} // namespace test
} // namespace fpng
