#include "capture/openvr_app_helpers.h"

#include <cmath>

namespace steamvr_capture::capture
{
namespace
{
constexpr const char* kReplaySerialPrefix = "svrcap_replay_slot_";
constexpr const char* kReplayManufacturerName = "steamvr-capture";
constexpr const char* kReplayControllerType = "svrcap_replay_tracker";

std::string GetTrackedDeviceString(
    vr::IVRSystem& vr_system, const std::uint32_t device_index, const vr::ETrackedDeviceProperty property)
{
    vr::ETrackedPropertyError error = vr::TrackedProp_Success;
    const std::uint32_t required =
        vr_system.GetStringTrackedDeviceProperty(device_index, property, nullptr, 0, &error);
    if (required == 0 || error == vr::TrackedProp_UnknownProperty)
    {
        return {};
    }

    std::string buffer(required, '\0');
    error = vr::TrackedProp_Success;
    vr_system.GetStringTrackedDeviceProperty(device_index, property, buffer.data(), required, &error);
    if (error != vr::TrackedProp_Success)
    {
        return {};
    }

    if (!buffer.empty() && buffer.back() == '\0')
    {
        buffer.pop_back();
    }
    return buffer;
}

std::string GetTrackerRole(
    vr::IVRSettings& settings, const std::string& tracking_system, const std::string& serial)
{
    if (tracking_system.empty() || serial.empty())
    {
        return {};
    }

    const std::string key = "/devices/" + tracking_system + "/" + serial;
    char buffer[1024] = {};
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    settings.GetString(vr::k_pch_Trackers_Section, key.c_str(), buffer, sizeof(buffer), &error);
    if (error != vr::VRSettingsError_None)
    {
        return {};
    }
    return buffer;
}

std::string GetControllerRole(vr::IVRSystem& vr_system, const std::uint32_t device_index)
{
    const vr::ETrackedControllerRole role = vr_system.GetControllerRoleForTrackedDeviceIndex(device_index);
    switch (role)
    {
    case vr::TrackedControllerRole_LeftHand:
        return "left_hand";
    case vr::TrackedControllerRole_RightHand:
        return "right_hand";
    default:
        return {};
    }
}

void NormalizeQuaternion(std::array<double, 4>* quaternion)
{
    const double length = std::sqrt(
        (*quaternion)[0] * (*quaternion)[0] +
        (*quaternion)[1] * (*quaternion)[1] +
        (*quaternion)[2] * (*quaternion)[2] +
        (*quaternion)[3] * (*quaternion)[3]);
    if (length <= 0.0)
    {
        *quaternion = {1.0, 0.0, 0.0, 0.0};
        return;
    }

    for (double& component : *quaternion)
    {
        component /= length;
    }
}

std::array<double, 4> QuaternionFromMatrix(const vr::HmdMatrix34_t& matrix)
{
    const double m00 = matrix.m[0][0];
    const double m01 = matrix.m[0][1];
    const double m02 = matrix.m[0][2];
    const double m10 = matrix.m[1][0];
    const double m11 = matrix.m[1][1];
    const double m12 = matrix.m[1][2];
    const double m20 = matrix.m[2][0];
    const double m21 = matrix.m[2][1];
    const double m22 = matrix.m[2][2];

    std::array<double, 4> quaternion{};
    const double trace = m00 + m11 + m22;
    if (trace > 0.0)
    {
        const double scale = std::sqrt(trace + 1.0) * 2.0;
        quaternion[0] = 0.25 * scale;
        quaternion[1] = (m21 - m12) / scale;
        quaternion[2] = (m02 - m20) / scale;
        quaternion[3] = (m10 - m01) / scale;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const double scale = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        quaternion[0] = (m21 - m12) / scale;
        quaternion[1] = 0.25 * scale;
        quaternion[2] = (m01 + m10) / scale;
        quaternion[3] = (m02 + m20) / scale;
    }
    else if (m11 > m22)
    {
        const double scale = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        quaternion[0] = (m02 - m20) / scale;
        quaternion[1] = (m01 + m10) / scale;
        quaternion[2] = 0.25 * scale;
        quaternion[3] = (m12 + m21) / scale;
    }
    else
    {
        const double scale = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        quaternion[0] = (m10 - m01) / scale;
        quaternion[1] = (m02 + m20) / scale;
        quaternion[2] = (m12 + m21) / scale;
        quaternion[3] = 0.25 * scale;
    }

    NormalizeQuaternion(&quaternion);
    return quaternion;
}

std::array<double, 12> Matrix34ToArray(const vr::HmdMatrix34_t& matrix)
{
    return {
        matrix.m[0][0], matrix.m[0][1], matrix.m[0][2], matrix.m[0][3],
        matrix.m[1][0], matrix.m[1][1], matrix.m[1][2], matrix.m[1][3],
        matrix.m[2][0], matrix.m[2][1], matrix.m[2][2], matrix.m[2][3]};
}

bool IsReplayVirtualTracker(
    vr::IVRSystem& vr_system,
    const std::uint32_t device_index,
    const std::string& serial)
{
    if (!serial.empty() && serial.rfind(kReplaySerialPrefix, 0) == 0)
    {
        return true;
    }

    const std::string manufacturer =
        GetTrackedDeviceString(vr_system, device_index, vr::Prop_ManufacturerName_String);
    if (manufacturer == kReplayManufacturerName)
    {
        return true;
    }

    const std::string controller_type =
        GetTrackedDeviceString(vr_system, device_index, vr::Prop_ControllerType_String);
    return controller_type == kReplayControllerType;
}

bool IsRecordableClass(const vr::ETrackedDeviceClass device_class)
{
    return device_class == vr::TrackedDeviceClass_GenericTracker ||
        device_class == vr::TrackedDeviceClass_Controller ||
        device_class == vr::TrackedDeviceClass_HMD;
}

std::vector<TrackerSnapshot> EnumerateDevices(
    vr::IVRSystem& vr_system,
    vr::IVRSettings& settings,
    const bool trackers_only)
{
    std::vector<TrackerSnapshot> trackers;
    for (std::uint32_t device_index = 0; device_index < vr::k_unMaxTrackedDeviceCount; ++device_index)
    {
        const vr::ETrackedDeviceClass device_class = vr_system.GetTrackedDeviceClass(device_index);
        if (trackers_only)
        {
            if (device_class != vr::TrackedDeviceClass_GenericTracker)
            {
                continue;
            }
        }
        else if (!IsRecordableClass(device_class))
        {
            continue;
        }

        TrackerSnapshot tracker;
        tracker.device_index = device_index;
        tracker.descriptor.device_class = static_cast<std::int32_t>(device_class);
        tracker.descriptor.serial =
            GetTrackedDeviceString(vr_system, device_index, vr::Prop_SerialNumber_String);
        if (IsReplayVirtualTracker(vr_system, device_index, tracker.descriptor.serial))
        {
            continue;
        }
        tracker.descriptor.tracking_system =
            GetTrackedDeviceString(vr_system, device_index, vr::Prop_TrackingSystemName_String);
        tracker.descriptor.model_number =
            GetTrackedDeviceString(vr_system, device_index, vr::Prop_ModelNumber_String);
        tracker.descriptor.manufacturer_name =
            GetTrackedDeviceString(vr_system, device_index, vr::Prop_ManufacturerName_String);
        tracker.descriptor.controller_type =
            GetTrackedDeviceString(vr_system, device_index, vr::Prop_ControllerType_String);
        if (device_class == vr::TrackedDeviceClass_GenericTracker)
        {
            tracker.descriptor.role =
                GetTrackerRole(settings, tracker.descriptor.tracking_system, tracker.descriptor.serial);
        }
        else if (device_class == vr::TrackedDeviceClass_Controller)
        {
            tracker.descriptor.role = GetControllerRole(vr_system, device_index);
        }
        trackers.push_back(std::move(tracker));
    }
    return trackers;
}
}  // namespace

std::vector<TrackerSnapshot> EnumerateTrackers(vr::IVRSystem& vr_system, vr::IVRSettings& settings)
{
    return EnumerateDevices(vr_system, settings, true);
}

std::vector<TrackerSnapshot> EnumerateRecordableDevices(vr::IVRSystem& vr_system, vr::IVRSettings& settings)
{
    return EnumerateDevices(vr_system, settings, false);
}

session::TrackingSpaceSnapshot CaptureTrackingSpaceSnapshot(vr::IVRSystem& vr_system)
{
    session::TrackingSpaceSnapshot snapshot;
    snapshot.has_raw_to_standing = true;
    snapshot.raw_to_standing = Matrix34ToArray(vr_system.GetRawZeroPoseToStandingAbsoluteTrackingPose());
    // Mixed-runtime setups can report a valid IVRSystem but still fault on the seated transform query.
    // Keep the field optional so recording remains usable; raw->standing is the transform replay needs today.
    snapshot.has_seated_to_standing = false;
    return snapshot;
}

bool PopulateSampleFromPose(
    const vr::TrackedDevicePose_t& pose, const std::uint64_t timestamp_ns, session::PoseSample* sample)
{
    if (sample == nullptr)
    {
        return false;
    }

    sample->timestamp_ns = timestamp_ns;
    sample->position_m = {
        pose.mDeviceToAbsoluteTracking.m[0][3],
        pose.mDeviceToAbsoluteTracking.m[1][3],
        pose.mDeviceToAbsoluteTracking.m[2][3]};
    sample->rotation_wxyz = QuaternionFromMatrix(pose.mDeviceToAbsoluteTracking);
    sample->linear_velocity_mps = {pose.vVelocity.v[0], pose.vVelocity.v[1], pose.vVelocity.v[2]};
    sample->angular_velocity_rps = {
        pose.vAngularVelocity.v[0],
        pose.vAngularVelocity.v[1],
        pose.vAngularVelocity.v[2]};
    sample->world_from_driver_rotation_wxyz = {1.0, 0.0, 0.0, 0.0};
    sample->driver_from_head_rotation_wxyz = {1.0, 0.0, 0.0, 0.0};
    sample->pose_valid = pose.bPoseIsValid;
    sample->device_connected = pose.bDeviceIsConnected;
    sample->tracking_result = static_cast<std::int32_t>(pose.eTrackingResult);
    sample->will_drift_in_yaw = false;
    sample->should_apply_head_model = false;
    return true;
}

}  // namespace steamvr_capture::capture
