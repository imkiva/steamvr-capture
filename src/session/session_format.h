#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace steamvr_capture::session
{
struct TrackerDescriptor
{
    std::string serial;
    std::string tracking_system;
    std::string model_number;
    std::string role;
};

struct PoseSample
{
    std::uint64_t timestamp_ns = 0;
    std::array<double, 3> position_m{0.0, 0.0, 0.0};
    std::array<double, 4> rotation_wxyz{1.0, 0.0, 0.0, 0.0};
    std::array<double, 3> linear_velocity_mps{0.0, 0.0, 0.0};
    std::array<double, 3> angular_velocity_rps{0.0, 0.0, 0.0};
    bool pose_valid = false;
    bool device_connected = false;
    std::int32_t tracking_result = 0;
};

struct SessionData
{
    std::vector<TrackerDescriptor> trackers;
    std::vector<std::vector<PoseSample>> samples_by_tracker;
    std::uint64_t duration_ns = 0;
};

class SessionWriter
{
public:
    SessionWriter() = default;
    ~SessionWriter();

    SessionWriter(const SessionWriter&) = delete;
    SessionWriter& operator=(const SessionWriter&) = delete;

    bool Open(const std::filesystem::path& path, std::string* error);
    bool WriteHeader(const std::vector<TrackerDescriptor>& trackers, std::string* error);
    bool WriteSample(std::size_t tracker_index, const PoseSample& sample, std::string* error);
    void Close();

private:
    std::ofstream stream_;
    bool header_written_ = false;
};

bool LoadSessionFile(const std::filesystem::path& path, SessionData* session, std::string* error);
std::optional<PoseSample> SampleAtOrBefore(const SessionData& session, std::size_t tracker_index, std::uint64_t timestamp_ns);
}  // namespace steamvr_capture::session
