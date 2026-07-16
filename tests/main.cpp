// The suite's entry point. vcpkg ships gtest_main in manual-link precisely so
// a project chooses its runner explicitly — this is that choice, with no
// linker magic.
#include <gtest/gtest.h>

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
