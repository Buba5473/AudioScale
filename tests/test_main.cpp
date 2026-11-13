/*
   tests/test_main.cpp — точка входа для unit-тестов scale.
*/

#include "test_common.hpp"

int main() {
    int passed = 0;
    for (auto& t : scale::test::registry()) {
        try {
            t.fn();
            std::printf("[PASS] %s\n", t.name);
            ++passed;
        } catch (const std::exception& e) {
            std::printf("[FAIL] %s: %s\n", t.name, e.what());
            ++scale::test::failure_count();
        }
    }
    std::printf("---\n%d passed, %d failed\n",
                passed, scale::test::failure_count());
    return scale::test::failure_count() == 0 ? 0 : 1;
}
