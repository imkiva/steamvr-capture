#include "hotpatch_shared/replay_bridge_exports.h"

#include <cstring>

#include "openvr_driver.h"

extern "C" __declspec(dllexport) void* SteamVRCapture_GetServerDriverHost()
{
    return vr::VRServerDriverHost();
}

extern "C" __declspec(dllexport) std::uint32_t SteamVRCapture_GetMaxTrackedDeviceCount()
{
    return vr::k_unMaxTrackedDeviceCount;
}

extern "C" __declspec(dllexport) bool SteamVRCapture_GetTrackedDeviceInfo(
    const std::uint32_t device_index, steamvr_capture::hotpatch::ReplayBridgeTrackedDeviceInfo* info)
{
    if (info == nullptr || vr::VRProperties() == nullptr || device_index >= vr::k_unMaxTrackedDeviceCount)
    {
        return false;
    }

    const vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(device_index);
    if (container == vr::k_ulInvalidPropertyContainer)
    {
        return false;
    }

    vr::ETrackedPropertyError error = vr::TrackedProp_Success;
    const std::string serial = vr::VRProperties()->GetStringProperty(container, vr::Prop_SerialNumber_String, &error);
    if (error != vr::TrackedProp_Success || serial.empty())
    {
        return false;
    }

    const std::int32_t device_class = vr::VRProperties()->GetInt32Property(container, vr::Prop_DeviceClass_Int32, &error);
    if (error != vr::TrackedProp_Success)
    {
        return false;
    }

    *info = steamvr_capture::hotpatch::ReplayBridgeTrackedDeviceInfo{};
    info->device_index = device_index;
    info->device_class = device_class;
    info->is_present = 1u;

    const std::size_t copy_length = std::min<std::size_t>(serial.size(), steamvr_capture::hotpatch::kReplayBridgeMaxSerialBytes - 1u);
    std::memcpy(info->serial, serial.data(), copy_length);
    info->serial[copy_length] = '\0';
    return true;
}
