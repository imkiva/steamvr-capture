#pragma once

#include <chrono>
#include <memory>
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
    bool LoadConfiguredSession();
    std::uint64_t CurrentPlaybackTimestampNs() const;

    session::SessionData session_;
    std::vector<std::unique_ptr<ReplayTrackerDevice>> tracker_devices_;
    std::chrono::steady_clock::time_point playback_started_at_{};
    bool loop_enabled_ = true;
    double playback_speed_ = 1.0;
};
}  // namespace steamvr_capture::replay
