#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <mmsystem.h>

#include "capture/openvr_app_helpers.h"
#include "openvr.h"
#include "session/replay_settings.h"
#include "session/session_format.h"

namespace
{
struct Options
{
    bool list_only = false;
    std::filesystem::path record_path;
    std::filesystem::path pretty_print_path;
    std::filesystem::path pretty_print_csv_path;
    std::filesystem::path output_path;
    std::wstring until_event_name;
    double duration_seconds = 10.0;
    double interval_ms = steamvr_capture::replay_settings::kDefaultRecordIntervalMs;
};

void PrintUsage()
{
    std::cout
        << "Usage:\n"
        << "  steamvr_capture_recorder --list\n"
        << "  steamvr_capture_recorder --record <file> [--duration <seconds>] [--interval-ms <ms>] [--until-event <name>]\n"
        << "  steamvr_capture_recorder --pretty-print <file>\n"
        << "  steamvr_capture_recorder --pretty-print-csv <file> [--output <file>]\n";
}

bool ParseDouble(const std::string& text, double* value)
{
    try
    {
        *value = std::stod(text);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

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

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), required);
    if (!result.empty() && result.back() == L'\0')
    {
        result.pop_back();
    }
    return result;
}

bool ParseOptions(const int argc, char** argv, Options* options)
{
    if (options == nullptr)
    {
        return false;
    }

    for (int index = 1; index < argc; ++index)
    {
        const std::string arg = argv[index];
        if (arg == "--list")
        {
            options->list_only = true;
            continue;
        }

        if (arg == "--record")
        {
            if (index + 1 >= argc)
            {
                return false;
            }
            options->record_path = argv[++index];
            continue;
        }

        if (arg == "--pretty-print")
        {
            if (index + 1 >= argc)
            {
                return false;
            }
            options->pretty_print_path = argv[++index];
            continue;
        }

        if (arg == "--pretty-print-csv")
        {
            if (index + 1 >= argc)
            {
                return false;
            }
            options->pretty_print_csv_path = argv[++index];
            continue;
        }

        if (arg == "--output")
        {
            if (index + 1 >= argc)
            {
                return false;
            }
            options->output_path = argv[++index];
            continue;
        }

        if (arg == "--duration")
        {
            if (index + 1 >= argc || !ParseDouble(argv[++index], &options->duration_seconds))
            {
                return false;
            }
            continue;
        }

        if (arg == "--interval-ms")
        {
            if (index + 1 >= argc || !ParseDouble(argv[++index], &options->interval_ms))
            {
                return false;
            }
            continue;
        }

        if (arg == "--until-event")
        {
            if (index + 1 >= argc)
            {
                return false;
            }
            options->until_event_name = Utf8ToWide(argv[++index]);
            continue;
        }

        if (arg == "--help" || arg == "-h")
        {
            PrintUsage();
            std::exit(0);
        }

        return false;
    }

    int mode_count = 0;
    mode_count += options->list_only ? 1 : 0;
    mode_count += options->record_path.empty() ? 0 : 1;
    mode_count += options->pretty_print_path.empty() ? 0 : 1;
    mode_count += options->pretty_print_csv_path.empty() ? 0 : 1;
    if (mode_count != 1)
    {
        return false;
    }

    if (!options->output_path.empty() && options->pretty_print_csv_path.empty())
    {
        return false;
    }

    if (!options->record_path.empty() && options->interval_ms <= 0.0)
    {
        return false;
    }

    if (!options->record_path.empty() && options->until_event_name.empty() && options->duration_seconds <= 0.0)
    {
        return false;
    }

    return true;
}

std::vector<std::string> SplitTabsPreservingEmpty(const std::string& line)
{
    std::vector<std::string> tokens;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= line.size(); ++index)
    {
        if (index == line.size() || line[index] == '\t')
        {
            tokens.push_back(line.substr(start, index - start));
            start = index + 1;
        }
    }
    return tokens;
}

template <typename T>
bool ParseValue(const std::string& text, T* value)
{
    std::stringstream stream(text);
    stream >> *value;
    return !stream.fail() && stream.eof();
}

std::string FormatVec3(const std::array<double, 3>& values)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6)
           << "(" << values[0] << ", " << values[1] << ", " << values[2] << ")";
    return stream.str();
}

std::string FormatQuat(const std::array<double, 4>& values)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6)
           << "(" << values[0] << ", " << values[1] << ", " << values[2] << ", " << values[3] << ")";
    return stream.str();
}

std::string FormatField(const std::string& value)
{
    return value.empty() ? "<unset>" : "\"" + value + "\"";
}

std::string EscapeCsvField(const std::string& value)
{
    bool needs_quotes = false;
    std::string escaped;
    escaped.reserve(value.size() + 4);

    for (const char ch : value)
    {
        if (ch == '"' || ch == ',' || ch == '\n' || ch == '\r')
        {
            needs_quotes = true;
        }

        if (ch == '"')
        {
            escaped += "\"\"";
        }
        else
        {
            escaped.push_back(ch);
        }
    }

    if (!needs_quotes)
    {
        return escaped;
    }

    return "\"" + escaped + "\"";
}

void WriteCsvRow(std::ostream& output, const std::vector<std::string>& columns)
{
    for (std::size_t index = 0; index < columns.size(); ++index)
    {
        if (index > 0)
        {
            output << ",";
        }
        output << EscapeCsvField(columns[index]);
    }
    output << "\n";
}

bool PrettyPrintSessionFile(const std::filesystem::path& path, std::string* error)
{
    steamvr_capture::session::SessionData session_data;
    std::string parse_error;
    if (!steamvr_capture::session::LoadSessionFile(path, &session_data, &parse_error))
    {
        if (error != nullptr)
        {
            *error = parse_error;
        }
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
    {
        if (error != nullptr)
        {
            *error = "Failed to open session file for pretty print: " + path.string();
        }
        return false;
    }

    std::cout << "Pretty print for " << path.string() << "\n";
    std::cout << "Summary: trackers=" << session_data.trackers.size()
              << " duration_ns=" << session_data.duration_ns << "\n\n";

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(stream, line))
    {
        ++line_number;
        const std::vector<std::string> tokens = SplitTabsPreservingEmpty(line);
        if (tokens.empty())
        {
            std::cout << "Line " << line_number << " [EMPTY]\n";
            continue;
        }

        const std::string& kind = tokens[0];
        if (kind == "SVRCAP" && tokens.size() == 2)
        {
            std::cout << "Line " << line_number << " [HEADER] "
                      << "magic=" << tokens[0]
                      << " version=" << tokens[1] << "\n";
            continue;
        }

        if (kind == "TRACKERS" && tokens.size() == 2)
        {
            std::cout << "Line " << line_number << " [TRACKER_COUNT] "
                      << "count=" << tokens[1] << "\n";
            continue;
        }

        if (kind == "TRACKER" && tokens.size() == 6)
        {
            std::cout << "Line " << line_number << " [TRACKER] "
                      << "index=" << tokens[1]
                      << " serial=" << FormatField(tokens[2])
                      << " tracking_system=" << FormatField(tokens[3])
                      << " model_number=" << FormatField(tokens[4])
                      << " role=" << FormatField(tokens[5]) << "\n";
            continue;
        }

        if (kind == "SAMPLE" && tokens.size() == 19)
        {
            std::uint64_t timestamp_ns = 0;
            std::size_t tracker_index = 0;
            std::array<double, 3> position{};
            std::array<double, 4> rotation{};
            std::array<double, 3> linear_velocity{};
            std::array<double, 3> angular_velocity{};
            int pose_valid = 0;
            int device_connected = 0;
            int tracking_result = 0;

            bool ok = ParseValue(tokens[1], &timestamp_ns) && ParseValue(tokens[2], &tracker_index);
            for (int i = 0; ok && i < 3; ++i)
            {
                ok = ParseValue(tokens[3 + i], &position[static_cast<std::size_t>(i)]);
            }
            for (int i = 0; ok && i < 4; ++i)
            {
                ok = ParseValue(tokens[6 + i], &rotation[static_cast<std::size_t>(i)]);
            }
            for (int i = 0; ok && i < 3; ++i)
            {
                ok = ParseValue(tokens[10 + i], &linear_velocity[static_cast<std::size_t>(i)]);
            }
            for (int i = 0; ok && i < 3; ++i)
            {
                ok = ParseValue(tokens[13 + i], &angular_velocity[static_cast<std::size_t>(i)]);
            }
            ok = ok &&
                 ParseValue(tokens[16], &pose_valid) &&
                 ParseValue(tokens[17], &device_connected) &&
                 ParseValue(tokens[18], &tracking_result);

            if (!ok)
            {
                std::cout << "Line " << line_number << " [SAMPLE] parse_error raw=" << line << "\n";
                continue;
            }

            std::cout << "Line " << line_number << " [SAMPLE] "
                      << "timestamp_ns=" << timestamp_ns
                      << " tracker_index=" << tracker_index
                      << " position_m=" << FormatVec3(position)
                      << " rotation_wxyz=" << FormatQuat(rotation)
                      << " linear_velocity_mps=" << FormatVec3(linear_velocity)
                      << " angular_velocity_rps=" << FormatVec3(angular_velocity)
                      << " pose_valid=" << (pose_valid != 0 ? "true" : "false")
                      << " device_connected=" << (device_connected != 0 ? "true" : "false")
                      << " tracking_result=" << tracking_result << "\n";
            continue;
        }

        std::cout << "Line " << line_number << " [UNKNOWN] raw=" << line << "\n";
    }

    return true;
}

bool PrettyPrintSessionFileCsv(
    const std::filesystem::path& path, std::ostream& output, std::string* error)
{
    steamvr_capture::session::SessionData session_data;
    std::string parse_error;
    if (!steamvr_capture::session::LoadSessionFile(path, &session_data, &parse_error))
    {
        if (error != nullptr)
        {
            *error = parse_error;
        }
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
    {
        if (error != nullptr)
        {
            *error = "Failed to open session file for CSV pretty print: " + path.string();
        }
        return false;
    }

    WriteCsvRow(
        output,
        {
            "line_number",
            "line_type",
            "magic",
            "version",
            "tracker_count",
            "tracker_index",
            "serial",
            "tracking_system",
            "model_number",
            "role",
            "timestamp_ns",
            "position_x_m",
            "position_y_m",
            "position_z_m",
            "rotation_w",
            "rotation_x",
            "rotation_y",
            "rotation_z",
            "linear_velocity_x_mps",
            "linear_velocity_y_mps",
            "linear_velocity_z_mps",
            "angular_velocity_x_rps",
            "angular_velocity_y_rps",
            "angular_velocity_z_rps",
            "pose_valid",
            "device_connected",
            "tracking_result",
            "raw_line",
        });

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(stream, line))
    {
        ++line_number;
        const std::vector<std::string> tokens = SplitTabsPreservingEmpty(line);
        if (tokens.empty())
        {
            WriteCsvRow(output, {std::to_string(line_number), "EMPTY", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", line});
            continue;
        }

        const std::string& kind = tokens[0];
        if (kind == "SVRCAP" && tokens.size() == 2)
        {
            WriteCsvRow(output, {std::to_string(line_number), "HEADER", tokens[0], tokens[1], "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", line});
            continue;
        }

        if (kind == "TRACKERS" && tokens.size() == 2)
        {
            WriteCsvRow(output, {std::to_string(line_number), "TRACKER_COUNT", "", "", tokens[1], "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", line});
            continue;
        }

        if (kind == "TRACKER" && tokens.size() == 6)
        {
            WriteCsvRow(output, {std::to_string(line_number), "TRACKER", "", "", "", tokens[1], tokens[2], tokens[3], tokens[4], tokens[5], "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", line});
            continue;
        }

        if (kind == "SAMPLE" && tokens.size() == 19)
        {
            WriteCsvRow(
                output,
                {
                    std::to_string(line_number),
                    "SAMPLE",
                    "",
                    "",
                    "",
                    tokens[2],
                    "",
                    "",
                    "",
                    "",
                    tokens[1],
                    tokens[3],
                    tokens[4],
                    tokens[5],
                    tokens[6],
                    tokens[7],
                    tokens[8],
                    tokens[9],
                    tokens[10],
                    tokens[11],
                    tokens[12],
                    tokens[13],
                    tokens[14],
                    tokens[15],
                    tokens[16],
                    tokens[17],
                    tokens[18],
                    line,
                });
            continue;
        }

        WriteCsvRow(output, {std::to_string(line_number), "UNKNOWN", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", line});
    }

    if (!output.good())
    {
        if (error != nullptr)
        {
            *error = "Failed while writing CSV output.";
        }
        return false;
    }

    return true;
}

void PrintTrackers(const std::vector<steamvr_capture::capture::TrackerSnapshot>& trackers)
{
    if (trackers.empty())
    {
        std::cout << "No SteamVR generic trackers are currently available.\n";
        return;
    }

    std::cout << "Discovered trackers:\n";
    for (const auto& tracker : trackers)
    {
        std::cout
            << "  index=" << tracker.device_index
            << " serial=" << tracker.descriptor.serial
            << " tracking_system=" << tracker.descriptor.tracking_system
            << " model=" << tracker.descriptor.model_number
            << " role=" << (tracker.descriptor.role.empty() ? "<unset>" : tracker.descriptor.role)
            << "\n";
    }
}
}  // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, &options))
    {
        PrintUsage();
        return 1;
    }

    if (!options.pretty_print_path.empty())
    {
        std::string error;
        if (!PrettyPrintSessionFile(options.pretty_print_path, &error))
        {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }

    if (!options.pretty_print_csv_path.empty())
    {
        std::ofstream file_output;
        std::ostream* output = &std::cout;
        if (!options.output_path.empty())
        {
            file_output.open(options.output_path, std::ios::binary | std::ios::trunc);
            if (!file_output.is_open())
            {
                std::cerr << "Failed to open CSV output path: " << options.output_path.string() << "\n";
                return 1;
            }
            output = &file_output;
        }

        std::string error;
        if (!PrettyPrintSessionFileCsv(options.pretty_print_csv_path, *output, &error))
        {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }

    vr::EVRInitError init_error = vr::VRInitError_None;
    vr::IVRSystem* vr_system = vr::VR_Init(&init_error, vr::VRApplication_Background);
    if (init_error != vr::VRInitError_None || vr_system == nullptr)
    {
        std::cerr << "Failed to initialize OpenVR: " << vr::VR_GetVRInitErrorAsEnglishDescription(init_error) << "\n";
        return 1;
    }

    auto shutdown_guard = std::unique_ptr<void, void (*)(void*)>(
        reinterpret_cast<void*>(1),
        [](void*) { vr::VR_Shutdown(); });

    vr::IVRSettings* settings = vr::VRSettings();
    if (settings == nullptr)
    {
        std::cerr << "Failed to acquire IVRSettings.\n";
        return 1;
    }

    const auto trackers = steamvr_capture::capture::EnumerateTrackers(*vr_system, *settings);
    PrintTrackers(trackers);

    if (options.list_only)
    {
        return 0;
    }

    if (trackers.empty())
    {
        std::cerr << "Cannot record because no generic trackers were detected.\n";
        return 1;
    }

    std::vector<steamvr_capture::session::TrackerDescriptor> descriptors;
    descriptors.reserve(trackers.size());
    for (const auto& tracker : trackers)
    {
        descriptors.push_back(tracker.descriptor);
    }

    steamvr_capture::session::SessionWriter writer;
    std::string error;
    if (!writer.Open(options.record_path, &error) || !writer.WriteHeader(descriptors, &error))
    {
        std::cerr << error << "\n";
        return 1;
    }

    std::vector<vr::TrackedDevicePose_t> poses(vr::k_unMaxTrackedDeviceCount);
    struct TimerResolutionGuard
    {
        TimerResolutionGuard()
        {
            enabled = timeBeginPeriod(1) == TIMERR_NOERROR;
        }

        ~TimerResolutionGuard()
        {
            if (enabled)
            {
                timeEndPeriod(1);
            }
        }

        bool enabled = false;
    } timer_resolution_guard;
    (void)timer_resolution_guard;

    const auto start_time = std::chrono::steady_clock::now();
    const auto end_time = start_time + std::chrono::duration<double>(options.duration_seconds);
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(options.interval_ms));
    auto next_sample_time = start_time;
    HANDLE stop_event = nullptr;
    if (!options.until_event_name.empty())
    {
        stop_event = OpenEventW(SYNCHRONIZE, FALSE, options.until_event_name.c_str());
        if (stop_event == nullptr)
        {
            std::cerr << "Failed to open stop event.\n";
            return 1;
        }
    }

    while (true)
    {
        if (stop_event != nullptr && WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0)
        {
            break;
        }

        if (stop_event == nullptr && std::chrono::steady_clock::now() >= end_time)
        {
            break;
        }

        vr_system->GetDeviceToAbsoluteTrackingPose(
            vr::TrackingUniverseStanding,
            0.0f,
            poses.data(),
            static_cast<std::uint32_t>(poses.size()));

        const auto now = std::chrono::steady_clock::now();
        const auto timestamp_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time).count());

        for (std::size_t tracker_index = 0; tracker_index < trackers.size(); ++tracker_index)
        {
            const auto device_index = trackers[tracker_index].device_index;
            if (device_index >= poses.size())
            {
                continue;
            }

            steamvr_capture::session::PoseSample sample;
            steamvr_capture::capture::PopulateSampleFromPose(poses[device_index], timestamp_ns, &sample);
            if (!writer.WriteSample(tracker_index, sample, &error))
            {
                std::cerr << error << "\n";
                return 1;
            }
        }

        next_sample_time += interval;
        const auto now_after_write = std::chrono::steady_clock::now();
        if (next_sample_time > now_after_write)
        {
            std::this_thread::sleep_until(next_sample_time);
        }
        else
        {
            next_sample_time = now_after_write;
        }
    }

    if (stop_event != nullptr)
    {
        CloseHandle(stop_event);
    }

    writer.Close();
    std::cout << "Recorded session to " << options.record_path.string() << "\n";
    return 0;
}
