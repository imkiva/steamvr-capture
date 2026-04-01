#include "proxy_driver/lighthouse_proxy_provider.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <utility>

#include "replay_driver/driver_log.h"
#include "session/replay_settings.h"
#include "session/session_format.h"

namespace steamvr_capture::proxy
{
namespace
{
std::string ReadSettingsString(const char* section, const char* key)
{
    char buffer[4096] = {};
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->GetString(section, key, buffer, sizeof(buffer), &error);
    return error == vr::VRSettingsError_None ? std::string(buffer) : std::string();
}

bool ReadSettingsBool(const char* section, const char* key, const bool fallback)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const bool value = vr::VRSettings()->GetBool(section, key, &error);
    return error == vr::VRSettingsError_None ? value : fallback;
}

std::string ToUtf8(const std::filesystem::path& path)
{
    return path.u8string();
}

std::wstring ToWide(const std::filesystem::path& path)
{
    return path.wstring();
}

std::string DescribeWindowsError(const DWORD error_code)
{
    LPSTR message_buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        0,
        reinterpret_cast<LPSTR>(&message_buffer),
        0,
        nullptr);

    if (size == 0 || message_buffer == nullptr)
    {
        return "Windows error " + std::to_string(error_code);
    }

    std::string message(message_buffer, size);
    LocalFree(message_buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n'))
    {
        message.pop_back();
    }
    return message;
}

std::filesystem::path ResolveConfiguredTargetPath(const std::filesystem::path& configured_path)
{
    if (configured_path.empty())
    {
        return {};
    }

    if (std::filesystem::is_regular_file(configured_path))
    {
        return configured_path;
    }

    if (std::filesystem::is_directory(configured_path))
    {
        const auto candidate = configured_path / "bin" / "win64" / "driver_lighthouse.dll";
        if (std::filesystem::is_regular_file(candidate))
        {
            return candidate;
        }
    }

    return configured_path;
}

std::filesystem::path ResolveDefaultTargetPath()
{
    const char* program_files_x86 = std::getenv("ProgramFiles(x86)");
    if (program_files_x86 == nullptr || std::strlen(program_files_x86) == 0)
    {
        program_files_x86 = std::getenv("PROGRAMFILES(X86)");
    }

    if (program_files_x86 == nullptr || std::strlen(program_files_x86) == 0)
    {
        return {};
    }

    return std::filesystem::path(program_files_x86) / "Steam" / "steamapps" / "common" / "SteamVR" / "drivers" /
           "lighthouse" / "bin" / "win64" / "driver_lighthouse.dll";
}
}  // namespace

LighthouseTrackedDeviceProxy::LighthouseTrackedDeviceProxy(
    LighthouseProxyProvider* owner,
    std::string serial,
    const vr::ETrackedDeviceClass device_class,
    vr::ITrackedDeviceServerDriver* inner_device)
    : owner_(owner),
      serial_(std::move(serial)),
      device_class_(device_class),
      inner_device_(inner_device)
{
}

vr::EVRInitError LighthouseTrackedDeviceProxy::Activate(const std::uint32_t object_id)
{
    object_id_ = object_id;
    if (inner_device_ == nullptr)
    {
        return vr::VRInitError_Driver_Failed;
    }

    const vr::EVRInitError error = inner_device_->Activate(object_id);
    if (error == vr::VRInitError_None && owner_ != nullptr)
    {
        owner_->OnTrackedDeviceActivated(object_id, serial_, device_class_);
    }
    return error;
}

void LighthouseTrackedDeviceProxy::EnterStandby()
{
    if (inner_device_ != nullptr)
    {
        inner_device_->EnterStandby();
    }
}

void* LighthouseTrackedDeviceProxy::GetComponent(const char* component_name_and_version)
{
    return inner_device_ != nullptr ? inner_device_->GetComponent(component_name_and_version) : nullptr;
}

void LighthouseTrackedDeviceProxy::DebugRequest(
    const char* request, char* response_buffer, const std::uint32_t response_buffer_size)
{
    if (inner_device_ != nullptr)
    {
        inner_device_->DebugRequest(request, response_buffer, response_buffer_size);
    }
}

vr::DriverPose_t LighthouseTrackedDeviceProxy::GetPose()
{
    if (inner_device_ == nullptr)
    {
        return owner_ != nullptr ? owner_->BuildDisconnectedPose() : vr::DriverPose_t{};
    }

    const vr::DriverPose_t pose = inner_device_->GetPose();
    if (owner_ != nullptr && owner_->ShouldSuppressDevice(object_id_))
    {
        return owner_->BuildDisconnectedPose();
    }
    return pose;
}

void LighthouseTrackedDeviceProxy::Deactivate()
{
    if (owner_ != nullptr)
    {
        owner_->OnTrackedDeviceDeactivated(object_id_);
    }

    if (inner_device_ != nullptr)
    {
        inner_device_->Deactivate();
    }
    object_id_ = vr::k_unTrackedDeviceIndexInvalid;
}

vr::EVRInitError LighthouseProxyProvider::Init(vr::IVRDriverContext* driver_context)
{
    VR_INIT_SERVER_DRIVER_CONTEXT(driver_context);

    driver_context_ = driver_context;
    last_settings_poll_at_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    server_driver_host_proxy_.SetInnerHost(vr::VRServerDriverHost());
    driver_context_proxy_.SetInnerContext(driver_context);
    driver_context_proxy_.SetServerDriverHostProxy(&server_driver_host_proxy_);

    const bool enabled =
        ReadSettingsBool(replay_settings::kLighthouseProxySection, replay_settings::kEnableKey, false);
    if (!enabled)
    {
        DriverLog("Lighthouse proxy driver disabled in SteamVR settings.");
        return vr::VRInitError_None;
    }

    PollReplaySettings();

    std::string load_error;
    if (!LoadInnerLighthouseDriver(&load_error))
    {
        DriverLog("Failed to initialize lighthouse proxy driver: %s", load_error.c_str());
        Cleanup();
        return vr::VRInitError_Driver_Failed;
    }

    DriverLog("Initialized lighthouse proxy driver.");
    return vr::VRInitError_None;
}

void LighthouseProxyProvider::Cleanup()
{
    UnloadInnerLighthouseDriver();
    tracked_device_wrappers_.clear();
    devices_by_index_.clear();
    suppressed_serials_.clear();
    playback_state_ = "stopped";
    loaded_session_path_.clear();
    target_driver_path_.clear();
    driver_context_ = nullptr;
    server_driver_host_proxy_.Reset();
    driver_context_proxy_.SetInnerContext(nullptr);
    driver_context_proxy_.SetServerDriverHostProxy(nullptr);
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

const char* const* LighthouseProxyProvider::GetInterfaceVersions()
{
    return vr::k_InterfaceVersions;
}

void LighthouseProxyProvider::RunFrame()
{
    PollReplaySettings();
    if (inner_provider_ != nullptr)
    {
        inner_provider_->RunFrame();
    }
}

bool LighthouseProxyProvider::ShouldBlockStandbyMode()
{
    return inner_provider_ != nullptr ? inner_provider_->ShouldBlockStandbyMode() : false;
}

void LighthouseProxyProvider::EnterStandby()
{
    if (inner_provider_ != nullptr)
    {
        inner_provider_->EnterStandby();
    }
}

void LighthouseProxyProvider::LeaveStandby()
{
    if (inner_provider_ != nullptr)
    {
        inner_provider_->LeaveStandby();
    }
}

void LighthouseProxyProvider::OnTrackedDeviceActivated(
    const vr::TrackedDeviceIndex_t device_index,
    const std::string& serial,
    const vr::ETrackedDeviceClass device_class)
{
    devices_by_index_[device_index] = DeviceMetadata{serial, device_class};
}

void LighthouseProxyProvider::OnTrackedDeviceDeactivated(const vr::TrackedDeviceIndex_t device_index)
{
    devices_by_index_.erase(device_index);
}

bool LighthouseProxyProvider::ShouldSuppressDevice(const vr::TrackedDeviceIndex_t device_index) const
{
    const auto it = devices_by_index_.find(device_index);
    if (it == devices_by_index_.end())
    {
        return false;
    }

    const DeviceMetadata& metadata = it->second;
    return suppression_enabled_ && metadata.device_class == vr::TrackedDeviceClass_GenericTracker &&
           suppressed_serials_.find(metadata.serial) != suppressed_serials_.end();
}

vr::DriverPose_t LighthouseProxyProvider::BuildDisconnectedPose() const
{
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation.w = 1.0;
    pose.qRotation.w = 1.0;
    pose.result = vr::TrackingResult_Uninitialized;
    pose.poseIsValid = false;
    pose.deviceIsConnected = false;
    pose.willDriftInYaw = false;
    pose.shouldApplyHeadModel = false;
    return pose;
}

void LighthouseProxyProvider::ServerDriverHostProxy::Reset()
{
    inner_host_ = nullptr;
    suppression_enabled_ = false;
    suppressed_serials_.clear();
}

void LighthouseProxyProvider::ServerDriverHostProxy::UpdateSuppression(
    const bool enabled, std::unordered_set<std::string> suppressed_serials)
{
    suppression_enabled_ = enabled;
    suppressed_serials_ = std::move(suppressed_serials);
}

bool LighthouseProxyProvider::ServerDriverHostProxy::TrackedDeviceAdded(
    const char* device_serial_number,
    const vr::ETrackedDeviceClass device_class,
    vr::ITrackedDeviceServerDriver* driver)
{
    if (inner_host_ == nullptr || driver == nullptr)
    {
        return false;
    }

    auto wrapper = std::make_unique<LighthouseTrackedDeviceProxy>(
        owner_,
        device_serial_number != nullptr ? std::string(device_serial_number) : std::string(),
        device_class,
        driver);

    LighthouseTrackedDeviceProxy* wrapper_raw = wrapper.get();
    const bool added = inner_host_->TrackedDeviceAdded(
        wrapper_raw->serial().c_str(), device_class, wrapper_raw);
    if (added)
    {
        owner_->tracked_device_wrappers_.push_back(std::move(wrapper));
    }
    return added;
}

void LighthouseProxyProvider::ServerDriverHostProxy::TrackedDevicePoseUpdated(
    const std::uint32_t which_device, const vr::DriverPose_t& new_pose, const std::uint32_t pose_struct_size)
{
    if (inner_host_ == nullptr)
    {
        return;
    }

    if (ShouldSuppress(which_device))
    {
        const vr::DriverPose_t suppressed_pose = owner_->BuildDisconnectedPose();
        inner_host_->TrackedDevicePoseUpdated(which_device, suppressed_pose, pose_struct_size);
        return;
    }

    inner_host_->TrackedDevicePoseUpdated(which_device, new_pose, pose_struct_size);
}

void LighthouseProxyProvider::ServerDriverHostProxy::VsyncEvent(const double vsync_time_offset_seconds)
{
    if (inner_host_ != nullptr)
    {
        inner_host_->VsyncEvent(vsync_time_offset_seconds);
    }
}

void LighthouseProxyProvider::ServerDriverHostProxy::VendorSpecificEvent(
    const std::uint32_t which_device,
    const vr::EVREventType event_type,
    const vr::VREvent_Data_t& event_data,
    const double event_time_offset)
{
    if (inner_host_ != nullptr)
    {
        inner_host_->VendorSpecificEvent(which_device, event_type, event_data, event_time_offset);
    }
}

bool LighthouseProxyProvider::ServerDriverHostProxy::IsExiting()
{
    return inner_host_ != nullptr ? inner_host_->IsExiting() : true;
}

bool LighthouseProxyProvider::ServerDriverHostProxy::PollNextEvent(vr::VREvent_t* event, const std::uint32_t event_size)
{
    return inner_host_ != nullptr && inner_host_->PollNextEvent(event, event_size);
}

void LighthouseProxyProvider::ServerDriverHostProxy::GetRawTrackedDevicePoses(
    const float predicted_seconds_from_now,
    vr::TrackedDevicePose_t* tracked_device_pose_array,
    const std::uint32_t tracked_device_pose_array_count)
{
    if (inner_host_ != nullptr)
    {
        inner_host_->GetRawTrackedDevicePoses(
            predicted_seconds_from_now, tracked_device_pose_array, tracked_device_pose_array_count);
    }
}

void LighthouseProxyProvider::ServerDriverHostProxy::RequestRestart(
    const char* localized_reason,
    const char* executable_to_start,
    const char* arguments,
    const char* working_directory)
{
    if (inner_host_ != nullptr)
    {
        inner_host_->RequestRestart(localized_reason, executable_to_start, arguments, working_directory);
    }
}

std::uint32_t LighthouseProxyProvider::ServerDriverHostProxy::GetFrameTimings(
    vr::Compositor_FrameTiming* timing, const std::uint32_t frames)
{
    return inner_host_ != nullptr ? inner_host_->GetFrameTimings(timing, frames) : 0;
}

void LighthouseProxyProvider::ServerDriverHostProxy::SetDisplayEyeToHead(
    const std::uint32_t which_device,
    const vr::HmdMatrix34_t& eye_to_head_left,
    const vr::HmdMatrix34_t& eye_to_head_right)
{
    if (inner_host_ != nullptr)
    {
        inner_host_->SetDisplayEyeToHead(which_device, eye_to_head_left, eye_to_head_right);
    }
}

void LighthouseProxyProvider::ServerDriverHostProxy::SetDisplayProjectionRaw(
    const std::uint32_t which_device, const vr::HmdRect2_t& eye_left, const vr::HmdRect2_t& eye_right)
{
    if (inner_host_ != nullptr)
    {
        inner_host_->SetDisplayProjectionRaw(which_device, eye_left, eye_right);
    }
}

void LighthouseProxyProvider::ServerDriverHostProxy::SetRecommendedRenderTargetSize(
    const std::uint32_t which_device, const std::uint32_t width, const std::uint32_t height)
{
    if (inner_host_ != nullptr)
    {
        inner_host_->SetRecommendedRenderTargetSize(which_device, width, height);
    }
}

vr::DriverPose_t LighthouseProxyProvider::ServerDriverHostProxy::BuildDisconnectedPose() const
{
    return owner_ != nullptr ? owner_->BuildDisconnectedPose() : vr::DriverPose_t{};
}

bool LighthouseProxyProvider::ServerDriverHostProxy::ShouldSuppress(const std::uint32_t which_device) const
{
    if (!suppression_enabled_ || owner_ == nullptr)
    {
        return false;
    }

    const auto it = owner_->devices_by_index_.find(which_device);
    if (it == owner_->devices_by_index_.end())
    {
        return false;
    }

    const DeviceMetadata& metadata = it->second;
    return metadata.device_class == vr::TrackedDeviceClass_GenericTracker &&
           suppressed_serials_.find(metadata.serial) != suppressed_serials_.end();
}

void* LighthouseProxyProvider::DriverContextProxy::GetGenericInterface(
    const char* interface_version, vr::EVRInitError* error)
{
    if (interface_version == nullptr)
    {
        if (error != nullptr)
        {
            *error = vr::VRInitError_Init_InterfaceNotFound;
        }
        return nullptr;
    }

    if (server_driver_host_proxy_ != nullptr &&
        std::strcmp(interface_version, vr::IVRServerDriverHost_Version) == 0)
    {
        if (error != nullptr)
        {
            *error = vr::VRInitError_None;
        }
        return server_driver_host_proxy_;
    }

    if (inner_context_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = vr::VRInitError_Init_InterfaceNotFound;
        }
        return nullptr;
    }

    return inner_context_->GetGenericInterface(interface_version, error);
}

vr::DriverHandle_t LighthouseProxyProvider::DriverContextProxy::GetDriverHandle()
{
    return inner_context_ != nullptr ? inner_context_->GetDriverHandle() : vr::k_ulInvalidDriverHandle;
}

bool LighthouseProxyProvider::LoadInnerLighthouseDriver(std::string* error)
{
    UnloadInnerLighthouseDriver();

    const std::string dll_path = ResolveInnerDriverPath();
    if (dll_path.empty())
    {
        if (error != nullptr)
        {
            *error = "No lighthouse driver path was configured and the default SteamVR path could not be resolved.";
        }
        return false;
    }

    if (!std::filesystem::is_regular_file(dll_path))
    {
        if (error != nullptr)
        {
            *error = "Inner lighthouse driver DLL was not found: " + dll_path;
        }
        return false;
    }

    inner_driver_library_ = LoadLibraryExW(ToWide(dll_path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (inner_driver_library_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = "LoadLibraryExW failed for " + dll_path + ": " + DescribeWindowsError(GetLastError());
        }
        return false;
    }

    inner_driver_factory_ =
        reinterpret_cast<DriverFactoryFunction>(GetProcAddress(inner_driver_library_, "HmdDriverFactory"));
    if (inner_driver_factory_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = "HmdDriverFactory export was not found in " + dll_path;
        }
        UnloadInnerLighthouseDriver();
        return false;
    }

    int return_code = vr::VRInitError_None;
    inner_provider_ = static_cast<vr::IServerTrackedDeviceProvider*>(
        inner_driver_factory_(vr::IServerTrackedDeviceProvider_Version, &return_code));
    if (inner_provider_ == nullptr)
    {
        if (error != nullptr)
        {
            std::ostringstream stream;
            stream << "Failed to acquire IServerTrackedDeviceProvider from inner lighthouse driver (code "
                   << return_code << ").";
            *error = stream.str();
        }
        UnloadInnerLighthouseDriver();
        return false;
    }

    const vr::EVRInitError init_error = inner_provider_->Init(&driver_context_proxy_);
    if (init_error != vr::VRInitError_None)
    {
        if (error != nullptr)
        {
            std::ostringstream stream;
            stream << "Inner lighthouse driver init failed with error " << init_error << '.';
            *error = stream.str();
        }
        UnloadInnerLighthouseDriver();
        return false;
    }

    DriverLog("Loaded inner lighthouse driver from %s", dll_path.c_str());
    return true;
}

void LighthouseProxyProvider::UnloadInnerLighthouseDriver()
{
    if (inner_provider_ != nullptr)
    {
        inner_provider_->Cleanup();
        inner_provider_ = nullptr;
    }

    tracked_device_wrappers_.clear();
    devices_by_index_.clear();
    server_driver_host_proxy_.UpdateSuppression(false, {});

    if (inner_driver_library_ != nullptr)
    {
        FreeLibrary(inner_driver_library_);
        inner_driver_library_ = nullptr;
    }

    inner_driver_factory_ = nullptr;
}

std::string LighthouseProxyProvider::ResolveInnerDriverPath() const
{
    const std::filesystem::path configured_path = ResolveConfiguredTargetPath(target_driver_path_);
    if (!configured_path.empty())
    {
        return ToUtf8(configured_path);
    }

    const std::filesystem::path default_path = ResolveDefaultTargetPath();
    return default_path.empty() ? std::string() : ToUtf8(default_path);
}

void LighthouseProxyProvider::PollReplaySettings()
{
    const auto now = std::chrono::steady_clock::now();
    if ((now - last_settings_poll_at_) < std::chrono::milliseconds(250))
    {
        return;
    }
    last_settings_poll_at_ = now;

    const std::string configured_target_driver_path =
        ReadSettingsString(replay_settings::kLighthouseProxySection, replay_settings::kTargetDriverPathKey);
    if (configured_target_driver_path != target_driver_path_)
    {
        target_driver_path_ = configured_target_driver_path;
    }

    const bool suppression_enabled =
        ReadSettingsBool(replay_settings::kLighthouseProxySection, replay_settings::kSuppressReplayTrackersKey, true);
    const std::string loaded_session_path =
        ReadSettingsString(replay_settings::kDriverSection, replay_settings::kLoadedSessionPathKey);
    const std::string playback_state =
        ReadSettingsString(replay_settings::kDriverSection, replay_settings::kPlaybackStateKey);

    if (suppression_enabled != suppression_enabled_ || loaded_session_path != loaded_session_path_ ||
        playback_state != playback_state_)
    {
        suppression_enabled_ = suppression_enabled;
        RefreshSuppressedSerials(loaded_session_path, playback_state);
    }
}

void LighthouseProxyProvider::RefreshSuppressedSerials(
    const std::string& loaded_session_path, const std::string& playback_state)
{
    loaded_session_path_ = loaded_session_path;
    playback_state_ = playback_state;
    suppressed_serials_.clear();

    if (!suppression_enabled_ || loaded_session_path_.empty() || playback_state_ == "stopped")
    {
        server_driver_host_proxy_.UpdateSuppression(false, {});
        return;
    }

    session::SessionData session_data;
    std::string parse_error;
    if (!session::LoadSessionFile(loaded_session_path_, &session_data, &parse_error))
    {
        DriverLog(
            "Lighthouse proxy could not parse session %s: %s",
            loaded_session_path_.c_str(),
            parse_error.c_str());
        server_driver_host_proxy_.UpdateSuppression(false, {});
        return;
    }

    for (const session::TrackerDescriptor& tracker : session_data.trackers)
    {
        if (!tracker.serial.empty())
        {
            suppressed_serials_.insert(tracker.serial);
        }
    }

    server_driver_host_proxy_.UpdateSuppression(true, suppressed_serials_);
    DriverLog(
        "Lighthouse proxy suppression %s for %zu tracker serial(s).",
        playback_state_.c_str(),
        suppressed_serials_.size());
}
}  // namespace steamvr_capture::proxy
