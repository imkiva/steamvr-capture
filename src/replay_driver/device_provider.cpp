#include "replay_driver/device_provider.h"

#include <cmath>
#include <string>

#include "replay_driver/driver_log.h"
#include "session/replay_settings.h"

namespace steamvr_capture::replay
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

double ReadSettingsFloat(const char* section, const char* key, const double fallback)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const float value = vr::VRSettings()->GetFloat(section, key, &error);
    return (error == vr::VRSettingsError_None && value > 0.0f) ? static_cast<double>(value) : fallback;
}

std::string NormalizeLiveMode(const std::string& live_mode)
{
    if (live_mode == "replace" || live_mode == "suppress" || live_mode == "passthrough")
    {
        return live_mode;
    }

    return "suppress";
}
}  // namespace

vr::EVRInitError DeviceProvider::Init(vr::IVRDriverContext* driver_context)
{
    VR_INIT_SERVER_DRIVER_CONTEXT(driver_context);

    vr::EVRSettingsError settings_error = vr::VRSettingsError_None;
    const bool enabled =
        vr::VRSettings()->GetBool(replay_settings::kDriverSection, replay_settings::kEnableKey, &settings_error);
    if (settings_error == vr::VRSettingsError_None && !enabled)
    {
        DriverLog("Replay driver disabled in SteamVR settings.");
        return vr::VRInitError_None;
    }

    loop_enabled_ = ReadSettingsBool(replay_settings::kDriverSection, replay_settings::kLoopKey, true);
    playback_speed_ = ReadSettingsFloat(replay_settings::kDriverSection, replay_settings::kPlaybackSpeedKey, 1.0);
    live_mode_ = NormalizeLiveMode(ReadSettingsString(replay_settings::kHotpatchSection, replay_settings::kLiveModeKey));
    requested_playback_state_ =
        ReadSettingsString(replay_settings::kDriverSection, replay_settings::kPlaybackStateKey);
    playback_state_ = ParsePlaybackState(requested_playback_state_);

    playback_started_at_ = std::chrono::steady_clock::now();
    last_control_poll_at_ = playback_started_at_ - std::chrono::seconds(1);
    WriteRuntimeStatus("Idle", "");
    WriteLoadedSessionMetadata();
    WritePlaybackStateSetting();
    PollControlSettings();
    DriverLog("Replay driver initialized");
    return vr::VRInitError_None;
}

const char* const* DeviceProvider::GetInterfaceVersions()
{
    return vr::k_InterfaceVersions;
}

void DeviceProvider::RunFrame()
{
    PollControlSettings();

    AdvancePlayback(std::chrono::steady_clock::now());

    if (!session_.trackers.empty())
    {
        for (std::size_t tracker_index = 0; tracker_index < tracker_devices_.size(); ++tracker_index)
        {
            if (tracker_index >= session_.trackers.size())
            {
                tracker_devices_[tracker_index]->SetDisconnected();
                continue;
            }

            if (!ShouldPublishVirtualTrackers())
            {
                tracker_devices_[tracker_index]->SetDisconnected();
                continue;
            }

            const auto sample = session::SampleAtOrBefore(session_, tracker_index, playback_timestamp_ns_);
            if (sample.has_value())
            {
                tracker_devices_[tracker_index]->UpdateSample(*sample);
            }
            else
            {
                tracker_devices_[tracker_index]->SetDisconnected();
            }
        }
    }

    vr::VREvent_t event{};
    while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event)))
    {
    }
}

bool DeviceProvider::ShouldBlockStandbyMode()
{
    return false;
}

void DeviceProvider::EnterStandby()
{
}

void DeviceProvider::LeaveStandby()
{
    SetPlaybackState(playback_state_, std::chrono::steady_clock::now(), false);
}

void DeviceProvider::Cleanup()
{
    session_ = {};
    tracker_devices_.clear();
    playback_base_timestamp_ns_ = 0;
    playback_timestamp_ns_ = 0;
    playback_state_ = PlaybackState::Stopped;
}

void DeviceProvider::PollControlSettings()
{
    const auto now = std::chrono::steady_clock::now();
    if ((now - last_control_poll_at_) < std::chrono::milliseconds(250))
    {
        return;
    }
    last_control_poll_at_ = now;

    const double requested_playback_speed =
        ReadSettingsFloat(replay_settings::kDriverSection, replay_settings::kPlaybackSpeedKey, 1.0);
    if (std::abs(requested_playback_speed - playback_speed_) > 0.0001)
    {
        AdvancePlayback(now);
        playback_speed_ = requested_playback_speed;
        playback_base_timestamp_ns_ = playback_timestamp_ns_;
        playback_started_at_ = now;
    }

    loop_enabled_ = ReadSettingsBool(replay_settings::kDriverSection, replay_settings::kLoopKey, true);
    live_mode_ = NormalizeLiveMode(ReadSettingsString(replay_settings::kHotpatchSection, replay_settings::kLiveModeKey));

    const std::string requested_session_path =
        ReadSettingsString(replay_settings::kDriverSection, replay_settings::kSessionPathKey);
    vr::EVRSettingsError settings_error = vr::VRSettingsError_None;
    const std::int32_t requested_generation = vr::VRSettings()->GetInt32(
        replay_settings::kDriverSection, replay_settings::kSessionGenerationKey, &settings_error);
    const std::int32_t effective_generation =
        settings_error == vr::VRSettingsError_None ? requested_generation : 0;
    const std::string requested_playback_state =
        ReadSettingsString(replay_settings::kDriverSection, replay_settings::kPlaybackStateKey);

    const bool session_changed =
        requested_session_path != requested_session_path_ || effective_generation != current_generation_;
    const bool playback_state_changed = requested_playback_state != requested_playback_state_;

    if (!session_changed && !playback_state_changed)
    {
        return;
    }

    if (session_changed)
    {
        requested_session_path_ = requested_session_path;
        ApplyRequestedSession(requested_session_path, effective_generation);
        return;
    }

    if (playback_state_changed)
    {
        requested_playback_state_ = requested_playback_state;
        SetPlaybackState(ParsePlaybackState(requested_playback_state), now, false);
    }
}

bool DeviceProvider::ApplyRequestedSession(const std::string& session_path, const std::int32_t generation)
{
    current_generation_ = generation;
    playback_base_timestamp_ns_ = 0;
    playback_timestamp_ns_ = 0;
    playback_started_at_ = std::chrono::steady_clock::now();

    if (session_path.empty())
    {
        session_ = {};
        loaded_session_path_.clear();
        playback_state_ = PlaybackState::Stopped;
        requested_playback_state_ = PlaybackStateToString(playback_state_);
        for (auto& tracker_device : tracker_devices_)
        {
            tracker_device->UpdateDescriptor(std::nullopt);
            tracker_device->SetDisconnected();
        }
        WriteRuntimeStatus("Idle", "");
        WriteLoadedSessionMetadata();
        WritePlaybackStateSetting();
        return true;
    }

    session::SessionData loaded_session;
    std::string parse_error;
    if (!session::LoadSessionFile(session_path, &loaded_session, &parse_error))
    {
        WriteRuntimeStatus("Failed to load session", parse_error);
        playback_state_ = PlaybackState::Stopped;
        WritePlaybackStateSetting();
        DriverLog("Failed to load session file %s: %s", session_path.c_str(), parse_error.c_str());
        return false;
    }

    if (loaded_session.trackers.empty())
    {
        const std::string error = "Selected session contains no trackers.";
        WriteRuntimeStatus("Failed to load session", error);
        playback_state_ = PlaybackState::Stopped;
        WritePlaybackStateSetting();
        DriverLog("%s", error.c_str());
        return false;
    }

    EnsureTrackerCapacity(loaded_session.trackers.size());

    session_ = std::move(loaded_session);
    loaded_session_path_ = session_path;
    playback_state_ = PlaybackState::Stopped;
    requested_playback_state_ = PlaybackStateToString(playback_state_);

    for (std::size_t index = 0; index < tracker_devices_.size(); ++index)
    {
        if (index < session_.trackers.size())
        {
            tracker_devices_[index]->UpdateDescriptor(session_.trackers[index]);
        }
        else
        {
            tracker_devices_[index]->UpdateDescriptor(std::nullopt);
        }
        tracker_devices_[index]->SetDisconnected();
    }

    WriteRuntimeStatus("Loaded session", "");
    WriteLoadedSessionMetadata();
    WritePlaybackStateSetting();
    DriverLog("Loaded session %s with %zu tracker(s)", session_path.c_str(), session_.trackers.size());
    return true;
}

void DeviceProvider::EnsureTrackerCapacity(const std::size_t required_count)
{
    while (tracker_devices_.size() < required_count)
    {
        auto tracker_device = std::make_unique<ReplayTrackerDevice>(tracker_devices_.size());
        if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
                tracker_device->serial_number().c_str(),
                vr::TrackedDeviceClass_GenericTracker,
                tracker_device.get()))
        {
            DriverLog("Failed to register replay tracker %s", tracker_device->serial_number().c_str());
            return;
        }
        tracker_devices_.push_back(std::move(tracker_device));
    }
}

void DeviceProvider::SetPlaybackState(
    const PlaybackState next_state,
    const std::chrono::steady_clock::time_point now,
    const bool update_settings)
{
    if (session_.trackers.empty() && next_state != PlaybackState::Stopped)
    {
        playback_state_ = PlaybackState::Stopped;
        requested_playback_state_ = PlaybackStateToString(playback_state_);
        WriteRuntimeStatus("Idle", "No session is loaded.");
        WritePlaybackStateSetting();
        return;
    }

    AdvancePlayback(now);
    playback_state_ = next_state;

    switch (playback_state_)
    {
    case PlaybackState::Playing:
        playback_base_timestamp_ns_ = playback_timestamp_ns_;
        playback_started_at_ = now;
        WriteRuntimeStatus("Playing", "");
        break;

    case PlaybackState::Paused:
        WriteRuntimeStatus("Paused", "");
        break;

    case PlaybackState::Stopped:
    default:
        playback_base_timestamp_ns_ = 0;
        playback_timestamp_ns_ = 0;
        playback_started_at_ = now;
        WriteRuntimeStatus(session_.trackers.empty() ? "Idle" : "Stopped", "");
        break;
    }

    if (update_settings)
    {
        WritePlaybackStateSetting();
    }
}

void DeviceProvider::AdvancePlayback(const std::chrono::steady_clock::time_point now)
{
    if (session_.duration_ns == 0)
    {
        playback_timestamp_ns_ = 0;
        return;
    }

    switch (playback_state_)
    {
    case PlaybackState::Stopped:
        playback_timestamp_ns_ = 0;
        return;

    case PlaybackState::Paused:
        return;

    case PlaybackState::Playing:
    default:
        break;
    }

    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - playback_started_at_).count();
    const auto advanced_ns =
        playback_base_timestamp_ns_ +
        static_cast<std::uint64_t>(std::llround(static_cast<long double>(elapsed_ns) * playback_speed_));

    if (loop_enabled_)
    {
        playback_timestamp_ns_ = advanced_ns % session_.duration_ns;
        return;
    }

    if (advanced_ns >= session_.duration_ns)
    {
        playback_state_ = PlaybackState::Stopped;
        requested_playback_state_ = PlaybackStateToString(playback_state_);
        playback_base_timestamp_ns_ = 0;
        playback_timestamp_ns_ = 0;
        playback_started_at_ = now;
        WriteRuntimeStatus("Stopped", "");
        WritePlaybackStateSetting();
        return;
    }

    playback_timestamp_ns_ = advanced_ns;
}

void DeviceProvider::WriteRuntimeStatus(const std::string& status_text, const std::string& last_error)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->SetString(
        replay_settings::kDriverSection, replay_settings::kStatusTextKey, status_text.c_str(), &error);
    vr::VRSettings()->SetString(
        replay_settings::kDriverSection, replay_settings::kLastErrorKey, last_error.c_str(), &error);
}

void DeviceProvider::WriteLoadedSessionMetadata()
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->SetString(
        replay_settings::kDriverSection,
        replay_settings::kLoadedSessionPathKey,
        loaded_session_path_.c_str(),
        &error);
    vr::VRSettings()->SetInt32(
        replay_settings::kDriverSection,
        replay_settings::kLoadedTrackerCountKey,
        static_cast<std::int32_t>(session_.trackers.size()),
        &error);
}

void DeviceProvider::WritePlaybackStateSetting()
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->SetString(
        replay_settings::kDriverSection,
        replay_settings::kPlaybackStateKey,
        PlaybackStateToString(playback_state_),
        &error);
}

bool DeviceProvider::ShouldPublishVirtualTrackers() const
{
    return live_mode_ != "replace";
}

DeviceProvider::PlaybackState DeviceProvider::ParsePlaybackState(const std::string& value)
{
    if (value == "playing")
    {
        return PlaybackState::Playing;
    }

    if (value == "paused")
    {
        return PlaybackState::Paused;
    }

    return PlaybackState::Stopped;
}

const char* DeviceProvider::PlaybackStateToString(const PlaybackState value)
{
    switch (value)
    {
    case PlaybackState::Playing:
        return "playing";

    case PlaybackState::Paused:
        return "paused";

    case PlaybackState::Stopped:
    default:
        return "stopped";
    }
}
}  // namespace steamvr_capture::replay
