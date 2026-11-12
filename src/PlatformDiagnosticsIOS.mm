#import <Foundation/Foundation.h>

#include <mach/mach.h>

#include "PlatformDiagnostics.hpp"

namespace geode::culpritfinder {
    PlatformSnapshot collectPlatformSnapshot() {
        PlatformSnapshot snapshot{};

        mach_task_basic_info_data_t taskInfo{};
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

        const kern_return_t result = task_info(
            mach_task_self(),
            MACH_TASK_BASIC_INFO,
            reinterpret_cast<task_info_t>(&taskInfo),
            &count
        );

        if (result == KERN_SUCCESS) {
            snapshot.memoryValid = true;
            snapshot.residentBytes = static_cast<std::uint64_t>(taskInfo.resident_size);
            snapshot.virtualBytes = static_cast<std::uint64_t>(taskInfo.virtual_size);
        }

        NSProcessInfo* processInfo = [NSProcessInfo processInfo];
        snapshot.physicalMemoryBytes =
            static_cast<std::uint64_t>(processInfo.physicalMemory);
        snapshot.thermalState = static_cast<int>(processInfo.thermalState);
        snapshot.activeProcessorCount =
            static_cast<int>(processInfo.activeProcessorCount);

        return snapshot;
    }
}
