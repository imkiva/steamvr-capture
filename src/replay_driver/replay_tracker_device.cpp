#include "replay_driver/replay_tracker_device.h"

#include "replay_driver/driver_log.h"

namespace steamvr_capture::replay
{
namespace
{
constexpr const char* kDriverName = "steamvr_capture_replay";
constexpr const char* kInputProfilePath = "{steamvr_capture_replay}/input/svrcap_tracker_profile.json";
constexpr const char* kControllerType = "svrcap_replay_tracker";
}  // namespace

ReplayTrackerDevice::ReplayTrackerDevice(session::TrackerDescriptor descriptor)
    : descriptor_(std::move(descriptor))
{
    serial_number_ = "svrcap_replay_" + descriptor_.serial;
}

vr::EVRInitError ReplayTrackerDevice::Activate(const std::uint32_t object_id)
{
    device_index_ = object_id;
    const vr::PropertyContainerHandle_t container =
        vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id);

    vr::VRProperties()->SetStringProperty(container, vr::Prop_ModelNumber_String, descriptor_.model_number.c_str());
    vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, "steamvr-capture");
    vr::VRProperties()->SetStringProperty(container, vr::Prop_ControllerType_String, kControllerType);
    vr::VRProperties()->SetStringProperty(container, vr::Prop_InputProfilePath_String, kInputProfilePath);
    vr::VRProperties()->SetBoolProperty(container, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);

    ApplyTrackerRoleSetting();
    DriverLog("Activated replay tracker serial=%s role=%s", serial_number_.c_str(), descriptor_.role.c_str());
    return vr::VRInitError_None;
}

void ReplayTrackerDevice::EnterStandby()
{
}

void* ReplayTrackerDevice::GetComponent(const char* component_name_and_version)
{
    (void)component_name_and_version;
    return nullptr;
}

void ReplayTrackerDevice::DebugRequest(
    const char* request, char* response_buffer, const std::uint32_t response_buffer_size)
{
    (void)request;
    if (response_buffer != nullptr && response_buffer_size > 0)
    {
        response_buffer[0] = '\0';
    }
}

vr::DriverPose_t ReplayTrackerDevice::GetPose()
{
    std::lock_guard<std::mutex> lock(pose_mutex_);
    return ToDriverPose(current_sample_);
}

void ReplayTrackerDevice::Deactivate()
{
    device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}

const std::string& ReplayTrackerDevice::serial_number() const
{
    return serial_number_;
}

void ReplayTrackerDevice::UpdateSample(const session::PoseSample& sample)
{
    vr::DriverPose_t pose{};
    {
        std::lock_guard<std::mutex> lock(pose_mutex_);
        current_sample_ = sample;
        pose = ToDriverPose(current_sample_);
    }

    const auto index = device_index_.load();
    if (index != vr::k_unTrackedDeviceIndexInvalid)
    {
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index, pose, sizeof(vr::DriverPose_t));
    }
}

void ReplayTrackerDevice::ApplyTrackerRoleSetting()
{
    if (descriptor_.role.empty())
    {
        return;
    }

    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const std::string key = "/devices/" + std::string(kDriverName) + "/" + serial_number_;
    vr::VRSettings()->SetString(vr::k_pch_Trackers_Section, key.c_str(), descriptor_.role.c_str(), &error);
    if (error != vr::VRSettingsError_None)
    {
        DriverLog("Failed to write tracker role for %s", serial_number_.c_str());
    }
}

vr::DriverPose_t ReplayTrackerDevice::ToDriverPose(const session::PoseSample& sample)
{
    vr::DriverPose_t pose{};
    pose.poseTimeOffset = 0.0;
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation.w = 1.0;
    pose.qRotation.w = sample.rotation_wxyz[0];
    pose.qRotation.x = sample.rotation_wxyz[1];
    pose.qRotation.y = sample.rotation_wxyz[2];
    pose.qRotation.z = sample.rotation_wxyz[3];
    pose.vecPosition[0] = sample.position_m[0];
    pose.vecPosition[1] = sample.position_m[1];
    pose.vecPosition[2] = sample.position_m[2];
    pose.vecVelocity[0] = sample.linear_velocity_mps[0];
    pose.vecVelocity[1] = sample.linear_velocity_mps[1];
    pose.vecVelocity[2] = sample.linear_velocity_mps[2];
    pose.vecAngularVelocity[0] = sample.angular_velocity_rps[0];
    pose.vecAngularVelocity[1] = sample.angular_velocity_rps[1];
    pose.vecAngularVelocity[2] = sample.angular_velocity_rps[2];
    pose.poseIsValid = sample.pose_valid;
    pose.deviceIsConnected = sample.device_connected;
    pose.result = static_cast<vr::ETrackingResult>(sample.tracking_result);
    return pose;
}
}  // namespace steamvr_capture::replay
