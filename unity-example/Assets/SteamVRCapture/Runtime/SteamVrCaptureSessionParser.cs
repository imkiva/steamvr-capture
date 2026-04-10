using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using UnityEngine;

namespace SteamVRCapture.UnityExample
{
    public static class SteamVrCaptureSessionParser
    {
        public sealed class ParseResult
        {
            public int formatVersion;
            public bool posesAreDriverSpace;
            public ulong durationNs;
            public SteamVrCaptureTrackingSpaceSnapshot trackingSpace = new();
            public List<SteamVrCaptureDeviceTrack> tracks = new();
        }

        private static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;

        public static ParseResult ParseFile(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                throw new ArgumentException("Session path is required.", nameof(path));
            }

            return ParseLines(File.ReadAllLines(path));
        }

        public static ParseResult ParseLines(IReadOnlyList<string> lines)
        {
            if (lines == null || lines.Count == 0)
            {
                throw new InvalidDataException("Session file is empty.");
            }

            List<string> header = SplitTabs(lines[0]);
            if (header.Count != 2 || header[0] != "SVRCAP" || !int.TryParse(header[1], out int formatVersion))
            {
                throw new InvalidDataException("Session file header is invalid.");
            }

            return formatVersion switch
            {
                1 => ParseLegacyV1(lines),
                2 => ParseDriverPoseV2(lines),
                3 => ParseCalibratedStandingPoseV3(lines),
                _ => throw new InvalidDataException("Unsupported session file version."),
            };
        }

        public static bool TryResolveScenePose(
            SteamVrCaptureSessionAsset session,
            SteamVrCapturePoseSample sample,
            out Vector3 unityPosition,
            out Quaternion unityRotation)
        {
            unityPosition = Vector3.zero;
            unityRotation = Quaternion.identity;

            if (session == null || sample == null || !sample.poseValid || !sample.deviceConnected)
            {
                return false;
            }

            Vector3 openVrPosition = sample.positionMeters;
            Quaternion openVrRotation = sample.rotationWxyz;

            if (session.PosesAreDriverSpace)
            {
                Quaternion worldFromDriverRotation = sample.worldFromDriverRotationWxyz;
                Vector3 worldFromDriverTranslation = sample.worldFromDriverTranslationMeters;
                openVrPosition = worldFromDriverTranslation + worldFromDriverRotation * openVrPosition;
                openVrRotation = worldFromDriverRotation * openVrRotation;
            }

            unityPosition = ToUnityPosition(openVrPosition);
            unityRotation = ToUnityRotation(openVrRotation);
            return true;
        }

        private static ParseResult ParseLegacyV1(IReadOnlyList<string> lines)
        {
            if (lines.Count < 2)
            {
                throw new InvalidDataException("Session file is missing tracker metadata.");
            }

            List<string> trackerCountTokens = SplitTabs(lines[1]);
            if (trackerCountTokens.Count != 2 || trackerCountTokens[0] != "TRACKERS" || !TryParseInt(trackerCountTokens[1], out int trackerCount))
            {
                throw new InvalidDataException("Session file tracker count line is invalid.");
            }

            ParseResult result = new()
            {
                formatVersion = 1,
                posesAreDriverSpace = false,
            };

            for (int index = 0; index < trackerCount; index++)
            {
                int lineIndex = index + 2;
                if (lineIndex >= lines.Count)
                {
                    throw new InvalidDataException("Session file ended before all tracker descriptors were read.");
                }

                List<string> tokens = SplitTabs(lines[lineIndex]);
                if (tokens.Count != 6 ||
                    tokens[0] != "TRACKER" ||
                    !TryParseInt(tokens[1], out int parsedIndex) ||
                    parsedIndex != index)
                {
                    throw new InvalidDataException("Tracker descriptor line is invalid.");
                }

                result.tracks.Add(new SteamVrCaptureDeviceTrack
                {
                    index = index,
                    deviceClass = SteamVrCaptureDeviceClass.GenericTracker,
                    serial = Unescape(tokens[2]),
                    trackingSystem = Unescape(tokens[3]),
                    modelNumber = Unescape(tokens[4]),
                    role = Unescape(tokens[5]),
                });
            }

            for (int lineIndex = trackerCount + 2; lineIndex < lines.Count; lineIndex++)
            {
                string line = lines[lineIndex];
                if (string.IsNullOrWhiteSpace(line))
                {
                    continue;
                }

                List<string> tokens = SplitTabs(line);
                if (tokens.Count != 19 || tokens[0] != "SAMPLE")
                {
                    if (lineIndex == lines.Count - 1 && tokens.Count > 0 && tokens[0] == "SAMPLE" && tokens.Count < 19)
                    {
                        break;
                    }

                    throw new InvalidDataException("Sample line is invalid.");
                }

                if (!TryParseUInt64(tokens[1], out ulong timestampNs) ||
                    !TryParseInt(tokens[2], out int trackIndex) ||
                    trackIndex < 0 || trackIndex >= result.tracks.Count)
                {
                    throw new InvalidDataException("Sample line contains an invalid tracker index or timestamp.");
                }

                SteamVrCapturePoseSample sample = new()
                {
                    timestampNs = timestampNs,
                    positionMeters = ParseVector3(tokens, 3),
                    rotationWxyz = ParseQuaternionWxyz(tokens, 6),
                    linearVelocityMetersPerSecond = ParseVector3(tokens, 10),
                    angularVelocityRadiansPerSecond = ParseVector3(tokens, 13),
                    poseValid = ParseBoolInt(tokens[16]),
                    deviceConnected = ParseBoolInt(tokens[17]),
                    trackingResult = ParseInt(tokens[18]),
                };

                result.tracks[trackIndex].samples.Add(sample);
                result.durationNs = Math.Max(result.durationNs, timestampNs);
            }

            return result;
        }

        private static ParseResult ParseDriverPoseV2(IReadOnlyList<string> lines)
        {
            return ParsePoseChannelSession(lines, 2, true, false);
        }

        private static ParseResult ParseCalibratedStandingPoseV3(IReadOnlyList<string> lines)
        {
            return ParsePoseChannelSession(lines, 3, false, true);
        }

        private static ParseResult ParsePoseChannelSession(
            IReadOnlyList<string> lines,
            int formatVersion,
            bool posesAreDriverSpace,
            bool expectPoseSpaceLine)
        {
            int cursor = 1;
            if (lines.Count < (expectPoseSpaceLine ? 5 : 4))
            {
                throw new InvalidDataException("Session file is missing device metadata.");
            }

            if (expectPoseSpaceLine)
            {
                List<string> poseSpaceTokens = SplitTabs(lines[cursor++]);
                if (poseSpaceTokens.Count != 2 ||
                    poseSpaceTokens[0] != "POSE_SPACE" ||
                    poseSpaceTokens[1] != "app_standing_calibrated")
                {
                    throw new InvalidDataException("Session file pose-space metadata is invalid.");
                }
            }

            List<string> deviceCountTokens = SplitTabs(lines[cursor++]);
            if (deviceCountTokens.Count != 2 || deviceCountTokens[0] != "DEVICES" || !TryParseInt(deviceCountTokens[1], out int deviceCount))
            {
                throw new InvalidDataException("Session file device count line is invalid.");
            }

            ParseResult result = new()
            {
                formatVersion = formatVersion,
                posesAreDriverSpace = posesAreDriverSpace,
            };

            for (int index = 0; index < deviceCount; index++)
            {
                int lineIndex = cursor + index;
                if (lineIndex >= lines.Count)
                {
                    throw new InvalidDataException("Session file ended before all device descriptors were read.");
                }

                List<string> tokens = SplitTabs(lines[lineIndex]);
                if (tokens.Count != 9 ||
                    tokens[0] != "DEVICE" ||
                    !TryParseInt(tokens[1], out int parsedIndex) ||
                    parsedIndex != index ||
                    !TryParseInt(tokens[2], out int deviceClass))
                {
                    throw new InvalidDataException("Device descriptor line is invalid.");
                }

                result.tracks.Add(new SteamVrCaptureDeviceTrack
                {
                    index = index,
                    deviceClass = (SteamVrCaptureDeviceClass)deviceClass,
                    serial = Unescape(tokens[3]),
                    trackingSystem = Unescape(tokens[4]),
                    modelNumber = Unescape(tokens[5]),
                    manufacturerName = Unescape(tokens[6]),
                    controllerType = Unescape(tokens[7]),
                    role = Unescape(tokens[8]),
                });
            }

            cursor += deviceCount;
            List<string> spaceTokens = SplitTabs(lines[cursor++]);
            if (spaceTokens.Count != 27 || spaceTokens[0] != "SPACE")
            {
                throw new InvalidDataException("Tracking-space metadata line is invalid.");
            }

            result.trackingSpace.hasRawToStanding = ParseBoolInt(spaceTokens[1]);
            result.trackingSpace.rawToStanding = ParseFloatArray(spaceTokens, 2, 12);
            result.trackingSpace.hasSeatedToStanding = ParseBoolInt(spaceTokens[14]);
            result.trackingSpace.seatedToStanding = ParseFloatArray(spaceTokens, 15, 12);

            for (int lineIndex = cursor; lineIndex < lines.Count; lineIndex++)
            {
                string line = lines[lineIndex];
                if (string.IsNullOrWhiteSpace(line))
                {
                    continue;
                }

                List<string> tokens = SplitTabs(line);
                if (tokens.Count != 36 || tokens[0] != "POSE")
                {
                    if (lineIndex == lines.Count - 1 && tokens.Count > 0 && tokens[0] == "POSE" && tokens.Count < 36)
                    {
                        break;
                    }

                    throw new InvalidDataException("Pose line is invalid.");
                }

                if (!TryParseUInt64(tokens[1], out ulong timestampNs) ||
                    !TryParseInt(tokens[2], out int trackIndex) ||
                    trackIndex < 0 || trackIndex >= result.tracks.Count)
                {
                    throw new InvalidDataException("Pose line contains an invalid timestamp or device index.");
                }

                SteamVrCapturePoseSample sample = new()
                {
                    timestampNs = timestampNs,
                    poseTimeOffsetSeconds = ParseDouble(tokens[3]),
                    positionMeters = ParseVector3(tokens, 4),
                    rotationWxyz = ParseQuaternionWxyz(tokens, 7),
                    linearVelocityMetersPerSecond = ParseVector3(tokens, 11),
                    angularVelocityRadiansPerSecond = ParseVector3(tokens, 14),
                    worldFromDriverRotationWxyz = ParseQuaternionWxyz(tokens, 17),
                    worldFromDriverTranslationMeters = ParseVector3(tokens, 21),
                    driverFromHeadRotationWxyz = ParseQuaternionWxyz(tokens, 24),
                    driverFromHeadTranslationMeters = ParseVector3(tokens, 28),
                    poseValid = ParseBoolInt(tokens[31]),
                    deviceConnected = ParseBoolInt(tokens[32]),
                    trackingResult = ParseInt(tokens[33]),
                    willDriftInYaw = ParseBoolInt(tokens[34]),
                    shouldApplyHeadModel = ParseBoolInt(tokens[35]),
                };

                result.tracks[trackIndex].samples.Add(sample);
                result.durationNs = Math.Max(result.durationNs, timestampNs);
            }

            return result;
        }

        private static Vector3 ToUnityPosition(Vector3 openVrPosition)
        {
            return new Vector3(openVrPosition.x, openVrPosition.y, -openVrPosition.z);
        }

        private static Quaternion ToUnityRotation(Quaternion openVrRotation)
        {
            return new Quaternion(-openVrRotation.x, -openVrRotation.y, openVrRotation.z, openVrRotation.w);
        }

        private static Vector3 ParseVector3(IReadOnlyList<string> tokens, int startIndex)
        {
            return new Vector3(
                ParseFloat(tokens[startIndex]),
                ParseFloat(tokens[startIndex + 1]),
                ParseFloat(tokens[startIndex + 2]));
        }

        private static Quaternion ParseQuaternionWxyz(IReadOnlyList<string> tokens, int startIndex)
        {
            float w = ParseFloat(tokens[startIndex]);
            float x = ParseFloat(tokens[startIndex + 1]);
            float y = ParseFloat(tokens[startIndex + 2]);
            float z = ParseFloat(tokens[startIndex + 3]);
            return new Quaternion(x, y, z, w);
        }

        private static float[] ParseFloatArray(IReadOnlyList<string> tokens, int startIndex, int count)
        {
            float[] values = new float[count];
            for (int i = 0; i < count; i++)
            {
                values[i] = ParseFloat(tokens[startIndex + i]);
            }
            return values;
        }

        private static List<string> SplitTabs(string line)
        {
            string[] tokens = line.Split('\t');
            return new List<string>(tokens);
        }

        private static string Unescape(string value)
        {
            if (string.IsNullOrEmpty(value))
            {
                return string.Empty;
            }

            char[] buffer = new char[value.Length];
            int count = 0;
            bool escaped = false;

            for (int i = 0; i < value.Length; i++)
            {
                char ch = value[i];
                if (!escaped)
                {
                    if (ch == '\\')
                    {
                        escaped = true;
                    }
                    else
                    {
                        buffer[count++] = ch;
                    }

                    continue;
                }

                buffer[count++] = ch switch
                {
                    '\\' => '\\',
                    't' => '\t',
                    'n' => '\n',
                    'r' => '\r',
                    _ => ch,
                };
                escaped = false;
            }

            if (escaped)
            {
                buffer[count++] = '\\';
            }

            return new string(buffer, 0, count);
        }

        private static bool ParseBoolInt(string text)
        {
            return ParseInt(text) != 0;
        }

        private static int ParseInt(string text)
        {
            if (!TryParseInt(text, out int value))
            {
                throw new InvalidDataException("Failed to parse integer value.");
            }

            return value;
        }

        private static float ParseFloat(string text)
        {
            if (!float.TryParse(text, NumberStyles.Float | NumberStyles.AllowThousands, Invariant, out float value))
            {
                throw new InvalidDataException("Failed to parse floating-point value.");
            }

            return value;
        }

        private static double ParseDouble(string text)
        {
            if (!double.TryParse(text, NumberStyles.Float | NumberStyles.AllowThousands, Invariant, out double value))
            {
                throw new InvalidDataException("Failed to parse double-precision value.");
            }

            return value;
        }

        private static bool TryParseInt(string text, out int value)
        {
            return int.TryParse(text, NumberStyles.Integer, Invariant, out value);
        }

        private static bool TryParseUInt64(string text, out ulong value)
        {
            return ulong.TryParse(text, NumberStyles.Integer, Invariant, out value);
        }
    }
}
