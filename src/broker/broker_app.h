#pragma once

#include <windows.h>

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "hotpatch_shared/hotpatch_protocol.h"
#include "session/session_format.h"

namespace steamvr_capture::broker
{
class BrokerApp
{
public:
    BrokerApp() = default;
    ~BrokerApp();

    BrokerApp(const BrokerApp&) = delete;
    BrokerApp& operator=(const BrokerApp&) = delete;

    bool Init(std::string* error);
    bool AttachForStatus(std::string* error);
    int Run(bool once);
    int PrintStatus();
    void Shutdown();

private:
    bool OpenSharedState(std::string* error);
    bool OpenSharedStateReadOnly(std::string* error);
    void CloseSharedState();
    bool EnsureOpenVr(std::string* error);
    void ShutdownOpenVr();
    void PollReplayState();
    void LoadSessionTargets();
    void SetPlaybackState(const std::string& next_state, std::chrono::steady_clock::time_point now);
    void AdvancePlayback(std::chrono::steady_clock::time_point now);
    void ClearLivePoseSlots();
    void UpdateLivePoseSlots();
    void RefreshVrServerTarget();
    void UpdateSharedState();
    bool EnsureInjected(std::string* error);
    bool IsDllLoadedInProcess(std::uint32_t process_id, const std::wstring& module_name) const;
    bool InjectDllIntoProcess(std::uint32_t process_id, const std::filesystem::path& dll_path, std::string* error) const;
    std::uint32_t FindProcessIdByName(const char* process_name) const;
    std::filesystem::path ResolveHotpatchDllPath() const;
    static std::uint64_t GetHeartbeatMilliseconds();

    HANDLE shared_mapping_ = nullptr;
    hotpatch::SharedState* shared_state_ = nullptr;
    bool shared_state_read_only_ = false;
    bool openvr_initialized_ = false;
    std::uint32_t openvr_target_pid_ = 0u;
    std::filesystem::path hotpatch_dll_path_;
    std::uint32_t target_pid_ = 0u;
    bool suppress_real_trackers_ = true;
    bool playback_active_ = false;
    bool loop_enabled_ = true;
    double playback_speed_ = 1.0;
    hotpatch::LiveMode live_mode_ = hotpatch::LiveMode::Suppress;
    std::string playback_state_ = "stopped";
    std::string session_path_;
    session::SessionData loaded_session_;
    std::vector<std::wstring> target_serials_;
    std::uint64_t playback_base_timestamp_ns_ = 0u;
    std::uint64_t playback_timestamp_ns_ = 0u;
    std::chrono::steady_clock::time_point playback_started_at_{};
};
}  // namespace steamvr_capture::broker
