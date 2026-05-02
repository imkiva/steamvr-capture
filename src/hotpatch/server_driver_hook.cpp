#include "hotpatch/server_driver_hook.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace steamvr_capture::hotpatch
{
namespace
{
ServerDriverHook* g_server_driver_hook = nullptr;
constexpr double kQuaternionEpsilon = 1e-9;

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

    std::wstring result(static_cast<std::size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), required);
    return result;
}

vr::HmdQuaternion_t NormalizeQuaternion(vr::HmdQuaternion_t quaternion)
{
    const double length = std::sqrt(
        quaternion.w * quaternion.w +
        quaternion.x * quaternion.x +
        quaternion.y * quaternion.y +
        quaternion.z * quaternion.z);
    if (length <= kQuaternionEpsilon)
    {
        return {1.0, 0.0, 0.0, 0.0};
    }

    quaternion.w /= length;
    quaternion.x /= length;
    quaternion.y /= length;
    quaternion.z /= length;
    return quaternion;
}

vr::HmdQuaternion_t ConjugateQuaternion(const vr::HmdQuaternion_t quaternion)
{
    return {quaternion.w, -quaternion.x, -quaternion.y, -quaternion.z};
}

vr::HmdQuaternion_t MultiplyQuaternion(const vr::HmdQuaternion_t lhs, const vr::HmdQuaternion_t rhs)
{
    return {
        (lhs.w * rhs.w) - (lhs.x * rhs.x) - (lhs.y * rhs.y) - (lhs.z * rhs.z),
        (lhs.w * rhs.x) + (lhs.x * rhs.w) + (lhs.y * rhs.z) - (lhs.z * rhs.y),
        (lhs.w * rhs.y) + (lhs.y * rhs.w) + (lhs.z * rhs.x) - (lhs.x * rhs.z),
        (lhs.w * rhs.z) + (lhs.z * rhs.w) + (lhs.x * rhs.y) - (lhs.y * rhs.x)};
}

std::array<double, 3> RotateVector(
    const vr::HmdQuaternion_t rotation,
    const std::array<double, 3>& vector)
{
    const vr::HmdQuaternion_t normalized_rotation = NormalizeQuaternion(rotation);
    const vr::HmdQuaternion_t vector_quaternion{0.0, vector[0], vector[1], vector[2]};
    const vr::HmdQuaternion_t rotated = MultiplyQuaternion(
        MultiplyQuaternion(normalized_rotation, vector_quaternion),
        ConjugateQuaternion(normalized_rotation));
    return {rotated.x, rotated.y, rotated.z};
}
}  // namespace

ServerDriverHook::~ServerDriverHook()
{
    Uninstall();
}

bool ServerDriverHook::EnsureInstalled(SharedState* shared_state, std::wstring* status_message)
{
    shared_state_ = shared_state;
    if (installed_)
    {
        if (status_message != nullptr)
        {
            *status_message = L"Server driver host hook is active.";
        }
        return true;
    }

    if (!ResolveReplayBridge(status_message))
    {
        return false;
    }

    host_object_ = get_server_driver_host_ != nullptr ? get_server_driver_host_() : nullptr;
    if (host_object_ == nullptr)
    {
        if (status_message != nullptr)
        {
            *status_message = L"Replay bridge is loaded, but IVRServerDriverHost is not ready yet.";
        }
        return false;
    }

    if (!PatchHostVtable(status_message))
    {
        return false;
    }

    PrimeExistingDevices();
    g_server_driver_hook = this;
    installed_ = true;

    if (status_message != nullptr)
    {
        *status_message = L"Installed live hook on IVRServerDriverHost.";
    }
    return true;
}

void ServerDriverHook::Uninstall()
{
    if (!installed_ || host_vtable_ == nullptr)
    {
        return;
    }

    DWORD old_protect = 0;
    if (VirtualProtect(host_vtable_, sizeof(void*) * 2u, PAGE_EXECUTE_READWRITE, &old_protect))
    {
        host_vtable_[0] = original_add_entry_;
        host_vtable_[1] = original_pose_entry_;
        DWORD ignored = 0;
        VirtualProtect(host_vtable_, sizeof(void*) * 2u, old_protect, &ignored);
    }

    installed_ = false;
    g_server_driver_hook = nullptr;
    tracked_device_wrappers_.clear();
    devices_by_index_.clear();
    if (shared_state_ != nullptr)
    {
        shared_state_->observed_device_count = 0u;
        for (std::size_t index = 0; index < kMaxObservedDevices; ++index)
        {
            shared_state_->observed_devices[index] = ObservedDeviceSlot{};
        }
    }
    shared_state_ = nullptr;
}

bool ServerDriverHook::ResolveReplayBridge(std::wstring* status_message)
{
    if (get_server_driver_host_ != nullptr && get_tracked_device_info_ != nullptr && get_max_tracked_device_count_ != nullptr)
    {
        return true;
    }

    replay_driver_module_ = GetModuleHandleW(L"driver_steamvr_capture_replay.dll");
    if (replay_driver_module_ == nullptr)
    {
        if (status_message != nullptr)
        {
            *status_message = L"Waiting for driver_steamvr_capture_replay.dll to load.";
        }
        return false;
    }

    get_server_driver_host_ = reinterpret_cast<GetServerDriverHostFn>(
        GetProcAddress(replay_driver_module_, "SteamVRCapture_GetServerDriverHost"));
    get_max_tracked_device_count_ = reinterpret_cast<GetMaxTrackedDeviceCountFn>(
        GetProcAddress(replay_driver_module_, "SteamVRCapture_GetMaxTrackedDeviceCount"));
    get_tracked_device_info_ = reinterpret_cast<GetTrackedDeviceInfoFn>(
        GetProcAddress(replay_driver_module_, "SteamVRCapture_GetTrackedDeviceInfo"));

    if (get_server_driver_host_ == nullptr || get_max_tracked_device_count_ == nullptr || get_tracked_device_info_ == nullptr)
    {
        if (status_message != nullptr)
        {
            *status_message = L"Replay bridge exports are not available from the replay driver.";
        }
        return false;
    }

    return true;
}

bool ServerDriverHook::PatchHostVtable(std::wstring* status_message)
{
    host_vtable_ = *reinterpret_cast<void***>(host_object_);
    if (host_vtable_ == nullptr)
    {
        if (status_message != nullptr)
        {
            *status_message = L"IVRServerDriverHost vtable pointer is null.";
        }
        return false;
    }

    original_add_entry_ = host_vtable_[0];
    original_pose_entry_ = host_vtable_[1];
    original_tracked_device_added_ = reinterpret_cast<TrackedDeviceAddedFn>(original_add_entry_);
    original_tracked_device_pose_updated_ = reinterpret_cast<TrackedDevicePoseUpdatedFn>(original_pose_entry_);

    DWORD old_protect = 0;
    if (!VirtualProtect(host_vtable_, sizeof(void*) * 2u, PAGE_EXECUTE_READWRITE, &old_protect))
    {
        if (status_message != nullptr)
        {
            *status_message = L"VirtualProtect failed while patching IVRServerDriverHost vtable.";
        }
        return false;
    }

    host_vtable_[0] = reinterpret_cast<void*>(&ServerDriverHook::HookedTrackedDeviceAdded);
    host_vtable_[1] = reinterpret_cast<void*>(&ServerDriverHook::HookedTrackedDevicePoseUpdated);

    DWORD ignored = 0;
    VirtualProtect(host_vtable_, sizeof(void*) * 2u, old_protect, &ignored);
    return true;
}

void ServerDriverHook::PrimeExistingDevices()
{
    if (get_max_tracked_device_count_ == nullptr || get_tracked_device_info_ == nullptr)
    {
        return;
    }

    const std::uint32_t max_count = get_max_tracked_device_count_();
    for (std::uint32_t device_index = 0; device_index < max_count; ++device_index)
    {
        ReplayBridgeTrackedDeviceInfo info{};
        if (!get_tracked_device_info_(device_index, &info) || info.is_present == 0u)
        {
            continue;
        }

        RecordActivatedDevice(
            device_index,
            std::string(info.serial),
            static_cast<vr::ETrackedDeviceClass>(info.device_class));
    }
}

void ServerDriverHook::RecordActivatedDevice(
    const vr::TrackedDeviceIndex_t device_index,
    const std::string& serial,
    const vr::ETrackedDeviceClass device_class)
{
    std::lock_guard<std::mutex> guard(mutex_);
    devices_by_index_[device_index] = DeviceMetadata{serial, device_class};
}

void ServerDriverHook::RemoveDevice(const vr::TrackedDeviceIndex_t device_index)
{
    std::lock_guard<std::mutex> guard(mutex_);
    devices_by_index_.erase(device_index);
}

bool ServerDriverHook::TryGetDeviceMetadata(const vr::TrackedDeviceIndex_t device_index, DeviceMetadata* metadata)
{
    if (metadata == nullptr)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto it = devices_by_index_.find(device_index);
        if (it != devices_by_index_.end())
        {
            *metadata = it->second;
            return true;
        }
    }

    if (get_tracked_device_info_ == nullptr)
    {
        return false;
    }

    ReplayBridgeTrackedDeviceInfo info{};
    if (!get_tracked_device_info_(device_index, &info) || info.is_present == 0u || info.serial[0] == '\0')
    {
        return false;
    }

    const DeviceMetadata resolved_metadata{
        std::string(info.serial),
        static_cast<vr::ETrackedDeviceClass>(info.device_class)};
    RecordActivatedDevice(device_index, resolved_metadata.serial, resolved_metadata.device_class);
    *metadata = resolved_metadata;
    return true;
}

bool ServerDriverHook::ResolveTargetDevice(
    const vr::TrackedDeviceIndex_t device_index, LiveMode* live_mode, std::uint32_t* slot_index)
{
    if (live_mode != nullptr)
    {
        *live_mode = LiveMode::Passthrough;
    }

    if (slot_index != nullptr)
    {
        *slot_index = 0u;
    }

    if (shared_state_ == nullptr || shared_state_->playback_active == 0u)
    {
        return false;
    }

    const LiveMode resolved_mode = static_cast<LiveMode>(shared_state_->live_mode);
    if (resolved_mode == LiveMode::Passthrough)
    {
        return false;
    }

    DeviceMetadata metadata;
    if (!TryGetDeviceMetadata(device_index, &metadata))
    {
        return false;
    }

    const std::wstring target_serial = Utf8ToWide(metadata.serial);
    for (std::uint32_t serial_index = 0; serial_index < shared_state_->serial_count && serial_index < kMaxTrackedSerials; ++serial_index)
    {
        if (_wcsicmp(shared_state_->serials[serial_index].serial, target_serial.c_str()) == 0 &&
            (shared_state_->serials[serial_index].device_class == 0u ||
                shared_state_->serials[serial_index].device_class == static_cast<std::uint32_t>(metadata.device_class)))
        {
            if (live_mode != nullptr)
            {
                *live_mode = resolved_mode;
            }
            if (slot_index != nullptr)
            {
                *slot_index = serial_index;
            }
            return true;
        }
    }

    return false;
}

bool ServerDriverHook::IsDeviceDisabled(const vr::TrackedDeviceIndex_t device_index)
{
    if (shared_state_ == nullptr || shared_state_->disabled_serial_count == 0u)
    {
        return false;
    }

    DeviceMetadata metadata;
    if (!TryGetDeviceMetadata(device_index, &metadata) || metadata.serial.empty())
    {
        return false;
    }

    const std::wstring target_serial = Utf8ToWide(metadata.serial);
    for (std::uint32_t serial_index = 0;
         serial_index < shared_state_->disabled_serial_count && serial_index < kMaxDisabledDeviceSerials;
         ++serial_index)
    {
        if (_wcsicmp(shared_state_->disabled_serials[serial_index].serial, target_serial.c_str()) == 0 &&
            (shared_state_->disabled_serials[serial_index].device_class == 0u ||
                shared_state_->disabled_serials[serial_index].device_class == static_cast<std::uint32_t>(metadata.device_class)))
        {
            return true;
        }
    }

    return false;
}

void ServerDriverHook::UpdateObservedDevicePose(const vr::TrackedDeviceIndex_t device_index, const vr::DriverPose_t& pose)
{
    if (shared_state_ == nullptr)
    {
        return;
    }

    DeviceMetadata metadata;
    if (!TryGetDeviceMetadata(device_index, &metadata) || metadata.serial.empty())
    {
        return;
    }

    if (metadata.serial.rfind("svrcap_replay_slot_", 0) == 0 || metadata.serial.rfind("ktk_", 0) == 0)
    {
        return;
    }

    std::uint32_t slot_index = kMaxObservedDevices;
    for (std::uint32_t index = 0; index < shared_state_->observed_device_count && index < kMaxObservedDevices; ++index)
    {
        if (shared_state_->observed_devices[index].present != 0u &&
            _wcsicmp(shared_state_->observed_devices[index].serial, Utf8ToWide(metadata.serial).c_str()) == 0)
        {
            slot_index = index;
            break;
        }
    }

    if (slot_index >= kMaxObservedDevices)
    {
        slot_index = std::min(shared_state_->observed_device_count, static_cast<std::uint32_t>(kMaxObservedDevices - 1u));
        if (shared_state_->observed_device_count < kMaxObservedDevices)
        {
            shared_state_->observed_device_count = slot_index + 1u;
        }
    }

    ObservedDeviceSlot& observed = shared_state_->observed_devices[slot_index];
    observed.present = 1u;
    observed.device_index = device_index;
    observed.device_class = static_cast<std::uint32_t>(metadata.device_class);
    std::wmemset(observed.serial, 0, kMaxWideSerialCharacters);
    const std::wstring wide_serial = Utf8ToWide(metadata.serial);
    if (!wide_serial.empty())
    {
        const std::size_t copy_length = std::min<std::size_t>(kMaxWideSerialCharacters - 1u, wide_serial.size());
        std::wmemcpy(observed.serial, wide_serial.c_str(), copy_length);
        observed.serial[copy_length] = L'\0';
    }

    LivePoseSlot& live_pose = observed.pose;
    live_pose = LivePoseSlot{};
    live_pose.sample_present = 1u;
    live_pose.device_connected = pose.deviceIsConnected ? 1u : 0u;
    live_pose.pose_valid = pose.poseIsValid ? 1u : 0u;
    live_pose.will_drift_in_yaw = pose.willDriftInYaw ? 1u : 0u;
    live_pose.should_apply_head_model = pose.shouldApplyHeadModel ? 1u : 0u;
    live_pose.tracking_result = static_cast<std::int32_t>(pose.result);
    live_pose.pose_time_offset_s = pose.poseTimeOffset;
    live_pose.position_m[0] = pose.vecPosition[0];
    live_pose.position_m[1] = pose.vecPosition[1];
    live_pose.position_m[2] = pose.vecPosition[2];
    live_pose.rotation_wxyz[0] = pose.qRotation.w;
    live_pose.rotation_wxyz[1] = pose.qRotation.x;
    live_pose.rotation_wxyz[2] = pose.qRotation.y;
    live_pose.rotation_wxyz[3] = pose.qRotation.z;
    live_pose.linear_velocity_mps[0] = pose.vecVelocity[0];
    live_pose.linear_velocity_mps[1] = pose.vecVelocity[1];
    live_pose.linear_velocity_mps[2] = pose.vecVelocity[2];
    live_pose.angular_velocity_rps[0] = pose.vecAngularVelocity[0];
    live_pose.angular_velocity_rps[1] = pose.vecAngularVelocity[1];
    live_pose.angular_velocity_rps[2] = pose.vecAngularVelocity[2];
    live_pose.world_from_driver_rotation_wxyz[0] = pose.qWorldFromDriverRotation.w;
    live_pose.world_from_driver_rotation_wxyz[1] = pose.qWorldFromDriverRotation.x;
    live_pose.world_from_driver_rotation_wxyz[2] = pose.qWorldFromDriverRotation.y;
    live_pose.world_from_driver_rotation_wxyz[3] = pose.qWorldFromDriverRotation.z;
    live_pose.world_from_driver_translation_m[0] = pose.vecWorldFromDriverTranslation[0];
    live_pose.world_from_driver_translation_m[1] = pose.vecWorldFromDriverTranslation[1];
    live_pose.world_from_driver_translation_m[2] = pose.vecWorldFromDriverTranslation[2];
    live_pose.driver_from_head_rotation_wxyz[0] = pose.qDriverFromHeadRotation.w;
    live_pose.driver_from_head_rotation_wxyz[1] = pose.qDriverFromHeadRotation.x;
    live_pose.driver_from_head_rotation_wxyz[2] = pose.qDriverFromHeadRotation.y;
    live_pose.driver_from_head_rotation_wxyz[3] = pose.qDriverFromHeadRotation.z;
    live_pose.driver_from_head_translation_m[0] = pose.vecDriverFromHeadTranslation[0];
    live_pose.driver_from_head_translation_m[1] = pose.vecDriverFromHeadTranslation[1];
    live_pose.driver_from_head_translation_m[2] = pose.vecDriverFromHeadTranslation[2];
}

vr::DriverPose_t ServerDriverHook::BuildDisconnectedPose() const
{
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation.w = 1.0;
    pose.qRotation.w = 1.0;
    pose.result = vr::TrackingResult_Uninitialized;
    pose.poseIsValid = false;
    pose.deviceIsConnected = false;
    pose.willDriftInYaw = false;
    pose.shouldApplyHeadModel = false;
    return pose;
}

vr::DriverPose_t ServerDriverHook::BuildReplacedPose(const LivePoseSlot& live_pose) const
{
    if (live_pose.sample_present == 0u)
    {
        return BuildDisconnectedPose();
    }

    vr::DriverPose_t pose{};
    pose.poseTimeOffset = live_pose.pose_time_offset_s;
    pose.qRotation = NormalizeQuaternion({
        live_pose.rotation_wxyz[0],
        live_pose.rotation_wxyz[1],
        live_pose.rotation_wxyz[2],
        live_pose.rotation_wxyz[3]});
    pose.vecPosition[0] = live_pose.position_m[0];
    pose.vecPosition[1] = live_pose.position_m[1];
    pose.vecPosition[2] = live_pose.position_m[2];
    pose.vecVelocity[0] = live_pose.linear_velocity_mps[0];
    pose.vecVelocity[1] = live_pose.linear_velocity_mps[1];
    pose.vecVelocity[2] = live_pose.linear_velocity_mps[2];
    pose.vecAngularVelocity[0] = live_pose.angular_velocity_rps[0];
    pose.vecAngularVelocity[1] = live_pose.angular_velocity_rps[1];
    pose.vecAngularVelocity[2] = live_pose.angular_velocity_rps[2];
    pose.qWorldFromDriverRotation = NormalizeQuaternion({
        live_pose.world_from_driver_rotation_wxyz[0],
        live_pose.world_from_driver_rotation_wxyz[1],
        live_pose.world_from_driver_rotation_wxyz[2],
        live_pose.world_from_driver_rotation_wxyz[3]});
    pose.vecWorldFromDriverTranslation[0] = live_pose.world_from_driver_translation_m[0];
    pose.vecWorldFromDriverTranslation[1] = live_pose.world_from_driver_translation_m[1];
    pose.vecWorldFromDriverTranslation[2] = live_pose.world_from_driver_translation_m[2];
    pose.qDriverFromHeadRotation = NormalizeQuaternion({
        live_pose.driver_from_head_rotation_wxyz[0],
        live_pose.driver_from_head_rotation_wxyz[1],
        live_pose.driver_from_head_rotation_wxyz[2],
        live_pose.driver_from_head_rotation_wxyz[3]});
    pose.vecDriverFromHeadTranslation[0] = live_pose.driver_from_head_translation_m[0];
    pose.vecDriverFromHeadTranslation[1] = live_pose.driver_from_head_translation_m[1];
    pose.vecDriverFromHeadTranslation[2] = live_pose.driver_from_head_translation_m[2];
    pose.poseIsValid = live_pose.pose_valid != 0u;
    pose.deviceIsConnected = live_pose.device_connected != 0u;
    pose.willDriftInYaw = live_pose.will_drift_in_yaw != 0u;
    pose.shouldApplyHeadModel = live_pose.should_apply_head_model != 0u;
    pose.result = static_cast<vr::ETrackingResult>(live_pose.tracking_result);
    return pose;
}

bool ServerDriverHook::HandleTrackedDeviceAdded(
    vr::IVRServerDriverHost* self,
    const char* serial,
    const vr::ETrackedDeviceClass device_class,
    vr::ITrackedDeviceServerDriver* driver)
{
    if (shared_state_ != nullptr)
    {
        ++shared_state_->tracked_device_add_calls;
    }

    if (original_tracked_device_added_ == nullptr || driver == nullptr)
    {
        return false;
    }

    auto wrapper = std::make_unique<TrackedDeviceProxy>(
        this,
        serial != nullptr ? std::string(serial) : std::string(),
        device_class,
        driver);

    TrackedDeviceProxy* wrapper_raw = wrapper.get();
    const bool added =
        original_tracked_device_added_(self, serial, device_class, wrapper_raw);
    if (added)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        tracked_device_wrappers_.push_back(std::move(wrapper));
    }
    return added;
}

void ServerDriverHook::HandleTrackedDevicePoseUpdated(
    vr::IVRServerDriverHost* self,
    const std::uint32_t which_device,
    const vr::DriverPose_t& pose,
    const std::uint32_t pose_struct_size)
{
    if (shared_state_ != nullptr)
    {
        ++shared_state_->pose_updates_seen;
    }

    UpdateObservedDevicePose(which_device, pose);

    if (original_tracked_device_pose_updated_ == nullptr)
    {
        return;
    }

    if (IsDeviceDisabled(which_device))
    {
        if (shared_state_ != nullptr)
        {
            ++shared_state_->pose_updates_disabled;
            ++shared_state_->pose_updates_suppressed;
        }
        const vr::DriverPose_t disabled_pose = BuildDisconnectedPose();
        original_tracked_device_pose_updated_(self, which_device, disabled_pose, pose_struct_size);
        return;
    }

    LiveMode live_mode = LiveMode::Passthrough;
    std::uint32_t slot_index = 0u;
    if (ResolveTargetDevice(which_device, &live_mode, &slot_index))
    {
        if (live_mode == LiveMode::Replace)
        {
            if (shared_state_ != nullptr)
            {
                ++shared_state_->pose_updates_replaced;
            }
            const vr::DriverPose_t replacement_pose = BuildReplacedPose(shared_state_->live_poses[slot_index]);
            original_tracked_device_pose_updated_(self, which_device, replacement_pose, pose_struct_size);
            return;
        }

        if (shared_state_ != nullptr)
        {
            ++shared_state_->pose_updates_suppressed;
        }
        const vr::DriverPose_t suppressed_pose = BuildDisconnectedPose();
        original_tracked_device_pose_updated_(self, which_device, suppressed_pose, pose_struct_size);
        return;
    }

    original_tracked_device_pose_updated_(self, which_device, pose, pose_struct_size);
}

bool ServerDriverHook::HookedTrackedDeviceAdded(
    vr::IVRServerDriverHost* self,
    const char* serial,
    const vr::ETrackedDeviceClass device_class,
    vr::ITrackedDeviceServerDriver* driver)
{
    return g_server_driver_hook != nullptr
        ? g_server_driver_hook->HandleTrackedDeviceAdded(self, serial, device_class, driver)
        : false;
}

void ServerDriverHook::HookedTrackedDevicePoseUpdated(
    vr::IVRServerDriverHost* self,
    const std::uint32_t which_device,
    const vr::DriverPose_t& pose,
    const std::uint32_t pose_struct_size)
{
    if (g_server_driver_hook != nullptr)
    {
        g_server_driver_hook->HandleTrackedDevicePoseUpdated(self, which_device, pose, pose_struct_size);
    }
}

ServerDriverHook::TrackedDeviceProxy::TrackedDeviceProxy(
    ServerDriverHook* owner,
    std::string serial,
    const vr::ETrackedDeviceClass device_class,
    vr::ITrackedDeviceServerDriver* inner_driver)
    : owner_(owner),
      serial_(std::move(serial)),
      device_class_(device_class),
      inner_driver_(inner_driver)
{
}

vr::EVRInitError ServerDriverHook::TrackedDeviceProxy::Activate(const std::uint32_t object_id)
{
    object_id_ = object_id;
    if (inner_driver_ == nullptr)
    {
        return vr::VRInitError_Driver_Failed;
    }

    const vr::EVRInitError error = inner_driver_->Activate(object_id);
    if (error == vr::VRInitError_None && owner_ != nullptr)
    {
        owner_->RecordActivatedDevice(object_id, serial_, device_class_);
    }
    return error;
}

void ServerDriverHook::TrackedDeviceProxy::EnterStandby()
{
    if (inner_driver_ != nullptr)
    {
        inner_driver_->EnterStandby();
    }
}

void* ServerDriverHook::TrackedDeviceProxy::GetComponent(const char* component_name_and_version)
{
    return inner_driver_ != nullptr ? inner_driver_->GetComponent(component_name_and_version) : nullptr;
}

void ServerDriverHook::TrackedDeviceProxy::DebugRequest(
    const char* request, char* response_buffer, const std::uint32_t response_buffer_size)
{
    if (inner_driver_ != nullptr)
    {
        inner_driver_->DebugRequest(request, response_buffer, response_buffer_size);
    }
}

vr::DriverPose_t ServerDriverHook::TrackedDeviceProxy::GetPose()
{
    if (inner_driver_ == nullptr)
    {
        return owner_ != nullptr ? owner_->BuildDisconnectedPose() : vr::DriverPose_t{};
    }

    const vr::DriverPose_t pose = inner_driver_->GetPose();
    if (owner_ != nullptr)
    {
        owner_->UpdateObservedDevicePose(object_id_, pose);
    }
    if (owner_ != nullptr)
    {
        if (owner_->IsDeviceDisabled(object_id_))
        {
            if (owner_->shared_state_ != nullptr)
            {
                ++owner_->shared_state_->pose_updates_disabled;
                ++owner_->shared_state_->pose_updates_suppressed;
            }
            return owner_->BuildDisconnectedPose();
        }

        LiveMode live_mode = LiveMode::Passthrough;
        std::uint32_t slot_index = 0u;
        if (owner_->ResolveTargetDevice(object_id_, &live_mode, &slot_index))
        {
            if (live_mode == LiveMode::Replace)
            {
                return owner_->BuildReplacedPose(owner_->shared_state_->live_poses[slot_index]);
            }
            return owner_->BuildDisconnectedPose();
        }
    }
    return pose;
}

void ServerDriverHook::TrackedDeviceProxy::Deactivate()
{
    if (owner_ != nullptr)
    {
        owner_->RemoveDevice(object_id_);
    }

    if (inner_driver_ != nullptr)
    {
        inner_driver_->Deactivate();
    }
    object_id_ = vr::k_unTrackedDeviceIndexInvalid;
}
}  // namespace steamvr_capture::hotpatch
