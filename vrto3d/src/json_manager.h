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
#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>


 // Configuration for VRto3D
struct StereoDisplayDriverConfiguration
{
    int32_t window_x;
    int32_t window_y;

    int32_t window_width;
    int32_t window_height;

    int32_t render_width;
    int32_t render_height;

    float hmd_height;

    float aspect_ratio;
    float fov;
    float depth;
    float convergence;
    bool disable_hotkeys;

    bool tab_enable;
    bool reverse_enable;
    bool depth_gauge;
    bool debug_enable;

    float display_latency;
    float display_frequency;
    int sleep_count_max;

    bool pitch_enable;
    bool yaw_enable;
    bool pitch_set;
    bool yaw_set;
    int32_t pose_reset_key;
    bool reset_xinput;
    bool pose_reset;
    int32_t ctrl_toggle_key;
    bool ctrl_xinput;
    int32_t ctrl_type;
    bool ctrl_held;
    float pitch_radius;
    float ctrl_deadzone;
    float ctrl_sensitivity;

    int32_t num_user_settings;
    std::vector<int32_t> user_load_key;
    std::vector<int32_t> user_store_key;
    std::vector<int32_t> user_key_type;
    std::vector<float> user_depth;
    std::vector<float> user_convergence;
    std::vector<float> prev_depth;
    std::vector<float> prev_convergence;
    std::vector<bool> was_held;
    std::vector<bool> load_xinput;
    std::vector<int32_t> sleep_count;
};


class JsonManager {
public:
    JsonManager();
    
    bool writeJsonToFile(const std::string& fileName, const nlohmann::json& jsonData);
    nlohmann::json readJsonFromFile(const std::string& fileName);

    bool LoadConfigFromJson(const std::string& filename, StereoDisplayDriverConfiguration& config);

private:
    std::string vrto3dFolder;
    std::string getDocumentsFolderPath();
    std::string getVrto3DPath();
    void createFolderIfNotExist(const std::string& path);
    std::vector<std::string> split(const std::string& str, char delimiter);
};
