#include "capture/openvr_app_helpers.h"

#include <cmath>

namespace steamvr_capture::capture
{
namespace
{
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
}  // namespace

std::vector<TrackerSnapshot> EnumerateTrackers(vr::IVRSystem& vr_system, vr::IVRSettings& settings)
{
    std::vector<TrackerSnapshot> trackers;
    for (std::uint32_t device_index = 0; device_index < vr::k_unMaxTrackedDeviceCount; ++device_index)
    {
        if (vr_system.GetTrackedDeviceClass(device_index) != vr::TrackedDeviceClass_GenericTracker)
        {
            continue;
        }

        TrackerSnapshot tracker;
        tracker.device_index = device_index;
        tracker.descriptor.serial =
            GetTrackedDeviceString(vr_system, device_index, vr::Prop_SerialNumber_String);
        tracker.descriptor.tracking_system =
            GetTrackedDeviceString(vr_system, device_index, vr::Prop_TrackingSystemName_String);
        tracker.descriptor.model_number =
            GetTrackedDeviceString(vr_system, device_index, vr::Prop_ModelNumber_String);
        tracker.descriptor.role =
            GetTrackerRole(settings, tracker.descriptor.tracking_system, tracker.descriptor.serial);
        trackers.push_back(std::move(tracker));
    }
    return trackers;
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
    sample->pose_valid = pose.bPoseIsValid;
    sample->device_connected = pose.bDeviceIsConnected;
    sample->tracking_result = static_cast<std::int32_t>(pose.eTrackingResult);
    return true;
}
}  // namespace steamvr_capture::capture
