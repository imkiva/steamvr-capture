#include "overlay/overlay_app.h"

#include <windows.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "hotpatch_shared/hotpatch_protocol.h"
#include "openvr.h"
#include "session/replay_settings.h"

namespace steamvr_capture::overlay
{
namespace
{
constexpr int kMainWidth = 1600;
constexpr int kMainHeight = 1000;
constexpr int kThumbnailWidth = 400;
constexpr int kThumbnailHeight = 400;
constexpr int kDesktopWindowWidth = 1280;
constexpr int kDesktopWindowHeight = 860;
constexpr int kPageSize = 8;
constexpr float kOverlayWidthMeters = 1.6f;
constexpr float kThumbnailWidthMeters = 0.32f;
constexpr std::chrono::milliseconds kUiTickInterval(16);
constexpr std::chrono::milliseconds kSettingsRefreshInterval(250);
constexpr wchar_t kUiFontFace[] = L"Segoe UI";
constexpr wchar_t kDesktopWindowClassName[] = L"SteamVRCaptureReplayDesktopWindow";

enum class ButtonAction
{
    None,
    Refresh,
    PrevPage,
    NextPage,
    Play,
    Pause,
    Stop,
    ToggleLoop,
    CycleLiveMode,
    EditRoot,
    EditPath,
    PasteClipboard,
    ReloadCurrent,
    LoadFile,
};

enum class KeyboardMode : std::uint64_t
{
    None = 0,
    SessionRoot = 1,
    DirectPath = 2,
};

struct Button
{
    RECT rect{};
    ButtonAction action = ButtonAction::None;
    std::size_t file_index = 0;
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

std::filesystem::path Utf8Path(const std::string& text)
{
    return std::filesystem::path(Utf8ToWide(text));
}

std::filesystem::path GetExecutablePath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

bool IsProcessRunning(const wchar_t* process_name)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    PROCESSENTRY32W process_entry{};
    process_entry.dwSize = sizeof(process_entry);
    if (!Process32FirstW(snapshot, &process_entry))
    {
        CloseHandle(snapshot);
        return false;
    }

    bool found = false;
    do
    {
        if (_wcsicmp(process_entry.szExeFile, process_name) == 0)
        {
            found = true;
            break;
        }
    } while (Process32NextW(snapshot, &process_entry));

    CloseHandle(snapshot);
    return found;
}

bool EnsureBrokerRunning(const std::filesystem::path& exe_directory, std::string* error)
{
    constexpr wchar_t kBrokerProcessName[] = L"steamvr_capture_broker.exe";
    if (IsProcessRunning(kBrokerProcessName))
    {
        return true;
    }

    const std::filesystem::path broker_path = exe_directory / kBrokerProcessName;
    std::error_code filesystem_error;
    if (!std::filesystem::exists(broker_path, filesystem_error))
    {
        if (error != nullptr)
        {
            *error = "Hotpatch broker executable was not found: " + broker_path.string();
        }
        return false;
    }

    std::wstring command_line = L"\"" + broker_path.wstring() + L"\"";
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};

    const BOOL started = CreateProcessW(
        broker_path.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        exe_directory.wstring().c_str(),
        &startup_info,
        &process_info);
    if (!started)
    {
        if (error != nullptr)
        {
            *error = "Failed to start hotpatch broker process.";
        }
        return false;
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return true;
}

std::string ReadSettingsString(const char* section, const char* key)
{
    char buffer[4096] = {};
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->GetString(section, key, buffer, sizeof(buffer), &error);
    return error == vr::VRSettingsError_None ? std::string(buffer) : std::string();
}

std::int32_t ReadSettingsInt(const char* section, const char* key, const std::int32_t fallback)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const std::int32_t value = vr::VRSettings()->GetInt32(section, key, &error);
    return error == vr::VRSettingsError_None ? value : fallback;
}

void WriteSettingsString(const char* section, const char* key, const std::string& value)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->SetString(section, key, value.c_str(), &error);
}

void WriteSettingsInt(const char* section, const char* key, const std::int32_t value)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->SetInt32(section, key, value, &error);
}

void WriteSettingsBool(const char* section, const char* key, const bool value)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->SetBool(section, key, value, &error);
}

bool ReadSettingsBool(const char* section, const char* key, const bool fallback)
{
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const bool value = vr::VRSettings()->GetBool(section, key, &error);
    return error == vr::VRSettingsError_None ? value : fallback;
}

std::filesystem::path ToAbsolutePath(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {};
    }

    std::error_code error;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    if (!error)
    {
        return canonical;
    }

    error.clear();
    canonical = std::filesystem::absolute(path, error);
    return error ? path : canonical;
}

std::wstring NormalizePathKey(const std::filesystem::path& path)
{
    std::wstring text = ToAbsolutePath(path).make_preferred().wstring();
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return text;
}

std::string NormalizeUtf8PathString(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    return PathToUtf8(ToAbsolutePath(Utf8Path(text)));
}

std::string LowerAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

std::wstring TruncateMiddle(const std::wstring& text, const std::size_t max_chars)
{
    if (text.size() <= max_chars)
    {
        return text;
    }

    if (max_chars < 7)
    {
        return text.substr(0, max_chars);
    }

    const std::size_t prefix = (max_chars - 3) / 2;
    const std::size_t suffix = max_chars - 3 - prefix;
    return text.substr(0, prefix) + L"..." + text.substr(text.size() - suffix);
}

std::wstring FilenameLabel(const std::filesystem::path& path)
{
    const std::wstring filename = path.filename().wstring();
    return filename.empty() ? path.wstring() : filename;
}

std::wstring TrimClipboardPathText(std::wstring text)
{
    auto is_trim_char = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; };
    while (!text.empty() && is_trim_char(text.front()))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && is_trim_char(text.back()))
    {
        text.pop_back();
    }

    const std::size_t line_break = text.find_first_of(L"\r\n");
    if (line_break != std::wstring::npos)
    {
        text.erase(line_break);
    }

    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"')
    {
        text = text.substr(1, text.size() - 2);
    }

    return text;
}

bool ContainsPoint(const RECT& rect, const LONG x, const LONG y)
{
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

std::filesystem::path FindDefaultSessionRoot(const std::filesystem::path& exe_directory)
{
    std::filesystem::path current = exe_directory;
    for (int depth = 0; depth < 6; ++depth)
    {
        const auto candidate = current / "sessions";
        std::error_code error;
        if (std::filesystem::exists(candidate, error) && std::filesystem::is_directory(candidate, error))
        {
            return ToAbsolutePath(candidate);
        }

        if (!current.has_parent_path())
        {
            break;
        }
        current = current.parent_path();
    }

    return {};
}

std::string GetApplicationsErrorName(const vr::EVRApplicationError error)
{
    if (vr::VRApplications() == nullptr)
    {
        return "IVRApplications unavailable";
    }
    return vr::VRApplications()->GetApplicationsErrorNameFromEnum(error);
}

std::string GetOverlayErrorName(const vr::EVROverlayError error)
{
    if (vr::VROverlay() == nullptr)
    {
        return "IVROverlay unavailable";
    }
    return vr::VROverlay()->GetOverlayErrorNameFromEnum(error);
}

bool RegisterManifestWithRuntime(const std::filesystem::path& manifest_path, const bool enable_auto_launch, std::string* error)
{
    if (vr::VRApplications() == nullptr)
    {
        if (error != nullptr)
        {
            *error = "IVRApplications is unavailable.";
        }
        return false;
    }

    std::error_code filesystem_error;
    if (!std::filesystem::exists(manifest_path, filesystem_error))
    {
        if (error != nullptr)
        {
            *error = "Overlay manifest was not found: " + manifest_path.string();
        }
        return false;
    }

    if (!vr::VRApplications()->IsApplicationInstalled(replay_settings::kOverlayAppKey))
    {
        const std::string manifest_utf8 = PathToUtf8(manifest_path);
        const vr::EVRApplicationError add_error = vr::VRApplications()->AddApplicationManifest(manifest_utf8.c_str(), false);
        if (add_error != vr::VRApplicationError_None)
        {
            if (error != nullptr)
            {
                *error = "Failed to register overlay manifest: " + GetApplicationsErrorName(add_error);
            }
            return false;
        }
    }

    if (enable_auto_launch)
    {
        const vr::EVRApplicationError auto_launch_error =
            vr::VRApplications()->SetApplicationAutoLaunch(replay_settings::kOverlayAppKey, true);
        if (auto_launch_error != vr::VRApplicationError_None)
        {
            if (error != nullptr)
            {
                *error = "Manifest registered, but enabling auto launch failed: " +
                    GetApplicationsErrorName(auto_launch_error);
            }
            return false;
        }
    }

    return true;
}

class GdiCanvas
{
public:
    GdiCanvas(const int width, const int height)
        : width_(width), height_(height), rgba_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 255)
    {
        dc_ = CreateCompatibleDC(nullptr);

        BITMAPINFO bitmap_info{};
        bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
        bitmap_info.bmiHeader.biWidth = width_;
        bitmap_info.bmiHeader.biHeight = -height_;
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;

        bitmap_ = CreateDIBSection(dc_, &bitmap_info, DIB_RGB_COLORS, reinterpret_cast<void**>(&bgra_pixels_), nullptr, 0);
        previous_bitmap_ = SelectObject(dc_, bitmap_);
        SetBkMode(dc_, TRANSPARENT);

        title_font_ = CreateFontW(42, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, kUiFontFace);
        body_font_ = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, kUiFontFace);
        small_font_ = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, kUiFontFace);
    }

    ~GdiCanvas()
    {
        if (title_font_ != nullptr)
        {
            DeleteObject(title_font_);
        }
        if (body_font_ != nullptr)
        {
            DeleteObject(body_font_);
        }
        if (small_font_ != nullptr)
        {
            DeleteObject(small_font_);
        }
        if (dc_ != nullptr)
        {
            SelectObject(dc_, previous_bitmap_);
        }
        if (bitmap_ != nullptr)
        {
            DeleteObject(bitmap_);
        }
        if (dc_ != nullptr)
        {
            DeleteDC(dc_);
        }
    }

    void Clear(const COLORREF color)
    {
        const std::uint8_t red = GetRValue(color);
        const std::uint8_t green = GetGValue(color);
        const std::uint8_t blue = GetBValue(color);
        const std::size_t pixel_count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
        for (std::size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index)
        {
            const std::size_t offset = pixel_index * 4u;
            bgra_pixels_[offset + 0] = blue;
            bgra_pixels_[offset + 1] = green;
            bgra_pixels_[offset + 2] = red;
            bgra_pixels_[offset + 3] = 255;
        }
    }

    void FillRectColor(const RECT& rect, COLORREF color)
    {
        HBRUSH brush = CreateSolidBrush(color);
        ::FillRect(dc_, &rect, brush);
        DeleteObject(brush);
    }

    void FrameRectColor(const RECT& rect, COLORREF color, const int thickness = 1)
    {
        RECT edge = rect;
        edge.bottom = edge.top + thickness;
        FillRectColor(edge, color);

        edge = rect;
        edge.top = edge.bottom - thickness;
        FillRectColor(edge, color);

        edge = rect;
        edge.right = edge.left + thickness;
        FillRectColor(edge, color);

        edge = rect;
        edge.left = edge.right - thickness;
        FillRectColor(edge, color);
    }

    void DrawTitle(const std::wstring& text, const RECT& rect, COLORREF color);
    void DrawBody(const std::wstring& text, const RECT& rect, COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    void DrawSmall(const std::wstring& text, const RECT& rect, COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    void BlitTo(HDC target_dc, const RECT& target_rect) const;
    const void* rgba_data();

    int width() const { return width_; }
    int height() const { return height_; }

private:
    void DrawTextBox(const std::wstring& text, const RECT& rect, HFONT font, COLORREF color, UINT format);

    int width_ = 0;
    int height_ = 0;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_bitmap_ = nullptr;
    std::uint8_t* bgra_pixels_ = nullptr;
    HFONT title_font_ = nullptr;
    HFONT body_font_ = nullptr;
    HFONT small_font_ = nullptr;
    std::vector<std::uint8_t> rgba_;
};

struct OverlayState
{
    std::filesystem::path exe_path;
    std::filesystem::path manifest_path;
    HWND desktop_window = nullptr;
    vr::VROverlayHandle_t main_overlay = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumbnail_overlay = vr::k_ulOverlayHandleInvalid;
    bool quit_requested = false;
    bool dirty = true;
    std::string session_root_utf8;
    std::vector<std::filesystem::path> session_files;
    std::size_t page_index = 0;
    std::string requested_session_utf8;
    std::string loaded_session_utf8;
    std::string status_text = "Idle";
    std::string playback_state = "stopped";
    std::string last_error;
    std::string hotpatch_broker_status = "Broker not connected.";
    std::string hotpatch_dll_status = "Hotpatch not injected.";
    std::string hotpatch_hook_state = "inactive";
    std::int32_t loaded_tracker_count = 0;
    std::uint32_t hotpatch_target_pid = 0;
    std::uint32_t hotpatch_injected_pid = 0;
    std::uint32_t hotpatch_tracked_device_add_calls = 0;
    std::uint32_t hotpatch_pose_updates_seen = 0;
    std::uint32_t hotpatch_pose_updates_suppressed = 0;
    std::uint32_t hotpatch_pose_updates_replaced = 0;
    bool loop_enabled = true;
    std::string live_mode = "suppress";
    std::chrono::steady_clock::time_point last_settings_refresh{};
    KeyboardMode keyboard_mode = KeyboardMode::None;
    std::vector<Button> buttons;
    GdiCanvas main_canvas{kMainWidth, kMainHeight};
    GdiCanvas thumbnail_canvas{kThumbnailWidth, kThumbnailHeight};
};

struct HotpatchSnapshot
{
    bool available = false;
    std::string broker_status = "Broker not connected.";
    std::string dll_status = "Hotpatch not injected.";
    std::string hook_state = "inactive";
    std::uint32_t target_pid = 0;
    std::uint32_t injected_pid = 0;
    std::uint32_t tracked_device_add_calls = 0;
    std::uint32_t pose_updates_seen = 0;
    std::uint32_t pose_updates_suppressed = 0;
    std::uint32_t pose_updates_replaced = 0;
};

const char* HookStateLabel(const hotpatch::HookState state)
{
    switch (state)
    {
    case hotpatch::HookState::Inactive:
        return "inactive";

    case hotpatch::HookState::BrokerReady:
        return "broker_ready";

    case hotpatch::HookState::Injected:
        return "injected";

    case hotpatch::HookState::LighthouseLoaded:
        return "bridge_ready";

    case hotpatch::HookState::HookInstalled:
        return "hook_installed";

    case hotpatch::HookState::HookFailed:
        return "hook_failed";
    }

    return "unknown";
}

std::string NormalizeLiveModeSetting(const std::string& live_mode, const bool suppress_real_trackers_fallback = true)
{
    if (live_mode == "replace" || live_mode == "suppress" || live_mode == "passthrough")
    {
        return live_mode;
    }

    return suppress_real_trackers_fallback ? "suppress" : "passthrough";
}

std::string NextLiveModeSetting(const std::string& current_live_mode)
{
    const std::string normalized = NormalizeLiveModeSetting(current_live_mode);
    if (normalized == "suppress")
    {
        return "replace";
    }

    if (normalized == "replace")
    {
        return "passthrough";
    }

    return "suppress";
}

std::wstring LiveModeLabel(const std::string& live_mode)
{
    const std::string normalized = NormalizeLiveModeSetting(live_mode);
    if (normalized == "replace")
    {
        return L"Replace";
    }

    if (normalized == "passthrough")
    {
        return L"Passthrough";
    }

    return L"Suppress";
}

bool CopyHotpatchSnapshot(const hotpatch::SharedState* shared_state, HotpatchSnapshot* snapshot)
{
    if (shared_state == nullptr || snapshot == nullptr)
    {
        return false;
    }

    const hotpatch::SharedState shared_copy = *shared_state;
    if (shared_copy.magic != hotpatch::kProtocolMagic || shared_copy.version != hotpatch::kProtocolVersion ||
        shared_copy.size != sizeof(hotpatch::SharedState))
    {
        return false;
    }

    snapshot->available = true;
    snapshot->broker_status = WideToUtf8(shared_copy.broker_status_text);
    snapshot->dll_status = WideToUtf8(shared_copy.dll_status_text);
    snapshot->hook_state = HookStateLabel(static_cast<hotpatch::HookState>(shared_copy.hook_state));
    snapshot->target_pid = shared_copy.target_pid;
    snapshot->injected_pid = shared_copy.injected_pid;
    snapshot->tracked_device_add_calls = shared_copy.tracked_device_add_calls;
    snapshot->pose_updates_seen = shared_copy.pose_updates_seen;
    snapshot->pose_updates_suppressed = shared_copy.pose_updates_suppressed;
    snapshot->pose_updates_replaced = shared_copy.pose_updates_replaced;
    return true;
}
}  // namespace

bool InstallApplicationManifest(const std::filesystem::path& manifest_path, const bool enable_auto_launch, std::string* error)
{
    vr::EVRInitError init_error = vr::VRInitError_None;
    vr::IVRSystem* vr_system = vr::VR_Init(&init_error, vr::VRApplication_Utility);
    if (vr_system == nullptr || init_error != vr::VRInitError_None)
    {
        if (error != nullptr)
        {
            *error = "Failed to initialize OpenVR utility runtime: " +
                std::string(vr::VR_GetVRInitErrorAsEnglishDescription(init_error));
        }
        return false;
    }

    auto shutdown_guard = std::unique_ptr<void, void (*)(void*)>(
        reinterpret_cast<void*>(1),
        [](void*) { vr::VR_Shutdown(); });
    (void)vr_system;

    return RegisterManifestWithRuntime(manifest_path, enable_auto_launch, error);
}

struct OverlayApp::Impl
{
    OverlayState state;

    bool Init(std::string* error);
    void Shutdown();
    int Run();

    bool CreateDesktopWindow(std::string* error);
    void DestroyDesktopWindow();
    void PumpWindowMessages();
    void RefreshSettings(bool force_rescan);
    void RescanSessionFiles();
    bool EnsureHotpatchStateMapped();
    void CloseHotpatchStateMapping();
    HotpatchSnapshot ReadHotpatchSnapshot() const;
    void UpdateHotpatchPlaybackHint(const char* playback_state);
    void UpdateHotpatchLiveModeHint(const std::string& live_mode);
    void PumpGlobalEvents();
    void PumpOverlayEvents(vr::VROverlayHandle_t overlay_handle);
    void HandleMouseButtonUp(LONG x, LONG y);
    void HandleDesktopWindowClick(LONG x, LONG y);
    void HandleButtonAction(const Button& button, bool from_desktop);
    void OpenKeyboard(KeyboardMode mode, const std::string& existing_text, const char* description);
    void HandleKeyboardDone(const vr::VREvent_t& event);
    bool PasteClipboardSelection();
    void ApplyPastedPath(const std::filesystem::path& clipboard_path);
    bool PickSessionRootWithDialog();
    bool PickSessionFileWithDialog();
    void RequestPlaybackState(const char* state_name);
    void TriggerLoad(const std::filesystem::path& session_path);
    void Render();
    void RenderMainOverlay();
    void RenderThumbnailOverlay();
    void AddButton(const RECT& rect, ButtonAction action, std::size_t file_index, const std::wstring& label);

    static LRESULT CALLBACK DesktopWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleDesktopWindowMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    HANDLE hotpatch_mapping = nullptr;
    const hotpatch::SharedState* hotpatch_shared_state = nullptr;
};

void GdiCanvas::DrawTextBox(const std::wstring& text, const RECT& rect, HFONT font, COLORREF color, UINT format)
{
    RECT mutable_rect = rect;
    const HGDIOBJ previous_font = SelectObject(dc_, font);
    SetTextColor(dc_, color);
    DrawTextW(dc_, text.c_str(), static_cast<int>(text.size()), &mutable_rect, format | DT_NOPREFIX);
    SelectObject(dc_, previous_font);
}

void GdiCanvas::DrawTitle(const std::wstring& text, const RECT& rect, COLORREF color)
{
    DrawTextBox(text, rect, title_font_, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void GdiCanvas::DrawBody(const std::wstring& text, const RECT& rect, COLORREF color, UINT format)
{
    DrawTextBox(text, rect, body_font_, color, format);
}

void GdiCanvas::DrawSmall(const std::wstring& text, const RECT& rect, COLORREF color, UINT format)
{
    DrawTextBox(text, rect, small_font_, color, format);
}

void GdiCanvas::BlitTo(HDC target_dc, const RECT& target_rect) const
{
    SetStretchBltMode(target_dc, HALFTONE);
    StretchBlt(
        target_dc,
        target_rect.left,
        target_rect.top,
        target_rect.right - target_rect.left,
        target_rect.bottom - target_rect.top,
        dc_,
        0,
        0,
        width_,
        height_,
        SRCCOPY);
}

const void* GdiCanvas::rgba_data()
{
    const std::size_t pixel_count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    for (std::size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index)
    {
        const std::size_t offset = pixel_index * 4u;
        rgba_[offset + 0] = bgra_pixels_[offset + 2];
        rgba_[offset + 1] = bgra_pixels_[offset + 1];
        rgba_[offset + 2] = bgra_pixels_[offset + 0];
        rgba_[offset + 3] = 255;
    }
    return rgba_.data();
}

bool OverlayApp::Impl::CreateDesktopWindow(std::string* error)
{
    static bool class_registered = false;
    if (!class_registered)
    {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &OverlayApp::Impl::DesktopWindowProc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.hbrBackground = nullptr;
        window_class.lpszClassName = kDesktopWindowClassName;
        if (RegisterClassExW(&window_class) == 0)
        {
            if (error != nullptr)
            {
                *error = "Failed to register desktop mirror window class.";
            }
            return false;
        }
        class_registered = true;
    }

    RECT window_rect{0, 0, kDesktopWindowWidth, kDesktopWindowHeight};
    AdjustWindowRectEx(&window_rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

    state.desktop_window = CreateWindowExW(
        WS_EX_APPWINDOW,
        kDesktopWindowClassName,
        L"SteamVR Capture Replay",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (state.desktop_window == nullptr)
    {
        if (error != nullptr)
        {
            *error = "Failed to create desktop mirror window.";
        }
        return false;
    }

    DragAcceptFiles(state.desktop_window, TRUE);
    ShowWindow(state.desktop_window, SW_SHOW);
    UpdateWindow(state.desktop_window);
    return true;
}

void OverlayApp::Impl::DestroyDesktopWindow()
{
    if (state.desktop_window != nullptr)
    {
        DragAcceptFiles(state.desktop_window, FALSE);
        DestroyWindow(state.desktop_window);
        state.desktop_window = nullptr;
    }
}

bool OverlayApp::Impl::EnsureHotpatchStateMapped()
{
    if (hotpatch_shared_state != nullptr)
    {
        return true;
    }

    if (hotpatch_mapping == nullptr)
    {
        hotpatch_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, hotpatch::kSharedStateMappingName);
        if (hotpatch_mapping == nullptr)
        {
            return false;
        }
    }

    hotpatch_shared_state = static_cast<const hotpatch::SharedState*>(
        MapViewOfFile(hotpatch_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hotpatch::SharedState)));
    if (hotpatch_shared_state == nullptr)
    {
        CloseHandle(hotpatch_mapping);
        hotpatch_mapping = nullptr;
        return false;
    }

    return true;
}

void OverlayApp::Impl::CloseHotpatchStateMapping()
{
    if (hotpatch_shared_state != nullptr)
    {
        UnmapViewOfFile(hotpatch_shared_state);
        hotpatch_shared_state = nullptr;
    }

    if (hotpatch_mapping != nullptr)
    {
        CloseHandle(hotpatch_mapping);
        hotpatch_mapping = nullptr;
    }
}

HotpatchSnapshot OverlayApp::Impl::ReadHotpatchSnapshot() const
{
    HotpatchSnapshot snapshot;
    if (!CopyHotpatchSnapshot(hotpatch_shared_state, &snapshot))
    {
        return HotpatchSnapshot{};
    }
    return snapshot;
}

void OverlayApp::Impl::UpdateHotpatchPlaybackHint(const char* playback_state)
{
    if (playback_state == nullptr)
    {
        return;
    }

    if (!EnsureHotpatchStateMapped())
    {
        return;
    }

    auto* shared_state = const_cast<hotpatch::SharedState*>(hotpatch_shared_state);
    if (shared_state == nullptr)
    {
        return;
    }

    const bool has_session = !state.loaded_session_utf8.empty() || !state.requested_session_utf8.empty();
    const bool playback_active = has_session && ::strcmp(playback_state, "stopped") != 0;
    shared_state->playback_active = playback_active ? 1u : 0u;
}

void OverlayApp::Impl::UpdateHotpatchLiveModeHint(const std::string& live_mode)
{
    if (!EnsureHotpatchStateMapped())
    {
        return;
    }

    auto* shared_state = const_cast<hotpatch::SharedState*>(hotpatch_shared_state);
    if (shared_state == nullptr)
    {
        return;
    }

    const std::string normalized = NormalizeLiveModeSetting(live_mode);
    if (normalized == "replace")
    {
        shared_state->live_mode = static_cast<std::uint32_t>(hotpatch::LiveMode::Replace);
        shared_state->suppress_real_trackers = 1u;
        return;
    }

    if (normalized == "passthrough")
    {
        shared_state->live_mode = static_cast<std::uint32_t>(hotpatch::LiveMode::Passthrough);
        shared_state->suppress_real_trackers = 0u;
        return;
    }

    shared_state->live_mode = static_cast<std::uint32_t>(hotpatch::LiveMode::Suppress);
    shared_state->suppress_real_trackers = 1u;
}

void OverlayApp::Impl::PumpWindowMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            state.quit_requested = true;
            continue;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool OverlayApp::Impl::Init(std::string* error)
{
    state.exe_path = GetExecutablePath();
    state.manifest_path = state.exe_path.parent_path() / replay_settings::kOverlayManifestFilename;

    vr::EVRInitError init_error = vr::VRInitError_None;
    vr::IVRSystem* vr_system = vr::VR_Init(&init_error, vr::VRApplication_Overlay);
    if (vr_system == nullptr || init_error != vr::VRInitError_None)
    {
        if (error != nullptr)
        {
            *error = "Failed to initialize OpenVR overlay runtime: " +
                std::string(vr::VR_GetVRInitErrorAsEnglishDescription(init_error));
        }
        return false;
    }
    (void)vr_system;

    if (vr::VROverlay() == nullptr || vr::VRSettings() == nullptr)
    {
        if (error != nullptr)
        {
            *error = "OpenVR overlay interfaces are unavailable.";
        }
        return false;
    }

    const vr::EVROverlayError overlay_error = vr::VROverlay()->CreateDashboardOverlay(
        replay_settings::kOverlayAppKey,
        "SteamVR Capture Replay",
        &state.main_overlay,
        &state.thumbnail_overlay);
    if (overlay_error != vr::VROverlayError_None)
    {
        if (error != nullptr)
        {
            *error = "Failed to create dashboard overlay: " + GetOverlayErrorName(overlay_error);
        }
        return false;
    }

    vr::VROverlay()->SetOverlayWidthInMeters(state.main_overlay, kOverlayWidthMeters);
    vr::VROverlay()->SetOverlayWidthInMeters(state.thumbnail_overlay, kThumbnailWidthMeters);
    vr::VROverlay()->SetOverlayInputMethod(state.main_overlay, vr::VROverlayInputMethod_Mouse);

    vr::HmdVector2_t mouse_scale{};
    mouse_scale.v[0] = static_cast<float>(kMainWidth);
    mouse_scale.v[1] = static_cast<float>(kMainHeight);
    vr::VROverlay()->SetOverlayMouseScale(state.main_overlay, &mouse_scale);

    vr::VROverlay()->SetOverlayFlag(state.main_overlay, vr::VROverlayFlags_VisibleInDashboard, true);
    vr::VROverlay()->SetOverlayFlag(state.main_overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
    vr::VROverlay()->SetOverlayFlag(state.main_overlay, vr::VROverlayFlags_EnableControlBarKeyboard, true);
    vr::VROverlay()->SetOverlayFlag(state.main_overlay, vr::VROverlayFlags_EnableControlBarClose, true);
    vr::VROverlay()->SetOverlayFlag(state.main_overlay, vr::VROverlayFlags_MinimalControlBar, true);

    if (!CreateDesktopWindow(error))
    {
        return false;
    }

    std::string broker_error;
    if (!EnsureBrokerRunning(state.exe_path.parent_path(), &broker_error))
    {
        state.last_error = broker_error;
    }

    RefreshSettings(true);
    Render();
    return true;
}

void OverlayApp::Impl::Shutdown()
{
    CloseHotpatchStateMapping();
    DestroyDesktopWindow();

    if (vr::VROverlay() != nullptr)
    {
        if (state.main_overlay != vr::k_ulOverlayHandleInvalid)
        {
            vr::VROverlay()->DestroyOverlay(state.main_overlay);
            state.main_overlay = vr::k_ulOverlayHandleInvalid;
        }
        if (state.thumbnail_overlay != vr::k_ulOverlayHandleInvalid)
        {
            vr::VROverlay()->DestroyOverlay(state.thumbnail_overlay);
            state.thumbnail_overlay = vr::k_ulOverlayHandleInvalid;
        }
    }

    vr::VR_Shutdown();
}

int OverlayApp::Impl::Run()
{
    state.last_settings_refresh = std::chrono::steady_clock::now();

    while (!state.quit_requested)
    {
        PumpWindowMessages();
        PumpGlobalEvents();
        PumpOverlayEvents(state.main_overlay);
        PumpOverlayEvents(state.thumbnail_overlay);

        const auto now = std::chrono::steady_clock::now();
        if ((now - state.last_settings_refresh) >= kSettingsRefreshInterval)
        {
            RefreshSettings(false);
            state.last_settings_refresh = now;
        }

        if (state.dirty)
        {
            Render();
        }

        std::this_thread::sleep_for(kUiTickInterval);
    }

    return 0;
}

void OverlayApp::Impl::RefreshSettings(const bool force_rescan)
{
    bool dirty = force_rescan;

    std::string session_root_utf8 = NormalizeUtf8PathString(
        ReadSettingsString(replay_settings::kOverlaySection, replay_settings::kSessionRootKey));
    if (session_root_utf8.empty())
    {
        const std::filesystem::path default_root = FindDefaultSessionRoot(state.exe_path.parent_path());
        if (!default_root.empty())
        {
            session_root_utf8 = PathToUtf8(default_root);
            WriteSettingsString(replay_settings::kOverlaySection, replay_settings::kSessionRootKey, session_root_utf8);
        }
    }

    if (session_root_utf8 != state.session_root_utf8)
    {
        state.session_root_utf8 = session_root_utf8;
        dirty = true;
    }

    const std::string requested_session_utf8 = NormalizeUtf8PathString(
        ReadSettingsString(replay_settings::kDriverSection, replay_settings::kSessionPathKey));
    if (requested_session_utf8 != state.requested_session_utf8)
    {
        state.requested_session_utf8 = requested_session_utf8;
        dirty = true;
    }

    const std::string loaded_session_utf8 = NormalizeUtf8PathString(
        ReadSettingsString(replay_settings::kDriverSection, replay_settings::kLoadedSessionPathKey));
    if (loaded_session_utf8 != state.loaded_session_utf8)
    {
        state.loaded_session_utf8 = loaded_session_utf8;
        dirty = true;
    }

    const std::string status_text =
        ReadSettingsString(replay_settings::kDriverSection, replay_settings::kStatusTextKey);
    if (status_text != state.status_text)
    {
        state.status_text = status_text.empty() ? "Idle" : status_text;
        dirty = true;
    }

    const std::string playback_state =
        ReadSettingsString(replay_settings::kDriverSection, replay_settings::kPlaybackStateKey);
    if (playback_state != state.playback_state)
    {
        state.playback_state = playback_state.empty() ? "stopped" : playback_state;
        dirty = true;
    }

    const std::string last_error =
        ReadSettingsString(replay_settings::kDriverSection, replay_settings::kLastErrorKey);
    if (last_error != state.last_error)
    {
        state.last_error = last_error;
        dirty = true;
    }

    const std::int32_t loaded_tracker_count = ReadSettingsInt(
        replay_settings::kDriverSection, replay_settings::kLoadedTrackerCountKey, 0);
    if (loaded_tracker_count != state.loaded_tracker_count)
    {
        state.loaded_tracker_count = loaded_tracker_count;
        dirty = true;
    }

    const bool loop_enabled =
        ReadSettingsBool(replay_settings::kDriverSection, replay_settings::kLoopKey, true);
    if (loop_enabled != state.loop_enabled)
    {
        state.loop_enabled = loop_enabled;
        dirty = true;
    }

    const bool suppress_real_trackers = ReadSettingsBool(
        replay_settings::kHotpatchSection,
        replay_settings::kSuppressRealTrackersKey,
        true);
    const std::string live_mode = NormalizeLiveModeSetting(
        ReadSettingsString(replay_settings::kHotpatchSection, replay_settings::kLiveModeKey),
        suppress_real_trackers);
    if (live_mode != state.live_mode)
    {
        state.live_mode = live_mode;
        dirty = true;
    }

    if (!EnsureHotpatchStateMapped())
    {
        CloseHotpatchStateMapping();
    }
    const HotpatchSnapshot hotpatch_snapshot = ReadHotpatchSnapshot();
    const std::string hotpatch_broker_status = hotpatch_snapshot.available &&
            !hotpatch_snapshot.broker_status.empty()
        ? hotpatch_snapshot.broker_status
        : "Broker not connected.";
    if (hotpatch_broker_status != state.hotpatch_broker_status)
    {
        state.hotpatch_broker_status = hotpatch_broker_status;
        dirty = true;
    }

    const std::string hotpatch_dll_status = hotpatch_snapshot.available && !hotpatch_snapshot.dll_status.empty()
        ? hotpatch_snapshot.dll_status
        : "Hotpatch not injected.";
    if (hotpatch_dll_status != state.hotpatch_dll_status)
    {
        state.hotpatch_dll_status = hotpatch_dll_status;
        dirty = true;
    }

    if (hotpatch_snapshot.hook_state != state.hotpatch_hook_state)
    {
        state.hotpatch_hook_state = hotpatch_snapshot.hook_state;
        dirty = true;
    }

    if (hotpatch_snapshot.target_pid != state.hotpatch_target_pid)
    {
        state.hotpatch_target_pid = hotpatch_snapshot.target_pid;
        dirty = true;
    }

    if (hotpatch_snapshot.injected_pid != state.hotpatch_injected_pid)
    {
        state.hotpatch_injected_pid = hotpatch_snapshot.injected_pid;
        dirty = true;
    }

    if (hotpatch_snapshot.tracked_device_add_calls != state.hotpatch_tracked_device_add_calls)
    {
        state.hotpatch_tracked_device_add_calls = hotpatch_snapshot.tracked_device_add_calls;
        dirty = true;
    }

    if (hotpatch_snapshot.pose_updates_seen != state.hotpatch_pose_updates_seen)
    {
        state.hotpatch_pose_updates_seen = hotpatch_snapshot.pose_updates_seen;
        dirty = true;
    }

    if (hotpatch_snapshot.pose_updates_suppressed != state.hotpatch_pose_updates_suppressed)
    {
        state.hotpatch_pose_updates_suppressed = hotpatch_snapshot.pose_updates_suppressed;
        dirty = true;
    }

    if (hotpatch_snapshot.pose_updates_replaced != state.hotpatch_pose_updates_replaced)
    {
        state.hotpatch_pose_updates_replaced = hotpatch_snapshot.pose_updates_replaced;
        dirty = true;
    }

    if (force_rescan || dirty)
    {
        RescanSessionFiles();
    }

    state.dirty = state.dirty || dirty;
}

void OverlayApp::Impl::RescanSessionFiles()
{
    const std::size_t previous_count = state.session_files.size();
    state.session_files.clear();

    const std::filesystem::path session_root = Utf8Path(state.session_root_utf8);
    std::error_code error;
    if (!session_root.empty() && std::filesystem::exists(session_root, error) && std::filesystem::is_directory(session_root, error))
    {
        std::filesystem::recursive_directory_iterator iterator(
            session_root,
            std::filesystem::directory_options::skip_permission_denied,
            error);
        const std::filesystem::recursive_directory_iterator end;

        while (!error && iterator != end)
        {
            if (iterator->is_regular_file(error))
            {
                std::string extension = LowerAscii(PathToUtf8(iterator->path().extension()));
                if (extension == ".svrcap")
                {
                    state.session_files.push_back(ToAbsolutePath(iterator->path()));
                }
            }

            error.clear();
            iterator.increment(error);
            if (error)
            {
                error.clear();
            }
        }
    }

    std::sort(state.session_files.begin(), state.session_files.end());

    const std::size_t page_count = std::max<std::size_t>(1, (state.session_files.size() + kPageSize - 1) / kPageSize);
    if (state.page_index >= page_count)
    {
        state.page_index = page_count - 1;
    }

    if (state.session_files.size() != previous_count)
    {
        state.dirty = true;
    }
}

void OverlayApp::Impl::PumpGlobalEvents()
{
    if (vr::VRSystem() == nullptr)
    {
        return;
    }

    vr::VREvent_t event{};
    while (vr::VRSystem()->PollNextEvent(&event, sizeof(event)))
    {
        switch (event.eventType)
        {
        case vr::VREvent_Quit:
        case vr::VREvent_ProcessQuit:
            vr::VRSystem()->AcknowledgeQuit_Exiting();
            state.quit_requested = true;
            break;

        default:
            break;
        }
    }
}

void OverlayApp::Impl::PumpOverlayEvents(const vr::VROverlayHandle_t overlay_handle)
{
    if (overlay_handle == vr::k_ulOverlayHandleInvalid || vr::VROverlay() == nullptr)
    {
        return;
    }

    vr::VREvent_t event{};
    while (vr::VROverlay()->PollNextOverlayEvent(overlay_handle, &event, sizeof(event)))
    {
        switch (event.eventType)
        {
        case vr::VREvent_MouseButtonUp:
            if (overlay_handle == state.main_overlay && event.data.mouse.button == vr::VRMouseButton_Left)
            {
                const LONG x = static_cast<LONG>(std::lround(event.data.mouse.x));
                const LONG y = static_cast<LONG>(kMainHeight - 1 - std::lround(event.data.mouse.y));
                HandleMouseButtonUp(x, y);
            }
            break;

        case vr::VREvent_KeyboardDone:
            if (event.data.keyboard.overlayHandle == overlay_handle)
            {
                HandleKeyboardDone(event);
            }
            break;

        case vr::VREvent_KeyboardClosed:
            if (event.data.keyboard.overlayHandle == overlay_handle)
            {
                state.keyboard_mode = KeyboardMode::None;
                state.dirty = true;
            }
            break;

        case vr::VREvent_OverlayShown:
        case vr::VREvent_DashboardActivated:
            state.dirty = true;
            break;

        case vr::VREvent_OverlayClosed:
            state.dirty = true;
            break;

        default:
            break;
        }
    }
}

void OverlayApp::Impl::HandleMouseButtonUp(const LONG x, const LONG y)
{
    for (const Button& button : state.buttons)
    {
        if (!ContainsPoint(button.rect, x, y))
        {
            continue;
        }
        HandleButtonAction(button, false);
        return;
    }
}

void OverlayApp::Impl::HandleDesktopWindowClick(const LONG x, const LONG y)
{
    if (state.desktop_window == nullptr)
    {
        return;
    }

    RECT client_rect{};
    GetClientRect(state.desktop_window, &client_rect);
    const LONG client_width = std::max<LONG>(1, client_rect.right - client_rect.left);
    const LONG client_height = std::max<LONG>(1, client_rect.bottom - client_rect.top);

    const LONG scaled_x = std::clamp<LONG>((x * kMainWidth) / client_width, 0, kMainWidth - 1);
    const LONG scaled_y = std::clamp<LONG>((y * kMainHeight) / client_height, 0, kMainHeight - 1);
    for (const Button& button : state.buttons)
    {
        if (!ContainsPoint(button.rect, scaled_x, scaled_y))
        {
            continue;
        }
        HandleButtonAction(button, true);
        return;
    }
}

void OverlayApp::Impl::HandleButtonAction(const Button& button, const bool from_desktop)
{
    switch (button.action)
    {
    case ButtonAction::Refresh:
        RefreshSettings(true);
        return;

    case ButtonAction::PrevPage:
        if (state.page_index > 0)
        {
            --state.page_index;
            state.dirty = true;
        }
        return;

    case ButtonAction::NextPage:
    {
        const std::size_t page_count = std::max<std::size_t>(1, (state.session_files.size() + kPageSize - 1) / kPageSize);
        if (state.page_index + 1 < page_count)
        {
            ++state.page_index;
            state.dirty = true;
        }
        return;
    }

    case ButtonAction::Play:
        RequestPlaybackState("playing");
        return;

    case ButtonAction::Pause:
        RequestPlaybackState("paused");
        return;

    case ButtonAction::Stop:
        RequestPlaybackState("stopped");
        return;

    case ButtonAction::ToggleLoop:
        state.loop_enabled = !state.loop_enabled;
        WriteSettingsBool(replay_settings::kDriverSection, replay_settings::kLoopKey, state.loop_enabled);
        state.dirty = true;
        return;

    case ButtonAction::CycleLiveMode:
        state.live_mode = NextLiveModeSetting(state.live_mode);
        WriteSettingsString(
            replay_settings::kHotpatchSection,
            replay_settings::kLiveModeKey,
            state.live_mode);
        WriteSettingsBool(
            replay_settings::kHotpatchSection,
            replay_settings::kSuppressRealTrackersKey,
            state.live_mode != "passthrough");
        UpdateHotpatchLiveModeHint(state.live_mode);
        state.status_text = "Requested hotpatch live mode: " + state.live_mode;
        state.last_error.clear();
        state.dirty = true;
        return;

    case ButtonAction::EditRoot:
        if (from_desktop)
        {
            PickSessionRootWithDialog();
        }
        else
        {
            OpenKeyboard(KeyboardMode::SessionRoot, state.session_root_utf8, "Session directory");
        }
        return;

    case ButtonAction::EditPath:
        if (from_desktop)
        {
            PickSessionFileWithDialog();
        }
        else
        {
            OpenKeyboard(KeyboardMode::DirectPath, state.requested_session_utf8, "Session file path");
        }
        return;

    case ButtonAction::PasteClipboard:
        PasteClipboardSelection();
        return;

    case ButtonAction::ReloadCurrent:
        if (!state.requested_session_utf8.empty())
        {
            TriggerLoad(Utf8Path(state.requested_session_utf8));
        }
        else if (!state.loaded_session_utf8.empty())
        {
            TriggerLoad(Utf8Path(state.loaded_session_utf8));
        }
        return;

    case ButtonAction::LoadFile:
        if (button.file_index < state.session_files.size())
        {
            TriggerLoad(state.session_files[button.file_index]);
        }
        return;

    case ButtonAction::None:
    default:
        return;
    }
}

void OverlayApp::Impl::OpenKeyboard(
    const KeyboardMode mode,
    const std::string& existing_text,
    const char* description)
{
    if (vr::VROverlay() == nullptr || state.main_overlay == vr::k_ulOverlayHandleInvalid)
    {
        return;
    }

    const vr::EVROverlayError error = vr::VROverlay()->ShowKeyboardForOverlay(
        state.main_overlay,
        vr::k_EGamepadTextInputModeNormal,
        vr::k_EGamepadTextInputLineModeSingleLine,
        vr::KeyboardFlag_Modal,
        description,
        4096,
        existing_text.c_str(),
        static_cast<std::uint64_t>(mode));

    if (error != vr::VROverlayError_None)
    {
        state.last_error = "Keyboard open failed: " + GetOverlayErrorName(error);
        state.dirty = true;
        return;
    }

    state.keyboard_mode = mode;
}

void OverlayApp::Impl::HandleKeyboardDone(const vr::VREvent_t& event)
{
    std::vector<char> text_buffer(4096, '\0');
    std::uint32_t required = vr::VROverlay()->GetKeyboardText(text_buffer.data(), static_cast<std::uint32_t>(text_buffer.size()));
    if (required > text_buffer.size())
    {
        text_buffer.assign(required, '\0');
        required = vr::VROverlay()->GetKeyboardText(text_buffer.data(), static_cast<std::uint32_t>(text_buffer.size()));
    }

    const std::string input_text = text_buffer.empty() ? std::string() : std::string(text_buffer.data());
    const KeyboardMode mode = event.data.keyboard.uUserValue == 0
        ? state.keyboard_mode
        : static_cast<KeyboardMode>(event.data.keyboard.uUserValue);

    state.keyboard_mode = KeyboardMode::None;

    switch (mode)
    {
    case KeyboardMode::SessionRoot:
    {
        const std::string normalized_root = NormalizeUtf8PathString(input_text);
        WriteSettingsString(replay_settings::kOverlaySection, replay_settings::kSessionRootKey, normalized_root);
        RefreshSettings(true);
        break;
    }

    case KeyboardMode::DirectPath:
        if (input_text.empty())
        {
            TriggerLoad({});
        }
        else
        {
            TriggerLoad(Utf8Path(input_text));
        }
        break;

    case KeyboardMode::None:
    default:
        break;
    }

    state.dirty = true;
}

bool OverlayApp::Impl::PasteClipboardSelection()
{
    if (!OpenClipboard(state.desktop_window))
    {
        state.last_error = "Failed to open the Windows clipboard.";
        state.dirty = true;
        return false;
    }

    auto close_clipboard = std::unique_ptr<void, void (*)(void*)>(
        reinterpret_cast<void*>(1),
        [](void*) { CloseClipboard(); });

    if (IsClipboardFormatAvailable(CF_HDROP))
    {
        const HDROP drop_handle = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        if (drop_handle != nullptr)
        {
            const UINT path_length = DragQueryFileW(drop_handle, 0, nullptr, 0);
            if (path_length > 0)
            {
                std::wstring path_text(static_cast<std::size_t>(path_length) + 1u, L'\0');
                DragQueryFileW(drop_handle, 0, path_text.data(), path_length + 1);
                path_text.resize(path_length);
                ApplyPastedPath(std::filesystem::path(path_text));
                return true;
            }
        }
    }

    if (IsClipboardFormatAvailable(CF_UNICODETEXT))
    {
        const HANDLE text_handle = GetClipboardData(CF_UNICODETEXT);
        if (text_handle != nullptr)
        {
            const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(text_handle));
            if (text != nullptr)
            {
                std::wstring clipboard_text(text);
                GlobalUnlock(text_handle);

                clipboard_text = TrimClipboardPathText(std::move(clipboard_text));
                if (!clipboard_text.empty())
                {
                    ApplyPastedPath(std::filesystem::path(clipboard_text));
                    return true;
                }
            }
        }
    }

    state.last_error = "Clipboard does not currently contain a usable file or folder path.";
    state.dirty = true;
    return false;
}

bool OverlayApp::Impl::PickSessionRootWithDialog()
{
    BROWSEINFOW browse_info{};
    browse_info.hwndOwner = state.desktop_window;
    browse_info.lpszTitle = L"Select a session root folder";
    browse_info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI | BIF_NONEWFOLDERBUTTON;

    PIDLIST_ABSOLUTE selection = SHBrowseForFolderW(&browse_info);
    if (selection == nullptr)
    {
        return false;
    }

    std::wstring path_buffer(MAX_PATH, L'\0');
    const BOOL ok = SHGetPathFromIDListW(selection, path_buffer.data());
    CoTaskMemFree(selection);

    if (!ok)
    {
        state.last_error = "Failed to resolve the selected folder path.";
        state.dirty = true;
        return false;
    }

    path_buffer.resize(wcslen(path_buffer.c_str()));
    const std::filesystem::path selected_path(path_buffer);
    WriteSettingsString(replay_settings::kOverlaySection, replay_settings::kSessionRootKey, PathToUtf8(selected_path));
    state.last_error.clear();
    RefreshSettings(true);
    return true;
}

bool OverlayApp::Impl::PickSessionFileWithDialog()
{
    std::array<wchar_t, 32768> filename{};
    std::wstring initial_directory;

    const std::filesystem::path requested_path = Utf8Path(state.requested_session_utf8);
    if (!requested_path.empty() && requested_path.has_parent_path())
    {
        initial_directory = requested_path.parent_path().wstring();
    }
    else if (!state.session_root_utf8.empty())
    {
        initial_directory = Utf8Path(state.session_root_utf8).wstring();
    }

    OPENFILENAMEW open_file{};
    open_file.lStructSize = sizeof(open_file);
    open_file.hwndOwner = state.desktop_window;
    open_file.lpstrFilter = L"SteamVR Capture Sessions (*.svrcap)\0*.svrcap\0All Files (*.*)\0*.*\0\0";
    open_file.lpstrFile = filename.data();
    open_file.nMaxFile = static_cast<DWORD>(filename.size());
    open_file.lpstrInitialDir = initial_directory.empty() ? nullptr : initial_directory.c_str();
    open_file.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    open_file.lpstrTitle = L"Select a replay session";

    if (!GetOpenFileNameW(&open_file))
    {
        return false;
    }

    ApplyPastedPath(std::filesystem::path(filename.data()));
    return true;
}

void OverlayApp::Impl::RequestPlaybackState(const char* state_name)
{
    WriteSettingsString(replay_settings::kDriverSection, replay_settings::kPlaybackStateKey, state_name);
    UpdateHotpatchPlaybackHint(state_name);
    state.playback_state = state_name;
    state.last_error.clear();
    state.dirty = true;
}

void OverlayApp::Impl::ApplyPastedPath(const std::filesystem::path& clipboard_path)
{
    if (clipboard_path.empty())
    {
        state.last_error = "Clipboard path was empty.";
        state.dirty = true;
        return;
    }

    const std::filesystem::path absolute_path = ToAbsolutePath(clipboard_path);
    std::error_code error;
    if (std::filesystem::exists(absolute_path, error) && std::filesystem::is_directory(absolute_path, error))
    {
        const std::string root_utf8 = PathToUtf8(absolute_path);
        WriteSettingsString(replay_settings::kOverlaySection, replay_settings::kSessionRootKey, root_utf8);
        state.last_error.clear();
        RefreshSettings(true);
        return;
    }

    const std::string extension = LowerAscii(PathToUtf8(absolute_path.extension()));
    if (extension != ".svrcap")
    {
        state.last_error = "Clipboard path is not a .svrcap file or a session directory.";
        state.dirty = true;
        return;
    }

    const std::filesystem::path parent_path = absolute_path.parent_path();
    if (!parent_path.empty() && std::filesystem::exists(parent_path, error) && std::filesystem::is_directory(parent_path, error))
    {
        WriteSettingsString(replay_settings::kOverlaySection, replay_settings::kSessionRootKey, PathToUtf8(parent_path));
        RefreshSettings(true);
    }

    state.last_error.clear();
    TriggerLoad(absolute_path);
}

void OverlayApp::Impl::TriggerLoad(const std::filesystem::path& session_path)
{
    const std::filesystem::path absolute_path = ToAbsolutePath(session_path);
    const std::string normalized_path = absolute_path.empty() ? std::string() : PathToUtf8(absolute_path);

    WriteSettingsBool(replay_settings::kDriverSection, replay_settings::kEnableKey, true);
    WriteSettingsString(replay_settings::kDriverSection, replay_settings::kPlaybackStateKey, "stopped");
    UpdateHotpatchPlaybackHint("stopped");
    WriteSettingsString(replay_settings::kDriverSection, replay_settings::kSessionPathKey, normalized_path);

    const std::int32_t next_generation = ReadSettingsInt(
        replay_settings::kDriverSection, replay_settings::kSessionGenerationKey, 0) + 1;
    WriteSettingsInt(replay_settings::kDriverSection, replay_settings::kSessionGenerationKey, next_generation);

    state.requested_session_utf8 = normalized_path;
    state.playback_state = "stopped";
    state.status_text = normalized_path.empty() ? "Cleared session request" : "Requested session load";
    state.last_error.clear();
    state.dirty = true;
}

void OverlayApp::Impl::Render()
{
    RenderMainOverlay();
    RenderThumbnailOverlay();
    if (state.desktop_window != nullptr)
    {
        InvalidateRect(state.desktop_window, nullptr, FALSE);
    }
    state.dirty = false;
}

void OverlayApp::Impl::RenderMainOverlay()
{
    state.buttons.clear();
    state.main_canvas.Clear(RGB(20, 25, 34));

    const std::wstring requested_key = NormalizePathKey(Utf8Path(state.requested_session_utf8));
    const std::wstring loaded_key = NormalizePathKey(Utf8Path(state.loaded_session_utf8));

    state.main_canvas.DrawTitle(L"SteamVR Capture Replay", RECT{40, 18, 900, 72}, RGB(240, 244, 248));
    state.main_canvas.DrawSmall(
        L"Pick a recorded .svrcap session in VR and stream it into the replay driver without editing SteamVR settings by hand.",
        RECT{42, 76, 1540, 108},
        RGB(170, 182, 198),
        DT_LEFT | DT_TOP | DT_WORDBREAK);

    AddButton(RECT{40, 124, 210, 176}, ButtonAction::Refresh, 0, L"Refresh");
    AddButton(RECT{230, 124, 350, 176}, ButtonAction::PrevPage, 0, L"Prev");
    AddButton(RECT{370, 124, 490, 176}, ButtonAction::NextPage, 0, L"Next");
    AddButton(RECT{510, 124, 750, 176}, ButtonAction::EditRoot, 0, L"Set Session Root");
    AddButton(RECT{770, 124, 980, 176}, ButtonAction::EditPath, 0, L"Direct Path");
    AddButton(RECT{1000, 124, 1220, 176}, ButtonAction::PasteClipboard, 0, L"Paste Clipboard");
    AddButton(RECT{1240, 124, 1510, 176}, ButtonAction::ReloadCurrent, 0, L"Reload Current");

    AddButton(RECT{40, 192, 220, 244}, ButtonAction::Play, 0, L"Play");
    AddButton(RECT{240, 192, 420, 244}, ButtonAction::Pause, 0, L"Pause");
    AddButton(RECT{440, 192, 620, 244}, ButtonAction::Stop, 0, L"Stop");
    AddButton(
        RECT{640, 192, 900, 244},
        ButtonAction::ToggleLoop,
        0,
        state.loop_enabled ? L"Loop: On" : L"Loop: Off");
    AddButton(
        RECT{920, 192, 1280, 244},
        ButtonAction::CycleLiveMode,
        0,
        L"Live Mode: " + LiveModeLabel(state.live_mode));

    const RECT left_panel{40, 276, 1010, 490};
    const RECT right_panel{1040, 276, 1560, 490};
    state.main_canvas.FillRectColor(left_panel, RGB(31, 39, 52));
    state.main_canvas.FillRectColor(right_panel, RGB(31, 39, 52));
    state.main_canvas.FrameRectColor(left_panel, RGB(73, 88, 109), 2);
    state.main_canvas.FrameRectColor(right_panel, RGB(73, 88, 109), 2);

    state.main_canvas.DrawBody(L"Session Root", RECT{60, 288, 260, 318}, RGB(239, 244, 248));
    state.main_canvas.DrawSmall(
        TruncateMiddle(Utf8ToWide(state.session_root_utf8.empty() ? "<not set>" : state.session_root_utf8), 90),
        RECT{60, 316, 990, 344},
        RGB(172, 187, 205));

    state.main_canvas.DrawBody(L"Requested Session", RECT{60, 352, 280, 380}, RGB(239, 244, 248));
    state.main_canvas.DrawSmall(
        TruncateMiddle(Utf8ToWide(state.requested_session_utf8.empty() ? "<none>" : state.requested_session_utf8), 90),
        RECT{60, 380, 990, 408},
        RGB(172, 187, 205));

    state.main_canvas.DrawBody(L"Loaded Session", RECT{60, 416, 260, 444}, RGB(239, 244, 248));
    state.main_canvas.DrawSmall(
        TruncateMiddle(Utf8ToWide(state.loaded_session_utf8.empty() ? "<none>" : state.loaded_session_utf8), 90),
        RECT{60, 444, 990, 460},
        RGB(172, 187, 205));

    state.main_canvas.DrawBody(L"Live Runtime", RECT{1060, 288, 1260, 316}, RGB(239, 244, 248));

    COLORREF hook_color = RGB(172, 187, 205);
    if (state.hotpatch_hook_state == "hook_installed")
    {
        hook_color = RGB(109, 204, 163);
    }
    else if (state.hotpatch_hook_state == "hook_failed")
    {
        hook_color = RGB(236, 107, 102);
    }
    else if (state.hotpatch_hook_state != "inactive")
    {
        hook_color = RGB(218, 186, 97);
    }

    int runtime_line_top = 320;
    const auto draw_runtime_line = [&](const std::wstring& text, const COLORREF color)
    {
        state.main_canvas.DrawSmall(
            TruncateMiddle(text, 78),
            RECT{1060, runtime_line_top, 1540, runtime_line_top + 22},
            color);
        runtime_line_top += 22;
    };

    draw_runtime_line(L"Driver: " + Utf8ToWide(state.status_text), RGB(109, 204, 163));
    draw_runtime_line(
        L"Playback: " + Utf8ToWide(state.playback_state) + L" / " + (state.loop_enabled ? L"loop" : L"once") +
            L" / trackers " + Utf8ToWide(std::to_string(state.loaded_tracker_count)),
        RGB(218, 186, 97));
    draw_runtime_line(L"Mode: " + LiveModeLabel(state.live_mode), RGB(133, 187, 237));
    draw_runtime_line(L"Hook: " + Utf8ToWide(state.hotpatch_hook_state), hook_color);
    draw_runtime_line(L"Broker: " + Utf8ToWide(state.hotpatch_broker_status), RGB(172, 187, 205));
    draw_runtime_line(L"Runtime: " + Utf8ToWide(state.hotpatch_dll_status), RGB(172, 187, 205));
    draw_runtime_line(
        L"Pose updates: " + Utf8ToWide(std::to_string(state.hotpatch_pose_updates_seen)) +
            L" / suppressed " + Utf8ToWide(std::to_string(state.hotpatch_pose_updates_suppressed)) +
            L" / replaced " + Utf8ToWide(std::to_string(state.hotpatch_pose_updates_replaced)) +
            L" / add calls " + Utf8ToWide(std::to_string(state.hotpatch_tracked_device_add_calls)),
        RGB(172, 187, 205));
    draw_runtime_line(
        L"PIDs: target " + Utf8ToWide(std::to_string(state.hotpatch_target_pid)) +
            L" / injected " + Utf8ToWide(std::to_string(state.hotpatch_injected_pid)),
        RGB(140, 154, 173));

    state.main_canvas.DrawBody(L"Last Error", RECT{40, 506, 220, 538}, RGB(239, 244, 248));
    state.main_canvas.DrawSmall(
        TruncateMiddle(Utf8ToWide(state.last_error.empty() ? "<none>" : state.last_error), 150),
        RECT{220, 508, 1560, 536},
        state.last_error.empty() ? RGB(172, 187, 205) : RGB(236, 107, 102));

    state.main_canvas.DrawBody(L"Sessions", RECT{40, 548, 220, 580}, RGB(239, 244, 248));

    const std::size_t page_count = std::max<std::size_t>(1, (state.session_files.size() + kPageSize - 1) / kPageSize);
    const std::size_t start_index = state.page_index * kPageSize;
    const std::size_t end_index = std::min(state.session_files.size(), start_index + kPageSize);

    if (state.session_files.empty())
    {
        const RECT empty_panel{40, 596, 1560, 900};
        state.main_canvas.FillRectColor(empty_panel, RGB(26, 31, 43));
        state.main_canvas.FrameRectColor(empty_panel, RGB(73, 88, 109), 2);
        state.main_canvas.DrawBody(
            L"No .svrcap files were found under the configured session root.",
            RECT{90, 660, 1510, 696},
            RGB(239, 244, 248));
        state.main_canvas.DrawSmall(
            L"Use Set Session Root to point at your sessions directory, paste a copied Explorer path, or enter a full file path.",
            RECT{90, 700, 1510, 764},
            RGB(172, 187, 205),
            DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    else
    {
        int row_top = 596;
        for (std::size_t file_index = start_index; file_index < end_index; ++file_index)
        {
            const std::filesystem::path& session_path = state.session_files[file_index];
            const std::wstring file_key = NormalizePathKey(session_path);
            const bool is_requested = !requested_key.empty() && file_key == requested_key;
            const bool is_loaded = !loaded_key.empty() && file_key == loaded_key;

            RECT row_rect{40, row_top, 1560, row_top + 66};
            COLORREF row_fill = RGB(31, 39, 52);
            COLORREF row_frame = RGB(73, 88, 109);

            if (is_loaded)
            {
                row_fill = RGB(25, 63, 60);
                row_frame = RGB(102, 189, 170);
            }
            else if (is_requested)
            {
                row_fill = RGB(58, 53, 33);
                row_frame = RGB(211, 180, 93);
            }

            state.main_canvas.FillRectColor(row_rect, row_fill);
            state.main_canvas.FrameRectColor(row_rect, row_frame, 2);

            state.main_canvas.DrawBody(
                TruncateMiddle(FilenameLabel(session_path), 56),
                RECT{60, row_top + 8, 1180, row_top + 34},
                RGB(244, 247, 250));
            state.main_canvas.DrawSmall(
                TruncateMiddle(Utf8ToWide(PathToUtf8(session_path)), 118),
                RECT{60, row_top + 34, 1180, row_top + 58},
                RGB(172, 187, 205));

            std::wstring status_tag;
            if (is_loaded)
            {
                status_tag = L"Loaded";
            }
            else if (is_requested)
            {
                status_tag = L"Requested";
            }
            if (!status_tag.empty())
            {
                state.main_canvas.DrawSmall(status_tag, RECT{1185, row_top + 18, 1310, row_top + 46}, row_frame);
            }

            AddButton(RECT{1320, row_top + 12, 1510, row_top + 54}, ButtonAction::LoadFile, file_index, L"Load");
            row_top += 72;
        }
    }

    state.main_canvas.DrawSmall(
        Utf8ToWide(
            "Found " + std::to_string(state.session_files.size()) + " session file(s). Page " +
            std::to_string(state.page_index + 1) + "/" + std::to_string(page_count) +
            ". Load keeps the session stopped at frame 0. Use Play to start. Hotpatch status is read directly from the shared-memory control block, not by polling broker --status. Live Mode cycles through Suppress, Replace, and Passthrough with no SteamVR restart once the broker is active. Paste Clipboard accepts Explorer-copied files or folders. The desktop mirror also supports Ctrl+V and drag-drop."),
        RECT{40, 930, 1560, 972},
        RGB(140, 154, 173),
        DT_LEFT | DT_TOP | DT_WORDBREAK);

    if (vr::VROverlay() != nullptr && state.main_overlay != vr::k_ulOverlayHandleInvalid)
    {
        vr::VROverlay()->SetOverlayRaw(
            state.main_overlay,
            const_cast<void*>(state.main_canvas.rgba_data()),
            static_cast<std::uint32_t>(state.main_canvas.width()),
            static_cast<std::uint32_t>(state.main_canvas.height()),
            4);
    }
}

void OverlayApp::Impl::RenderThumbnailOverlay()
{
    state.thumbnail_canvas.Clear(RGB(29, 36, 49));

    state.thumbnail_canvas.DrawTitle(L"Replay", RECT{24, 36, 376, 92}, RGB(240, 244, 248));
    state.thumbnail_canvas.DrawSmall(
        Utf8ToWide(std::to_string(state.loaded_tracker_count) + " tracker(s)"),
        RECT{26, 104, 374, 132},
        RGB(109, 204, 163));
    state.thumbnail_canvas.DrawSmall(
        TruncateMiddle(Utf8ToWide(
            (state.playback_state.empty() ? "stopped" : state.playback_state) +
            std::string(" / ") +
            (state.loop_enabled ? "loop" : "once")), 28),
        RECT{26, 146, 374, 178},
        RGB(218, 186, 97));
    state.thumbnail_canvas.DrawSmall(
        TruncateMiddle(Utf8ToWide(state.status_text.empty() ? "Idle" : state.status_text), 28),
        RECT{26, 180, 374, 212},
        RGB(172, 187, 205));
    state.thumbnail_canvas.DrawSmall(
        TruncateMiddle(
            Utf8ToWide(
                std::string("hook ") + state.hotpatch_hook_state + " / sup " +
                std::to_string(state.hotpatch_pose_updates_suppressed) + " / rep " +
                std::to_string(state.hotpatch_pose_updates_replaced)),
            28),
        RECT{26, 206, 374, 238},
        state.hotpatch_hook_state == "hook_installed" ? RGB(109, 204, 163) : RGB(172, 187, 205));
    state.thumbnail_canvas.DrawSmall(
        TruncateMiddle(Utf8ToWide(
            state.loaded_session_utf8.empty() ? state.requested_session_utf8 : state.loaded_session_utf8), 28),
        RECT{26, 250, 374, 320},
        RGB(213, 221, 231),
        DT_LEFT | DT_TOP | DT_WORDBREAK);

    if (vr::VROverlay() != nullptr && state.thumbnail_overlay != vr::k_ulOverlayHandleInvalid)
    {
        vr::VROverlay()->SetOverlayRaw(
            state.thumbnail_overlay,
            const_cast<void*>(state.thumbnail_canvas.rgba_data()),
            static_cast<std::uint32_t>(state.thumbnail_canvas.width()),
            static_cast<std::uint32_t>(state.thumbnail_canvas.height()),
            4);
    }
}

void OverlayApp::Impl::AddButton(
    const RECT& rect,
    const ButtonAction action,
    const std::size_t file_index,
    const std::wstring& label)
{
    COLORREF fill = RGB(52, 61, 77);
    COLORREF frame = RGB(112, 166, 216);

    switch (action)
    {
    case ButtonAction::LoadFile:
        fill = RGB(31, 94, 76);
        frame = RGB(109, 204, 163);
        break;

    case ButtonAction::Play:
        fill = RGB(31, 94, 76);
        frame = RGB(109, 204, 163);
        break;

    case ButtonAction::Pause:
        fill = RGB(102, 75, 34);
        frame = RGB(218, 186, 97);
        break;

    case ButtonAction::Stop:
        fill = RGB(102, 45, 43);
        frame = RGB(236, 107, 102);
        break;

    case ButtonAction::ToggleLoop:
    case ButtonAction::CycleLiveMode:
        fill = RGB(48, 68, 97);
        frame = RGB(133, 187, 237);
        break;

    case ButtonAction::ReloadCurrent:
        fill = RGB(102, 75, 34);
        frame = RGB(218, 186, 97);
        break;

    case ButtonAction::EditRoot:
    case ButtonAction::EditPath:
        fill = RGB(55, 57, 70);
        frame = RGB(140, 154, 173);
        break;

    case ButtonAction::Refresh:
    case ButtonAction::PrevPage:
    case ButtonAction::NextPage:
    case ButtonAction::PasteClipboard:
    case ButtonAction::None:
    default:
        break;
    }

    state.main_canvas.FillRectColor(rect, fill);
    state.main_canvas.FrameRectColor(rect, frame, 2);
    state.main_canvas.DrawBody(label, rect, RGB(245, 248, 250), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    state.buttons.push_back(Button{rect, action, file_index});
}

LRESULT CALLBACK OverlayApp::Impl::DesktopWindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wparam,
    LPARAM lparam)
{
    OverlayApp::Impl* impl = reinterpret_cast<OverlayApp::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* create_struct = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        impl = static_cast<OverlayApp::Impl*>(create_struct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
    }

    if (impl != nullptr)
    {
        return impl->HandleDesktopWindowMessage(hwnd, message, wparam, lparam);
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT OverlayApp::Impl::HandleDesktopWindowMessage(
    HWND hwnd,
    UINT message,
    WPARAM wparam,
    LPARAM lparam)
{
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client_rect{};
        GetClientRect(hwnd, &client_rect);
        state.main_canvas.BlitTo(dc, client_rect);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_LBUTTONUP:
        SetFocus(hwnd);
        HandleDesktopWindowClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;

    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (wparam == 'V' || wparam == 'v'))
        {
            PasteClipboardSelection();
            return 0;
        }
        break;

    case WM_DROPFILES:
    {
        const HDROP drop_handle = reinterpret_cast<HDROP>(wparam);
        const UINT path_length = DragQueryFileW(drop_handle, 0, nullptr, 0);
        if (path_length > 0)
        {
            std::wstring path_text(static_cast<std::size_t>(path_length) + 1u, L'\0');
            DragQueryFileW(drop_handle, 0, path_text.data(), path_length + 1);
            path_text.resize(path_length);
            ApplyPastedPath(std::filesystem::path(path_text));
        }
        DragFinish(drop_handle);
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_MINIMIZE);
        return 0;

    case WM_DESTROY:
        if (state.desktop_window == hwnd)
        {
            state.desktop_window = nullptr;
        }
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

OverlayApp::OverlayApp()
    : impl_(new Impl())
{
}

OverlayApp::~OverlayApp()
{
    delete impl_;
    impl_ = nullptr;
}

bool OverlayApp::Init(std::string* error)
{
    return impl_->Init(error);
}

int OverlayApp::Run()
{
    return impl_->Run();
}

void OverlayApp::Shutdown()
{
    impl_->Shutdown();
}
}  // namespace steamvr_capture::overlay
