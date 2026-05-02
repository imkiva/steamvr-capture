#pragma once

#include <cstddef>
#include <cstdint>

namespace steamvr_capture::hotpatch
{
constexpr std::uint32_t kProtocolMagic = 0x53564348u;
constexpr std::uint32_t kProtocolVersion = 4u;
constexpr wchar_t kSharedStateMappingName[] = L"Local\\SteamVRCaptureHotpatchStateV4";
constexpr std::size_t kMaxTrackedSerials = 32u;
constexpr std::size_t kMaxDisabledDeviceSerials = 64u;
constexpr std::size_t kMaxObservedDevices = 32u;
constexpr std::size_t kMaxWidePathCharacters = 260u;
constexpr std::size_t kMaxWideStatusCharacters = 256u;
constexpr std::size_t kMaxWideSerialCharacters = 96u;

enum class LiveMode : std::uint32_t
{
    Passthrough = 0u,
    Suppress = 1u,
    Replace = 2u,
};

enum class HookState : std::uint32_t
{
    Inactive = 0u,
    BrokerReady = 1u,
    Injected = 2u,
    LighthouseLoaded = 3u,
    HookInstalled = 4u,
    HookFailed = 5u,
};

struct TrackedSerialSlot
{
    std::uint32_t device_class = 0u;
    wchar_t serial[kMaxWideSerialCharacters]{};
};

struct LivePoseSlot
{
    std::uint32_t sample_present = 0u;
    std::uint32_t device_connected = 0u;
    std::uint32_t pose_valid = 0u;
    std::uint32_t will_drift_in_yaw = 0u;
    std::uint32_t should_apply_head_model = 0u;
    std::int32_t tracking_result = 0;
    std::uint64_t sample_timestamp_ns = 0u;
    double pose_time_offset_s = 0.0;
    double position_m[3]{};
    double rotation_wxyz[4]{1.0, 0.0, 0.0, 0.0};
    double linear_velocity_mps[3]{};
    double angular_velocity_rps[3]{};
    double world_from_driver_rotation_wxyz[4]{1.0, 0.0, 0.0, 0.0};
    double world_from_driver_translation_m[3]{};
    double driver_from_head_rotation_wxyz[4]{1.0, 0.0, 0.0, 0.0};
    double driver_from_head_translation_m[3]{};
};

struct ObservedDeviceSlot
{
    std::uint32_t present = 0u;
    std::uint32_t device_index = 0u;
    std::uint32_t device_class = 0u;
    wchar_t serial[kMaxWideSerialCharacters]{};
    LivePoseSlot pose{};
};

struct SharedState
{
    std::uint32_t magic = kProtocolMagic;
    std::uint32_t version = kProtocolVersion;
    std::uint32_t size = sizeof(SharedState);
    std::uint32_t broker_pid = 0u;
    std::uint32_t target_pid = 0u;
    std::uint32_t injected_pid = 0u;
    std::uint32_t playback_active = 0u;
    std::uint32_t suppress_real_trackers = 0u;
    std::uint32_t live_mode = static_cast<std::uint32_t>(LiveMode::Passthrough);
    std::uint32_t serial_count = 0u;
    std::uint32_t hook_state = static_cast<std::uint32_t>(HookState::Inactive);
    std::uint32_t tracked_device_add_calls = 0u;
    std::uint32_t pose_updates_seen = 0u;
    std::uint32_t pose_updates_suppressed = 0u;
    std::uint32_t pose_updates_replaced = 0u;
    std::uint32_t pose_updates_disabled = 0u;
    std::uint32_t recording_active = 0u;
    std::uint32_t recording_device_count = 0u;
    std::uint32_t observed_device_count = 0u;
    std::uint32_t disabled_serial_count = 0u;
    std::uint64_t broker_heartbeat_ms = 0u;
    std::uint64_t injected_heartbeat_ms = 0u;
    std::uint64_t playback_timestamp_ns = 0u;
    std::uint64_t recorded_sample_count = 0u;
    wchar_t session_path[kMaxWidePathCharacters]{};
    wchar_t recording_output_path[kMaxWidePathCharacters]{};
    wchar_t hotpatch_dll_path[kMaxWidePathCharacters]{};
    wchar_t broker_status_text[kMaxWideStatusCharacters]{};
    wchar_t dll_status_text[kMaxWideStatusCharacters]{};
    wchar_t recording_status_text[kMaxWideStatusCharacters]{};
    TrackedSerialSlot serials[kMaxTrackedSerials]{};
    TrackedSerialSlot disabled_serials[kMaxDisabledDeviceSerials]{};
    LivePoseSlot live_poses[kMaxTrackedSerials]{};
    ObservedDeviceSlot observed_devices[kMaxObservedDevices]{};
};

static_assert(sizeof(SharedState) < 65536u, "Shared hotpatch state must remain compact.");
}  // namespace steamvr_capture::hotpatch
