/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VRto3D is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with VRto3D. If not, see <http://www.gnu.org/licenses/>.
 */
#include "hmd_device_driver.h"
#include "key_mappings.h"
#include "driverlog.h"
#include "vrmath.h"
#include "json_manager.h"

#include <string>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <windows.h>
#include <xinput.h>
#include <nlohmann/json.hpp>

// Link the XInput library
#pragma comment(lib, "XInput.lib")

//-----------------------------------------------------------------------------
// Purpose:
// Set a function pointer to the xinput get state call. By default, set it to
// XInputGetState() in whichever xinput we are linked to (xinput9_1_0.dll). If
// the d3dx.ini is using the guide button we will try to switch to either
// xinput 1.3 or 1.4 to get access to the undocumented XInputGetStateEx() call.
// We can't rely on these existing on Win7 though, so if we fail to load them
// don't treat it as fatal and continue using the original one.
//-----------------------------------------------------------------------------
static HMODULE xinput_lib;
typedef DWORD(WINAPI* tXInputGetState)(DWORD dwUserIndex, XINPUT_STATE* pState);
static tXInputGetState _XInputGetState = XInputGetState;
static void SwitchToXinpuGetStateEx()
{
    tXInputGetState XInputGetStateEx;

    if (xinput_lib)
        return;

    // 3DMigoto is linked against xinput9_1_0.dll, but that version does
    // not export XInputGetStateEx to get the guide button. Try loading
    // xinput 1.3 and 1.4, which both support this functionality.
    xinput_lib = LoadLibrary(L"xinput1_3.dll");
    if (xinput_lib) {
        DriverLog("Loaded xinput1_3.dll for guide button support\n");
    }
    else {
        xinput_lib = LoadLibrary(L"xinput1_4.dll");
        if (xinput_lib) {
            DriverLog("Loaded xinput1_4.dll for guide button support\n");
        }
        else {
            DriverLog("ERROR: Unable to load xinput 1.3 or 1.4: Guide button will not be available\n");
            return;
        }
    }

    // Unnamed and undocumented exports FTW
    LPCSTR XInputGetStateExOrdinal = (LPCSTR)100;
    XInputGetStateEx = (tXInputGetState)GetProcAddress(xinput_lib, XInputGetStateExOrdinal);
    if (!XInputGetStateEx) {
        DriverLog("ERROR: Unable to get XInputGetStateEx: Guide button will not be available\n");
        return;
    }

    _XInputGetState = XInputGetStateEx;
}


//-----------------------------------------------------------------------------
// Purpose: Split a string by a delimiter
//-----------------------------------------------------------------------------
std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;

    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }

    return tokens;
}


//-----------------------------------------------------------------------------
// Purpose: Signify Operation Success
//-----------------------------------------------------------------------------
static void BeepSuccess()
{
    // High beep for success
    Beep(1800, 400);
}


// Load settings from default.vrsettings
static const char *stereo_main_settings_section = "driver_vrto3d";
static const char *stereo_display_settings_section = "vrto3d_display";

MockControllerDeviceDriver::MockControllerDeviceDriver()
{
    // Keep track of whether Activate() has been called
    is_active_ = false;
    vr::DriverPose_t curr_pose_ = { 0 };
    app_name_ = "";

    auto* vrs = vr::VRSettings();

    char model_number[ 1024 ];
    vrs->GetString( stereo_main_settings_section, "model_number", model_number, sizeof( model_number ) );
    stereo_model_number_ = model_number;
    char serial_number[ 1024 ];
    vrs->GetString( stereo_main_settings_section, "serial_number", serial_number, sizeof( serial_number ) );
    stereo_serial_number_ = serial_number;

    DriverLog( "VRto3D Model Number: %s", stereo_model_number_.c_str() );
    DriverLog( "VRto3D Serial Number: %s", stereo_serial_number_.c_str() );

    SwitchToXinpuGetStateEx();

    // Display settings
    StereoDisplayDriverConfiguration display_configuration{};
    display_configuration.window_x = 0;
    display_configuration.window_y = 0;

    display_configuration.window_width = vrs->GetInt32( stereo_display_settings_section, "window_width" );
    display_configuration.window_height = vrs->GetInt32( stereo_display_settings_section, "window_height" );
    display_configuration.render_width = vrs->GetInt32(stereo_display_settings_section, "render_width");
    display_configuration.render_height = vrs->GetInt32(stereo_display_settings_section, "render_height");

    display_configuration.hmd_height = vrs->GetFloat(stereo_display_settings_section, "hmd_height");
    
    display_configuration.aspect_ratio = vrs->GetFloat(stereo_display_settings_section, "aspect_ratio");
    display_configuration.fov = vrs->GetFloat(stereo_display_settings_section, "fov");
    display_configuration.depth = vrs->GetFloat(stereo_display_settings_section, "depth");
    display_configuration.convergence = vrs->GetFloat(stereo_display_settings_section, "convergence");
    display_configuration.disable_hotkeys = vrs->GetBool(stereo_display_settings_section, "disable_hotkeys");

    display_configuration.debug_enable = vrs->GetBool(stereo_display_settings_section, "debug_enable");
    display_configuration.tab_enable = vrs->GetBool(stereo_display_settings_section, "tab_enable");
    display_configuration.reverse_enable = vrs->GetBool(stereo_display_settings_section, "reverse_enable");
    display_configuration.depth_gauge = vrs->GetBool(stereo_display_settings_section, "depth_gauge");

    display_configuration.display_latency = vrs->GetFloat(stereo_display_settings_section, "display_latency");
    display_configuration.display_frequency = vrs->GetFloat(stereo_display_settings_section, "display_frequency");
    display_configuration.sleep_count_max = (int)(floor(1600.0 / (1000.0 / display_configuration.display_frequency)));

    // Controller settings
    display_configuration.pitch_enable = vrs->GetBool(stereo_display_settings_section, "pitch_enable");
    display_configuration.yaw_enable = vrs->GetBool(stereo_display_settings_section, "yaw_enable");
    char pose_reset_key[1024];
    vrs->GetString(stereo_display_settings_section, "pose_reset_key", pose_reset_key, sizeof(pose_reset_key));
    if (VirtualKeyMappings.find(pose_reset_key) != VirtualKeyMappings.end()) {
        display_configuration.pose_reset_key = VirtualKeyMappings[pose_reset_key];
        display_configuration.reset_xinput = false;
    }
    else if (XInputMappings.find(pose_reset_key) != XInputMappings.end() || std::string(pose_reset_key).find('+') != std::string::npos) {
        display_configuration.pose_reset_key = 0x0;
        auto hotkeys = split(pose_reset_key, '+');
        for (const auto& hotkey : hotkeys) {
            if (XInputMappings.find(hotkey) != XInputMappings.end()) {
                display_configuration.pose_reset_key |= XInputMappings[hotkey];
            }
        }
        display_configuration.reset_xinput = true;
    }
    display_configuration.pose_reset = false;
    char ctrl_toggle_key[1024];
    vrs->GetString(stereo_display_settings_section, "ctrl_toggle_key", ctrl_toggle_key, sizeof(ctrl_toggle_key));
    if (VirtualKeyMappings.find(ctrl_toggle_key) != VirtualKeyMappings.end()) {
        display_configuration.ctrl_toggle_key = VirtualKeyMappings[ctrl_toggle_key];
        display_configuration.ctrl_xinput = false;
    }
    else if (XInputMappings.find(ctrl_toggle_key) != XInputMappings.end() || std::string(ctrl_toggle_key).find('+') != std::string::npos) {
        display_configuration.ctrl_toggle_key = 0x0;
        auto hotkeys = split(ctrl_toggle_key, '+');
        for (const auto& hotkey : hotkeys) {
            if (XInputMappings.find(hotkey) != XInputMappings.end()) {
                display_configuration.ctrl_toggle_key |= XInputMappings[hotkey];
            }
        }
        display_configuration.ctrl_xinput = true;
    }
    char ctrl_toggle_type[1024];
    vrs->GetString(stereo_display_settings_section, "ctrl_toggle_type", ctrl_toggle_type, sizeof(ctrl_toggle_type));
    display_configuration.ctrl_type = KeyBindTypes[ctrl_toggle_type];
    display_configuration.pitch_radius = vrs->GetFloat(stereo_display_settings_section, "pitch_radius");
    display_configuration.ctrl_deadzone = vrs->GetFloat(stereo_display_settings_section, "ctrl_deadzone");
    display_configuration.ctrl_sensitivity = vrs->GetFloat(stereo_display_settings_section, "ctrl_sensitivity");

    // Read user binds
    display_configuration.num_user_settings = vrs->GetInt32(stereo_display_settings_section, "num_user_settings");
    display_configuration.user_load_key.resize(display_configuration.num_user_settings);
    display_configuration.user_store_key.resize(display_configuration.num_user_settings);
    display_configuration.user_key_type.resize(display_configuration.num_user_settings);
    display_configuration.user_depth.resize(display_configuration.num_user_settings);
    display_configuration.user_convergence.resize(display_configuration.num_user_settings);
    display_configuration.prev_depth.resize(display_configuration.num_user_settings);
    display_configuration.prev_convergence.resize(display_configuration.num_user_settings);
    display_configuration.was_held.resize(display_configuration.num_user_settings);
    display_configuration.load_xinput.resize(display_configuration.num_user_settings);
    display_configuration.sleep_count.resize(display_configuration.num_user_settings);
    for (int i = 0; i < display_configuration.num_user_settings; i++)
    {
        char user_key[1024];
        auto si = std::to_string(i + 1);
        vrs->GetString(stereo_display_settings_section, ("user_load_key" + si).c_str(), user_key, sizeof(user_key));
        if (VirtualKeyMappings.find(user_key) != VirtualKeyMappings.end()) {
            display_configuration.user_load_key[i] = VirtualKeyMappings[user_key];
            display_configuration.load_xinput[i] = false;
        }
        else if (XInputMappings.find(user_key) != XInputMappings.end() || std::string(user_key).find('+') != std::string::npos) {
            display_configuration.user_load_key[i] = 0x0;
            auto hotkeys = split(user_key, '+');
            for (const auto& hotkey : hotkeys) {
                if (XInputMappings.find(hotkey) != XInputMappings.end()) {
                    display_configuration.user_load_key[i] |= XInputMappings[hotkey];
                }
            }
            display_configuration.load_xinput[i] = true;
        }
        vrs->GetString(stereo_display_settings_section, ("user_store_key" + si).c_str(), user_key, sizeof(user_key));
        if (VirtualKeyMappings.find(user_key) != VirtualKeyMappings.end()) {
            display_configuration.user_store_key[i] = VirtualKeyMappings[user_key];
        }
        vrs->GetString(stereo_display_settings_section, ("user_key_type" + si).c_str(), user_key, sizeof(user_key));
        if (KeyBindTypes.find(user_key) != KeyBindTypes.end()) {
            display_configuration.user_key_type[i] = KeyBindTypes[user_key];
        }
        display_configuration.user_depth[i] = vrs->GetFloat(stereo_display_settings_section, ("user_depth" + si).c_str());
        display_configuration.user_convergence[i] = vrs->GetFloat(stereo_display_settings_section, ("user_convergence" + si).c_str());
    }

    // Instantiate our display component
    stereo_display_component_ = std::make_unique< StereoDisplayComponent >( display_configuration );

    DriverLog("Default Config Loaded\n");
}

//-----------------------------------------------------------------------------
// Purpose: Initialize all settings and notify SteamVR
//-----------------------------------------------------------------------------
vr::EVRInitError MockControllerDeviceDriver::Activate( uint32_t unObjectId )
{
    device_index_ = unObjectId;
    is_active_ = true;
    is_on_top_ = false;

    // A list of properties available is contained in vr::ETrackedDeviceProperty.
    auto* vrp = vr::VRProperties();
    auto* vrs = vr::VRSettings();
    vr::PropertyContainerHandle_t container = vrp->TrackedDeviceToPropertyContainer( device_index_ );
    vrp->SetStringProperty( container, vr::Prop_ModelNumber_String, stereo_model_number_.c_str() );
    vrp->SetStringProperty( container, vr::Prop_ManufacturerName_String, "VRto3D");
    vrp->SetStringProperty( container, vr::Prop_TrackingFirmwareVersion_String, "1.0");
    vrp->SetStringProperty( container, vr::Prop_HardwareRevision_String, "1.0");

    // Display settings
    vrp->SetFloatProperty( container, vr::Prop_UserIpdMeters_Float, stereo_display_component_->GetConfig().depth);
    vrp->SetFloatProperty( container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
    vrp->SetFloatProperty( container, vr::Prop_DisplayFrequency_Float, stereo_display_component_->GetConfig().display_frequency * 1.5f );
    vrp->SetFloatProperty( container, vr::Prop_SecondsFromVsyncToPhotons_Float, stereo_display_component_->GetConfig().display_latency);
    vrp->SetFloatProperty( container, vr::Prop_SecondsFromPhotonsToVblank_Float, 0.0);
    vrp->SetBoolProperty( container, vr::Prop_ReportsTimeSinceVSync_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_IsOnDesktop_Bool, !stereo_display_component_->GetConfig().debug_enable);
    vrp->SetBoolProperty( container, vr::Prop_DisplayDebugMode_Bool, stereo_display_component_->GetConfig().debug_enable);
    vrp->SetBoolProperty( container, vr::Prop_HasDriverDirectModeComponent_Bool, false);
    if (stereo_display_component_->GetConfig().depth_gauge)
    {
        vrp->SetFloatProperty(container, vr::Prop_DashboardScale_Float, 1.0f);
    }
    else
    {
        vrp->SetFloatProperty(container, vr::Prop_DashboardScale_Float, 0.0f);
    }

    // Set the chaperone JSON property
    // Get the current time
    std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_s(&tm, &t);
    // Construct the JSON string with variables
    std::stringstream ss;
    ss << R"(
        {
           "jsonid" : "chaperone_info",
           "universes" : [
              {
                 "collision_bounds" : [
                    [
                       [ -1.0, 0.0, -1.0 ],
                       [ -1.0, 3.0, -1.0 ],
                       [ -1.0, 3.0, 1.0 ],
                       [ -1.0, 0.0, 1.0 ]
                    ],
                    [
                       [ -1.0, 0.0, 1.0 ],
                       [ -1.0, 3.0, 1.0 ],
                       [ 1.0, 3.0, 1.0 ],
                       [ 1.0, 0.0, 1.0 ]
                    ],
                    [
                       [ 1.0, 0.0, 1.0 ],
                       [ 1.0, 3.0, 1.0 ],
                       [ 1.0, 3.0, -1.0 ],
                       [ 1.0, 0.0, -1.0 ]
                    ],
                    [
                       [ 1.0, 0.0, -1.0 ],
                       [ 1.0, 3.0, -1.0 ],
                       [ -1.0, 3.0, -1.0 ],
                       [ -1.0, 0.0, -1.0 ]
                    ]
                 ],
                 "play_area" : [ 2.0, 2.0 ],
                 "seated" : {
                    "translation" : [ 0.0, 0.5, 0.0 ],
                    "yaw" : 0.0
                 },
                 "standing" : {
                    "translation" : [ 0.0, 1.0, 0.0 ],
                    "yaw" : 0.0
                 },
                 "time" : ")" << std::put_time(&tm, "%a %b %d %H:%M:%S %Y") << R"(",
                 "universeID" : "64"
              }
           ],
           "version" : 5
        }
        )";
    // Convert the stringstream to a string
    std::string chaperoneJson = ss.str();
    // Set the chaperone JSON property
    vrp->SetStringProperty(container, vr::Prop_DriverProvidedChaperoneJson_String, chaperoneJson.c_str());
    vrp->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, 64);
    vrs->SetInt32(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_Style_Int32, vr::COLLISION_BOUNDS_STYLE_NONE);
    vrs->SetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_GroundPerimeterOn_Bool, false);

    // Miscellaneous settings
    vrp->SetBoolProperty( container, vr::Prop_WillDriftInYaw_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_DeviceIsWireless_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_DeviceIsCharging_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_ContainsProximitySensor_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_DeviceCanPowerOff_Bool, false);

    // set proximity senser to always on, always head present
    vr::VRInputComponentHandle_t  prox;
    vr::VRDriverInput()->CreateBooleanComponent(container, "/proximity", &prox);
    vr::VRDriverInput()->UpdateBooleanComponent(prox, true, 0.0);
    
    // Miscellaneous settings
    vrs->SetBool(vr::k_pch_DirectMode_Section, vr::k_pch_DirectMode_Enable_Bool, false);
    vrs->SetFloat(vr::k_pch_Power_Section, vr::k_pch_Power_TurnOffScreensTimeout_Float, 86400.0f);
    vrs->SetBool(vr::k_pch_Power_Section, vr::k_pch_Power_PauseCompositorOnStandby_Bool, false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, vr::k_pch_Dashboard_EnableDashboard_Bool, false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, vr::k_pch_Dashboard_ArcadeMode_Bool, true);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "allowAppQuitting", false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "autoShowGameTheater", false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "showDesktop", false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "showPowerOptions", false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "inputCaptureEnabled", false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_EnableHomeApp, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_MirrorViewVisibility_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_EnableSafeMode, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_DisplayDebug_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_MotionSmoothing_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_DisableAsyncReprojection_Bool, true);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_AllowSupersampleFiltering_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_SupersampleManualOverride_Bool, true);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_ForceFadeOnBadTracking_Bool, false);
    
    pose_thread_ = std::thread(&MockControllerDeviceDriver::PoseUpdateThread, this);
    hotkey_thread_ = std::thread(&MockControllerDeviceDriver::PollHotkeysThread, this);
    focus_thread_ = std::thread(&MockControllerDeviceDriver::FocusUpdateThread, this);

    HANDLE thread_handle = pose_thread_.native_handle();

    // Set the thread priority
    if (!SetThreadPriority(thread_handle, THREAD_PRIORITY_HIGHEST)) {
        // Handle error if setting priority fails
        DriverLog("Failed to set thread priority: %d\n", GetLastError());
    }

    DriverLog("Activation Complete\n");

    return vr::VRInitError_None;
}

//-----------------------------------------------------------------------------
// Purpose: Return StereoDisplayComponent as vr::IVRDisplayComponent
//-----------------------------------------------------------------------------
void *MockControllerDeviceDriver::GetComponent( const char *pchComponentNameAndVersion )
{
    if ( strcmp( pchComponentNameAndVersion, vr::IVRDisplayComponent_Version ) == 0 )
    {
        return stereo_display_component_.get();
    }

    return nullptr;
}

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver when a debug request has been made from an application to the driver.
// What is in the response and request is up to the application and driver to figure out themselves.
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize )
{
    if ( unResponseBufferSize >= 1 )
        pchResponseBuffer[ 0 ] = 0;
}


//-----------------------------------------------------------------------------
// Purpose: Static Pose with pitch & yaw adjustment
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::PoseUpdateThread()
{
    static float currentPitch = 0.0f; // Keep track of the current pitch
    static vr::HmdQuaternion_t currentYawQuat = { 1.0f, 0.0f, 0.0f, 0.0f }; // Initial yaw quaternion
    static float lastPitch = 0.0f;
    static float lastYaw = 0.0f;
    static vr::DriverPose_t lastPose = { 0 };

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (is_active_)
    {
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto deltaTime = std::chrono::duration_cast<std::chrono::duration<float>>(currentTime - lastTime).count();
        lastTime = currentTime;

        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(XINPUT_STATE));
        bool got_xinput = (_XInputGetState(0, &state) == ERROR_SUCCESS);

        auto config = stereo_display_component_->GetConfig();

        vr::DriverPose_t pose = { 0 };

        pose.qWorldFromDriverRotation = HmdQuaternion_Identity;
        pose.qDriverFromHeadRotation = HmdQuaternion_Identity;
        pose.qRotation = HmdQuaternion_Identity;

        // Adjust pitch based on controller input
        if (config.pitch_enable && got_xinput)
        {
            float normalizedY = state.Gamepad.sThumbRY / 32767.0f;

            // Apply deadzone
            if (std::abs(normalizedY) < config.ctrl_deadzone)
            {
                normalizedY = 0.0f;
            }
            else
            {
                if (normalizedY > 0)
                    normalizedY = (normalizedY - config.ctrl_deadzone) / (1.0f - config.ctrl_deadzone);
                else
                    normalizedY = (normalizedY + config.ctrl_deadzone) / (1.0f - config.ctrl_deadzone);
            }

            // Scale Pitch
            currentPitch += (normalizedY * config.ctrl_sensitivity);
            if (currentPitch > 90.0f) currentPitch = 90.0f;
            if (currentPitch < -90.0f) currentPitch = -90.0f;
        }

        // Adjust yaw based on controller input
        if (config.yaw_enable && got_xinput)
        {
            float normalizedX = state.Gamepad.sThumbRX / 32767.0f;

            // Apply deadzone
            if (std::abs(normalizedX) < config.ctrl_deadzone)
            {
                normalizedX = 0.0f;
            }
            else
            {
                if (normalizedX > 0)
                    normalizedX = (normalizedX - config.ctrl_deadzone) / (1.0f - config.ctrl_deadzone);
                else
                    normalizedX = (normalizedX + config.ctrl_deadzone) / (1.0f - config.ctrl_deadzone);
            }

            // Scale Yaw
            float yawAdjustment = -normalizedX * config.ctrl_sensitivity;

            // Create a quaternion for the yaw adjustment
            vr::HmdQuaternion_t yawQuatAdjust = QuaternionFromAxisAngle(0.0f, 1.0f, 0.0f, DEG_TO_RAD(yawAdjustment));

            // Update the current yaw quaternion
            currentYawQuat = HmdQuaternion_Normalize(yawQuatAdjust * currentYawQuat);
        }

        // Reset Pose to origin
        if (config.pose_reset)
        {
            currentPitch = 0.0f;
            currentYawQuat = { 1.0f, 0.0f, 0.0f, 0.0f };
            stereo_display_component_->SetReset();
        }

        float pitchRadians = DEG_TO_RAD(currentPitch);
        float yawRadians = 2.0f * acos(currentYawQuat.w);

        // Recompose the rotation quaternion from pitch and yaw
        vr::HmdQuaternion_t pitchQuaternion = QuaternionFromAxisAngle(1.0f, 0.0f, 0.0f, pitchRadians);
        pose.qRotation = HmdQuaternion_Normalize(currentYawQuat * pitchQuaternion);

        // Calculate the new position relative to the current pitch & yaw
        pose.vecPosition[0] = config.pitch_radius * cos(pitchRadians) * sin(yawRadians) - config.pitch_radius * sin(yawRadians);
        pose.vecPosition[1] = config.hmd_height - config.pitch_radius * sin(pitchRadians);
        pose.vecPosition[2] = config.pitch_radius * cos(pitchRadians) * cos(yawRadians) - config.pitch_radius * cos(yawRadians);
        if (pose.vecPosition[1] < 0.0)
        {
            pose.vecPosition[1] = 0.0;
        }

        // Calculate velocity using known update interval
        pose.vecVelocity[0] = (pose.vecPosition[0] - lastPose.vecPosition[0]) / deltaTime;
        pose.vecVelocity[1] = (pose.vecPosition[1] - lastPose.vecPosition[1]) / deltaTime;
        pose.vecVelocity[2] = (pose.vecPosition[2] - lastPose.vecPosition[2]) / deltaTime;
        pose.vecAngularVelocity[0] = AngleDifference(pitchRadians, lastPitch) / deltaTime; // Pitch angular velocity
        pose.vecAngularVelocity[1] = AngleDifference(yawRadians, lastYaw) / deltaTime; // Yaw angular velocity
        pose.vecAngularVelocity[2] = 0.0f;

        // Calculate acceleration based on change in velocity
        pose.vecAcceleration[0] = (pose.vecVelocity[0] - lastPose.vecVelocity[0]) / deltaTime;
        pose.vecAcceleration[1] = (pose.vecVelocity[1] - lastPose.vecVelocity[1]) / deltaTime;
        pose.vecAcceleration[2] = (pose.vecVelocity[2] - lastPose.vecVelocity[2]) / deltaTime;
        pose.vecAngularAcceleration[0] = (pose.vecAngularVelocity[0] - lastPose.vecAngularVelocity[0]) / deltaTime;
        pose.vecAngularAcceleration[1] = (pose.vecAngularVelocity[1] - lastPose.vecAngularVelocity[1]) / deltaTime;
        pose.vecAngularAcceleration[2] = 0.0f;

        pose.poseIsValid = true;
        pose.deviceIsConnected = true;
        pose.result = vr::TrackingResult_Running_OK;
        pose.shouldApplyHeadModel = false;
        pose.willDriftInYaw = false;
        pose.poseTimeOffset = 0;

        // Update the pose
        pose_mutex_.lock();
        curr_pose_ = pose;
        pose_mutex_.unlock();

        // Update for next iteration
        lastPitch = pitchRadians;
        lastYaw = yawRadians;
        lastPose = pose;

        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(device_index_, pose, sizeof(vr::DriverPose_t));

        // XInput polling limit is 125Hz
        auto endTime = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - currentTime);
        std::this_thread::sleep_for(std::chrono::milliseconds(8) - elapsed);
    }
}


//-----------------------------------------------------------------------------
// Purpose: Return current pose
//-----------------------------------------------------------------------------
vr::DriverPose_t MockControllerDeviceDriver::GetPose()
{
    std::lock_guard<std::mutex> lock(pose_mutex_);
    return curr_pose_;
}


//-----------------------------------------------------------------------------
// Purpose: Update HMD position, Depth, Convergence, and user binds
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::PollHotkeysThread()
{
    static int sleep_time = (int)(floor(1000.0 / stereo_display_component_->GetConfig().display_frequency));
    static int height_sleep = 0;
    static int top_sleep = 0;
    static int save_sleep = 0;

    while (is_active_)
    {
        if (!stereo_display_component_->GetConfig().disable_hotkeys) {
            // Ctrl+F3 Decrease Depth
            if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F3) & 0x8000)) {
                stereo_display_component_->AdjustDepth(-0.001f, true, device_index_);
            }
            // Ctrl+F4 Increase Depth
            else if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F4) & 0x8000)) {
                stereo_display_component_->AdjustDepth(0.001f, true, device_index_);
            }
            // Ctrl+F5 Decrease Convergence
            if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F5) & 0x8000)) {
                stereo_display_component_->AdjustConvergence(-0.001f, true, device_index_);
            }
            // Ctrl+F6 Increase Convergence
            else if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F6) & 0x8000)) {
                stereo_display_component_->AdjustConvergence(0.001f, true, device_index_);
            }
            // Ctrl+F7 Store settings into game profile
            if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F7) & 0x8000) && save_sleep == 0) {
                save_sleep = stereo_display_component_->GetConfig().sleep_count_max;
                SaveSettings();
            }
            // Ctrl+F10 Reload settings from default.vrsettings
            else if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F10) & 0x8000) && save_sleep == 0) {
                save_sleep = stereo_display_component_->GetConfig().sleep_count_max;
                stereo_display_component_->LoadDefaults(device_index_);
            }
            else if (save_sleep > 0) {
                save_sleep--;
            }
        }
        // Ctrl+F8 Toggle Always On Top
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F8) & 0x8000) && top_sleep == 0) {
            top_sleep = stereo_display_component_->GetConfig().sleep_count_max;
            is_on_top_ = !is_on_top_;
        }
        else if (top_sleep > 0) {
            top_sleep--;
        }
        // Ctrl+F9 Toggle HMD height
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F9) & 0x8000) && height_sleep == 0) {
            height_sleep = stereo_display_component_->GetConfig().sleep_count_max;
            stereo_display_component_->SetHeight();
        }
        else if (height_sleep > 0) {
            height_sleep--;
        }
        // Ctrl+- Decrease Sensitivity
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000)) {
            stereo_display_component_->AdjustSensitivity(-0.01f);
        }
        // Ctrl++ Increase Sensitivity
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000)) {
            stereo_display_component_->AdjustSensitivity(0.01f);
        }
        // Ctrl+[ Decrease Pitch Radius
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_OEM_4) & 0x8000)) {
            stereo_display_component_->AdjustRadius(-0.01f);
        }
        // Ctrl+] Increase Pitch Radius
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_OEM_6) & 0x8000)) {
            stereo_display_component_->AdjustRadius(0.01f);
        }

        // Check User binds
        stereo_display_component_->CheckUserSettings(device_index_);

        // Sleep for ~ 1 frame
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
    }
}


//-----------------------------------------------------------------------------
// Purpose: Keep Headset Window on top if set
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::FocusUpdateThread()
{
    static int sleep_time = 1000;
    static HWND vr_window = NULL;
    static HWND top_window = NULL;

    while (is_active_)
    {
        // Keep VR display always on top for 3D rendering
        if (is_on_top_) {
            top_window = GetTopWindow(GetDesktopWindow());
            if (vr_window != NULL && vr_window != top_window) {
                SetWindowPos(vr_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
            else if (vr_window == NULL) {
                vr_window = FindWindow(NULL, L"Headset Window");
            }
        }
        else if (vr_window != NULL) {
            SetWindowPos(vr_window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            vr_window = NULL;
        }

        // Sleep for 1s
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
    }
}


//-----------------------------------------------------------------------------
// Purpose: Load Game Specific Settings from Documents\My games\vrto3d\app_name_config.json
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::LoadSettings(const std::string& app_name)
{
    if (app_name != app_name_)
    {
        app_name_ = app_name;
        stereo_display_component_->LoadSettings(app_name, device_index_);
    }
}


//-----------------------------------------------------------------------------
// Purpose: Save Game Specific Settings to Documents\My games\vrto3d\app_name_config.json
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::SaveSettings() {
    // Create a JSON object to hold all the configuration data
    nlohmann::json jsonConfig;
    auto config = stereo_display_component_->GetConfig();

    // Populate the JSON object with settings
    jsonConfig["depth"] = stereo_display_component_->GetDepth();
    jsonConfig["convergence"] = stereo_display_component_->GetConvergence();
    jsonConfig["hmd_height"] = config.hmd_height;
    jsonConfig["pitch_enable"] = config.pitch_enable;
    jsonConfig["yaw_enable"] = config.yaw_enable;
    jsonConfig["pose_reset_key"] = config.pose_reset_key;
    jsonConfig["reset_xinput"] = config.reset_xinput;
    jsonConfig["ctrl_toggle_key"] = config.ctrl_toggle_key;
    jsonConfig["ctrl_toggle_type"] = config.ctrl_type;
    jsonConfig["ctrl_xinput"] = config.ctrl_xinput;
    jsonConfig["pitch_radius"] = config.pitch_radius;
    jsonConfig["ctrl_deadzone"] = config.ctrl_deadzone;
    jsonConfig["ctrl_sensitivity"] = config.ctrl_sensitivity;
    jsonConfig["num_user_settings"] = config.num_user_settings;

    // Store user settings as an array
    for (int i = 0; i < config.num_user_settings; i++) {
        nlohmann::json userSettings;
        userSettings["user_load_key"] = config.user_load_key[i];
        userSettings["user_store_key"] = config.user_store_key[i];
        userSettings["user_key_type"] = config.user_key_type[i];
        userSettings["user_depth"] = config.user_depth[i];
        userSettings["user_convergence"] = config.user_convergence[i];
        userSettings["load_xinput"] = config.load_xinput[i];

        // Append to JSON array in the main config
        jsonConfig["user_settings"].push_back(userSettings);
    }

    // Now write this JSON object to a file
    JsonManager json_manager;
    if (json_manager.writeJsonToFile(app_name_ + "_config.json", jsonConfig)) {
        DriverLog("Settings saved to %s profile\n", app_name_.c_str());
        BeepSuccess();
    }
}


//-----------------------------------------------------------------------------
// Purpose: Stub for Standby mode
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::EnterStandby()
{
    DriverLog( "HMD has been put into standby." );
}

//-----------------------------------------------------------------------------
// Purpose: Shutdown process
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::Deactivate()
{
    if ( is_active_.exchange( false ) )
    {
        pose_thread_.join();
        hotkey_thread_.join();
        focus_thread_.join();
    }

    // unassign our controller index (we don't want to be calling vrserver anymore after Deactivate() has been called
    device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}


//-----------------------------------------------------------------------------
// DISPLAY DRIVER METHOD DEFINITIONS
//-----------------------------------------------------------------------------

StereoDisplayComponent::StereoDisplayComponent( const StereoDisplayDriverConfiguration &config )
    : config_( config ), def_config_(config), depth_(config.depth), convergence_(config.convergence)
{
}

//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor if this display is considered an on-desktop display.
//-----------------------------------------------------------------------------
bool StereoDisplayComponent::IsDisplayOnDesktop()
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    return !config_.debug_enable;
}

//-----------------------------------------------------------------------------
// Purpose: To as vrcompositor to search for this display.
//-----------------------------------------------------------------------------
bool StereoDisplayComponent::IsDisplayRealDisplay()
{
    return false;
}

//-----------------------------------------------------------------------------
// Purpose: To inform the rest of the vr system what the recommended target size should be
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    *pnWidth = config_.render_width;
    *pnHeight = config_.render_height;
}

//-----------------------------------------------------------------------------
// Purpose: Render in SbS or TaB Stereo3D
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetEyeOutputViewport( vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    if (config_.reverse_enable)
    {
        eEye = static_cast<vr::EVREye>(!static_cast<bool> (eEye));
    }
    // Use Top and Bottom Rendering
    if (config_.tab_enable)
    {
        *pnX = 0;
        // Each eye will have full width
        *pnWidth = config_.window_width;
        // Each eye will have half height
        *pnHeight = config_.window_height / 2;
        if (eEye == vr::Eye_Left)
        {
            // Left eye viewport on the top half of the window
            *pnY = 0;
        }
        else
        {
            // Right eye viewport on the bottom half of the window
            *pnY = config_.window_height / 2;
        }
    }

    // Use Side by Side Rendering
    else
    {
        *pnY = 0;
        // Each eye will have half width
        *pnWidth = config_.window_width / 2;
        // Each eye will have full height
        *pnHeight = config_.window_height;
        if (eEye == vr::Eye_Left)
        {
            // Left eye viewport on the left half of the window
            *pnX = 0;
        }
        else
        {
            // Right eye viewport on the right half of the window
            *pnX = config_.window_width / 2;
        }
    }
}

//-----------------------------------------------------------------------------
// Purpose: Utilize the desired FoV, Aspect Ratio, and Convergence settings
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    // Convert horizontal FOV from degrees to radians
    float horFovRadians = tan((config_.fov * (M_PI / 180.0f)) / 2);

    // Calculate the vertical FOV in radians
    float verFovRadians = tan(atan(horFovRadians / config_.aspect_ratio));

    // Get convergence value
    float convergence = GetConvergence();

    // Calculate the raw projection values
    *pfTop = -verFovRadians;
    *pfBottom = verFovRadians;

    // Adjust the frustum based on the eye
    if (eEye == vr::Eye_Left) {
        *pfLeft = -horFovRadians + convergence;
        *pfRight = horFovRadians + convergence;
    }
    else {
        *pfLeft = -horFovRadians - convergence;
        *pfRight = horFovRadians - convergence;
    }
}

//-----------------------------------------------------------------------------
// Purpose: Don't distort any coordinates for Stereo3D
//-----------------------------------------------------------------------------
vr::DistortionCoordinates_t StereoDisplayComponent::ComputeDistortion( vr::EVREye eEye, float fU, float fV )
{
    vr::DistortionCoordinates_t coordinates{};
    coordinates.rfBlue[ 0 ] = fU;
    coordinates.rfBlue[ 1 ] = fV;
    coordinates.rfGreen[ 0 ] = fU;
    coordinates.rfGreen[ 1 ] = fV;
    coordinates.rfRed[ 0 ] = fU;
    coordinates.rfRed[ 1 ] = fV;
    return coordinates;
}
bool StereoDisplayComponent::ComputeInverseDistortion(vr::HmdVector2_t* pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV)
{
    return false;
}

//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor what the window bounds for this virtual HMD are.
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    *pnX = config_.window_x;
    *pnY = config_.window_y;
    *pnWidth = config_.window_width;
    *pnHeight = config_.window_height;
}

//-----------------------------------------------------------------------------
// Purpose: To provide access to settings
//-----------------------------------------------------------------------------
StereoDisplayDriverConfiguration StereoDisplayComponent::GetConfig()
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    return config_;
}


//-----------------------------------------------------------------------------
// Purpose: To update the Depth value
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustDepth(float new_depth, bool is_delta, uint32_t device_index)
{
    float cur_depth = GetDepth();
    if (is_delta)
        new_depth += cur_depth;
    while (!depth_.compare_exchange_weak(cur_depth, new_depth, std::memory_order_relaxed));
    vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(device_index);
    vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, new_depth);
}


//-----------------------------------------------------------------------------
// Purpose: To update the Convergence value
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustConvergence(float new_conv, bool is_delta, uint32_t device_index)
{
    float cur_conv = GetConvergence();
    if (is_delta)
        new_conv += cur_conv;
    if (cur_conv == new_conv)
        return;
    while (!convergence_.compare_exchange_weak(cur_conv, new_conv, std::memory_order_relaxed));
    // Regenerate the Projection
    vr::HmdRect2_t eyeLeft, eyeRight;
    GetProjectionRaw(vr::Eye_Left, &eyeLeft.vTopLeft.v[0], &eyeLeft.vBottomRight.v[0], &eyeLeft.vTopLeft.v[1], &eyeLeft.vBottomRight.v[1]);
    GetProjectionRaw(vr::Eye_Right, &eyeRight.vTopLeft.v[0], &eyeRight.vBottomRight.v[0], &eyeRight.vTopLeft.v[1], &eyeRight.vBottomRight.v[1]);
    vr::VREvent_Data_t temp;
    vr::VRServerDriverHost()->SetDisplayProjectionRaw(device_index, eyeLeft, eyeRight);
    vr::VRServerDriverHost()->VendorSpecificEvent(device_index, vr::VREvent_LensDistortionChanged, temp, 0.0f);
}


//-----------------------------------------------------------------------------
// Purpose: Get Depth value
//-----------------------------------------------------------------------------
float StereoDisplayComponent::GetDepth()
{
    return depth_.load(std::memory_order_relaxed);
}


//-----------------------------------------------------------------------------
// Purpose: Get Convergence value
//-----------------------------------------------------------------------------
float StereoDisplayComponent::GetConvergence()
{
    return convergence_.load(std::memory_order_relaxed);
}


//-----------------------------------------------------------------------------
// Purpose: Check User Settings and act on them
//-----------------------------------------------------------------------------
void StereoDisplayComponent::CheckUserSettings(uint32_t device_index)
{
    static bool pitch_set = config_.pitch_enable;
    static bool yaw_set = config_.yaw_enable;
    static int sleep_ctrl = 0;
    static int sleep_rest = 0;
    
    // Get the state of the first controller (index 0)
    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(XINPUT_STATE));
    bool got_xinput = (_XInputGetState(0, &state) == ERROR_SUCCESS);

    DWORD xstate = 0x00000000;
    xstate = state.Gamepad.wButtons;
    if (state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        xstate |= XINPUT_GAMEPAD_LEFT_TRIGGER;
    }
    if (state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        xstate |= XINPUT_GAMEPAD_RIGHT_TRIGGER;
    }

    auto config = GetConfig();

    // Toggle Pitch and Yaw control
    if ((config.ctrl_xinput && got_xinput &&
        ((xstate & config.ctrl_toggle_key) == config.ctrl_toggle_key))
        || (!config.ctrl_xinput && (GetAsyncKeyState(config.ctrl_toggle_key) & 0x8000)))
    {
        if (config.ctrl_type == HOLD && !config.ctrl_held)
        {
            config.ctrl_held = true;
            config.pitch_enable = false;
            config.yaw_enable = false;
        }
        else if ((config.ctrl_type == TOGGLE || config.ctrl_type == SWITCH) && sleep_ctrl < 1)
        {
            sleep_ctrl = config.sleep_count_max;
            if (pitch_set) {
                config.pitch_enable = !config.pitch_enable;
            }
            if (yaw_set) {
                config.yaw_enable = !config.yaw_enable;
            }
        }
    }
    else if (config.ctrl_type == HOLD && config.ctrl_held)
    {
        config.ctrl_held = false;
        config.pitch_enable = pitch_set;
        config.yaw_enable = yaw_set;
    }
    if (sleep_ctrl > 0) {
        sleep_ctrl--;
    }

    // Reset HMD position
    if (((config.reset_xinput && got_xinput &&
        ((xstate & config.pose_reset_key) == config.pose_reset_key))
        || (!config.reset_xinput && (GetAsyncKeyState(config.pose_reset_key) & 0x8000)))
        && sleep_rest == 0)
    {
        sleep_rest = config.sleep_count_max;
        if (!config.pose_reset) {
            config.pose_reset = true;
        }
    }
    else if (sleep_rest > 0) {
        sleep_rest--;
    }

    for (int i = 0; i < config.num_user_settings; i++)
    {
        // Decrement the sleep count if it's greater than zero
        if (config.sleep_count[i] > 0)
            config.sleep_count[i]--;

        // Load stored depth & convergence
        if ((config.load_xinput[i] && got_xinput &&
            ((xstate & config.user_load_key[i]) == config.user_load_key[i]))
            || (!config.load_xinput[i] && (GetAsyncKeyState(config.user_load_key[i]) & 0x8000)))
        {
            if (config.user_key_type[i] == HOLD && !config.was_held[i])
            {
                config.prev_depth[i] = GetDepth();
                config.prev_convergence[i] = GetConvergence();
                config.was_held[i] = true;
                AdjustDepth(config.user_depth[i], false, device_index);
                AdjustConvergence(config.user_convergence[i], false, device_index);
            }
            else if (config.user_key_type[i] == TOGGLE && config.sleep_count[i] < 1)
            {
                config.sleep_count[i] = config.sleep_count_max;
                if (GetDepth() == config.user_depth[i] && GetConvergence() == config.user_convergence[i])
                {
                    // If the current state matches the user settings, revert to the previous state
                    AdjustDepth(config.prev_depth[i], false, device_index);
                    AdjustConvergence(config.prev_convergence[i], false, device_index);
                }
                else
                {
                    // Save the current state and apply the user settings
                    config.prev_depth[i] = GetDepth();
                    config.prev_convergence[i] = GetConvergence();
                    AdjustDepth(config.user_depth[i], false, device_index);
                    AdjustConvergence(config.user_convergence[i], false, device_index);
                }
            }
            else if (config.user_key_type[i] == SWITCH)
            {
                AdjustDepth(config.user_depth[i], false, device_index);
                AdjustConvergence(config.user_convergence[i], false, device_index);
            }
        }
        // Release depth & convergence back to normal for HOLD key type
        else if (config.user_key_type[i] == HOLD && config.was_held[i])
        {
            config.was_held[i] = false;
            AdjustDepth(config.prev_depth[i], false, device_index);
            AdjustConvergence(config.prev_convergence[i], false, device_index);
        }

        // Store current depth & convergence to user setting
        if ((GetAsyncKeyState(config.user_store_key[i]) & 0x8000))
        {
            config.user_depth[i] = GetDepth();
            config.user_convergence[i] = GetConvergence();
        }
    }

    // Update the config
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_ = config;
}


//-----------------------------------------------------------------------------
// Purpose: Adjust XInput Right Stick sensitivity
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustSensitivity(float delta)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    if (config_.pitch_enable || config_.yaw_enable)
    {
        config_.ctrl_sensitivity += delta;
        if (config_.ctrl_sensitivity < 0.0f)
            config_.ctrl_sensitivity = 0.0f;
    }
}


//-----------------------------------------------------------------------------
// Purpose: Adjust XInput Pitch Radius
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustRadius(float delta)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    if (config_.pitch_enable)
    {
        config_.pitch_radius += delta;
        if (config_.pitch_radius < 0.0f)
            config_.pitch_radius = 0.0f;
    }
}


//-----------------------------------------------------------------------------
// Purpose: Toggle HMD Height for games that have incorrect HMD position
//-----------------------------------------------------------------------------
void StereoDisplayComponent::SetHeight()
{
    static float user_height = config_.hmd_height;

    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    if (config_.hmd_height == user_height)
        config_.hmd_height = 0.1;
    else
        config_.hmd_height = user_height;
}


//-----------------------------------------------------------------------------
// Purpose: Toggle Reset off
//-----------------------------------------------------------------------------
void StereoDisplayComponent::SetReset()
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_.pose_reset = false;
}


//-----------------------------------------------------------------------------
// Purpose: Load Game Specific Settings from Documents\My games\vrto3d\app_name_config.json
//-----------------------------------------------------------------------------
void StereoDisplayComponent::LoadSettings(const std::string& app_name, uint32_t device_index) {
    JsonManager json_manager;
    auto config = GetConfig();

    // Attempt to read the JSON settings file
    nlohmann::json jsonConfig = json_manager.readJsonFromFile(app_name + "_config.json");

    // Check if the JSON file was successfully read
    if (jsonConfig.is_null()) {
        DriverLog("No profile found for %s\n", app_name.c_str());
        return;
    }

    try {
        // Attempt to load all settings from the JSON object
        config.depth = jsonConfig.at("depth").get<float>();
        config.convergence = jsonConfig.at("convergence").get<float>();
        config.hmd_height = jsonConfig.at("hmd_height").get<float>();
        config.pitch_enable = jsonConfig.at("pitch_enable").get<bool>();
        config.yaw_enable = jsonConfig.at("yaw_enable").get<bool>();
        config.pose_reset_key = jsonConfig.at("pose_reset_key").get<int>();
        config.reset_xinput = jsonConfig.at("reset_xinput").get<bool>();
        config.ctrl_toggle_key = jsonConfig.at("ctrl_toggle_key").get<int>();
        config.ctrl_type = jsonConfig.at("ctrl_toggle_type").get<int>();
        config.ctrl_xinput = jsonConfig.at("ctrl_xinput").get<bool>();
        config.pitch_radius = jsonConfig.at("pitch_radius").get<float>();
        config.ctrl_deadzone = jsonConfig.at("ctrl_deadzone").get<float>();
        config.ctrl_sensitivity = jsonConfig.at("ctrl_sensitivity").get<float>();
        config.num_user_settings = jsonConfig.at("num_user_settings").get<int>();

        // Resize vectors based on the number of user settings
        config.user_load_key.resize(config.num_user_settings);
        config.user_store_key.resize(config.num_user_settings);
        config.user_key_type.resize(config.num_user_settings);
        config.user_depth.resize(config.num_user_settings);
        config.user_convergence.resize(config.num_user_settings);
        config.prev_depth.resize(config.num_user_settings);
        config.prev_convergence.resize(config.num_user_settings);
        config.was_held.resize(config.num_user_settings);
        config.load_xinput.resize(config.num_user_settings);
        config.sleep_count.resize(config.num_user_settings);

        // Load the user settings array
        const auto& userSettingsArray = jsonConfig.at("user_settings");
        for (int i = 0; i < config.num_user_settings; i++) {
            const auto& userSetting = userSettingsArray.at(i);

            config.user_load_key[i] = userSetting.at("user_load_key").get<int>();
            config.user_store_key[i] = userSetting.at("user_store_key").get<int>();
            config.user_key_type[i] = userSetting.at("user_key_type").get<int>();
            config.user_depth[i] = userSetting.at("user_depth").get<float>();
            config.user_convergence[i] = userSetting.at("user_convergence").get<float>();
            config.load_xinput[i] = userSetting.at("load_xinput").get<bool>();
        }
    }
    catch (const nlohmann::json::exception& e) {
        // Catch any JSON-related exceptions (missing field, wrong type, etc.)
        DriverLog("Profile corrupt or missing fields %s: %s\n", app_name.c_str(), e.what());
        return;
    }

    // Apply loaded settings
    AdjustDepth(config.depth, false, device_index);
    AdjustConvergence(config.convergence, false, device_index);
    config.pose_reset = true;
    
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_ = config;
    DriverLog("Loaded %s profile\n", app_name.c_str());
    BeepSuccess();
}


//-----------------------------------------------------------------------------
// Purpose: Reload settings from default.vrsettings
//-----------------------------------------------------------------------------
void StereoDisplayComponent::LoadDefaults(uint32_t device_index)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_ = def_config_;
    AdjustDepth(config_.depth, false, device_index);
    AdjustConvergence(config_.convergence, false, device_index);
    config_.pose_reset = true;
    DriverLog("Loaded defaults from user config file\n");
    BeepSuccess();
}
