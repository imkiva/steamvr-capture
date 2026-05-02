#include "hotpatch/hotpatch_runtime.h"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <thread>

#include "hotpatch/server_driver_hook.h"

namespace steamvr_capture::hotpatch
{
namespace
{
template <std::size_t N>
void CopyWideText(wchar_t (&destination)[N], const wchar_t* value)
{
    std::wmemset(destination, 0, N);
    if (value == nullptr)
    {
        return;
    }

    const std::size_t copy_length = std::min<std::size_t>(N - 1u, std::wcslen(value));
    std::wmemcpy(destination, value, copy_length);
    destination[copy_length] = L'\0';
}
}  // namespace

HotpatchRuntime::HotpatchRuntime(HMODULE module_handle)
    : module_handle_(module_handle),
      server_driver_hook_(std::make_unique<ServerDriverHook>())
{
}

HotpatchRuntime::~HotpatchRuntime()
{
    if (server_driver_hook_ != nullptr)
    {
        server_driver_hook_->Uninstall();
    }
    CloseSharedState();
}

void HotpatchRuntime::Run()
{
    while (InterlockedCompareExchange(&stop_requested_, 0, 0) == 0)
    {
        if (!OpenSharedState())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        shared_state_->injected_pid = GetCurrentProcessId();
        shared_state_->injected_heartbeat_ms = GetHeartbeatMilliseconds();

        if (!IsReplayDriverLoaded())
        {
            PublishStatus(
                L"Injected into vrserver. Waiting for driver_steamvr_capture_replay.dll to load before resolving the hook bridge.",
                HookState::Injected);
        }
        else if (!IsLighthouseModuleLoaded())
        {
            PublishStatus(
                L"Injected into vrserver. Waiting for driver_lighthouse.dll to appear before installing hooks.",
                HookState::Injected);
        }
        else if ((shared_state_->playback_active == 0u && shared_state_->recording_active == 0u &&
                     shared_state_->disabled_serial_count == 0u) ||
            (shared_state_->recording_active == 0u &&
                static_cast<LiveMode>(shared_state_->live_mode) == LiveMode::Passthrough &&
                shared_state_->disabled_serial_count == 0u))
        {
            if (server_driver_hook_ != nullptr && server_driver_hook_->installed())
            {
                server_driver_hook_->Uninstall();
            }
            PublishStatus(
                shared_state_->recording_active != 0u
                    ? L"Injected into vrserver. Live hook is active for driver-pose recording."
                    : L"Injected into vrserver. Live hook is idle until replay or recording starts.",
                HookState::LighthouseLoaded);
        }
        else
        {
            std::wstring hook_status;
            if (server_driver_hook_ != nullptr && server_driver_hook_->EnsureInstalled(shared_state_, &hook_status))
            {
                PublishStatus(hook_status.c_str(), HookState::HookInstalled);
            }
            else
            {
                PublishStatus(
                    hook_status.empty()
                        ? L"Injected into vrserver, but the live hook is not ready yet."
                        : hook_status.c_str(),
                    HookState::LighthouseLoaded);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    if (shared_state_ != nullptr)
    {
        PublishStatus(L"Hotpatch DLL is unloading.", HookState::Inactive);
        shared_state_->injected_pid = 0u;
    }
}

void HotpatchRuntime::RequestStop()
{
    InterlockedExchange(&stop_requested_, 1);
}

bool HotpatchRuntime::OpenSharedState()
{
    if (shared_state_ != nullptr)
    {
        return true;
    }

    shared_mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kSharedStateMappingName);
    if (shared_mapping_ == nullptr)
    {
        return false;
    }

    shared_state_ = static_cast<SharedState*>(
        MapViewOfFile(shared_mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (shared_state_ == nullptr)
    {
        CloseHandle(shared_mapping_);
        shared_mapping_ = nullptr;
        return false;
    }

    return true;
}

void HotpatchRuntime::CloseSharedState()
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
}

void HotpatchRuntime::PublishStatus(const wchar_t* text, const HookState hook_state)
{
    if (shared_state_ == nullptr)
    {
        return;
    }

    shared_state_->hook_state = static_cast<std::uint32_t>(hook_state);
    shared_state_->injected_heartbeat_ms = GetHeartbeatMilliseconds();
    CopyWideText(shared_state_->dll_status_text, text);
}

bool HotpatchRuntime::IsLighthouseModuleLoaded() const
{
    return GetModuleHandleW(L"driver_lighthouse.dll") != nullptr;
}

bool HotpatchRuntime::IsReplayDriverLoaded() const
{
    return GetModuleHandleW(L"driver_steamvr_capture_replay.dll") != nullptr;
}

std::uint64_t HotpatchRuntime::GetHeartbeatMilliseconds()
{
    return static_cast<std::uint64_t>(GetTickCount64());
}
}  // namespace steamvr_capture::hotpatch
