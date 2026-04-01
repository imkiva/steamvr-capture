#include "broker/broker_app.h"

#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

#include "openvr.h"
#include "session/replay_settings.h"
#include "session/session_format.h"

namespace steamvr_capture::broker
{
namespace
{
std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (required <= 1)
    {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), required);
    return result;
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
    {
        return {};
    }

    std::string result(static_cast<std::size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), required, nullptr, nullptr);
    return result;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    return WideToUtf8(path.wstring());
}

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

std::string DescribeWindowsError(const DWORD error_code)
{
    LPSTR message_buffer = nullptr;
    const DWORD message_size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        0,
        reinterpret_cast<LPSTR>(&message_buffer),
        0,
        nullptr);

    if (message_size == 0 || message_buffer == nullptr)
    {
        return "Windows error " + std::to_string(error_code);
    }

    std::string message(message_buffer, message_size);
    LocalFree(message_buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n'))
    {
        message.pop_back();
    }
    return message;
}

template <std::size_t N>
void CopyWideText(wchar_t (&destination)[N], const std::wstring& value)
{
    std::wmemset(destination, 0, N);
    if (value.empty())
    {
        return;
    }

    const std::size_t copy_length = std::min<std::size_t>(N - 1u, value.size());
    std::wmemcpy(destination, value.c_str(), copy_length);
    destination[copy_length] = L'\0';
}
}  // namespace

BrokerApp::~BrokerApp()
{
    Shutdown();
}

bool BrokerApp::Init(std::string* error)
{
    if (!OpenSharedState(error))
    {
        return false;
    }

    hotpatch_dll_path_ = ResolveHotpatchDllPath();
    if (hotpatch_dll_path_.empty() || !std::filesystem::is_regular_file(hotpatch_dll_path_))
    {
        if (error != nullptr)
        {
            *error = "Hotpatch DLL was not found next to the broker: " + PathToUtf8(hotpatch_dll_path_);
        }
        return false;
    }

    if (shared_state_ != nullptr)
    {
        shared_state_->broker_pid = GetCurrentProcessId();
        shared_state_->hook_state = static_cast<std::uint32_t>(hotpatch::HookState::BrokerReady);
        CopyWideText(shared_state_->hotpatch_dll_path, hotpatch_dll_path_.wstring());
        CopyWideText(shared_state_->broker_status_text, L"Broker initialized.");
    }

    return true;
}

bool BrokerApp::AttachForStatus(std::string* error)
{
    return OpenSharedStateReadOnly(error);
}

int BrokerApp::Run(const bool once)
{
    do
    {
        RefreshVrServerTarget();

        std::string error;
        if (!EnsureOpenVr(&error))
        {
            if (shared_state_ != nullptr)
            {
                CopyWideText(shared_state_->broker_status_text, Utf8ToWide("Waiting for SteamVR: " + error));
                shared_state_->broker_heartbeat_ms = GetHeartbeatMilliseconds();
            }
        }
        else
        {
            PollReplayState();
            UpdateSharedState();
            const bool should_inject = !session_path_.empty() && suppress_real_trackers_;
            if (should_inject)
            {
                std::string inject_error;
                if (!EnsureInjected(&inject_error) && shared_state_ != nullptr)
                {
                    CopyWideText(shared_state_->broker_status_text, Utf8ToWide("Inject failed: " + inject_error));
                }
            }
        }

        if (shared_state_ != nullptr)
        {
            if (!openvr_initialized_)
            {
                UpdateSharedState();
            }
            shared_state_->broker_heartbeat_ms = GetHeartbeatMilliseconds();
        }

        if (!once)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    } while (!once);

    return 0;
}

int BrokerApp::PrintStatus()
{
    if (shared_state_ == nullptr)
    {
        return 1;
    }

    const auto hook_state = static_cast<hotpatch::HookState>(shared_state_->hook_state);
    std::printf("broker_pid=%u\n", shared_state_->broker_pid);
    std::printf("target_pid=%u\n", shared_state_->target_pid);
    std::printf("injected_pid=%u\n", shared_state_->injected_pid);
    std::printf("playback_active=%u\n", shared_state_->playback_active);
    std::printf("suppress_real_trackers=%u\n", shared_state_->suppress_real_trackers);
    std::printf("mode=%u\n", shared_state_->live_mode);
    std::printf("hook_state=%u\n", static_cast<unsigned>(hook_state));
    std::printf("session=%s\n", WideToUtf8(shared_state_->session_path).c_str());
    std::printf("broker_status=%s\n", WideToUtf8(shared_state_->broker_status_text).c_str());
    std::printf("dll_status=%s\n", WideToUtf8(shared_state_->dll_status_text).c_str());
    std::printf("serial_count=%u\n", shared_state_->serial_count);
    for (std::uint32_t index = 0; index < shared_state_->serial_count && index < hotpatch::kMaxTrackedSerials; ++index)
    {
        std::printf("serial[%u]=%s\n", index, WideToUtf8(shared_state_->serials[index].serial).c_str());
    }
    return 0;
}

void BrokerApp::Shutdown()
{
    ShutdownOpenVr();
    CloseSharedState();
}

bool BrokerApp::OpenSharedState(std::string* error)
{
    if (shared_state_ != nullptr)
    {
        return true;
    }

    shared_mapping_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(hotpatch::SharedState)),
        hotpatch::kSharedStateMappingName);
    if (shared_mapping_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = "CreateFileMappingW failed: " + DescribeWindowsError(GetLastError());
        }
        return false;
    }

    shared_state_ = static_cast<hotpatch::SharedState*>(
        MapViewOfFile(shared_mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hotpatch::SharedState)));
    if (shared_state_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = "MapViewOfFile failed: " + DescribeWindowsError(GetLastError());
        }
        CloseHandle(shared_mapping_);
        shared_mapping_ = nullptr;
        return false;
    }

    if (shared_state_->magic != hotpatch::kProtocolMagic || shared_state_->version != hotpatch::kProtocolVersion ||
        shared_state_->size != sizeof(hotpatch::SharedState))
    {
        *shared_state_ = hotpatch::SharedState{};
    }

    shared_state_read_only_ = false;

    return true;
}

bool BrokerApp::OpenSharedStateReadOnly(std::string* error)
{
    if (shared_state_ != nullptr)
    {
        return true;
    }

    shared_mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, hotpatch::kSharedStateMappingName);
    if (shared_mapping_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = "OpenFileMappingW failed: " + DescribeWindowsError(GetLastError());
        }
        return false;
    }

    shared_state_ = static_cast<hotpatch::SharedState*>(
        MapViewOfFile(shared_mapping_, FILE_MAP_READ, 0, 0, sizeof(hotpatch::SharedState)));
    if (shared_state_ == nullptr)
    {
        if (error != nullptr)
        {
            *error = "MapViewOfFile failed: " + DescribeWindowsError(GetLastError());
        }
        CloseHandle(shared_mapping_);
        shared_mapping_ = nullptr;
        return false;
    }

    shared_state_read_only_ = true;

    return true;
}

void BrokerApp::CloseSharedState()
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

    shared_state_read_only_ = false;
}

bool BrokerApp::EnsureOpenVr(std::string* error)
{
    if (openvr_initialized_)
    {
        return vr::VRSettings() != nullptr;
    }

    vr::EVRInitError init_error = vr::VRInitError_None;
    vr::IVRSystem* system = vr::VR_Init(&init_error, vr::VRApplication_Utility);
    if (system == nullptr || init_error != vr::VRInitError_None)
    {
        if (error != nullptr)
        {
            *error = vr::VR_GetVRInitErrorAsEnglishDescription(init_error);
        }
        return false;
    }

    (void)system;
    openvr_initialized_ = true;
    return true;
}

void BrokerApp::ShutdownOpenVr()
{
    if (openvr_initialized_)
    {
        vr::VR_Shutdown();
        openvr_initialized_ = false;
    }
}

void BrokerApp::PollReplayState()
{
    if (vr::VRSettings() == nullptr)
    {
        return;
    }

    const std::string loaded_session_path = ReadSettingsString(
        replay_settings::kDriverSection,
        replay_settings::kLoadedSessionPathKey);
    const std::string requested_session_path = ReadSettingsString(
        replay_settings::kDriverSection,
        replay_settings::kSessionPathKey);
    const std::string next_session_path = !loaded_session_path.empty() ? loaded_session_path : requested_session_path;
    const std::string playback_state = ReadSettingsString(
        replay_settings::kDriverSection,
        replay_settings::kPlaybackStateKey);
    const bool suppress_real_trackers = ReadSettingsBool(
        replay_settings::kHotpatchSection,
        replay_settings::kSuppressRealTrackersKey,
        true);

    const bool state_changed = session_path_ != next_session_path || playback_state_ != playback_state ||
        suppress_real_trackers_ != suppress_real_trackers;

    session_path_ = next_session_path;
    playback_state_ = playback_state.empty() ? "stopped" : playback_state;
    suppress_real_trackers_ = suppress_real_trackers;
    playback_active_ = !session_path_.empty() && playback_state_ != "stopped";

    if (state_changed)
    {
        LoadSessionTargets();
    }
}

void BrokerApp::LoadSessionTargets()
{
    target_serials_.clear();

    if (session_path_.empty())
    {
        return;
    }

    session::SessionData session_data;
    std::string parse_error;
    if (!session::LoadSessionFile(session_path_, &session_data, &parse_error))
    {
        if (shared_state_ != nullptr)
        {
            CopyWideText(shared_state_->broker_status_text, Utf8ToWide("Session parse failed: " + parse_error));
        }
        return;
    }

    for (const session::TrackerDescriptor& tracker : session_data.trackers)
    {
        if (tracker.serial.empty())
        {
            continue;
        }

        if (target_serials_.size() >= hotpatch::kMaxTrackedSerials)
        {
            break;
        }

        target_serials_.push_back(Utf8ToWide(tracker.serial));
    }
}

void BrokerApp::RefreshVrServerTarget()
{
    target_pid_ = FindProcessIdByName("vrserver.exe");
}

void BrokerApp::UpdateSharedState()
{
    if (shared_state_ == nullptr)
    {
        return;
    }

    shared_state_->broker_pid = GetCurrentProcessId();
    shared_state_->target_pid = target_pid_;
    shared_state_->playback_active = playback_active_ ? 1u : 0u;
    shared_state_->suppress_real_trackers = suppress_real_trackers_ ? 1u : 0u;
    shared_state_->live_mode = static_cast<std::uint32_t>(
        suppress_real_trackers_ ? hotpatch::LiveMode::Suppress : hotpatch::LiveMode::Passthrough);
    shared_state_->serial_count = static_cast<std::uint32_t>(target_serials_.size());
    CopyWideText(shared_state_->session_path, Utf8ToWide(session_path_));

    for (std::size_t index = 0; index < hotpatch::kMaxTrackedSerials; ++index)
    {
        if (index < target_serials_.size())
        {
            CopyWideText(shared_state_->serials[index].serial, target_serials_[index]);
        }
        else
        {
            shared_state_->serials[index].serial[0] = L'\0';
        }
    }

    if (target_pid_ == 0u)
    {
        CopyWideText(shared_state_->broker_status_text, L"Waiting for vrserver.exe.");
    }
    else if (!playback_active_)
    {
        CopyWideText(
            shared_state_->broker_status_text,
            session_path_.empty()
                ? L"Broker idle. No replay session is selected."
                : L"Broker armed. Session is loaded and waiting for playback.");
    }
    else
    {
        CopyWideText(shared_state_->broker_status_text, L"Broker active. Ensuring hotpatch injection.");
    }
}

bool BrokerApp::EnsureInjected(std::string* error)
{
    if (target_pid_ == 0u)
    {
        if (error != nullptr)
        {
            *error = "vrserver.exe is not running.";
        }
        return false;
    }

    const std::wstring module_name = hotpatch_dll_path_.filename().wstring();
    if (IsDllLoadedInProcess(target_pid_, module_name))
    {
        return true;
    }

    return InjectDllIntoProcess(target_pid_, hotpatch_dll_path_, error);
}

bool BrokerApp::IsDllLoadedInProcess(const std::uint32_t process_id, const std::wstring& module_name) const
{
    if (process_id == 0u || module_name.empty())
    {
        return false;
    }

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    MODULEENTRY32W module_entry{};
    module_entry.dwSize = sizeof(module_entry);
    if (!Module32FirstW(snapshot, &module_entry))
    {
        CloseHandle(snapshot);
        return false;
    }

    bool found = false;
    do
    {
        if (_wcsicmp(module_entry.szModule, module_name.c_str()) == 0)
        {
            found = true;
            break;
        }
    } while (Module32NextW(snapshot, &module_entry));

    CloseHandle(snapshot);
    return found;
}

bool BrokerApp::InjectDllIntoProcess(
    const std::uint32_t process_id, const std::filesystem::path& dll_path, std::string* error) const
{
    const HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        process_id);
    if (process == nullptr)
    {
        if (error != nullptr)
        {
            *error = "OpenProcess failed: " + DescribeWindowsError(GetLastError());
        }
        return false;
    }

    const std::wstring dll_path_text = dll_path.wstring();
    const std::size_t buffer_bytes = (dll_path_text.size() + 1u) * sizeof(wchar_t);
    void* remote_buffer = VirtualAllocEx(process, nullptr, buffer_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_buffer == nullptr)
    {
        if (error != nullptr)
        {
            *error = "VirtualAllocEx failed: " + DescribeWindowsError(GetLastError());
        }
        CloseHandle(process);
        return false;
    }

    const BOOL wrote = WriteProcessMemory(
        process,
        remote_buffer,
        dll_path_text.c_str(),
        buffer_bytes,
        nullptr);
    if (!wrote)
    {
        if (error != nullptr)
        {
            *error = "WriteProcessMemory failed: " + DescribeWindowsError(GetLastError());
        }
        VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const FARPROC load_library = kernel32 != nullptr ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr;
    if (load_library == nullptr)
    {
        if (error != nullptr)
        {
            *error = "GetProcAddress(LoadLibraryW) failed.";
        }
        VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const HANDLE remote_thread = CreateRemoteThread(
        process,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(load_library),
        remote_buffer,
        0,
        nullptr);
    if (remote_thread == nullptr)
    {
        if (error != nullptr)
        {
            *error = "CreateRemoteThread failed: " + DescribeWindowsError(GetLastError());
        }
        VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(remote_thread, 5000);
    DWORD thread_exit_code = 0;
    GetExitCodeThread(remote_thread, &thread_exit_code);

    CloseHandle(remote_thread);
    VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
    CloseHandle(process);

    if (thread_exit_code == 0)
    {
        if (error != nullptr)
        {
            *error = "Remote LoadLibraryW returned 0.";
        }
        return false;
    }

    return true;
}

std::uint32_t BrokerApp::FindProcessIdByName(const char* process_name) const
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return 0u;
    }

    PROCESSENTRY32 process_entry{};
    process_entry.dwSize = sizeof(process_entry);
    if (!Process32First(snapshot, &process_entry))
    {
        CloseHandle(snapshot);
        return 0u;
    }

    std::uint32_t result = 0u;
    do
    {
        if (_stricmp(process_entry.szExeFile, process_name) == 0)
        {
            result = process_entry.th32ProcessID;
            break;
        }
    } while (Process32Next(snapshot, &process_entry));

    CloseHandle(snapshot);
    return result;
}

std::filesystem::path BrokerApp::ResolveHotpatchDllPath() const
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    if (buffer.empty())
    {
        return {};
    }

    const std::filesystem::path exe_path(buffer);
    return exe_path.parent_path() / "steamvr_capture_hotpatch.dll";
}

std::uint64_t BrokerApp::GetHeartbeatMilliseconds()
{
    return static_cast<std::uint64_t>(GetTickCount64());
}
}  // namespace steamvr_capture::broker
