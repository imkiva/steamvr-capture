#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "openvr_driver.h"
#include "replay_driver/replay_tracker_device.h"
#include "session/session_format.h"

namespace steamvr_capture::replay
{
class DeviceProvider : public vr::IServerTrackedDeviceProvider
{
public:
    vr::EVRInitError Init(vr::IVRDriverContext* driver_context) override;
    const char* const* GetInterfaceVersions() override;
    void RunFrame() override;
    bool ShouldBlockStandbyMode() override;
    void EnterStandby() override;
    void LeaveStandby() override;
    void Cleanup() override;

private:
    enum class PlaybackState
    {
        Stopped,
        Playing,
        Paused,
    };

    void PollControlSettings();
    bool ApplyRequestedSession(const std::string& session_path, std::int32_t generation);
    void EnsureTrackerCapacity(std::size_t required_count);
    void SetPlaybackState(PlaybackState next_state, std::chrono::steady_clock::time_point now, bool update_settings);
    void AdvancePlayback(std::chrono::steady_clock::time_point now);
    void WriteRuntimeStatus(const std::string& status_text, const std::string& last_error);
    void WriteLoadedSessionMetadata();
    void WritePlaybackStateSetting();
    bool ShouldPublishVirtualTrackers() const;
    static PlaybackState ParsePlaybackState(const std::string& value);
    static const char* PlaybackStateToString(PlaybackState value);
    static bool IsTrackerDescriptor(const session::TrackerDescriptor& descriptor);

    session::SessionData session_;
    std::vector<std::unique_ptr<ReplayTrackerDevice>> tracker_devices_;
    std::vector<std::size_t> tracker_device_session_indices_;
    std::chrono::steady_clock::time_point playback_started_at_{};
    std::chrono::steady_clock::time_point last_control_poll_at_{};
    std::uint64_t playback_base_timestamp_ns_ = 0;
    std::uint64_t playback_timestamp_ns_ = 0;
    bool loop_enabled_ = true;
    double playback_speed_ = 1.0;
    PlaybackState playback_state_ = PlaybackState::Stopped;
    std::string live_mode_ = "suppress";
    std::string requested_session_path_;
    std::string loaded_session_path_;
    std::string requested_playback_state_;
    std::int32_t current_generation_ = -1;
};
}  // namespace steamvr_capture::replay
