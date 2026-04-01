#include "session/session_format.h"

#include <algorithm>
#include <sstream>

namespace steamvr_capture::session
{
namespace
{
constexpr char kTab = '\t';

std::string EscapeField(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::string UnescapeField(const std::string& value)
{
    std::string unescaped;
    unescaped.reserve(value.size());
    bool escaped = false;
    for (char ch : value)
    {
        if (!escaped)
        {
            if (ch == '\\')
            {
                escaped = true;
            }
            else
            {
                unescaped.push_back(ch);
            }
            continue;
        }

        switch (ch)
        {
        case '\\':
            unescaped.push_back('\\');
            break;
        case 't':
            unescaped.push_back('\t');
            break;
        case 'n':
            unescaped.push_back('\n');
            break;
        case 'r':
            unescaped.push_back('\r');
            break;
        default:
            unescaped.push_back(ch);
            break;
        }
        escaped = false;
    }
    if (escaped)
    {
        unescaped.push_back('\\');
    }
    return unescaped;
}

std::vector<std::string> SplitTabs(const std::string& line)
{
    std::vector<std::string> tokens;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= line.size(); ++index)
    {
        if (index == line.size() || line[index] == kTab)
        {
            tokens.push_back(line.substr(start, index - start));
            start = index + 1;
        }
    }
    return tokens;
}

template <typename T>
bool ParseInteger(const std::string& text, T* value)
{
    std::stringstream stream(text);
    stream >> *value;
    return !stream.fail() && stream.eof();
}

bool ParseDouble(const std::string& text, double* value)
{
    std::stringstream stream(text);
    stream >> *value;
    return !stream.fail() && stream.eof();
}

template <std::size_t N>
void WriteDoubleArray(std::ofstream& stream, const std::array<double, N>& values)
{
    for (double value : values)
    {
        stream << kTab << value;
    }
}

template <std::size_t N>
bool ParseDoubleArray(
    const std::vector<std::string>& tokens,
    const std::size_t start_index,
    std::array<double, N>* values)
{
    if (values == nullptr || start_index + N > tokens.size())
    {
        return false;
    }

    for (std::size_t index = 0; index < N; ++index)
    {
        if (!ParseDouble(tokens[start_index + index], &(*values)[index]))
        {
            return false;
        }
    }
    return true;
}

bool ParseLegacyV1(std::ifstream& stream, SessionData* session, std::string* error)
{
    std::string line;
    if (!std::getline(stream, line))
    {
        if (error != nullptr)
        {
            *error = "Session file is missing tracker metadata.";
        }
        return false;
    }

    const std::vector<std::string> tracker_count_tokens = SplitTabs(line);
    std::size_t tracker_count = 0;
    if (tracker_count_tokens.size() != 2 || tracker_count_tokens[0] != "TRACKERS" ||
        !ParseInteger(tracker_count_tokens[1], &tracker_count))
    {
        if (error != nullptr)
        {
            *error = "Session file tracker count line is invalid.";
        }
        return false;
    }

    SessionData result;
    result.format_version = SessionFileVersion::LegacyV1;
    result.poses_are_driver_space = false;
    result.trackers.resize(tracker_count);
    result.samples_by_tracker.resize(tracker_count);

    for (std::size_t expected_index = 0; expected_index < tracker_count; ++expected_index)
    {
        if (!std::getline(stream, line))
        {
            if (error != nullptr)
            {
                *error = "Session file ended before all tracker descriptors were read.";
            }
            return false;
        }

        const std::vector<std::string> tokens = SplitTabs(line);
        std::size_t parsed_index = 0;
        if (tokens.size() != 6 || tokens[0] != "TRACKER" || !ParseInteger(tokens[1], &parsed_index) ||
            parsed_index != expected_index)
        {
            if (error != nullptr)
            {
                *error = "Tracker descriptor line is invalid.";
            }
            return false;
        }

        TrackerDescriptor descriptor;
        descriptor.device_class = 3;
        descriptor.serial = UnescapeField(tokens[2]);
        descriptor.tracking_system = UnescapeField(tokens[3]);
        descriptor.model_number = UnescapeField(tokens[4]);
        descriptor.role = UnescapeField(tokens[5]);
        result.trackers[expected_index] = std::move(descriptor);
    }

    while (std::getline(stream, line))
    {
        if (line.empty())
        {
            continue;
        }

        const std::vector<std::string> tokens = SplitTabs(line);
        if (tokens.size() != 19 || tokens[0] != "SAMPLE")
        {
            if (error != nullptr)
            {
                *error = "Sample line is invalid.";
            }
            return false;
        }

        PoseSample sample;
        std::size_t tracker_index = 0;
        int pose_valid = 0;
        int device_connected = 0;

        if (!ParseInteger(tokens[1], &sample.timestamp_ns) ||
            !ParseInteger(tokens[2], &tracker_index) ||
            tracker_index >= result.samples_by_tracker.size())
        {
            if (error != nullptr)
            {
                *error = "Sample line contains an invalid tracker index or timestamp.";
            }
            return false;
        }

        if (!ParseDoubleArray(tokens, 3, &sample.position_m) ||
            !ParseDoubleArray(tokens, 6, &sample.rotation_wxyz) ||
            !ParseDoubleArray(tokens, 10, &sample.linear_velocity_mps) ||
            !ParseDoubleArray(tokens, 13, &sample.angular_velocity_rps))
        {
            if (error != nullptr)
            {
                *error = "Failed to parse legacy sample pose fields.";
            }
            return false;
        }

        if (!ParseInteger(tokens[16], &pose_valid) ||
            !ParseInteger(tokens[17], &device_connected) ||
            !ParseInteger(tokens[18], &sample.tracking_result))
        {
            if (error != nullptr)
            {
                *error = "Failed to parse legacy sample state flags.";
            }
            return false;
        }

        sample.pose_valid = pose_valid != 0;
        sample.device_connected = device_connected != 0;
        result.samples_by_tracker[tracker_index].push_back(sample);
        result.duration_ns = std::max(result.duration_ns, sample.timestamp_ns);
    }

    *session = std::move(result);
    return true;
}

bool ParseDriverPoseV2(std::ifstream& stream, SessionData* session, std::string* error)
{
    std::string line;
    if (!std::getline(stream, line))
    {
        if (error != nullptr)
        {
            *error = "Session file is missing device metadata.";
        }
        return false;
    }

    const std::vector<std::string> device_count_tokens = SplitTabs(line);
    std::size_t device_count = 0;
    if (device_count_tokens.size() != 2 || device_count_tokens[0] != "DEVICES" ||
        !ParseInteger(device_count_tokens[1], &device_count))
    {
        if (error != nullptr)
        {
            *error = "Session file device count line is invalid.";
        }
        return false;
    }

    SessionData result;
    result.format_version = SessionFileVersion::DriverPoseV2;
    result.poses_are_driver_space = true;
    result.trackers.resize(device_count);
    result.samples_by_tracker.resize(device_count);

    for (std::size_t expected_index = 0; expected_index < device_count; ++expected_index)
    {
        if (!std::getline(stream, line))
        {
            if (error != nullptr)
            {
                *error = "Session file ended before all device descriptors were read.";
            }
            return false;
        }

        const std::vector<std::string> tokens = SplitTabs(line);
        std::size_t parsed_index = 0;
        if (tokens.size() != 9 || tokens[0] != "DEVICE" || !ParseInteger(tokens[1], &parsed_index) ||
            parsed_index != expected_index || !ParseInteger(tokens[2], &result.trackers[expected_index].device_class))
        {
            if (error != nullptr)
            {
                *error = "Device descriptor line is invalid.";
            }
            return false;
        }

        result.trackers[expected_index].serial = UnescapeField(tokens[3]);
        result.trackers[expected_index].tracking_system = UnescapeField(tokens[4]);
        result.trackers[expected_index].model_number = UnescapeField(tokens[5]);
        result.trackers[expected_index].manufacturer_name = UnescapeField(tokens[6]);
        result.trackers[expected_index].controller_type = UnescapeField(tokens[7]);
        result.trackers[expected_index].role = UnescapeField(tokens[8]);
    }

    if (!std::getline(stream, line))
    {
        if (error != nullptr)
        {
            *error = "Session file is missing tracking-space metadata.";
        }
        return false;
    }

    const std::vector<std::string> space_tokens = SplitTabs(line);
    int has_raw = 0;
    int has_seated = 0;
    if (space_tokens.size() != 27 || space_tokens[0] != "SPACE" ||
        !ParseInteger(space_tokens[1], &has_raw) ||
        !ParseDoubleArray(space_tokens, 2, &result.tracking_space.raw_to_standing) ||
        !ParseInteger(space_tokens[14], &has_seated) ||
        !ParseDoubleArray(space_tokens, 15, &result.tracking_space.seated_to_standing))
    {
        if (error != nullptr)
        {
            *error = "Tracking-space metadata line is invalid.";
        }
        return false;
    }
    result.tracking_space.has_raw_to_standing = has_raw != 0;
    result.tracking_space.has_seated_to_standing = has_seated != 0;

    while (std::getline(stream, line))
    {
        if (line.empty())
        {
            continue;
        }

        const std::vector<std::string> tokens = SplitTabs(line);
        if (tokens.size() != 36 || tokens[0] != "POSE")
        {
            if (error != nullptr)
            {
                *error = "Pose line is invalid.";
            }
            return false;
        }

        PoseSample sample;
        std::size_t tracker_index = 0;
        int pose_valid = 0;
        int device_connected = 0;
        int will_drift_in_yaw = 0;
        int should_apply_head_model = 0;

        if (!ParseInteger(tokens[1], &sample.timestamp_ns) ||
            !ParseInteger(tokens[2], &tracker_index) ||
            tracker_index >= result.samples_by_tracker.size() ||
            !ParseDouble(tokens[3], &sample.pose_time_offset_s))
        {
            if (error != nullptr)
            {
                *error = "Pose line contains an invalid timestamp, device index, or pose time offset.";
            }
            return false;
        }

        if (!ParseDoubleArray(tokens, 4, &sample.position_m) ||
            !ParseDoubleArray(tokens, 7, &sample.rotation_wxyz) ||
            !ParseDoubleArray(tokens, 11, &sample.linear_velocity_mps) ||
            !ParseDoubleArray(tokens, 14, &sample.angular_velocity_rps) ||
            !ParseDoubleArray(tokens, 17, &sample.world_from_driver_rotation_wxyz) ||
            !ParseDoubleArray(tokens, 21, &sample.world_from_driver_translation_m) ||
            !ParseDoubleArray(tokens, 24, &sample.driver_from_head_rotation_wxyz) ||
            !ParseDoubleArray(tokens, 28, &sample.driver_from_head_translation_m))
        {
            if (error != nullptr)
            {
                *error = "Failed to parse pose line vectors or quaternions.";
            }
            return false;
        }

        if (!ParseInteger(tokens[31], &pose_valid) ||
            !ParseInteger(tokens[32], &device_connected) ||
            !ParseInteger(tokens[33], &sample.tracking_result) ||
            !ParseInteger(tokens[34], &will_drift_in_yaw) ||
            !ParseInteger(tokens[35], &should_apply_head_model))
        {
            if (error != nullptr)
            {
                *error = "Failed to parse pose line state flags.";
            }
            return false;
        }

        sample.pose_valid = pose_valid != 0;
        sample.device_connected = device_connected != 0;
        sample.will_drift_in_yaw = will_drift_in_yaw != 0;
        sample.should_apply_head_model = should_apply_head_model != 0;
        result.samples_by_tracker[tracker_index].push_back(sample);
        result.duration_ns = std::max(result.duration_ns, sample.timestamp_ns);
    }

    *session = std::move(result);
    return true;
}
}  // namespace

SessionWriter::~SessionWriter()
{
    Close();
}

bool SessionWriter::Open(const std::filesystem::path& path, std::string* error)
{
    return Open(path, SessionFileVersion::LegacyV1, error);
}

bool SessionWriter::Open(const std::filesystem::path& path, const SessionFileVersion version, std::string* error)
{
    Close();
    stream_.open(path, std::ios::binary | std::ios::trunc);
    if (!stream_.is_open())
    {
        if (error != nullptr)
        {
            *error = "Failed to open session file for writing: " + path.string();
        }
        return false;
    }
    version_ = version;
    header_written_ = false;
    return true;
}

bool SessionWriter::WriteHeader(const std::vector<TrackerDescriptor>& trackers, std::string* error)
{
    return WriteHeader(trackers, TrackingSpaceSnapshot{}, error);
}

bool SessionWriter::WriteHeader(
    const std::vector<TrackerDescriptor>& trackers,
    const TrackingSpaceSnapshot& tracking_space,
    std::string* error)
{
    if (!stream_.is_open())
    {
        if (error != nullptr)
        {
            *error = "Session file is not open.";
        }
        return false;
    }

    stream_ << "SVRCAP" << kTab << static_cast<std::uint32_t>(version_) << "\n";

    if (version_ == SessionFileVersion::DriverPoseV2)
    {
        stream_ << "DEVICES" << kTab << trackers.size() << "\n";
        for (std::size_t index = 0; index < trackers.size(); ++index)
        {
            const TrackerDescriptor& tracker = trackers[index];
            stream_ << "DEVICE" << kTab << index << kTab << tracker.device_class << kTab
                    << EscapeField(tracker.serial) << kTab
                    << EscapeField(tracker.tracking_system) << kTab
                    << EscapeField(tracker.model_number) << kTab
                    << EscapeField(tracker.manufacturer_name) << kTab
                    << EscapeField(tracker.controller_type) << kTab
                    << EscapeField(tracker.role) << "\n";
        }

        stream_ << "SPACE" << kTab << (tracking_space.has_raw_to_standing ? 1 : 0);
        WriteDoubleArray(stream_, tracking_space.raw_to_standing);
        stream_ << kTab << (tracking_space.has_seated_to_standing ? 1 : 0);
        WriteDoubleArray(stream_, tracking_space.seated_to_standing);
        stream_ << "\n";
    }
    else
    {
        stream_ << "TRACKERS\t" << trackers.size() << "\n";
        for (std::size_t index = 0; index < trackers.size(); ++index)
        {
            const TrackerDescriptor& tracker = trackers[index];
            stream_ << "TRACKER" << kTab << index << kTab
                    << EscapeField(tracker.serial) << kTab
                    << EscapeField(tracker.tracking_system) << kTab
                    << EscapeField(tracker.model_number) << kTab
                    << EscapeField(tracker.role) << "\n";
        }
    }

    if (!stream_.good())
    {
        if (error != nullptr)
        {
            *error = "Failed while writing session header.";
        }
        return false;
    }

    header_written_ = true;
    return true;
}

bool SessionWriter::WriteSample(const std::size_t tracker_index, const PoseSample& sample, std::string* error)
{
    if (!header_written_)
    {
        if (error != nullptr)
        {
            *error = "Session header must be written before samples.";
        }
        return false;
    }

    if (version_ == SessionFileVersion::DriverPoseV2)
    {
        stream_ << "POSE" << kTab << sample.timestamp_ns << kTab << tracker_index << kTab << sample.pose_time_offset_s;
        WriteDoubleArray(stream_, sample.position_m);
        WriteDoubleArray(stream_, sample.rotation_wxyz);
        WriteDoubleArray(stream_, sample.linear_velocity_mps);
        WriteDoubleArray(stream_, sample.angular_velocity_rps);
        WriteDoubleArray(stream_, sample.world_from_driver_rotation_wxyz);
        WriteDoubleArray(stream_, sample.world_from_driver_translation_m);
        WriteDoubleArray(stream_, sample.driver_from_head_rotation_wxyz);
        WriteDoubleArray(stream_, sample.driver_from_head_translation_m);
        stream_ << kTab << (sample.pose_valid ? 1 : 0)
                << kTab << (sample.device_connected ? 1 : 0)
                << kTab << sample.tracking_result
                << kTab << (sample.will_drift_in_yaw ? 1 : 0)
                << kTab << (sample.should_apply_head_model ? 1 : 0)
                << "\n";
    }
    else
    {
        stream_ << "SAMPLE" << kTab << sample.timestamp_ns << kTab << tracker_index;
        WriteDoubleArray(stream_, sample.position_m);
        WriteDoubleArray(stream_, sample.rotation_wxyz);
        WriteDoubleArray(stream_, sample.linear_velocity_mps);
        WriteDoubleArray(stream_, sample.angular_velocity_rps);
        stream_ << kTab << (sample.pose_valid ? 1 : 0)
                << kTab << (sample.device_connected ? 1 : 0)
                << kTab << sample.tracking_result << "\n";
    }

    if (!stream_.good())
    {
        if (error != nullptr)
        {
            *error = "Failed while writing a session sample.";
        }
        return false;
    }

    return true;
}

void SessionWriter::Close()
{
    if (stream_.is_open())
    {
        stream_.close();
    }
    header_written_ = false;
    version_ = SessionFileVersion::LegacyV1;
}

bool LoadSessionFile(const std::filesystem::path& path, SessionData* session, std::string* error)
{
    if (session == nullptr)
    {
        if (error != nullptr)
        {
            *error = "Session output pointer was null.";
        }
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
    {
        if (error != nullptr)
        {
            *error = "Failed to open session file for reading: " + path.string();
        }
        return false;
    }

    std::string line;
    if (!std::getline(stream, line))
    {
        if (error != nullptr)
        {
            *error = "Session file is empty.";
        }
        return false;
    }

    const std::vector<std::string> header_tokens = SplitTabs(line);
    std::uint32_t format_version = 0u;
    if (header_tokens.size() != 2 || header_tokens[0] != "SVRCAP" || !ParseInteger(header_tokens[1], &format_version))
    {
        if (error != nullptr)
        {
            *error = "Session file header is invalid.";
        }
        return false;
    }

    switch (static_cast<SessionFileVersion>(format_version))
    {
    case SessionFileVersion::LegacyV1:
        return ParseLegacyV1(stream, session, error);

    case SessionFileVersion::DriverPoseV2:
        return ParseDriverPoseV2(stream, session, error);

    default:
        if (error != nullptr)
        {
            *error = "Unsupported session file version.";
        }
        return false;
    }
}

std::optional<PoseSample> SampleAtOrBefore(
    const SessionData& session, const std::size_t tracker_index, const std::uint64_t timestamp_ns)
{
    if (tracker_index >= session.samples_by_tracker.size())
    {
        return std::nullopt;
    }

    const std::vector<PoseSample>& samples = session.samples_by_tracker[tracker_index];
    if (samples.empty())
    {
        return std::nullopt;
    }

    const auto it = std::upper_bound(
        samples.begin(),
        samples.end(),
        timestamp_ns,
        [](const std::uint64_t timestamp, const PoseSample& sample) { return timestamp < sample.timestamp_ns; });

    if (it == samples.begin())
    {
        return samples.front();
    }

    return *(it - 1);
}
}  // namespace steamvr_capture::session
