extern "C" __attribute__((visibility("default"), noinline, aligned(4))) int
dejavu_test_cold_target(int value) {
    return value + 7;
}
