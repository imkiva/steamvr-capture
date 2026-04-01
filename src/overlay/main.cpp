#include <filesystem>
#include <iostream>
#include <string>

#include <windows.h>

#include "overlay/overlay_app.h"
#include "session/replay_settings.h"

namespace
{
void PrintUsage()
{
    std::cout
        << "Usage:\n"
        << "  steamvr_capture_overlay\n"
        << "  steamvr_capture_overlay --install-manifest\n";
}

std::filesystem::path ResolveManifestPath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path() /
        steamvr_capture::replay_settings::kOverlayManifestFilename;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc > 2)
    {
        PrintUsage();
        return 1;
    }

    if (argc == 2)
    {
        const std::string argument = argv[1];
        if (argument != "--install-manifest")
        {
            PrintUsage();
            return 1;
        }

        const std::filesystem::path manifest_path = ResolveManifestPath();
        std::string error;
        if (!steamvr_capture::overlay::InstallApplicationManifest(manifest_path, true, &error))
        {
            std::cerr << error << "\n";
            return 1;
        }

        std::cout << "Installed overlay manifest from " << manifest_path.string() << "\n";
        return 0;
    }

    steamvr_capture::overlay::OverlayApp app;
    std::string error;
    if (!app.Init(&error))
    {
        std::cerr << error << "\n";
        return 1;
    }

    const int result = app.Run();
    app.Shutdown();
    return result;
}
