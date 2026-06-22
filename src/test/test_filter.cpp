#include "test/test_framework.hpp"
#include "png/filter.hpp"

using namespace fpng;
using namespace fpng::test;

void test_paeth() {
    std::cout << "Paeth Predictor Tests:\n";
    run_test("Paeth: left wins (a=20,b=5,c=5)",     paeth_predictor(20,5,5) == 20);
    run_test("Paeth: up wins (a=50,b=10,c=40)",      paeth_predictor(50,10,40) == 10);
    run_test("Paeth: upper-left wins (a=50,b=60,c=55)", paeth_predictor(50,60,55) == 55);
    run_test("Paeth: tie a-b (a=10,b=10,c=50)",      paeth_predictor(10,10,50) == 10);
    run_test("Paeth: tie a-c (a=10,b=25,c=20)",      paeth_predictor(10,25,20) == 10);
    run_test("Paeth: all zero",                      paeth_predictor(0,0,0) == 0);
    run_test("Paeth: all 255",                       paeth_predictor(255,255,255) == 255);
}
