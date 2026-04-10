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
enum class SessionFileVersion : std::uint32_t
{
    LegacyV1 = 1u,
    DriverPoseV2 = 2u,
    CalibratedStandingPoseV3 = 3u,
};

struct TrackerDescriptor
{
    std::int32_t device_class = 0;
    std::string serial;
    std::string tracking_system;
    std::string model_number;
    std::string manufacturer_name;
    std::string controller_type;
    std::string role;
};

struct PoseSample
{
    std::uint64_t timestamp_ns = 0;
    double pose_time_offset_s = 0.0;
    std::array<double, 3> position_m{0.0, 0.0, 0.0};
    std::array<double, 4> rotation_wxyz{1.0, 0.0, 0.0, 0.0};
    std::array<double, 3> linear_velocity_mps{0.0, 0.0, 0.0};
    std::array<double, 3> angular_velocity_rps{0.0, 0.0, 0.0};
    std::array<double, 4> world_from_driver_rotation_wxyz{1.0, 0.0, 0.0, 0.0};
    std::array<double, 3> world_from_driver_translation_m{0.0, 0.0, 0.0};
    std::array<double, 4> driver_from_head_rotation_wxyz{1.0, 0.0, 0.0, 0.0};
    std::array<double, 3> driver_from_head_translation_m{0.0, 0.0, 0.0};
    bool pose_valid = false;
    bool device_connected = false;
    std::int32_t tracking_result = 0;
    bool will_drift_in_yaw = false;
    bool should_apply_head_model = false;
};

struct TrackingSpaceSnapshot
{
    bool has_raw_to_standing = false;
    std::array<double, 12> raw_to_standing{};
    bool has_seated_to_standing = false;
    std::array<double, 12> seated_to_standing{};
};

struct SessionData
{
    SessionFileVersion format_version = SessionFileVersion::LegacyV1;
    bool poses_are_driver_space = false;
    std::vector<TrackerDescriptor> trackers;
    std::vector<std::vector<PoseSample>> samples_by_tracker;
    std::uint64_t duration_ns = 0;
    TrackingSpaceSnapshot tracking_space;
};

class SessionWriter
{
public:
    SessionWriter() = default;
    ~SessionWriter();

    SessionWriter(const SessionWriter&) = delete;
    SessionWriter& operator=(const SessionWriter&) = delete;

    bool Open(const std::filesystem::path& path, std::string* error);
    bool Open(const std::filesystem::path& path, SessionFileVersion version, std::string* error);
    bool WriteHeader(const std::vector<TrackerDescriptor>& trackers, std::string* error);
    bool WriteHeader(
        const std::vector<TrackerDescriptor>& trackers,
        const TrackingSpaceSnapshot& tracking_space,
        std::string* error);
    bool WriteSample(std::size_t tracker_index, const PoseSample& sample, std::string* error);
    void Close();

private:
    std::ofstream stream_;
    bool header_written_ = false;
    SessionFileVersion version_ = SessionFileVersion::LegacyV1;
};

bool LoadSessionFile(const std::filesystem::path& path, SessionData* session, std::string* error);
std::optional<PoseSample> SampleAtOrBefore(const SessionData& session, std::size_t tracker_index, std::uint64_t timestamp_ns);
}  // namespace steamvr_capture::session
