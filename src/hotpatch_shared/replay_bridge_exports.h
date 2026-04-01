#pragma once

#include <cstddef>
#include <cstdint>

namespace steamvr_capture::hotpatch
{
constexpr std::size_t kReplayBridgeMaxSerialBytes = 96u;

struct ReplayBridgeTrackedDeviceInfo
{
    std::uint32_t device_index = 0u;
    std::int32_t device_class = 0;
    std::uint32_t is_present = 0u;
    char serial[kReplayBridgeMaxSerialBytes]{};
};

using GetServerDriverHostFn = void* (*)();
using GetMaxTrackedDeviceCountFn = std::uint32_t (*)();
using GetTrackedDeviceInfoFn = bool (*)(std::uint32_t device_index, ReplayBridgeTrackedDeviceInfo* info);
}  // namespace steamvr_capture::hotpatch
