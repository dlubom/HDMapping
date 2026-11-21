#pragma once

#include <string>

class Session;

bool save_trajectories_to_laz(
    const Session& session,
    std::string output_file_name,
    float curve_consecutive_distance_meters,
    float not_curve_consecutive_distance_meters,
    bool is_trajectory_export_downsampling);

bool save_scale_board_to_laz(const Session& session, std::string output_file_name, float dec, float side_len = -1.0);

bool save_trajectories(
    const Session& session,
    std::string output_file_name,
    float curve_consecutive_distance_meters,
    float not_curve_consecutive_distance_meters,
    bool is_trajectory_export_downsampling,
    bool write_lidar_timestamp = true,
    bool write_unix_timestamp = false,
    bool use_quaternions = true,
    bool save_to_dxf = false);
