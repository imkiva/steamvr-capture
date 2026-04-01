#include <cstring>

#include "openvr_driver.h"
#include "proxy_driver/lighthouse_proxy_provider.h"

namespace
{
steamvr_capture::proxy::LighthouseProxyProvider g_lighthouse_proxy_provider;
}

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* interface_name, int* return_code)
{
    if (std::strcmp(interface_name, vr::IServerTrackedDeviceProvider_Version) == 0)
    {
        return &g_lighthouse_proxy_provider;
    }

    if (return_code != nullptr)
    {
        *return_code = vr::VRInitError_Init_InterfaceNotFound;
    }
    return nullptr;
}
