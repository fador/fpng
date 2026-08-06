#include "test/test_framework.hpp"

using namespace fpng::test;

extern void test_crc32();
extern void test_filter();
extern void test_paeth();
extern void test_inflate();
extern void test_roundtrip();
extern void test_full_compress_pipeline();
extern void test_deflate();

int main() {
    std::cout << "fpng Test Suite\n";
    std::cout << "===============\n\n";

    test_crc32();
    std::cout << "\n";
    test_filter();
    std::cout << "\n";
    test_paeth();
    std::cout << "\n";
    test_inflate();
    std::cout << "\n";
    test_deflate();
    std::cout << "\n";
    test_roundtrip();
    std::cout << "\n";
    test_full_compress_pipeline();

    return summary();
}
