#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openvr.h"
#include "session/session_format.h"

namespace steamvr_capture::capture
{
struct TrackerSnapshot
{
    std::uint32_t device_index = vr::k_unTrackedDeviceIndexInvalid;
    session::TrackerDescriptor descriptor;
};

std::vector<TrackerSnapshot> EnumerateTrackers(vr::IVRSystem& vr_system, vr::IVRSettings& settings);
bool PopulateSampleFromPose(const vr::TrackedDevicePose_t& pose, std::uint64_t timestamp_ns, session::PoseSample* sample);
}  // namespace steamvr_capture::capture
