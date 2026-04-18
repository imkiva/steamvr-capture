#include "replay_driver/replay_tracker_device.h"

#include "replay_driver/driver_log.h"

#include <cctype>
#include <utility>

namespace steamvr_capture::replay
{
namespace
{
constexpr const char* kDriverName = "steamvr_capture_replay";
constexpr const char* kInputProfilePath = "{steamvr_capture_replay}/input/svrcap_tracker_profile.json";
constexpr const char* kControllerType = "svrcap_replay_tracker";
constexpr const char* kManufacturerName = "steamvr-capture";
constexpr const char* kLighthouseTrackingSystem = "lighthouse";

constexpr std::int32_t kHmdClass = static_cast<std::int32_t>(vr::TrackedDeviceClass_HMD);
constexpr std::int32_t kControllerClass = static_cast<std::int32_t>(vr::TrackedDeviceClass_Controller);
constexpr std::int32_t kGenericTrackerClass = static_cast<std::int32_t>(vr::TrackedDeviceClass_GenericTracker);

bool IsSourceTracker(const session::TrackerDescriptor& descriptor)
{
    return descriptor.device_class == 0 || descriptor.device_class == kGenericTrackerClass;
}

bool IsSourceHmdOrController(const session::TrackerDescriptor& descriptor)
{
    return descriptor.device_class == kHmdClass || descriptor.device_class == kControllerClass;
}

std::string SourceClassLabel(const session::TrackerDescriptor& descriptor)
{
    switch (descriptor.device_class)
    {
    case kHmdClass:
        return "HMD";
    case kControllerClass:
        return "Controller";
    case kGenericTrackerClass:
        return "Tracker";
    default:
        return "Device";
    }
}

std::string BuildReplayModelNumber(const std::optional<session::TrackerDescriptor>& descriptor)
{
    if (!descriptor.has_value())
    {
        return "Kiva Tracker";
    }

    if (!descriptor->model_number.empty())
    {
        return descriptor->model_number;
    }

    return "Kiva " + SourceClassLabel(*descriptor);
}

std::string BuildReplayTrackingSystem(const std::optional<session::TrackerDescriptor>& descriptor)
{
    if (!descriptor.has_value())
    {
        return kDriverName;
    }

    if (IsSourceHmdOrController(*descriptor))
    {
        return kLighthouseTrackingSystem;
    }

    if (!descriptor->tracking_system.empty())
    {
        return descriptor->tracking_system;
    }

    return kDriverName;
}
}  // namespace

ReplayTrackerDevice::ReplayTrackerDevice(
    const std::size_t slot_index,
    const session::TrackerDescriptor& descriptor,
    std::string serial_number)
    : slot_index_(slot_index)
    , descriptor_(descriptor)
    , serial_number_(std::move(serial_number))
{
}

vr::EVRInitError ReplayTrackerDevice::Activate(const std::uint32_t object_id)
{
    device_index_ = object_id;
    property_container_ = vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id);

    {
        std::lock_guard<std::mutex> lock(pose_mutex_);
        ApplyPropertiesLocked();
    }

    DriverLog("Activated replay tracker serial=%s", serial_number_.c_str());
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
    std::lock_guard<std::mutex> lock(pose_mutex_);
    device_index_ = vr::k_unTrackedDeviceIndexInvalid;
    property_container_ = vr::k_ulInvalidPropertyContainer;
}

const std::string& ReplayTrackerDevice::serial_number() const
{
    return serial_number_;
}

std::string ReplayTrackerDevice::BuildSerialNumber(
    const session::TrackerDescriptor& descriptor,
    const std::size_t fallback_slot_index)
{
    std::string source_serial = descriptor.serial;
    if (source_serial.empty())
    {
        source_serial = "device_" + std::to_string(fallback_slot_index + 1);
    }

    std::string sanitized;
    sanitized.reserve(source_serial.size());
    for (const unsigned char ch : source_serial)
    {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_')
        {
            sanitized.push_back(static_cast<char>(ch));
        }
        else
        {
            sanitized.push_back('_');
        }
    }

    return "ktk_" + sanitized;
}

void ReplayTrackerDevice::UpdateDescriptor(const std::optional<session::TrackerDescriptor>& descriptor)
{
    std::lock_guard<std::mutex> lock(pose_mutex_);
    descriptor_ = descriptor;
    ApplyPropertiesLocked();
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

void ReplayTrackerDevice::SetDisconnected()
{
    vr::DriverPose_t pose{};
    {
        std::lock_guard<std::mutex> lock(pose_mutex_);
        current_sample_.reset();
        pose = ToDriverPose(current_sample_);
    }

    const auto index = device_index_.load();
    if (index != vr::k_unTrackedDeviceIndexInvalid)
    {
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index, pose, sizeof(vr::DriverPose_t));
    }
}

void ReplayTrackerDevice::ApplyPropertiesLocked()
{
    if (property_container_ == vr::k_ulInvalidPropertyContainer)
    {
        return;
    }

    const bool source_is_tracker = descriptor_.has_value() && IsSourceTracker(*descriptor_);
    const std::string model_number = BuildReplayModelNumber(descriptor_);

    const std::string tracking_system = BuildReplayTrackingSystem(descriptor_);
    const std::string manufacturer =
        source_is_tracker && descriptor_.has_value() && !descriptor_->manufacturer_name.empty()
        ? descriptor_->manufacturer_name
        : kManufacturerName;
    const std::string controller_type =
        descriptor_.has_value() && !descriptor_->controller_type.empty()
        ? descriptor_->controller_type
        : kControllerType;

    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_ModelNumber_String, model_number.c_str());
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_SerialNumber_String, serial_number_.c_str());
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_ManufacturerName_String, manufacturer.c_str());
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_ControllerType_String, controller_type.c_str());
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_TrackingSystemName_String, tracking_system.c_str());
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_InputProfilePath_String, kInputProfilePath);
    vr::VRProperties()->SetBoolProperty(property_container_, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);

    ApplyTrackerRoleSettingLocked();
}

void ReplayTrackerDevice::ApplyTrackerRoleSettingLocked()
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const std::string key = "/devices/" + std::string(kDriverName) + "/" + serial_number_;
    if (!descriptor_.has_value() || !IsSourceTracker(*descriptor_) || descriptor_->role.empty())
    {
        vr::VRSettings()->RemoveKeyInSection(vr::k_pch_Trackers_Section, key.c_str(), &error);
        return;
    }

    vr::VRSettings()->SetString(vr::k_pch_Trackers_Section, key.c_str(), descriptor_->role.c_str(), &error);
    if (error != vr::VRSettingsError_None)
    {
        DriverLog("Failed to write tracker role for %s", serial_number_.c_str());
    }
}

vr::DriverPose_t ReplayTrackerDevice::ToDriverPose(const std::optional<session::PoseSample>& sample)
{
    vr::DriverPose_t pose{};
    pose.poseTimeOffset = 0.0;
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation.w = 1.0;
    pose.qRotation.w = 1.0;
    pose.result = vr::TrackingResult_Uninitialized;

    if (!sample.has_value())
    {
        pose.poseIsValid = false;
        pose.deviceIsConnected = false;
        return pose;
    }

    pose.poseTimeOffset = sample->pose_time_offset_s;
    pose.qRotation.w = sample->rotation_wxyz[0];
    pose.qRotation.x = sample->rotation_wxyz[1];
    pose.qRotation.y = sample->rotation_wxyz[2];
    pose.qRotation.z = sample->rotation_wxyz[3];
    pose.vecPosition[0] = sample->position_m[0];
    pose.vecPosition[1] = sample->position_m[1];
    pose.vecPosition[2] = sample->position_m[2];
    pose.vecVelocity[0] = sample->linear_velocity_mps[0];
    pose.vecVelocity[1] = sample->linear_velocity_mps[1];
    pose.vecVelocity[2] = sample->linear_velocity_mps[2];
    pose.vecAngularVelocity[0] = sample->angular_velocity_rps[0];
    pose.vecAngularVelocity[1] = sample->angular_velocity_rps[1];
    pose.vecAngularVelocity[2] = sample->angular_velocity_rps[2];
    pose.qWorldFromDriverRotation.w = sample->world_from_driver_rotation_wxyz[0];
    pose.qWorldFromDriverRotation.x = sample->world_from_driver_rotation_wxyz[1];
    pose.qWorldFromDriverRotation.y = sample->world_from_driver_rotation_wxyz[2];
    pose.qWorldFromDriverRotation.z = sample->world_from_driver_rotation_wxyz[3];
    pose.vecWorldFromDriverTranslation[0] = sample->world_from_driver_translation_m[0];
    pose.vecWorldFromDriverTranslation[1] = sample->world_from_driver_translation_m[1];
    pose.vecWorldFromDriverTranslation[2] = sample->world_from_driver_translation_m[2];
    pose.qDriverFromHeadRotation.w = sample->driver_from_head_rotation_wxyz[0];
    pose.qDriverFromHeadRotation.x = sample->driver_from_head_rotation_wxyz[1];
    pose.qDriverFromHeadRotation.y = sample->driver_from_head_rotation_wxyz[2];
    pose.qDriverFromHeadRotation.z = sample->driver_from_head_rotation_wxyz[3];
    pose.vecDriverFromHeadTranslation[0] = sample->driver_from_head_translation_m[0];
    pose.vecDriverFromHeadTranslation[1] = sample->driver_from_head_translation_m[1];
    pose.vecDriverFromHeadTranslation[2] = sample->driver_from_head_translation_m[2];
    pose.poseIsValid = sample->pose_valid;
    pose.deviceIsConnected = sample->device_connected;
    pose.result = static_cast<vr::ETrackingResult>(sample->tracking_result);
    pose.willDriftInYaw = sample->will_drift_in_yaw;
    pose.shouldApplyHeadModel = sample->should_apply_head_model;
    return pose;
}
}  // namespace steamvr_capture::replay
