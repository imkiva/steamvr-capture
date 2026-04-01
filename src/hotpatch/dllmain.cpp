#include <windows.h>

#include <memory>

#include "hotpatch/hotpatch_runtime.h"

namespace
{
steamvr_capture::hotpatch::HotpatchRuntime* g_runtime = nullptr;
HANDLE g_worker_thread = nullptr;

DWORD WINAPI HotpatchWorkerThread(LPVOID parameter)
{
    auto* runtime = static_cast<steamvr_capture::hotpatch::HotpatchRuntime*>(parameter);
    runtime->Run();
    return 0;
}
}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(instance);
        g_runtime = new steamvr_capture::hotpatch::HotpatchRuntime(instance);
        g_worker_thread = CreateThread(nullptr, 0, HotpatchWorkerThread, g_runtime, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        if (g_runtime != nullptr)
        {
            g_runtime->RequestStop();
        }
        if (g_worker_thread != nullptr)
        {
            WaitForSingleObject(g_worker_thread, 1000);
            CloseHandle(g_worker_thread);
            g_worker_thread = nullptr;
        }
        delete g_runtime;
        g_runtime = nullptr;
        break;

    default:
        break;
    }

    return TRUE;
}
