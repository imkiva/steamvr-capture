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
}  // namespace

SessionWriter::~SessionWriter()
{
    Close();
}

bool SessionWriter::Open(const std::filesystem::path& path, std::string* error)
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
    header_written_ = false;
    return true;
}

bool SessionWriter::WriteHeader(const std::vector<TrackerDescriptor>& trackers, std::string* error)
{
    if (!stream_.is_open())
    {
        if (error != nullptr)
        {
            *error = "Session file is not open.";
        }
        return false;
    }

    stream_ << "SVRCAP\t1\n";
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

bool SessionWriter::WriteSample(std::size_t tracker_index, const PoseSample& sample, std::string* error)
{
    if (!header_written_)
    {
        if (error != nullptr)
        {
            *error = "Session header must be written before samples.";
        }
        return false;
    }

    stream_ << "SAMPLE" << kTab << sample.timestamp_ns << kTab << tracker_index;
    for (double value : sample.position_m)
    {
        stream_ << kTab << value;
    }
    for (double value : sample.rotation_wxyz)
    {
        stream_ << kTab << value;
    }
    for (double value : sample.linear_velocity_mps)
    {
        stream_ << kTab << value;
    }
    for (double value : sample.angular_velocity_rps)
    {
        stream_ << kTab << value;
    }
    stream_ << kTab << (sample.pose_valid ? 1 : 0)
            << kTab << (sample.device_connected ? 1 : 0)
            << kTab << sample.tracking_result << "\n";

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

    SessionData result;
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
    if (header_tokens.size() != 2 || header_tokens[0] != "SVRCAP" || header_tokens[1] != "1")
    {
        if (error != nullptr)
        {
            *error = "Session file header is invalid.";
        }
        return false;
    }

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

        for (int i = 0; i < 3; ++i)
        {
            if (!ParseDouble(tokens[3 + i], &sample.position_m[static_cast<std::size_t>(i)]))
            {
                if (error != nullptr)
                {
                    *error = "Failed to parse sample position.";
                }
                return false;
            }
        }

        for (int i = 0; i < 4; ++i)
        {
            if (!ParseDouble(tokens[6 + i], &sample.rotation_wxyz[static_cast<std::size_t>(i)]))
            {
                if (error != nullptr)
                {
                    *error = "Failed to parse sample rotation.";
                }
                return false;
            }
        }

        for (int i = 0; i < 3; ++i)
        {
            if (!ParseDouble(tokens[10 + i], &sample.linear_velocity_mps[static_cast<std::size_t>(i)]))
            {
                if (error != nullptr)
                {
                    *error = "Failed to parse sample linear velocity.";
                }
                return false;
            }
        }

        for (int i = 0; i < 3; ++i)
        {
            if (!ParseDouble(tokens[13 + i], &sample.angular_velocity_rps[static_cast<std::size_t>(i)]))
            {
                if (error != nullptr)
                {
                    *error = "Failed to parse sample angular velocity.";
                }
                return false;
            }
        }

        if (!ParseInteger(tokens[16], &pose_valid) ||
            !ParseInteger(tokens[17], &device_connected) ||
            !ParseInteger(tokens[18], &sample.tracking_result))
        {
            if (error != nullptr)
            {
                *error = "Failed to parse sample state flags.";
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
