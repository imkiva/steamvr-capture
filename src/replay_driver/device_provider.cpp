#include "replay_driver/device_provider.h"

#include <cmath>

#include "replay_driver/driver_log.h"

namespace steamvr_capture::replay
{
namespace
{
constexpr const char* kSettingsSection = "driver_steamvr_capture_replay";
constexpr const char* kEnableKey = "enable";
constexpr const char* kSessionPathKey = "session_path";
constexpr const char* kLoopKey = "loop";
constexpr const char* kPlaybackSpeedKey = "playback_speed";
}  // namespace

vr::EVRInitError DeviceProvider::Init(vr::IVRDriverContext* driver_context)
{
    VR_INIT_SERVER_DRIVER_CONTEXT(driver_context);

    vr::EVRSettingsError settings_error = vr::VRSettingsError_None;
    const bool enabled = vr::VRSettings()->GetBool(kSettingsSection, kEnableKey, &settings_error);
    if (settings_error == vr::VRSettingsError_None && !enabled)
    {
        DriverLog("Replay driver disabled in SteamVR settings.");
        return vr::VRInitError_None;
    }

    loop_enabled_ = vr::VRSettings()->GetBool(kSettingsSection, kLoopKey, &settings_error);
    if (settings_error != vr::VRSettingsError_None)
    {
        loop_enabled_ = true;
    }

    playback_speed_ = vr::VRSettings()->GetFloat(kSettingsSection, kPlaybackSpeedKey, &settings_error);
    if (settings_error != vr::VRSettingsError_None || playback_speed_ <= 0.0)
    {
        playback_speed_ = 1.0;
    }

    if (!LoadConfiguredSession())
    {
        DriverLog("Replay driver started without a valid session file.");
        return vr::VRInitError_None;
    }

    for (const auto& tracker : session_.trackers)
    {
        auto device = std::make_unique<ReplayTrackerDevice>(tracker);
        if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
                device->serial_number().c_str(),
                vr::TrackedDeviceClass_GenericTracker,
                device.get()))
        {
            DriverLog("Failed to register replay tracker %s", device->serial_number().c_str());
            return vr::VRInitError_Driver_Unknown;
        }

        tracker_devices_.push_back(std::move(device));
    }

    playback_started_at_ = std::chrono::steady_clock::now();
    DriverLog("Replay driver initialized with %zu tracker(s)", tracker_devices_.size());
    return vr::VRInitError_None;
}

const char* const* DeviceProvider::GetInterfaceVersions()
{
    return vr::k_InterfaceVersions;
}

void DeviceProvider::RunFrame()
{
    if (tracker_devices_.empty())
    {
        return;
    }

    const std::uint64_t playback_timestamp_ns = CurrentPlaybackTimestampNs();
    for (std::size_t tracker_index = 0; tracker_index < tracker_devices_.size(); ++tracker_index)
    {
        const auto sample = session::SampleAtOrBefore(session_, tracker_index, playback_timestamp_ns);
        if (sample.has_value())
        {
            tracker_devices_[tracker_index]->UpdateSample(*sample);
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
    playback_started_at_ = std::chrono::steady_clock::now();
}

void DeviceProvider::Cleanup()
{
    tracker_devices_.clear();
}

bool DeviceProvider::LoadConfiguredSession()
{
    char session_path[4096] = {};
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->GetString(kSettingsSection, kSessionPathKey, session_path, sizeof(session_path), &error);
    if (error != vr::VRSettingsError_None || session_path[0] == '\0')
    {
        DriverLog("No session_path configured under %s", kSettingsSection);
        return false;
    }

    std::string parse_error;
    if (!session::LoadSessionFile(session_path, &session_, &parse_error))
    {
        DriverLog("Failed to load session file %s: %s", session_path, parse_error.c_str());
        return false;
    }

    if (session_.trackers.empty())
    {
        DriverLog("Configured session file contains no trackers.");
        return false;
    }

    return true;
}

std::uint64_t DeviceProvider::CurrentPlaybackTimestampNs() const
{
    if (session_.duration_ns == 0)
    {
        return 0;
    }

    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - playback_started_at_)
                                .count();
    const auto scaled =
        static_cast<std::uint64_t>(std::llround(static_cast<long double>(elapsed_ns) * playback_speed_));

    if (loop_enabled_)
    {
        return scaled % session_.duration_ns;
    }

    return scaled > session_.duration_ns ? session_.duration_ns : scaled;
}
}  // namespace steamvr_capture::replay
