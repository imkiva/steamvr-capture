#pragma once

#include <cstddef>
#include <cstdint>

namespace steamvr_capture::hotpatch
{
constexpr std::uint32_t kProtocolMagic = 0x53564348u;
constexpr std::uint32_t kProtocolVersion = 2u;
constexpr wchar_t kSharedStateMappingName[] = L"Local\\SteamVRCaptureHotpatchState";
constexpr std::size_t kMaxTrackedSerials = 16u;
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
    wchar_t serial[kMaxWideSerialCharacters]{};
};

struct LivePoseSlot
{
    std::uint32_t sample_present = 0u;
    std::uint32_t device_connected = 0u;
    std::uint32_t pose_valid = 0u;
    std::int32_t tracking_result = 0;
    std::uint64_t sample_timestamp_ns = 0u;
    double position_m[3]{};
    double rotation_wxyz[4]{1.0, 0.0, 0.0, 0.0};
    double linear_velocity_mps[3]{};
    double angular_velocity_rps[3]{};
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
    std::uint64_t broker_heartbeat_ms = 0u;
    std::uint64_t injected_heartbeat_ms = 0u;
    std::uint64_t playback_timestamp_ns = 0u;
    wchar_t session_path[kMaxWidePathCharacters]{};
    wchar_t hotpatch_dll_path[kMaxWidePathCharacters]{};
    wchar_t broker_status_text[kMaxWideStatusCharacters]{};
    wchar_t dll_status_text[kMaxWideStatusCharacters]{};
    TrackedSerialSlot serials[kMaxTrackedSerials]{};
    LivePoseSlot live_poses[kMaxTrackedSerials]{};
};

static_assert(sizeof(SharedState) < 16384u, "Shared hotpatch state must remain compact.");
}  // namespace steamvr_capture::hotpatch
