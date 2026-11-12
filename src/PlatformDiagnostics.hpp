#pragma once

#include <cstdint>

namespace geode::culpritfinder {
    struct PlatformSnapshot {
        bool memoryValid = false;
        std::uint64_t residentBytes = 0;
        std::uint64_t virtualBytes = 0;
        std::uint64_t physicalMemoryBytes = 0;
        int thermalState = -1;
        int activeProcessorCount = 0;
    };

    PlatformSnapshot collectPlatformSnapshot();
}
