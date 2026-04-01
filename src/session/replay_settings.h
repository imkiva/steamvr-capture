#pragma once

namespace steamvr_capture::replay_settings
{
constexpr char kDriverSection[] = "driver_steamvr_capture_replay";
constexpr char kHotpatchSection[] = "steamvr_capture_hotpatch";
constexpr char kOverlaySection[] = "overlay_steamvr_capture";

constexpr char kEnableKey[] = "enable";
constexpr char kSessionPathKey[] = "session_path";
constexpr char kLoopKey[] = "loop";
constexpr char kPlaybackSpeedKey[] = "playback_speed";
constexpr char kPlaybackStateKey[] = "playback_state";
constexpr char kSessionGenerationKey[] = "session_generation";
constexpr char kLoadedSessionPathKey[] = "loaded_session_path";
constexpr char kLoadedTrackerCountKey[] = "loaded_tracker_count";
constexpr char kStatusTextKey[] = "status_text";
constexpr char kLastErrorKey[] = "last_error";
constexpr char kSuppressRealTrackersKey[] = "suppress_real_trackers";

constexpr char kSessionRootKey[] = "session_root";

constexpr char kOverlayAppKey[] = "steamvr_capture.overlay";
constexpr char kOverlayManifestFilename[] = "steamvr_capture_overlay.vrmanifest";
}  // namespace steamvr_capture::replay_settings
