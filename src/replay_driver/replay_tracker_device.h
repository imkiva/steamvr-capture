#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "openvr_driver.h"
#include "session/session_format.h"

namespace steamvr_capture::replay
{
class ReplayTrackerDevice : public vr::ITrackedDeviceServerDriver
{
public:
    explicit ReplayTrackerDevice(session::TrackerDescriptor descriptor);

    vr::EVRInitError Activate(std::uint32_t object_id) override;
    void EnterStandby() override;
    void* GetComponent(const char* component_name_and_version) override;
    void DebugRequest(const char* request, char* response_buffer, std::uint32_t response_buffer_size) override;
    vr::DriverPose_t GetPose() override;
    void Deactivate() override;

    const std::string& serial_number() const;
    void UpdateSample(const session::PoseSample& sample);

private:
    void ApplyTrackerRoleSetting();
    static vr::DriverPose_t ToDriverPose(const session::PoseSample& sample);

    session::TrackerDescriptor descriptor_;
    std::string serial_number_;
    std::atomic<vr::TrackedDeviceIndex_t> device_index_{vr::k_unTrackedDeviceIndexInvalid};
    std::mutex pose_mutex_;
    session::PoseSample current_sample_;
};
}  // namespace steamvr_capture::replay
