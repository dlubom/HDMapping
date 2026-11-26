#pragma once

#include <iomanip>
#include <sstream>

#include "lidar_odometry_utils.h"
#include "run_report.h"
#include "toml_io.h"
#include <HDMapping/HDMAPPING_ConfigureInfo.hpp>
#include <laszip/laszip_api.h>
#include <nlohmann/json.hpp>

#include <Core/export_laz.h>
#include <Core/session.h>

using Trajectory = std::map<double, std::tuple<Eigen::Matrix4d, double, RawIMUData>>;
using Imu = std::vector<std::tuple<std::pair<double, double>, Eigen::Vector3f, Eigen::Vector3f>>;

//! Use the default Step 1 preset shared by the CLI, GUI and managed operations.
void set_lidar_odometry_default_params(LidarOdometryParams& params);

bool load_data(
    std::vector<std::string>& input_file_names,
    LidarOdometryParams& params,
    std::vector<std::vector<Point3Di>>& pointsPerFile,
    Imu& imu_data,
    bool debugMsg,
    const OdometryControl& control = {},
    const fs::path& working_directory_override = {});
void calculate_trajectory(
    Trajectory& trajectory, Imu& imu_data, LidarOdometryParams& params, bool debugMsg, const OdometryControl& control = {});
// pointsPerFile is read, and with lazy_load_raw_clouds loaded and freed file by file
bool compute_step_1(
    std::vector<std::vector<Point3Di>>& pointsPerFile,
    LidarOdometryParams& params,
    Trajectory& trajectory,
    std::vector<WorkerData>& worker_data,
    const std::atomic<bool>& pause,
    const OdometryControl& control = {});
// after a successful step 1: free the raw clouds (and the lazy loader) — nothing after step 1 reads them
void release_raw_clouds(std::vector<std::vector<Point3Di>>& pointsPerFile, LidarOdometryParams& params);
// remove params.working_directory_cache; call only once nothing reads worker_data[*].*_cache_file_name any more
void remove_cache(const LidarOdometryParams& params);
void run_consistency(std::vector<WorkerData>& worker_data, const LidarOdometryParams& params, const OdometryControl& control = {});
void filter_reference_buckets(LidarOdometryParams& params, std::stop_token stop = {});
bool load_reference_point_clouds(
    const std::vector<std::string>& input_file_names, LidarOdometryParams& params, const OdometryControl& control = {});
bool save_result(
    std::vector<WorkerData>& worker_data,
    LidarOdometryParams& params,
    fs::path outwd,
    double elapsed_seconds,
    const OdometryControl& control = {});
bool save_parameters_toml(const LidarOdometryParams& params, const fs::path& outwd, double elapsed_seconds);
bool save_processing_results_json(const LidarOdometryParams& params, const fs::path& outwd, double elapsed_seconds);
bool save_trajectory_to_ascii(const std::vector<WorkerData>& worker_data, const fs::path& output_file_name, std::stop_token stop = {});
std::string save_results_automatic(
    LidarOdometryParams& params, std::vector<WorkerData>& worker_data, const std::string& working_directory, double elapsed_seconds);
std::vector<WorkerData> run_lidar_odometry(const std::string& input_dir, LidarOdometryParams& params);
// bool SaveParametersToTomlFile(const std::string &filepath, const LidarOdometryParams &params);
// bool LoadParametersFromTomlFile(const std::string &filepath, LidarOdometryParams &params);

bool save_failed_run_report(LidarOdometryParams& params, const fs::path& output, const std::string& error);

bool save_run_status_report(LidarOdometryParams& params, const fs::path& output, const std::string& status, const std::string& error);
