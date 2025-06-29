// Read the CPU timestamp counter (TSC) with minimal overhead
inline uint64_t rdtsc() {
    return __rdtsc();
}

// Estimate CPU frequency (cycles per nanosecond) using sleep
inline double calibrate_cycles_per_ns() {
    uint64_t t0 = rdtsc();
    timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    uint64_t t1 = rdtsc();

    uint64_t delta_cycles = t1 - t0;
    uint64_t delta_ns = (ts1.tv_sec - ts0.tv_sec) * 1'000'000'000 + (ts1.tv_nsec - ts0.tv_nsec);
    return static_cast<double>(delta_cycles) / delta_ns;
}
