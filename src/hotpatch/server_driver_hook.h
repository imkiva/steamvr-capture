#pragma once

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "hotpatch_shared/hotpatch_protocol.h"
#include "hotpatch_shared/replay_bridge_exports.h"
#include "openvr_driver.h"

namespace steamvr_capture::hotpatch
{
class ServerDriverHook
{
public:
    ServerDriverHook() = default;
    ~ServerDriverHook();

    ServerDriverHook(const ServerDriverHook&) = delete;
    ServerDriverHook& operator=(const ServerDriverHook&) = delete;

    bool EnsureInstalled(SharedState* shared_state, std::wstring* status_message);
    void Uninstall();
    bool installed() const { return installed_; }

private:
    struct DeviceMetadata
    {
        std::string serial;
        vr::ETrackedDeviceClass device_class = vr::TrackedDeviceClass_Invalid;
    };

    class TrackedDeviceProxy : public vr::ITrackedDeviceServerDriver
    {
    public:
        TrackedDeviceProxy(
            ServerDriverHook* owner,
            std::string serial,
            vr::ETrackedDeviceClass device_class,
            vr::ITrackedDeviceServerDriver* inner_driver);

        vr::EVRInitError Activate(std::uint32_t object_id) override;
        void EnterStandby() override;
        void* GetComponent(const char* component_name_and_version) override;
        void DebugRequest(const char* request, char* response_buffer, std::uint32_t response_buffer_size) override;
        vr::DriverPose_t GetPose() override;
        void Deactivate() override;

    private:
        ServerDriverHook* owner_ = nullptr;
        std::string serial_;
        vr::ETrackedDeviceClass device_class_ = vr::TrackedDeviceClass_Invalid;
        vr::ITrackedDeviceServerDriver* inner_driver_ = nullptr;
        vr::TrackedDeviceIndex_t object_id_ = vr::k_unTrackedDeviceIndexInvalid;
    };

    using TrackedDeviceAddedFn =
        bool (*)(vr::IVRServerDriverHost* self, const char* serial, vr::ETrackedDeviceClass device_class, vr::ITrackedDeviceServerDriver* driver);
    using TrackedDevicePoseUpdatedFn =
        void (*)(vr::IVRServerDriverHost* self, std::uint32_t which_device, const vr::DriverPose_t& pose, std::uint32_t pose_struct_size);

    bool ResolveReplayBridge(std::wstring* status_message);
    bool PatchHostVtable(std::wstring* status_message);
    void PrimeExistingDevices();
    void RecordActivatedDevice(vr::TrackedDeviceIndex_t device_index, const std::string& serial, vr::ETrackedDeviceClass device_class);
    void RemoveDevice(vr::TrackedDeviceIndex_t device_index);
    bool TryGetDeviceMetadata(vr::TrackedDeviceIndex_t device_index, DeviceMetadata* metadata);
    bool ResolveTargetDevice(vr::TrackedDeviceIndex_t device_index, LiveMode* live_mode, std::uint32_t* slot_index);
    vr::DriverPose_t BuildDisconnectedPose() const;
    vr::DriverPose_t BuildReplacedPose(const LivePoseSlot& live_pose, const vr::DriverPose_t& reference_pose) const;

    bool HandleTrackedDeviceAdded(
        vr::IVRServerDriverHost* self,
        const char* serial,
        vr::ETrackedDeviceClass device_class,
        vr::ITrackedDeviceServerDriver* driver);
    void HandleTrackedDevicePoseUpdated(
        vr::IVRServerDriverHost* self,
        std::uint32_t which_device,
        const vr::DriverPose_t& pose,
        std::uint32_t pose_struct_size);

    static bool HookedTrackedDeviceAdded(
        vr::IVRServerDriverHost* self,
        const char* serial,
        vr::ETrackedDeviceClass device_class,
        vr::ITrackedDeviceServerDriver* driver);
    static void HookedTrackedDevicePoseUpdated(
        vr::IVRServerDriverHost* self,
        std::uint32_t which_device,
        const vr::DriverPose_t& pose,
        std::uint32_t pose_struct_size);

    SharedState* shared_state_ = nullptr;
    HMODULE replay_driver_module_ = nullptr;
    void* host_object_ = nullptr;
    void** host_vtable_ = nullptr;
    void* original_add_entry_ = nullptr;
    void* original_pose_entry_ = nullptr;
    TrackedDeviceAddedFn original_tracked_device_added_ = nullptr;
    TrackedDevicePoseUpdatedFn original_tracked_device_pose_updated_ = nullptr;
    GetServerDriverHostFn get_server_driver_host_ = nullptr;
    GetMaxTrackedDeviceCountFn get_max_tracked_device_count_ = nullptr;
    GetTrackedDeviceInfoFn get_tracked_device_info_ = nullptr;
    bool installed_ = false;
    mutable std::mutex mutex_;
    std::unordered_map<vr::TrackedDeviceIndex_t, DeviceMetadata> devices_by_index_;
    std::vector<std::unique_ptr<TrackedDeviceProxy>> tracked_device_wrappers_;
};
}  // namespace steamvr_capture::hotpatch
