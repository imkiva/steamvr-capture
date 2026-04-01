#pragma once

#include <windows.h>

#include "hotpatch_shared/hotpatch_protocol.h"

namespace steamvr_capture::hotpatch
{
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
    static std::uint64_t GetHeartbeatMilliseconds();

    HMODULE module_handle_ = nullptr;
    HANDLE shared_mapping_ = nullptr;
    SharedState* shared_state_ = nullptr;
    volatile LONG stop_requested_ = 0;
};
}  // namespace steamvr_capture::hotpatch
