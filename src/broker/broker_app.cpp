#include "broker/broker_app.h"

#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

#include "capture/openvr_app_helpers.h"
#include "openvr.h"
#include "session/replay_settings.h"
#include "session/session_format.h"

namespace steamvr_capture::broker
{
namespace
{
std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (required <= 1)
    {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), required);
    return result;
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
    {
        return {};
    }

    std::string result(static_cast<std::size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), required, nullptr, nullptr);
    return result;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    return WideToUtf8(path.wstring());
}

std::string ReadSettingsString(const char* section, const char* key)
{
    char buffer[4096] = {};
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->GetString(section, key, buffer, sizeof(buffer), &error);
    return error == vr::VRSettingsError_None ? std::string(buffer) : std::string();
}

void WriteSettingsString(const char* section, const char* key, const std::string& value)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->SetString(section, key, value.c_str(), &error);
}

bool ReadSettingsBool(const char* section, const char* key, const bool fallback)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const bool value = vr::VRSettings()->GetBool(section, key, &error);
    return error == vr::VRSettingsError_None ? value : fallback;
}

double ReadSettingsFloat(const char* section, const char* key, const double fallback)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const float value = vr::VRSettings()->GetFloat(section, key, &error);
    return (error == vr::VRSettingsError_None && value > 0.0f) ? static_cast<double>(value) : fallback;
}

std::string NormalizePlaybackState(const std::string& playback_state)
{
    if (playback_state == "playing" || playback_state == "paused")
    {
        return playback_state;
    }

    return "stopped";
}

std::string NormalizeRecordState(const std::string& record_state)
{
    return record_state == "recording" ? "recording" : "stopped";
}

hotpatch::LiveMode ParseLiveModeSetting(const std::string& live_mode, const bool suppress_real_trackers)
{
    if (live_mode == "replace")
    {
        return hotpatch::LiveMode::Replace;
    }

    if (live_mode == "suppress")
    {
        return hotpatch::LiveMode::Suppress;
    }

    if (live_mode == "passthrough")
    {
        return hotpatch::LiveMode::Passthrough;
    }

    return suppress_real_trackers ? hotpatch::LiveMode::Suppress : hotpatch::LiveMode::Passthrough;
}

const wchar_t* LiveModeStatusText(const hotpatch::LiveMode live_mode)
{
    switch (live_mode)
    {
    case hotpatch::LiveMode::Replace:
        return L"replace";

    case hotpatch::LiveMode::Suppress:
        return L"suppress";

    case hotpatch::LiveMode::Passthrough:
    default:
        return L"passthrough";
    }
}

std::string DescribeWindowsError(const DWORD error_code)
{
    LPSTR message_buffer = nullptr;
    const DWORD message_size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        0,
        reinterpret_cast<LPSTR>(&message_buffer),
        0,
        nullptr);

    if (message_size == 0 || message_buffer == nullptr)
    {
        return "Windows error " + std::to_string(error_code);
    }

    std::string message(message_buffer, message_size);
    LocalFree(message_buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n'))
    {
        message.pop_back();
    }
    return message;
}

template <std::size_t N>
void CopyWideText(wchar_t (&destination)[N], const std::wstring& value)
{
    std::wmemset(destination, 0, N);
    if (value.empty())
    {
        return;
    }

    const std::size_t copy_length = std::min<std::size_t>(N - 1u, value.size());
    std::wmemcpy(destination, value.c_str(), copy_length);
    destination[copy_length] = L'\0';
}
}  // namespace

BrokerApp::~BrokerApp()
{
    Shutdown();
}

bool BrokerApp::Init(std::string* error)
{
    if (!OpenSharedState(error))
    {
        return false;
    }

    hotpatch_dll_path_ = ResolveHotpatchDllPath();
    if (hotpatch_dll_path_.empty() || !std::filesystem::is_regular_file(hotpatch_dll_path_))
    {
        if (error != nullptr)
        {
            *error = "Hotpatch DLL was not found next to the broker: " + PathToUtf8(hotpatch_dll_path_);
        }
        return false;
    }

    playback_started_at_ = std::chrono::steady_clock::now();

    if (shared_state_ != nullptr)
    {
        shared_state_->broker_pid = GetCurrentProcessId();
        shared_state_->hook_state = static_cast<std::uint32_t>(hotpatch::HookState::BrokerReady);
        CopyWideText(shared_state_->hotpatch_dll_path, hotpatch_dll_path_.wstring());
        CopyWideText(shared_state_->broker_status_text, L"Broker initialized.");
        CopyWideText(shared_state_->recording_status_text, L"Recorder idle.");
    }

    return true;
}

bool BrokerApp::AttachForStatus(std::string* error)
{
    return OpenSharedStateReadOnly(error);
}

int BrokerApp::Run(const bool once)
{
    do
    {
        const auto now = std::chrono::steady_clock::now();
        RefreshVrServerTarget();
        if (openvr_initialized_ && target_pid_ != openvr_target_pid_)
        {
            ShutdownOpenVr();
        }

        std::string error;
        if (!EnsureOpenVr(&error))
        {
            if (shared_state_ != nullptr)
            {
                CopyWideText(shared_state_->broker_status_text, Utf8ToWide("Waiting for SteamVR: " + error));
                shared_state_->broker_heartbeat_ms = GetHeartbeatMilliseconds();
            }
        }
        else
        {
            PollReplayState();
            PollRecordingState(now);
            AdvancePlayback(now);
            CaptureRecordingSamples(now);
            UpdateSharedState();
            const bool playback_requires_injection =
                playback_active_ &&
                live_mode_ != hotpatch::LiveMode::Passthrough &&
                !(live_mode_ == hotpatch::LiveMode::Replace && !loaded_session_.poses_are_driver_space);
            const bool should_inject = recording_active_ || playback_requires_injection;
            if (should_inject)
            {
                std::string inject_error;
                if (!EnsureInjected(&inject_error) && shared_state_ != nullptr)
                {
                    CopyWideText(shared_state_->broker_status_text, Utf8ToWide("Inject failed: " + inject_error));
                }
            }
        }

        if (shared_state_ != nullptr)
        {
            if (!openvr_initialized_)
            {
                UpdateSharedState();
            }
            shared_state_->broker_heartbeat_ms = GetHeartbeatMilliseconds();
        }

        if (!once)
        {
            const auto sleep_interval =
                (recording_active_ || (playback_active_ && live_mode_ == hotpatch::LiveMode::Replace))
                ? std::chrono::milliseconds(8)
                : std::chrono::milliseconds(250);
            std::this_thread::sleep_for(sleep_interval);
        }
    } while (!once);

    return 0;
}

int BrokerApp::PrintStatus()
{
    if (shared_state_ == nullptr)
    {
        return 1;
    }

    const auto hook_state = static_cast<hotpatch::HookState>(shared_state_->hook_state);
    std::printf("broker_pid=%u\n", shared_state_->broker_pid);
    std::printf("target_pid=%u\n", shared_state_->target_pid);
    std::printf("injected_pid=%u\n", shared_state_->injected_pid);
    std::printf("playback_active=%u\n", shared_state_->playback_active);
    std::printf("recording_active=%u\n", shared_state_->recording_active);
    std::printf("suppress_real_trackers=%u\n", shared_state_->suppress_real_trackers);
    std::printf("mode=%u\n", shared_state_->live_mode);
    std::printf("hook_state=%u\n", static_cast<unsigned>(hook_state));
    std::printf("tracked_device_add_calls=%u\n", shared_state_->tracked_device_add_calls);
    std::printf("pose_updates_seen=%u\n", shared_state_->pose_updates_seen);
    std::printf("pose_updates_suppressed=%u\n", shared_state_->pose_updates_suppressed);
    std::printf("pose_updates_replaced=%u\n", shared_state_->pose_updates_replaced);
    std::printf("recording_devices=%u\n", shared_state_->recording_device_count);
    std::printf("recorded_sample_count=%llu\n", static_cast<unsigned long long>(shared_state_->recorded_sample_count));
    std::printf("playback_timestamp_ns=%llu\n", static_cast<unsigned long long>(shared_state_->playback_timestamp_ns));
    std::printf("session=%s\n", WideToUtf8(shared_state_->session_path).c_str());
    std::printf("recording_output=%s\n", WideToUtf8(shared_state_->recording_output_path).c_str());
    std::printf("broker_status=%s\n", WideToUtf8(shared_state_->broker_status_text).c_str());
    std::printf("dll_status=%s\n", WideToUtf8(shared_state_->dll_status_text).c_str());
    std::printf("recording_status=%s\n", WideToUtf8(shared_state_->recording_status_text).c_str());
    std::printf("serial_count=%u\n", shared_state_->serial_count);
    for (std::uint32_t index = 0; index < shared_state_->serial_count && index < hotpatch::kMaxTrackedSerials; ++index)
    {
        std::printf("serial[%u]=%s\n", index, WideToUtf8(shared_state_->serials[index].serial).c_str());
    }
    return 0;
}

void BrokerApp::Shutdown()
{
    ShutdownOpenVr();
    CloseSharedState();
}

bool BrokerApp::OpenSharedState(std::string* error)
{
    if (shared_state_ != nullptr)
    {
        return true;
    }

    shared_mapping_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(hotpatch::SharedState)),
        hotpatch::kSharedStateMappingName);
    if (shared_mapping_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = "CreateFileMappingW failed: " + DescribeWindowsError(GetLastError());
        }
        return false;
    }

    shared_state_ = static_cast<hotpatch::SharedState*>(
        MapViewOfFile(shared_mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hotpatch::SharedState)));
    if (shared_state_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = "MapViewOfFile failed: " + DescribeWindowsError(GetLastError());
        }
        CloseHandle(shared_mapping_);
        shared_mapping_ = nullptr;
        return false;
    }

    if (shared_state_->magic != hotpatch::kProtocolMagic || shared_state_->version != hotpatch::kProtocolVersion ||
        shared_state_->size != sizeof(hotpatch::SharedState))
    {
        *shared_state_ = hotpatch::SharedState{};
    }

    shared_state_read_only_ = false;

    return true;
}

bool BrokerApp::OpenSharedStateReadOnly(std::string* error)
{
    if (shared_state_ != nullptr)
    {
        return true;
    }

    shared_mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, hotpatch::kSharedStateMappingName);
    if (shared_mapping_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = "OpenFileMappingW failed: " + DescribeWindowsError(GetLastError());
        }
        return false;
    }

    shared_state_ = static_cast<hotpatch::SharedState*>(
        MapViewOfFile(shared_mapping_, FILE_MAP_READ, 0, 0, sizeof(hotpatch::SharedState)));
    if (shared_state_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = "MapViewOfFile failed: " + DescribeWindowsError(GetLastError());
        }
        CloseHandle(shared_mapping_);
        shared_mapping_ = nullptr;
        return false;
    }

    shared_state_read_only_ = true;

    return true;
}

void BrokerApp::CloseSharedState()
{
    if (shared_state_ != nullptr)
    {
        UnmapViewOfFile(shared_state_);
        shared_state_ = nullptr;
    }

    if (shared_mapping_ != nullptr)
    {
        CloseHandle(shared_mapping_);
        shared_mapping_ = nullptr;
    }

    shared_state_read_only_ = false;
}

bool BrokerApp::EnsureOpenVr(std::string* error)
{
    if (openvr_initialized_)
    {
        return vr_system_ != nullptr && vr_settings_ != nullptr;
    }

    vr::EVRInitError init_error = vr::VRInitError_None;
    vr::IVRSystem* system = vr::VR_Init(&init_error, vr::VRApplication_Background);
    if (system == nullptr || init_error != vr::VRInitError_None)
    {
        if (error != nullptr)
        {
            *error = vr::VR_GetVRInitErrorAsEnglishDescription(init_error);
        }
        return false;
    }

    vr_system_ = system;
    vr_settings_ = vr::VRSettings();
    if (vr_settings_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = "OpenVR settings interface was unavailable after initialization.";
        }
        vr::VR_Shutdown();
        vr_system_ = nullptr;
        vr_settings_ = nullptr;
        return false;
    }

    openvr_initialized_ = true;
    openvr_target_pid_ = target_pid_;
    return true;
}

void BrokerApp::ShutdownOpenVr()
{
    if (openvr_initialized_)
    {
        vr::VR_Shutdown();
        openvr_initialized_ = false;
    }
    vr_system_ = nullptr;
    vr_settings_ = nullptr;
    openvr_target_pid_ = 0u;
}

void BrokerApp::PollReplayState()
{
    if (vr::VRSettings() == nullptr)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const std::string loaded_session_path = ReadSettingsString(
        replay_settings::kDriverSection,
        replay_settings::kLoadedSessionPathKey);
    const std::string requested_session_path = ReadSettingsString(
        replay_settings::kDriverSection,
        replay_settings::kSessionPathKey);
    const std::string next_session_path = !loaded_session_path.empty() ? loaded_session_path : requested_session_path;
    const std::string playback_state = ReadSettingsString(
        replay_settings::kDriverSection,
        replay_settings::kPlaybackStateKey);
    const std::string live_mode_setting = ReadSettingsString(
        replay_settings::kHotpatchSection,
        replay_settings::kLiveModeKey);
    const bool suppress_real_trackers = ReadSettingsBool(
        replay_settings::kHotpatchSection,
        replay_settings::kSuppressRealTrackersKey,
        true);
    const bool loop_enabled = ReadSettingsBool(
        replay_settings::kDriverSection,
        replay_settings::kLoopKey,
        true);
    const double playback_speed = ReadSettingsFloat(
        replay_settings::kDriverSection,
        replay_settings::kPlaybackSpeedKey,
        1.0);

    const hotpatch::LiveMode live_mode = ParseLiveModeSetting(live_mode_setting, suppress_real_trackers);
    const std::string normalized_playback_state = NormalizePlaybackState(playback_state);
    const bool session_changed = session_path_ != next_session_path;
    const bool playback_state_changed = playback_state_ != normalized_playback_state;
    const bool speed_changed = std::abs(playback_speed_ - playback_speed) > 0.0001;
    const bool loop_changed = loop_enabled_ != loop_enabled;
    const bool live_mode_changed = live_mode_ != live_mode || suppress_real_trackers_ != suppress_real_trackers;
    const bool state_changed = session_changed || playback_state_changed || speed_changed || loop_changed || live_mode_changed;

    session_path_ = next_session_path;
    suppress_real_trackers_ = suppress_real_trackers;
    live_mode_ = live_mode;

    if (session_changed)
    {
        LoadSessionTargets();
    }

    if (speed_changed)
    {
        AdvancePlayback(now);
        playback_speed_ = playback_speed;
        playback_base_timestamp_ns_ = playback_timestamp_ns_;
        playback_started_at_ = now;
    }
    else
    {
        playback_speed_ = playback_speed;
    }

    loop_enabled_ = loop_enabled;

    if (playback_state_changed)
    {
        SetPlaybackState(normalized_playback_state, now);
    }
    else
    {
        playback_state_ = normalized_playback_state;
    }

    suppress_real_trackers_ = suppress_real_trackers;
    playback_active_ = !session_path_.empty() && playback_state_ != "stopped";

    if (state_changed)
    {
        UpdateLivePoseSlots();
    }
}

void BrokerApp::PollRecordingState(const std::chrono::steady_clock::time_point now)
{
    if (vr::VRSettings() == nullptr)
    {
        return;
    }

    record_interval_ms_ = static_cast<float>(ReadSettingsFloat(
        replay_settings::kOverlaySection,
        replay_settings::kRecordIntervalMsKey,
        replay_settings::kDefaultRecordIntervalMs));
    if (record_interval_ms_ <= 0.0f)
    {
        record_interval_ms_ = replay_settings::kDefaultRecordIntervalMs;
    }

    const std::string record_state = NormalizeRecordState(ReadSettingsString(
        replay_settings::kOverlaySection,
        replay_settings::kRecordStateKey));
    const std::filesystem::path output_path = std::filesystem::path(Utf8ToWide(ReadSettingsString(
        replay_settings::kOverlaySection,
        replay_settings::kRecordOutputPathKey)));

    if (record_state == "recording")
    {
        if (!recording_active_)
        {
            std::string error;
            if (!BeginRecording(output_path, &error))
            {
                recording_status_text_ = error;
                if (shared_state_ != nullptr)
                {
                    CopyWideText(shared_state_->recording_status_text, Utf8ToWide(error));
                }
                WriteSettingsString(replay_settings::kOverlaySection, replay_settings::kRecordStateKey, "stopped");
            }
        }
        else
        {
            next_record_sample_at_ = std::min(next_record_sample_at_, now);
        }
        return;
    }

    if (recording_active_)
    {
        EndRecording("Saved recording to " + recording_output_path_.string());
    }
}

bool BrokerApp::BeginRecording(const std::filesystem::path& output_path, std::string* error)
{
    if (!openvr_initialized_ || vr_system_ == nullptr || vr_settings_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = "SteamVR is not ready for recording.";
        }
        return false;
    }

    if (output_path.empty())
    {
        if (error != nullptr)
        {
            *error = "Recording output path was empty.";
        }
        return false;
    }

    const auto devices = capture::EnumerateRecordableDevices(*vr_system_, *vr_settings_);
    if (devices.empty())
    {
        if (error != nullptr)
        {
            *error = "No recordable HMD/controller/tracker devices were found.";
        }
        return false;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error)
    {
        if (error != nullptr)
        {
            *error = "Failed to create recording directory: " + output_path.parent_path().string();
        }
        return false;
    }

    recording_devices_.clear();
    recording_serials_.clear();
    recording_devices_.reserve(devices.size());
    recording_serials_.reserve(devices.size());
    for (const auto& device : devices)
    {
        recording_devices_.push_back(device.descriptor);
        recording_serials_.push_back(Utf8ToWide(device.descriptor.serial));
    }

    recording_tracking_space_ = capture::CaptureTrackingSpaceSnapshot(*vr_system_);
    if (!recording_writer_.Open(output_path, session::SessionFileVersion::DriverPoseV2, error) ||
        !recording_writer_.WriteHeader(recording_devices_, recording_tracking_space_, error))
    {
        recording_writer_.Close();
        recording_devices_.clear();
        recording_serials_.clear();
        return false;
    }

    recording_active_ = true;
    recording_output_path_ = output_path;
    recording_started_at_ = std::chrono::steady_clock::now();
    next_record_sample_at_ = recording_started_at_;
    recorded_sample_count_ = 0u;
    recording_status_text_ = "Recording driver poses.";
    if (shared_state_ != nullptr)
    {
        CopyWideText(shared_state_->recording_status_text, Utf8ToWide(recording_status_text_));
    }
    return true;
}

void BrokerApp::EndRecording(const std::string& status_text)
{
    recording_writer_.Close();
    recording_active_ = false;
    recording_devices_.clear();
    recording_serials_.clear();
    recording_tracking_space_ = {};
    recorded_sample_count_ = 0u;
    recording_status_text_ = status_text.empty() ? "Recorder idle." : status_text;
    if (shared_state_ != nullptr)
    {
        CopyWideText(shared_state_->recording_status_text, Utf8ToWide(recording_status_text_));
    }
    WriteSettingsString(replay_settings::kOverlaySection, replay_settings::kRecordStateKey, "stopped");
}

void BrokerApp::CaptureRecordingSamples(const std::chrono::steady_clock::time_point now)
{
    if (!recording_active_ || recording_devices_.empty())
    {
        return;
    }

    if (shared_state_ == nullptr)
    {
        return;
    }

    const auto interval = std::chrono::duration<double, std::milli>(record_interval_ms_);
    while (now >= next_record_sample_at_)
    {
        const std::uint64_t timestamp_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(next_record_sample_at_ - recording_started_at_).count());

        for (std::size_t device_index = 0; device_index < recording_devices_.size(); ++device_index)
        {
            session::PoseSample sample;
            bool observed = false;
            sample.timestamp_ns = timestamp_ns;
            sample.world_from_driver_rotation_wxyz = {1.0, 0.0, 0.0, 0.0};
            sample.driver_from_head_rotation_wxyz = {1.0, 0.0, 0.0, 0.0};

            const std::wstring& serial = recording_serials_[device_index];
            const std::uint32_t observed_count = std::min<std::uint32_t>(
                shared_state_->observed_device_count,
                hotpatch::kMaxObservedDevices);
            for (std::uint32_t observed_index = 0; observed_index < observed_count; ++observed_index)
            {
                const hotpatch::ObservedDeviceSlot& observed_device = shared_state_->observed_devices[observed_index];
                if (observed_device.present == 0u ||
                    observed_device.device_class != static_cast<std::uint32_t>(recording_devices_[device_index].device_class) ||
                    _wcsicmp(observed_device.serial, serial.c_str()) != 0)
                {
                    continue;
                }

                const hotpatch::LivePoseSlot& live_pose = observed_device.pose;
                sample.pose_time_offset_s = live_pose.pose_time_offset_s;
                sample.position_m = {
                    live_pose.position_m[0],
                    live_pose.position_m[1],
                    live_pose.position_m[2]};
                sample.rotation_wxyz = {
                    live_pose.rotation_wxyz[0],
                    live_pose.rotation_wxyz[1],
                    live_pose.rotation_wxyz[2],
                    live_pose.rotation_wxyz[3]};
                sample.linear_velocity_mps = {
                    live_pose.linear_velocity_mps[0],
                    live_pose.linear_velocity_mps[1],
                    live_pose.linear_velocity_mps[2]};
                sample.angular_velocity_rps = {
                    live_pose.angular_velocity_rps[0],
                    live_pose.angular_velocity_rps[1],
                    live_pose.angular_velocity_rps[2]};
                sample.world_from_driver_rotation_wxyz = {
                    live_pose.world_from_driver_rotation_wxyz[0],
                    live_pose.world_from_driver_rotation_wxyz[1],
                    live_pose.world_from_driver_rotation_wxyz[2],
                    live_pose.world_from_driver_rotation_wxyz[3]};
                sample.world_from_driver_translation_m = {
                    live_pose.world_from_driver_translation_m[0],
                    live_pose.world_from_driver_translation_m[1],
                    live_pose.world_from_driver_translation_m[2]};
                sample.driver_from_head_rotation_wxyz = {
                    live_pose.driver_from_head_rotation_wxyz[0],
                    live_pose.driver_from_head_rotation_wxyz[1],
                    live_pose.driver_from_head_rotation_wxyz[2],
                    live_pose.driver_from_head_rotation_wxyz[3]};
                sample.driver_from_head_translation_m = {
                    live_pose.driver_from_head_translation_m[0],
                    live_pose.driver_from_head_translation_m[1],
                    live_pose.driver_from_head_translation_m[2]};
                sample.pose_valid = live_pose.pose_valid != 0u;
                sample.device_connected = live_pose.device_connected != 0u;
                sample.tracking_result = live_pose.tracking_result;
                sample.will_drift_in_yaw = live_pose.will_drift_in_yaw != 0u;
                sample.should_apply_head_model = live_pose.should_apply_head_model != 0u;
                observed = true;
                break;
            }

            if (!observed)
            {
                sample.timestamp_ns = timestamp_ns;
                sample.pose_valid = false;
                sample.device_connected = false;
                sample.tracking_result = static_cast<std::int32_t>(vr::TrackingResult_Uninitialized);
            }

            std::string write_error;
            if (!recording_writer_.WriteSample(device_index, sample, &write_error))
            {
                EndRecording("Recording failed: " + write_error);
                return;
            }
        }

        ++recorded_sample_count_;
        next_record_sample_at_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
    }
}

void BrokerApp::LoadSessionTargets()
{
    target_serials_.clear();
    target_device_classes_.clear();
    target_session_indices_.clear();
    loaded_session_ = {};
    playback_base_timestamp_ns_ = 0u;
    playback_timestamp_ns_ = 0u;
    playback_started_at_ = std::chrono::steady_clock::now();

    if (session_path_.empty())
    {
        return;
    }

    session::SessionData session_data;
    std::string parse_error;
    if (!session::LoadSessionFile(session_path_, &session_data, &parse_error))
    {
        if (shared_state_ != nullptr)
        {
            CopyWideText(shared_state_->broker_status_text, Utf8ToWide("Session parse failed: " + parse_error));
        }
        return;
    }

    loaded_session_ = std::move(session_data);

    for (std::size_t session_index = 0; session_index < loaded_session_.trackers.size(); ++session_index)
    {
        const session::TrackerDescriptor& tracker = loaded_session_.trackers[session_index];
        if (tracker.serial.empty())
        {
            continue;
        }

        if (target_serials_.size() >= hotpatch::kMaxTrackedSerials)
        {
            break;
        }

        target_serials_.push_back(Utf8ToWide(tracker.serial));
        target_device_classes_.push_back(static_cast<std::uint32_t>(tracker.device_class));
        target_session_indices_.push_back(session_index);
    }
}

void BrokerApp::SetPlaybackState(const std::string& next_state, const std::chrono::steady_clock::time_point now)
{
    const std::string normalized_state = NormalizePlaybackState(next_state);
    if (loaded_session_.trackers.empty() && normalized_state != "stopped")
    {
        playback_state_ = "stopped";
        playback_base_timestamp_ns_ = 0u;
        playback_timestamp_ns_ = 0u;
        playback_started_at_ = now;
        return;
    }

    AdvancePlayback(now);
    playback_state_ = normalized_state;

    if (playback_state_ == "playing")
    {
        playback_base_timestamp_ns_ = playback_timestamp_ns_;
        playback_started_at_ = now;
        return;
    }

    if (playback_state_ == "stopped")
    {
        playback_base_timestamp_ns_ = 0u;
        playback_timestamp_ns_ = 0u;
        playback_started_at_ = now;
    }
}

void BrokerApp::AdvancePlayback(const std::chrono::steady_clock::time_point now)
{
    if (loaded_session_.duration_ns == 0u)
    {
        playback_timestamp_ns_ = 0u;
        return;
    }

    if (playback_state_ == "stopped")
    {
        playback_timestamp_ns_ = 0u;
        return;
    }

    if (playback_state_ == "paused")
    {
        return;
    }

    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - playback_started_at_).count();
    const auto advanced_ns =
        playback_base_timestamp_ns_ +
        static_cast<std::uint64_t>(std::llround(static_cast<long double>(elapsed_ns) * playback_speed_));

    if (loop_enabled_)
    {
        playback_timestamp_ns_ = advanced_ns % loaded_session_.duration_ns;
        return;
    }

    if (advanced_ns >= loaded_session_.duration_ns)
    {
        playback_state_ = "stopped";
        playback_base_timestamp_ns_ = 0u;
        playback_timestamp_ns_ = 0u;
        playback_started_at_ = now;
        playback_active_ = false;
        return;
    }

    playback_timestamp_ns_ = advanced_ns;
}

void BrokerApp::ClearLivePoseSlots()
{
    if (shared_state_ == nullptr)
    {
        return;
    }

    for (std::size_t index = 0; index < hotpatch::kMaxTrackedSerials; ++index)
    {
        shared_state_->live_poses[index] = hotpatch::LivePoseSlot{};
    }
}

void BrokerApp::UpdateLivePoseSlots()
{
    if (shared_state_ == nullptr)
    {
        return;
    }

    ClearLivePoseSlots();

    if (live_mode_ != hotpatch::LiveMode::Replace || !playback_active_ || !loaded_session_.poses_are_driver_space)
    {
        return;
    }

    const std::size_t tracker_count = std::min<std::size_t>(target_session_indices_.size(), hotpatch::kMaxTrackedSerials);
    for (std::size_t tracker_index = 0; tracker_index < tracker_count; ++tracker_index)
    {
        const std::size_t session_index = target_session_indices_[tracker_index];
        const auto sample = session::SampleAtOrBefore(loaded_session_, session_index, playback_timestamp_ns_);
        if (!sample.has_value())
        {
            continue;
        }

        hotpatch::LivePoseSlot& live_pose = shared_state_->live_poses[tracker_index];
        live_pose = hotpatch::LivePoseSlot{};
        live_pose.sample_present = 1u;
        live_pose.device_connected = sample->device_connected ? 1u : 0u;
        live_pose.pose_valid = sample->pose_valid ? 1u : 0u;
        live_pose.will_drift_in_yaw = sample->will_drift_in_yaw ? 1u : 0u;
        live_pose.should_apply_head_model = sample->should_apply_head_model ? 1u : 0u;
        live_pose.tracking_result = sample->tracking_result;
        live_pose.sample_timestamp_ns = sample->timestamp_ns;
        live_pose.pose_time_offset_s = sample->pose_time_offset_s;

        for (std::size_t axis = 0; axis < 3u; ++axis)
        {
            live_pose.position_m[axis] = sample->position_m[axis];
            live_pose.linear_velocity_mps[axis] = sample->linear_velocity_mps[axis];
            live_pose.angular_velocity_rps[axis] = sample->angular_velocity_rps[axis];
            live_pose.world_from_driver_translation_m[axis] = sample->world_from_driver_translation_m[axis];
            live_pose.driver_from_head_translation_m[axis] = sample->driver_from_head_translation_m[axis];
        }

        for (std::size_t component = 0; component < 4u; ++component)
        {
            live_pose.rotation_wxyz[component] = sample->rotation_wxyz[component];
            live_pose.world_from_driver_rotation_wxyz[component] =
                sample->world_from_driver_rotation_wxyz[component];
            live_pose.driver_from_head_rotation_wxyz[component] =
                sample->driver_from_head_rotation_wxyz[component];
        }
    }
}

void BrokerApp::RefreshVrServerTarget()
{
    target_pid_ = FindProcessIdByName("vrserver.exe");
}

void BrokerApp::UpdateSharedState()
{
    if (shared_state_ == nullptr)
    {
        return;
    }

    UpdateLivePoseSlots();
    shared_state_->broker_pid = GetCurrentProcessId();
    shared_state_->target_pid = target_pid_;
    shared_state_->playback_active = playback_active_ ? 1u : 0u;
    shared_state_->suppress_real_trackers = live_mode_ == hotpatch::LiveMode::Passthrough ? 0u : 1u;
    shared_state_->live_mode = static_cast<std::uint32_t>(live_mode_);
    shared_state_->serial_count = static_cast<std::uint32_t>(target_serials_.size());
    shared_state_->playback_timestamp_ns = playback_timestamp_ns_;
    shared_state_->recording_active = recording_active_ ? 1u : 0u;
    shared_state_->recording_device_count = static_cast<std::uint32_t>(recording_devices_.size());
    shared_state_->recorded_sample_count = recorded_sample_count_;
    CopyWideText(shared_state_->session_path, Utf8ToWide(session_path_));
    CopyWideText(shared_state_->recording_output_path, recording_output_path_.wstring());

    for (std::size_t index = 0; index < hotpatch::kMaxTrackedSerials; ++index)
    {
        if (index < target_serials_.size())
        {
            shared_state_->serials[index].device_class =
                index < target_device_classes_.size() ? target_device_classes_[index] : 0u;
            CopyWideText(shared_state_->serials[index].serial, target_serials_[index]);
        }
        else
        {
            shared_state_->serials[index].device_class = 0u;
            shared_state_->serials[index].serial[0] = L'\0';
        }
    }

    if (recording_active_)
    {
        recording_status_text_ =
            "Recording " + std::to_string(recording_devices_.size()) + " device(s), samples=" +
            std::to_string(recorded_sample_count_);
        CopyWideText(
            shared_state_->recording_status_text,
            Utf8ToWide(recording_status_text_));
    }
    else
    {
        CopyWideText(shared_state_->recording_status_text, Utf8ToWide(recording_status_text_));
    }

    if (target_pid_ == 0u)
    {
        CopyWideText(shared_state_->broker_status_text, L"Waiting for vrserver.exe.");
    }
    else if (!playback_active_)
    {
        CopyWideText(
            shared_state_->broker_status_text,
            session_path_.empty()
                ? L"Broker idle. No replay session is selected."
                : std::wstring(L"Broker armed in ") + LiveModeStatusText(live_mode_) + L" mode. Session is loaded and waiting for playback.");
    }
    else
    {
        if (live_mode_ == hotpatch::LiveMode::Replace && !loaded_session_.poses_are_driver_space)
        {
            CopyWideText(
                shared_state_->broker_status_text,
                L"Broker active. Replace is disabled for calibrated standing-pose sessions.");
        }
        else if (live_mode_ == hotpatch::LiveMode::Replace)
        {
            CopyWideText(shared_state_->broker_status_text, L"Broker active. Streaming recorded poses into the hotpatch table.");
        }
        else
        {
            CopyWideText(shared_state_->broker_status_text, L"Broker active. Ensuring hotpatch injection.");
        }
    }
}

bool BrokerApp::EnsureInjected(std::string* error)
{
    if (target_pid_ == 0u)
    {
        if (error != nullptr)
        {
            *error = "vrserver.exe is not running.";
        }
        return false;
    }

    const std::wstring module_name = hotpatch_dll_path_.filename().wstring();
    if (IsDllLoadedInProcess(target_pid_, module_name))
    {
        return true;
    }

    return InjectDllIntoProcess(target_pid_, hotpatch_dll_path_, error);
}

bool BrokerApp::IsDllLoadedInProcess(const std::uint32_t process_id, const std::wstring& module_name) const
{
    if (process_id == 0u || module_name.empty())
    {
        return false;
    }

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    MODULEENTRY32W module_entry{};
    module_entry.dwSize = sizeof(module_entry);
    if (!Module32FirstW(snapshot, &module_entry))
    {
        CloseHandle(snapshot);
        return false;
    }

    bool found = false;
    do
    {
        if (_wcsicmp(module_entry.szModule, module_name.c_str()) == 0)
        {
            found = true;
            break;
        }
    } while (Module32NextW(snapshot, &module_entry));

    CloseHandle(snapshot);
    return found;
}

bool BrokerApp::InjectDllIntoProcess(
    const std::uint32_t process_id, const std::filesystem::path& dll_path, std::string* error) const
{
    const HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        process_id);
    if (process == nullptr)
    {
        if (error != nullptr)
        {
            *error = "OpenProcess failed: " + DescribeWindowsError(GetLastError());
        }
        return false;
    }

    const std::wstring dll_path_text = dll_path.wstring();
    const std::size_t buffer_bytes = (dll_path_text.size() + 1u) * sizeof(wchar_t);
    void* remote_buffer = VirtualAllocEx(process, nullptr, buffer_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_buffer == nullptr)
    {
        if (error != nullptr)
        {
            *error = "VirtualAllocEx failed: " + DescribeWindowsError(GetLastError());
        }
        CloseHandle(process);
        return false;
    }

    const BOOL wrote = WriteProcessMemory(
        process,
        remote_buffer,
        dll_path_text.c_str(),
        buffer_bytes,
        nullptr);
    if (!wrote)
    {
        if (error != nullptr)
        {
            *error = "WriteProcessMemory failed: " + DescribeWindowsError(GetLastError());
        }
        VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const FARPROC load_library = kernel32 != nullptr ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr;
    if (load_library == nullptr)
    {
        if (error != nullptr)
        {
            *error = "GetProcAddress(LoadLibraryW) failed.";
        }
        VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const HANDLE remote_thread = CreateRemoteThread(
        process,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(load_library),
        remote_buffer,
        0,
        nullptr);
    if (remote_thread == nullptr)
    {
        if (error != nullptr)
        {
            *error = "CreateRemoteThread failed: " + DescribeWindowsError(GetLastError());
        }
        VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(remote_thread, 5000);
    DWORD thread_exit_code = 0;
    GetExitCodeThread(remote_thread, &thread_exit_code);

    CloseHandle(remote_thread);
    VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
    CloseHandle(process);

    if (thread_exit_code == 0)
    {
        if (error != nullptr)
        {
            *error = "Remote LoadLibraryW returned 0.";
        }
        return false;
    }

    return true;
}

std::uint32_t BrokerApp::FindProcessIdByName(const char* process_name) const
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return 0u;
    }

    PROCESSENTRY32 process_entry{};
    process_entry.dwSize = sizeof(process_entry);
    if (!Process32First(snapshot, &process_entry))
    {
        CloseHandle(snapshot);
        return 0u;
    }

    std::uint32_t result = 0u;
    do
    {
        if (_stricmp(process_entry.szExeFile, process_name) == 0)
        {
            result = process_entry.th32ProcessID;
            break;
        }
    } while (Process32Next(snapshot, &process_entry));

    CloseHandle(snapshot);
    return result;
}

std::filesystem::path BrokerApp::ResolveHotpatchDllPath() const
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    if (buffer.empty())
    {
        return {};
    }

    const std::filesystem::path exe_path(buffer);
    return exe_path.parent_path() / "steamvr_capture_hotpatch.dll";
}

std::uint64_t BrokerApp::GetHeartbeatMilliseconds()
{
    return static_cast<std::uint64_t>(GetTickCount64());
}
}  // namespace steamvr_capture::broker
