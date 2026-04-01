#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include "openvr_driver.h"

namespace steamvr_capture::proxy
{
class LighthouseProxyProvider;

class LighthouseTrackedDeviceProxy : public vr::ITrackedDeviceServerDriver
{
public:
    LighthouseTrackedDeviceProxy(
        LighthouseProxyProvider* owner,
        std::string serial,
        vr::ETrackedDeviceClass device_class,
        vr::ITrackedDeviceServerDriver* inner_device);

    vr::EVRInitError Activate(std::uint32_t object_id) override;
    void EnterStandby() override;
    void* GetComponent(const char* component_name_and_version) override;
    void DebugRequest(const char* request, char* response_buffer, std::uint32_t response_buffer_size) override;
    vr::DriverPose_t GetPose() override;
    void Deactivate() override;

    const std::string& serial() const { return serial_; }
    vr::ETrackedDeviceClass device_class() const { return device_class_; }

private:
    LighthouseProxyProvider* owner_ = nullptr;
    std::string serial_;
    vr::ETrackedDeviceClass device_class_ = vr::TrackedDeviceClass_Invalid;
    vr::ITrackedDeviceServerDriver* inner_device_ = nullptr;
    vr::TrackedDeviceIndex_t object_id_ = vr::k_unTrackedDeviceIndexInvalid;
};

class LighthouseProxyProvider : public vr::IServerTrackedDeviceProvider
{
public:
    LighthouseProxyProvider() = default;

    vr::EVRInitError Init(vr::IVRDriverContext* driver_context) override;
    void Cleanup() override;
    const char* const* GetInterfaceVersions() override;
    void RunFrame() override;
    bool ShouldBlockStandbyMode() override;
    void EnterStandby() override;
    void LeaveStandby() override;

    void OnTrackedDeviceActivated(
        vr::TrackedDeviceIndex_t device_index,
        const std::string& serial,
        vr::ETrackedDeviceClass device_class);
    void OnTrackedDeviceDeactivated(vr::TrackedDeviceIndex_t device_index);
    bool ShouldSuppressDevice(vr::TrackedDeviceIndex_t device_index) const;
    vr::DriverPose_t BuildDisconnectedPose() const;

private:
    struct DeviceMetadata
    {
        std::string serial;
        vr::ETrackedDeviceClass device_class = vr::TrackedDeviceClass_Invalid;
    };

    class ServerDriverHostProxy : public vr::IVRServerDriverHost
    {
    public:
        explicit ServerDriverHostProxy(LighthouseProxyProvider* owner)
            : owner_(owner)
        {
        }

        void SetInnerHost(vr::IVRServerDriverHost* inner_host) { inner_host_ = inner_host; }
        void Reset();
        void UpdateSuppression(bool enabled, std::unordered_set<std::string> suppressed_serials);

        bool TrackedDeviceAdded(
            const char* device_serial_number,
            vr::ETrackedDeviceClass device_class,
            vr::ITrackedDeviceServerDriver* driver) override;
        void TrackedDevicePoseUpdated(
            std::uint32_t which_device, const vr::DriverPose_t& new_pose, std::uint32_t pose_struct_size) override;
        void VsyncEvent(double vsync_time_offset_seconds) override;
        void VendorSpecificEvent(
            std::uint32_t which_device,
            vr::EVREventType event_type,
            const vr::VREvent_Data_t& event_data,
            double event_time_offset) override;
        bool IsExiting() override;
        bool PollNextEvent(vr::VREvent_t* event, std::uint32_t event_size) override;
        void GetRawTrackedDevicePoses(
            float predicted_seconds_from_now,
            vr::TrackedDevicePose_t* tracked_device_pose_array,
            std::uint32_t tracked_device_pose_array_count) override;
        void RequestRestart(
            const char* localized_reason,
            const char* executable_to_start,
            const char* arguments,
            const char* working_directory) override;
        std::uint32_t GetFrameTimings(vr::Compositor_FrameTiming* timing, std::uint32_t frames) override;
        void SetDisplayEyeToHead(
            std::uint32_t which_device,
            const vr::HmdMatrix34_t& eye_to_head_left,
            const vr::HmdMatrix34_t& eye_to_head_right) override;
        void SetDisplayProjectionRaw(
            std::uint32_t which_device, const vr::HmdRect2_t& eye_left, const vr::HmdRect2_t& eye_right) override;
        void SetRecommendedRenderTargetSize(std::uint32_t which_device, std::uint32_t width, std::uint32_t height) override;

    private:
        vr::DriverPose_t BuildDisconnectedPose() const;
        bool ShouldSuppress(std::uint32_t which_device) const;

        LighthouseProxyProvider* owner_ = nullptr;
        vr::IVRServerDriverHost* inner_host_ = nullptr;
        bool suppression_enabled_ = false;
        std::unordered_set<std::string> suppressed_serials_;
    };

    class DriverContextProxy : public vr::IVRDriverContext
    {
    public:
        DriverContextProxy() = default;

        void SetInnerContext(vr::IVRDriverContext* inner_context) { inner_context_ = inner_context; }
        void SetServerDriverHostProxy(ServerDriverHostProxy* server_driver_host_proxy)
        {
            server_driver_host_proxy_ = server_driver_host_proxy;
        }

        void* GetGenericInterface(const char* interface_version, vr::EVRInitError* error = nullptr) override;
        vr::DriverHandle_t GetDriverHandle() override;

    private:
        vr::IVRDriverContext* inner_context_ = nullptr;
        ServerDriverHostProxy* server_driver_host_proxy_ = nullptr;
    };

    bool LoadInnerLighthouseDriver(std::string* error);
    void UnloadInnerLighthouseDriver();
    std::string ResolveInnerDriverPath() const;
    void PollReplaySettings();
    void RefreshSuppressedSerials(const std::string& loaded_session_path, const std::string& playback_state);

    using DriverFactoryFunction = void* (__cdecl*)(const char*, int*);

    HMODULE inner_driver_library_ = nullptr;
    DriverFactoryFunction inner_driver_factory_ = nullptr;
    vr::IServerTrackedDeviceProvider* inner_provider_ = nullptr;
    vr::IVRDriverContext* driver_context_ = nullptr;
    ServerDriverHostProxy server_driver_host_proxy_{this};
    DriverContextProxy driver_context_proxy_{};
    std::vector<std::unique_ptr<LighthouseTrackedDeviceProxy>> tracked_device_wrappers_;
    std::unordered_map<vr::TrackedDeviceIndex_t, DeviceMetadata> devices_by_index_;
    std::chrono::steady_clock::time_point last_settings_poll_at_{};
    bool suppression_enabled_ = true;
    std::string target_driver_path_;
    std::string loaded_session_path_;
    std::string playback_state_ = "stopped";
    std::unordered_set<std::string> suppressed_serials_;
};
}  // namespace steamvr_capture::proxy
