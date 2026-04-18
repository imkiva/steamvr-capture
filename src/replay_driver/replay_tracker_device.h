#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "openvr_driver.h"
#include "session/session_format.h"

namespace steamvr_capture::replay
{
class ReplayTrackerDevice : public vr::ITrackedDeviceServerDriver
{
public:
    ReplayTrackerDevice(
        std::size_t slot_index,
        const session::TrackerDescriptor& descriptor,
        std::string serial_number);

    vr::EVRInitError Activate(std::uint32_t object_id) override;
    void EnterStandby() override;
    void* GetComponent(const char* component_name_and_version) override;
    void DebugRequest(const char* request, char* response_buffer, std::uint32_t response_buffer_size) override;
    vr::DriverPose_t GetPose() override;
    void Deactivate() override;

    const std::string& serial_number() const;
    static std::string BuildSerialNumber(const session::TrackerDescriptor& descriptor, std::size_t fallback_slot_index);
    void UpdateDescriptor(const std::optional<session::TrackerDescriptor>& descriptor);
    void UpdateSample(const session::PoseSample& sample);
    void SetDisconnected();

private:
    void ApplyPropertiesLocked();
    void ApplyTrackerRoleSettingLocked();
    static vr::DriverPose_t ToDriverPose(const std::optional<session::PoseSample>& sample);

    std::size_t slot_index_ = 0;
    std::optional<session::TrackerDescriptor> descriptor_;
    std::string serial_number_;
    std::atomic<vr::TrackedDeviceIndex_t> device_index_{vr::k_unTrackedDeviceIndexInvalid};
    vr::PropertyContainerHandle_t property_container_{vr::k_ulInvalidPropertyContainer};
    std::mutex pose_mutex_;
    std::optional<session::PoseSample> current_sample_;
};
}  // namespace steamvr_capture::replay
