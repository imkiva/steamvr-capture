#pragma once

#include <windows.h>

#include <memory>

#include "hotpatch_shared/hotpatch_protocol.h"

namespace steamvr_capture::hotpatch
{
class ServerDriverHook;

class HotpatchRuntime
{
public:
    explicit HotpatchRuntime(HMODULE module_handle);
    ~HotpatchRuntime();

    HotpatchRuntime(const HotpatchRuntime&) = delete;
    HotpatchRuntime& operator=(const HotpatchRuntime&) = delete;

    void Run();
    void RequestStop();

private:
    bool OpenSharedState();
    void CloseSharedState();
    void PublishStatus(const wchar_t* text, HookState hook_state);
    bool IsLighthouseModuleLoaded() const;
    bool IsReplayDriverLoaded() const;
    static std::uint64_t GetHeartbeatMilliseconds();

    HMODULE module_handle_ = nullptr;
    HANDLE shared_mapping_ = nullptr;
    SharedState* shared_state_ = nullptr;
    std::unique_ptr<ServerDriverHook> server_driver_hook_;
    volatile LONG stop_requested_ = 0;
};
}  // namespace steamvr_capture::hotpatch
