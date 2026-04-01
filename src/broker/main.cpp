#include "broker/broker_app.h"

#include <cstdio>
#include <string>

namespace
{
void PrintUsage()
{
    std::puts("Usage:");
    std::puts("  steamvr_capture_broker.exe            Run the hotpatch broker watch loop");
    std::puts("  steamvr_capture_broker.exe --once     Poll once and exit");
    std::puts("  steamvr_capture_broker.exe --status   Print the current shared hotpatch state");
}
}  // namespace

int main(int argc, char** argv)
{
    bool once = false;
    bool status = false;

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--once")
        {
            once = true;
        }
        else if (argument == "--status")
        {
            status = true;
        }
        else if (argument == "--help" || argument == "-h")
        {
            PrintUsage();
            return 0;
        }
        else
        {
            std::fprintf(stderr, "Unknown argument: %s\n", argument.c_str());
            PrintUsage();
            return 1;
        }
    }

    steamvr_capture::broker::BrokerApp app;
    std::string error;
    if (status)
    {
        if (!app.AttachForStatus(&error))
        {
            std::fprintf(stderr, "Broker status failed: %s\n", error.c_str());
            return 1;
        }
        return app.PrintStatus();
    }

    if (!app.Init(&error))
    {
        std::fprintf(stderr, "Broker init failed: %s\n", error.c_str());
        return 1;
    }

    return app.Run(once);
}
