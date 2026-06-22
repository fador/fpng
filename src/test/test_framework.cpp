#include "test/test_framework.hpp"

#include <iostream>

namespace fpng {
namespace test {

int tests_passed = 0;
int tests_failed = 0;

int summary() {
    std::cout << "\nResults: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}

} // namespace test
} // namespace fpng
