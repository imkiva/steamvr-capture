#pragma once

#include <filesystem>
#include <string>

namespace steamvr_capture::overlay
{
bool InstallApplicationManifest(const std::filesystem::path& manifest_path, bool enable_auto_launch, std::string* error);

class OverlayApp
{
public:
    OverlayApp();
    ~OverlayApp();

    OverlayApp(const OverlayApp&) = delete;
    OverlayApp& operator=(const OverlayApp&) = delete;

    bool Init(std::string* error);
    int Run();
    void Shutdown();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
}  // namespace steamvr_capture::overlay
