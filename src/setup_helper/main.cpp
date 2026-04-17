#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "openvr.h"
#include "session/replay_settings.h"

namespace
{
constexpr wchar_t kDriverName[] = L"steamvr_capture_replay";
constexpr char kDriverNameUtf8[] = "steamvr_capture_replay";
constexpr wchar_t kRegistryKey[] = L"Software\\SteamVRCapture";
constexpr wchar_t kInstallRootValue[] = L"InstallRoot";
constexpr wchar_t kOverlayManifestValue[] = L"OverlayManifestPath";

struct ProcessResult
{
    bool started = false;
    DWORD exit_code = 1;
    std::string output;
};

std::wstring Utf8ToWide(const std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required);
    return result;
}

std::string WideToUtf8(const std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int required =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required, nullptr, nullptr);
    return result;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    return WideToUtf8(path.wstring());
}

std::filesystem::path GetExecutablePath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

std::filesystem::path ToAbsolutePath(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::weakly_canonical(path, error);
    if (!error)
    {
        return absolute;
    }

    error.clear();
    absolute = std::filesystem::absolute(path, error);
    return error ? path : absolute;
}

std::wstring LowercasePath(const std::filesystem::path& path)
{
    std::wstring text = ToAbsolutePath(path).make_preferred().wstring();
    std::transform(text.begin(), text.end(), text.begin(), [](const wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return text;
}

std::filesystem::path GetInstallRoot()
{
    const std::filesystem::path exe_path = GetExecutablePath();
    const std::filesystem::path exe_dir = exe_path.parent_path();
    if (_wcsicmp(exe_dir.filename().c_str(), L"tools") == 0)
    {
        return exe_dir.parent_path();
    }
    return exe_dir;
}

std::wstring QuoteCommandLineArg(const std::wstring& argument)
{
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument)
    {
        if (ch == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (ch == L'"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted += ch;
            backslashes = 0;
            continue;
        }

        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted += ch;
    }

    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

ProcessResult RunProcessCapture(
    const std::filesystem::path& executable_path,
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& working_directory = {})
{
    ProcessResult result;

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security_attributes, 0))
    {
        result.output = "CreatePipe failed.";
        return result;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring command_line = QuoteCommandLineArg(executable_path.wstring());
    for (const std::wstring& argument : arguments)
    {
        command_line += L" ";
        command_line += QuoteCommandLineArg(argument);
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdOutput = write_pipe;
    startup_info.hStdError = write_pipe;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process_info{};
    const BOOL started = CreateProcessW(
        executable_path.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        working_directory.empty() ? nullptr : working_directory.wstring().c_str(),
        &startup_info,
        &process_info);

    CloseHandle(write_pipe);

    if (!started)
    {
        CloseHandle(read_pipe);
        result.output = "CreateProcess failed for " + PathToUtf8(executable_path);
        return result;
    }

    result.started = true;

    char buffer[4096];
    DWORD bytes_read = 0;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0)
    {
        result.output.append(buffer, buffer + bytes_read);
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    GetExitCodeProcess(process_info.hProcess, &result.exit_code);

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    CloseHandle(read_pipe);
    return result;
}

std::string TrimAscii(std::string text)
{
    const auto is_space = [](const unsigned char ch) { return std::isspace(ch) != 0; };
    while (!text.empty() && is_space(static_cast<unsigned char>(text.front())))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && is_space(static_cast<unsigned char>(text.back())))
    {
        text.pop_back();
    }
    return text;
}

std::filesystem::path ParseDriverPathFromVrpathregOutput(const std::string& output)
{
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line))
    {
        line = TrimAscii(line);
        if (line.empty())
        {
            continue;
        }
        if (line.find("not found") != std::string::npos || line.find("not present") != std::string::npos)
        {
            continue;
        }
        return std::filesystem::path(Utf8ToWide(line));
    }
    return {};
}

std::optional<std::filesystem::path> ReadRegistryInstallLocation(HKEY root, const wchar_t* subkey, REGSAM flags)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | flags, &key) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }

    std::wstring buffer(32768, L'\0');
    DWORD size_bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    const LONG query_result = RegGetValueW(key, nullptr, L"InstallLocation", RRF_RT_REG_SZ, nullptr, buffer.data(), &size_bytes);
    RegCloseKey(key);

    if (query_result != ERROR_SUCCESS || size_bytes < sizeof(wchar_t))
    {
        return std::nullopt;
    }

    buffer.resize((size_bytes / sizeof(wchar_t)) - 1);
    if (buffer.empty())
    {
        return std::nullopt;
    }
    return std::filesystem::path(buffer);
}

std::optional<std::filesystem::path> ReadUserRegistryPathValue(const wchar_t* value_name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }

    std::wstring buffer(32768, L'\0');
    DWORD size_bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    const LONG query_result = RegGetValueW(key, nullptr, value_name, RRF_RT_REG_SZ, nullptr, buffer.data(), &size_bytes);
    RegCloseKey(key);

    if (query_result != ERROR_SUCCESS || size_bytes < sizeof(wchar_t))
    {
        return std::nullopt;
    }

    buffer.resize((size_bytes / sizeof(wchar_t)) - 1);
    return std::filesystem::path(buffer);
}

void WriteUserRegistryPathValue(const wchar_t* value_name, const std::filesystem::path& path)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        return;
    }

    const std::wstring text = ToAbsolutePath(path).make_preferred().wstring();
    RegSetValueExW(
        key,
        value_name,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(text.c_str()),
        static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

void DeleteUserRegistryState()
{
    RegDeleteTreeW(HKEY_CURRENT_USER, kRegistryKey);
}

std::optional<std::filesystem::path> GetRuntimePathFromOpenVR()
{
    char buffer[4096] = {};
    std::uint32_t required = 0;
    if (vr::VR_GetRuntimePath(buffer, sizeof(buffer), &required))
    {
        return std::filesystem::path(Utf8ToWide(buffer));
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> FindSteamVrRuntimePath()
{
    if (const auto runtime_path = GetRuntimePathFromOpenVR())
    {
        return runtime_path;
    }

    constexpr wchar_t kSteamVrUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 250820";
    for (const auto root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
    {
        constexpr REGSAM kRegistryViews[] = {
            static_cast<REGSAM>(KEY_WOW64_32KEY),
            static_cast<REGSAM>(KEY_WOW64_64KEY),
            static_cast<REGSAM>(0),
        };
        for (const REGSAM flags : kRegistryViews)
        {
            if (const auto install_location = ReadRegistryInstallLocation(root, kSteamVrUninstallKey, flags))
            {
                return install_location;
            }
        }
    }

    const std::filesystem::path default_path =
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR";
    if (std::filesystem::exists(default_path))
    {
        return default_path;
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> FindVrPathReg()
{
    const auto runtime_path = FindSteamVrRuntimePath();
    if (!runtime_path)
    {
        return std::nullopt;
    }

    const std::filesystem::path candidate = *runtime_path / L"bin" / L"win64" / L"vrpathreg.exe";
    if (std::filesystem::exists(candidate))
    {
        return candidate;
    }
    return std::nullopt;
}

std::filesystem::path GetDefaultSessionRoot()
{
    PWSTR documents_path_raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents_path_raw)) &&
        documents_path_raw != nullptr)
    {
        std::filesystem::path result = std::filesystem::path(documents_path_raw) / L"SteamVR Capture" / L"Sessions";
        CoTaskMemFree(documents_path_raw);
        return result;
    }

    wchar_t user_profile[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (GetEnvironmentVariableW(L"USERPROFILE", user_profile, size) > 0)
    {
        return std::filesystem::path(user_profile) / L"Documents" / L"SteamVR Capture" / L"Sessions";
    }

    return GetInstallRoot() / L"sessions";
}

std::string JsonEscape(const std::string& text)
{
    std::string result;
    result.reserve(text.size() + 16);
    for (const char ch : text)
    {
        switch (ch)
        {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += ch;
            break;
        }
    }
    return result;
}

bool WriteOverlayManifest(const std::filesystem::path& manifest_path, const std::filesystem::path& overlay_exe, std::string* error)
{
    std::error_code filesystem_error;
    std::filesystem::create_directories(manifest_path.parent_path(), filesystem_error);
    if (filesystem_error)
    {
        if (error != nullptr)
        {
            *error = "Failed to create manifest directory: " + filesystem_error.message();
        }
        return false;
    }

    std::ofstream stream(manifest_path, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        if (error != nullptr)
        {
            *error = "Failed to write overlay manifest: " + PathToUtf8(manifest_path);
        }
        return false;
    }

    stream
        << "{\n"
        << "  \"source\": \"builtin\",\n"
        << "  \"applications\": [\n"
        << "    {\n"
        << "      \"app_key\": \"" << steamvr_capture::replay_settings::kOverlayAppKey << "\",\n"
        << "      \"launch_type\": \"binary\",\n"
        << "      \"binary_path_windows\": \"" << JsonEscape(PathToUtf8(ToAbsolutePath(overlay_exe))) << "\",\n"
        << "      \"is_dashboard_overlay\": true,\n"
        << "      \"strings\": {\n"
        << "        \"en_us\": {\n"
        << "          \"name\": \"SteamVR Capture Replay\",\n"
        << "          \"description\": \"Select replay sessions and record SteamVR tracked-device motion.\"\n"
        << "        }\n"
        << "      }\n"
        << "    }\n"
        << "  ]\n"
        << "}\n";

    return true;
}

std::string GetApplicationsErrorName(const vr::EVRApplicationError error)
{
    if (vr::VRApplications() == nullptr)
    {
        return "IVRApplications unavailable";
    }
    return vr::VRApplications()->GetApplicationsErrorNameFromEnum(error);
}

bool InitOpenVRUtility(std::string* error)
{
    vr::EVRInitError init_error = vr::VRInitError_None;
    vr::VR_Init(&init_error, vr::VRApplication_Utility);
    if (init_error != vr::VRInitError_None)
    {
        if (error != nullptr)
        {
            *error = std::string("OpenVR utility init failed: ") + vr::VR_GetVRInitErrorAsEnglishDescription(init_error);
        }
        return false;
    }
    return true;
}

std::string GetApplicationPropertyString(const char* app_key, const vr::EVRApplicationProperty property)
{
    if (vr::VRApplications() == nullptr)
    {
        return {};
    }

    char buffer[4096] = {};
    vr::EVRApplicationError error = vr::VRApplicationError_None;
    vr::VRApplications()->GetApplicationPropertyString(app_key, property, buffer, sizeof(buffer), &error);
    return error == vr::VRApplicationError_None ? std::string(buffer) : std::string();
}

bool RegisteredOverlayBinaryMatches(const std::filesystem::path& overlay_exe)
{
    const std::string binary_path = GetApplicationPropertyString(
        steamvr_capture::replay_settings::kOverlayAppKey,
        vr::VRApplicationProperty_BinaryPath_String);
    if (binary_path.empty())
    {
        return false;
    }
    return LowercasePath(std::filesystem::path(Utf8ToWide(binary_path))) == LowercasePath(overlay_exe);
}

bool RegisterOverlayManifest(const std::filesystem::path& manifest_path, const std::filesystem::path& overlay_exe, std::string* error)
{
    if (vr::VRApplications() == nullptr)
    {
        if (error != nullptr)
        {
            *error = "IVRApplications is unavailable.";
        }
        return false;
    }

    const std::string manifest_utf8 = PathToUtf8(ToAbsolutePath(manifest_path));
    const vr::EVRApplicationError add_error = vr::VRApplications()->AddApplicationManifest(manifest_utf8.c_str(), false);
    if (add_error != vr::VRApplicationError_None)
    {
        // Existing installs with the same app key often make AddApplicationManifest non-idempotent.
        // If the app is installed, still force auto launch below so repair remains useful.
        if (!vr::VRApplications()->IsApplicationInstalled(steamvr_capture::replay_settings::kOverlayAppKey))
        {
            if (error != nullptr)
            {
                *error = "Failed to register overlay manifest: " + GetApplicationsErrorName(add_error);
            }
            return false;
        }
        if (!RegisteredOverlayBinaryMatches(overlay_exe))
        {
            if (error != nullptr)
            {
                *error =
                    "Overlay app key is already registered to a different binary path. "
                    "Uninstall the previous SteamVR Capture build or remove the old application manifest, then run repair.";
            }
            return false;
        }
    }

    const vr::EVRApplicationError auto_launch_error =
        vr::VRApplications()->SetApplicationAutoLaunch(steamvr_capture::replay_settings::kOverlayAppKey, true);
    if (auto_launch_error != vr::VRApplicationError_None)
    {
        if (error != nullptr)
        {
            *error = "Overlay manifest registered, but enabling auto launch failed: " +
                GetApplicationsErrorName(auto_launch_error);
        }
        return false;
    }

    return true;
}

bool RemoveOverlayManifest(const std::filesystem::path& manifest_path, std::string* error)
{
    if (vr::VRApplications() == nullptr)
    {
        return true;
    }

    if (vr::VRApplications()->IsApplicationInstalled(steamvr_capture::replay_settings::kOverlayAppKey))
    {
        vr::VRApplications()->SetApplicationAutoLaunch(steamvr_capture::replay_settings::kOverlayAppKey, false);
    }

    const std::string manifest_utf8 = PathToUtf8(ToAbsolutePath(manifest_path));
    const vr::EVRApplicationError remove_error = vr::VRApplications()->RemoveApplicationManifest(manifest_utf8.c_str());
    if (remove_error != vr::VRApplicationError_None &&
        remove_error != vr::VRApplicationError_UnknownApplication)
    {
        if (error != nullptr)
        {
            *error = "Failed to remove overlay manifest: " + GetApplicationsErrorName(remove_error);
        }
        return false;
    }

    return true;
}

std::string ReadSettingsString(const char* section, const char* key)
{
    char buffer[4096] = {};
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->GetString(section, key, buffer, sizeof(buffer), &error);
    return error == vr::VRSettingsError_None ? std::string(buffer) : std::string();
}

void SetStringIfEmpty(const char* section, const char* key, const std::string& value)
{
    if (!ReadSettingsString(section, key).empty())
    {
        return;
    }

    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->SetString(section, key, value.c_str(), &error);
}

void SetFloatIfMissing(const char* section, const char* key, const float value)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->GetFloat(section, key, &error);
    if (error == vr::VRSettingsError_None)
    {
        return;
    }

    error = vr::VRSettingsError_None;
    vr::VRSettings()->SetFloat(section, key, value, &error);
}

void SetBoolIfMissing(const char* section, const char* key, const bool value)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->GetBool(section, key, &error);
    if (error == vr::VRSettingsError_None)
    {
        return;
    }

    error = vr::VRSettingsError_None;
    vr::VRSettings()->SetBool(section, key, value, &error);
}

void InitDefaultSettings(const std::filesystem::path& default_session_root)
{
    std::error_code filesystem_error;
    std::filesystem::create_directories(default_session_root, filesystem_error);

    using namespace steamvr_capture::replay_settings;
    SetStringIfEmpty(kOverlaySection, kSessionRootKey, PathToUtf8(ToAbsolutePath(default_session_root)));
    SetFloatIfMissing(kOverlaySection, kRecordIntervalMsKey, kDefaultRecordIntervalMs);
    SetStringIfEmpty(kOverlaySection, kRecordModeKey, "driver_pose");
    SetStringIfEmpty(kOverlaySection, kRecordStateKey, "stopped");
    SetStringIfEmpty(kDriverSection, kPlaybackStateKey, "stopped");
    SetBoolIfMissing(kDriverSection, kLoopKey, true);
    SetFloatIfMissing(kDriverSection, kPlaybackSpeedKey, 1.0f);
    SetStringIfEmpty(kHotpatchSection, kLiveModeKey, "passthrough");
    SetBoolIfMissing(kHotpatchSection, kSuppressRealTrackersKey, false);
}

bool RegisterDriver(const std::filesystem::path& driver_dir, std::string* error)
{
    const auto vrpathreg = FindVrPathReg();
    if (!vrpathreg)
    {
        if (error != nullptr)
        {
            *error = "SteamVR vrpathreg.exe was not found. Is SteamVR installed?";
        }
        return false;
    }

    const ProcessResult find_result = RunProcessCapture(*vrpathreg, {L"finddriver", kDriverName});
    if (find_result.started && find_result.exit_code == 2)
    {
        RunProcessCapture(*vrpathreg, {L"removedriverswithname", kDriverName});
    }
    else if (find_result.started && find_result.exit_code == 0)
    {
        const std::filesystem::path existing_path = ParseDriverPathFromVrpathregOutput(find_result.output);
        if (!existing_path.empty() && LowercasePath(existing_path) != LowercasePath(driver_dir))
        {
            RunProcessCapture(*vrpathreg, {L"removedriver", existing_path.wstring()});
        }
        else if (!existing_path.empty())
        {
            std::cout << "Replay driver is already registered at " << PathToUtf8(existing_path) << "\n";
            return true;
        }
    }

    const ProcessResult add_result = RunProcessCapture(*vrpathreg, {L"adddriver", ToAbsolutePath(driver_dir).wstring()});
    if (!add_result.started || add_result.exit_code != 0)
    {
        if (error != nullptr)
        {
            *error = "vrpathreg adddriver failed: " + add_result.output;
        }
        return false;
    }

    std::cout << "Registered replay driver: " << PathToUtf8(ToAbsolutePath(driver_dir)) << "\n";
    return true;
}

bool UnregisterDriver(const std::filesystem::path& driver_dir, std::string* error)
{
    const auto vrpathreg = FindVrPathReg();
    if (!vrpathreg)
    {
        std::cout << "SteamVR vrpathreg.exe was not found; skipping driver unregister.\n";
        return true;
    }

    ProcessResult find_result = RunProcessCapture(*vrpathreg, {L"finddriver", kDriverName});
    if (find_result.started && find_result.exit_code == 2)
    {
        const ProcessResult remove_all_result = RunProcessCapture(*vrpathreg, {L"removedriverswithname", kDriverName});
        if (!remove_all_result.started || remove_all_result.exit_code != 0)
        {
            if (error != nullptr)
            {
                *error = "vrpathreg removedriverswithname failed: " + remove_all_result.output;
            }
            return false;
        }
        std::cout << "Removed duplicate replay driver registrations.\n";
        return true;
    }

    std::filesystem::path target_path = driver_dir;
    if (find_result.started && find_result.exit_code == 0)
    {
        const std::filesystem::path found_path = ParseDriverPathFromVrpathregOutput(find_result.output);
        if (!found_path.empty())
        {
            target_path = found_path;
        }
    }

    const ProcessResult remove_result = RunProcessCapture(*vrpathreg, {L"removedriver", ToAbsolutePath(target_path).wstring()});
    if (remove_result.started && remove_result.exit_code == 0)
    {
        std::cout << "Unregistered replay driver: " << PathToUtf8(ToAbsolutePath(target_path)) << "\n";
        return true;
    }

    std::cout << "Replay driver was not registered or was already removed.\n";
    return true;
}

bool ValidateRuntimeFiles(const std::filesystem::path& install_root, std::string* error)
{
    const std::vector<std::filesystem::path> required_files = {
        install_root / L"tools" / L"steamvr_capture_overlay.exe",
        install_root / L"tools" / L"steamvr_capture_broker.exe",
        install_root / L"tools" / L"steamvr_capture_recorder.exe",
        install_root / L"tools" / L"steamvr_capture_hotpatch.dll",
        install_root / L"tools" / L"openvr_api.dll",
        install_root / L"steamvr_capture_replay" / L"driver.vrdrivermanifest",
        install_root / L"steamvr_capture_replay" / L"bin" / L"win64" / L"driver_steamvr_capture_replay.dll",
    };

    for (const auto& file : required_files)
    {
        if (!std::filesystem::exists(file))
        {
            if (error != nullptr)
            {
                *error = "Required runtime file is missing: " + PathToUtf8(file);
            }
            return false;
        }
    }
    return true;
}

bool Install()
{
    const std::filesystem::path install_root = ToAbsolutePath(GetInstallRoot());
    const std::filesystem::path tools_dir = install_root / L"tools";
    const std::filesystem::path driver_dir = install_root / L"steamvr_capture_replay";
    const std::filesystem::path overlay_exe = tools_dir / L"steamvr_capture_overlay.exe";
    const std::filesystem::path overlay_manifest =
        tools_dir / Utf8ToWide(steamvr_capture::replay_settings::kOverlayManifestFilename);
    const std::filesystem::path session_root = GetDefaultSessionRoot();

    std::string error;
    if (!ValidateRuntimeFiles(install_root, &error))
    {
        std::cerr << error << "\n";
        return false;
    }

    if (!WriteOverlayManifest(overlay_manifest, overlay_exe, &error))
    {
        std::cerr << error << "\n";
        return false;
    }

    if (!RegisterDriver(driver_dir, &error))
    {
        std::cerr << error << "\n";
        return false;
    }

    if (!InitOpenVRUtility(&error))
    {
        std::cerr << error << "\n";
        return false;
    }

    if (const auto previous_manifest = ReadUserRegistryPathValue(kOverlayManifestValue);
        previous_manifest && LowercasePath(*previous_manifest) != LowercasePath(overlay_manifest))
    {
        std::string remove_error;
        RemoveOverlayManifest(*previous_manifest, &remove_error);
    }

    const bool overlay_registered = RegisterOverlayManifest(overlay_manifest, overlay_exe, &error);
    if (overlay_registered)
    {
        InitDefaultSettings(session_root);
    }
    vr::VR_Shutdown();

    if (!overlay_registered)
    {
        std::cerr << error << "\n";
        return false;
    }

    std::cout << "Registered overlay manifest: " << PathToUtf8(ToAbsolutePath(overlay_manifest)) << "\n";
    std::cout << "Enabled overlay auto launch.\n";
    std::cout << "Default session directory: " << PathToUtf8(ToAbsolutePath(session_root)) << "\n";
    WriteUserRegistryPathValue(kInstallRootValue, install_root);
    WriteUserRegistryPathValue(kOverlayManifestValue, overlay_manifest);
    return true;
}

bool Uninstall()
{
    const std::filesystem::path install_root = ToAbsolutePath(GetInstallRoot());
    const std::filesystem::path driver_dir = install_root / L"steamvr_capture_replay";
    const std::filesystem::path overlay_manifest =
        install_root / L"tools" / Utf8ToWide(steamvr_capture::replay_settings::kOverlayManifestFilename);
    const std::optional<std::filesystem::path> previous_manifest = ReadUserRegistryPathValue(kOverlayManifestValue);

    bool ok = true;
    std::string error;
    if (!UnregisterDriver(driver_dir, &error))
    {
        std::cerr << error << "\n";
        ok = false;
    }

    if (InitOpenVRUtility(&error))
    {
        if (previous_manifest && LowercasePath(*previous_manifest) != LowercasePath(overlay_manifest))
        {
            std::string remove_error;
            RemoveOverlayManifest(*previous_manifest, &remove_error);
        }
        if (!RemoveOverlayManifest(overlay_manifest, &error))
        {
            std::cerr << error << "\n";
            ok = false;
        }
        vr::VR_Shutdown();
    }
    else
    {
        std::cout << "OpenVR utility init failed; skipping overlay manifest removal: " << error << "\n";
    }

    DeleteUserRegistryState();
    return ok;
}

bool PrintStatus()
{
    const std::filesystem::path install_root = ToAbsolutePath(GetInstallRoot());
    const std::filesystem::path driver_dir = install_root / L"steamvr_capture_replay";
    const std::filesystem::path overlay_manifest =
        install_root / L"tools" / Utf8ToWide(steamvr_capture::replay_settings::kOverlayManifestFilename);

    std::cout << "Install root: " << PathToUtf8(install_root) << "\n";
    std::cout << "Overlay manifest: " << PathToUtf8(overlay_manifest) << "\n";

    std::string error;
    std::cout << "Runtime files: " << (ValidateRuntimeFiles(install_root, &error) ? "ok" : ("missing: " + error)) << "\n";

    if (const auto runtime_path = FindSteamVrRuntimePath())
    {
        std::cout << "SteamVR runtime: " << PathToUtf8(*runtime_path) << "\n";
    }
    else
    {
        std::cout << "SteamVR runtime: not found\n";
    }

    if (const auto vrpathreg = FindVrPathReg())
    {
        std::cout << "vrpathreg: " << PathToUtf8(*vrpathreg) << "\n";
        const ProcessResult find_result = RunProcessCapture(*vrpathreg, {L"finddriver", kDriverName});
        if (find_result.started && find_result.exit_code == 0)
        {
            std::cout << "Replay driver: " << TrimAscii(find_result.output) << "\n";
        }
        else if (find_result.started)
        {
            std::cout << "Replay driver: not registered (exit " << find_result.exit_code << ")\n";
        }
        else
        {
            std::cout << "Replay driver: status unavailable\n";
        }
    }
    else
    {
        std::cout << "vrpathreg: not found\n";
    }

    if (InitOpenVRUtility(&error))
    {
        const bool app_installed = vr::VRApplications() != nullptr &&
            vr::VRApplications()->IsApplicationInstalled(steamvr_capture::replay_settings::kOverlayAppKey);
        const bool auto_launch = app_installed &&
            vr::VRApplications()->GetApplicationAutoLaunch(steamvr_capture::replay_settings::kOverlayAppKey);
        std::cout << "Overlay app installed: " << (app_installed ? "yes" : "no") << "\n";
        std::cout << "Overlay auto launch: " << (auto_launch ? "yes" : "no") << "\n";
        std::cout << "Session root: " << ReadSettingsString(
            steamvr_capture::replay_settings::kOverlaySection,
            steamvr_capture::replay_settings::kSessionRootKey) << "\n";
        vr::VR_Shutdown();
    }
    else
    {
        std::cout << "OpenVR utility: unavailable (" << error << ")\n";
    }

    return true;
}

void PrintUsage()
{
    std::cout
        << "Usage:\n"
        << "  steamvr_capture_setup_helper --install\n"
        << "  steamvr_capture_setup_helper --repair\n"
        << "  steamvr_capture_setup_helper --uninstall\n"
        << "  steamvr_capture_setup_helper --status\n";
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        PrintUsage();
        return 1;
    }

    const std::string command = argv[1];
    if (command == "--install" || command == "--repair")
    {
        return Install() ? 0 : 1;
    }
    if (command == "--uninstall")
    {
        return Uninstall() ? 0 : 1;
    }
    if (command == "--status")
    {
        return PrintStatus() ? 0 : 1;
    }

    PrintUsage();
    return 1;
}
