#include "multi_view_tls_registration.h"
#include <Core/export_laz.h>
#include <fstream>
#include <laszip/laszip_api.h>

bool has_extension(const std::string file_path, const std::string extension)
{
    std::string::size_type dot_pos = file_path.find_last_of('.');
    if (dot_pos == std::string::npos)
    {
        return false; // No extension found
    }
    std::string file_extension = file_path.substr(dot_pos);
    return file_extension == extension;
}

void initial_pose_to_identity(Session& session)
{
    if (session.point_clouds_container.point_clouds.size() > 0)
    {
        auto m_inv = session.point_clouds_container.point_clouds[0].m_pose.inverse();
        for (int i = 0; i < session.point_clouds_container.point_clouds.size(); i++)
        {
            session.point_clouds_container.point_clouds[i].m_pose = m_inv * session.point_clouds_container.point_clouds[i].m_pose;
        }
    }
}

void save_separately_to_las(const Session& session, fs::path outwd, std::string extension)
{
    const auto& clouds = session.point_clouds_container.point_clouds;
    if (clouds.size() > 65535)
    {
        std::cerr << "warning: more than 65535 scans, point_source_ID capped at 65535\n";
    }
    for (size_t scan_idx = 0; scan_idx < clouds.size(); ++scan_idx)
    {
        const auto& p = clouds[scan_idx];
        if (p.visible)
        {
            const unsigned short psid = static_cast<unsigned short>(std::min<size_t>(scan_idx, 65535));
            fs::path file_path_in = p.file_name;
            fs::path file_path_put = outwd;
            file_path_put /= (file_path_in.stem().string() + "_processed" + extension);
            std::cout << "file_in: " << file_path_in << std::endl;
            std::cout << "file_out: " << file_path_put << std::endl;
            std::cout << "start save_processed_pc" << std::endl;
            bool compressed = (extension == ".laz");
            save_processed_pc(file_path_in, file_path_put, p.m_pose, session.point_clouds_container.offset, compressed, psid);
            std::cout << "save_processed_pc finished" << std::endl;
        }
    }
}

std::vector<std::string> get_matching_files(const std::string& directory, const std::string& pattern)
{
    std::vector<std::string> matching_files;
    std::regex regex_pattern(pattern);
    try
    {
        for (const auto& entry : fs::directory_iterator(directory))
        {
            if (entry.is_regular_file())
            { // Ensure it's a regular file
                const std::string filename = entry.path().filename().string();
                if (std::regex_match(filename, regex_pattern))
                {
                    matching_files.push_back(entry.path().string());
                }
            }
        }
    } catch (const std::exception& e)
    {
        std::cerr << "Error accessing directory: " << e.what() << std::endl;
    }

    return matching_files;
}

void run_multi_view_tls_registration(std::string input_file_name, TLSRegistration& tls_registration, std::string output_dir)
{
    fs::path outwd = fs::path(output_dir);
    Session session;
    if (!fs::exists(input_file_name))
    {
        std::cout << "Provided input path does not exist." << std::endl;
        return;
    }
    if (has_extension(input_file_name, ".json") || has_extension(input_file_name, ".mjs"))
    {
        std::cout << "Session file: '" << input_file_name << "'" << std::endl;
        session.working_directory = fs::path(input_file_name).parent_path().string();
        session.load(
            fs::path(input_file_name).string(),
            tls_registration.is_decimate,
            tls_registration.bucket_x,
            tls_registration.bucket_y,
            tls_registration.bucket_z,
            tls_registration.calculate_offset);
    }
    else if (has_extension(input_file_name, ".reg") || has_extension(input_file_name, ".mjp"))
    {
        std::cout << "RESSO file: '" << input_file_name << "'" << std::endl;
        session.working_directory = fs::path(input_file_name).parent_path().string();
        session.point_clouds_container.load(
            session.working_directory.c_str(),
            input_file_name.c_str(),
            tls_registration.is_decimate,
            tls_registration.bucket_x,
            tls_registration.bucket_y,
            tls_registration.bucket_z,
            session.load_cache_mode);
    }
    else if (has_extension(input_file_name, ".txt"))
    {
        std::cout << "ETH file: '" << input_file_name << "'" << std::endl;
        session.working_directory = fs::path(input_file_name).parent_path().string();
        session.point_clouds_container.load_eth(
            session.working_directory.c_str(),
            input_file_name.c_str(),
            tls_registration.is_decimate,
            tls_registration.bucket_x,
            tls_registration.bucket_y,
            tls_registration.bucket_z);
    }
    else if (!has_extension(input_file_name, ""))
    {
        session.point_clouds_container.point_clouds.clear();
        session.working_directory = fs::path(input_file_name).string();
        std::vector<std::string> las_files;
        std::vector<std::string> txt_files;
        for (const auto& entry : std::filesystem::directory_iterator(input_file_name))
        {
            auto file_name = entry.path().string();
            if (has_extension(file_name, ".laz") || (has_extension(file_name, ".las")))
            {
                las_files.push_back(file_name);
            }
            else if (has_extension(file_name, ".txt"))
            {
                txt_files.push_back(file_name);
            }
        }
        if (las_files.size() > 0)
        {
            std::cout << "Las/Laz files:" << std::endl;
            for (size_t i = 0; i < las_files.size(); i++)
            {
                std::cout << las_files[i] << std::endl;
            }
            session.point_clouds_container.load_whu_tls(
                las_files,
                tls_registration.is_decimate,
                tls_registration.bucket_x,
                tls_registration.bucket_y,
                tls_registration.bucket_z,
                tls_registration.calculate_offset,
                session.load_cache_mode);
        }
        else if (txt_files.size() > 0)
        {
            std::cout << "txt files:" << std::endl;
            for (size_t i = 0; i < txt_files.size(); i++)
            {
                std::cout << txt_files[i] << std::endl;
            }
            session.point_clouds_container.load_3DTK_tls(
                txt_files, tls_registration.is_decimate, tls_registration.bucket_x, tls_registration.bucket_y, tls_registration.bucket_z);
        }
        else
        {
            std::cout << "No WHU-TLS / 3DTK files available in the given directory, check path." << std::endl;
            return;
        }
    }
    else
    {
        std::cout << "Session file: '" << input_file_name << "'" << std::endl;
        session.load(
            fs::path(input_file_name).string(),
            tls_registration.is_decimate,
            tls_registration.bucket_x,
            tls_registration.bucket_y,
            tls_registration.bucket_z,
            tls_registration.calculate_offset);
    }
    std::cout << "loaded: " << session.point_clouds_container.point_clouds.size() << " point_clouds" << std::endl;

    int number_of_point = 0;
    for (const auto& pc : session.point_clouds_container.point_clouds)
    {
        number_of_point += pc.points_local.size();
    }
    session.point_clouds_container.print_point_cloud_dimension();

    if (tls_registration.resso_upd_init.size() > 0)
    {
        std::cout << "RESSO file: '" << tls_registration.resso_upd_init << "'" << std::endl;
        session.working_directory = fs::path(tls_registration.resso_upd_init).parent_path().string();
        if (!session.point_clouds_container.update_initial_poses_from_RESSO(
                session.working_directory.c_str(), tls_registration.resso_upd_init.c_str()))
        {
            std::cout << "check input files" << std::endl;
            return;
        }
        else
        {
            session.point_clouds_container.initial_poses_file_name = tls_registration.resso_upd_init;
            std::cout << "updated: " << session.point_clouds_container.point_clouds.size() << " point_clouds" << std::endl;
        }
    }
    if (tls_registration.resso_upd.size() > 0)
    {
        std::cout << "RESSO file: '" << tls_registration.resso_upd << "'" << std::endl;
        session.working_directory = fs::path(tls_registration.resso_upd).parent_path().string();
        if (!session.point_clouds_container.update_poses_from_RESSO(session.working_directory.c_str(), tls_registration.resso_upd.c_str()))
        {
            std::cout << "check input files" << std::endl;
            return;
        }
        else
        {
            std::cout << "updated: " << session.point_clouds_container.point_clouds.size() << " point_clouds" << std::endl;
            session.point_clouds_container.poses_file_name = tls_registration.resso_upd;
        }
    }
    if (tls_registration.resso_upd_inv.size() > 0)
    {
        std::cout << "RESSO file: '" << tls_registration.resso_upd_inv << "'" << std::endl;
        session.working_directory = fs::path(tls_registration.resso_upd_inv).parent_path().string();
        if (!session.point_clouds_container.update_poses_from_RESSO_inverse(
                session.working_directory.c_str(), tls_registration.resso_upd_inv.c_str()))
        {
            std::cout << "check input files" << std::endl;
            return;
        }
        else
        {
            std::cout << "updated: " << session.point_clouds_container.point_clouds.size() << " point_clouds" << std::endl;
            session.point_clouds_container.poses_file_name = tls_registration.resso_upd_inv;
        }
    }

    if (tls_registration.initial_pose_to_identity)
    {
        initial_pose_to_identity(session);
    }

    if (tls_registration.use_ndt)
    {
        if (tls_registration.compute_only_mahalanobis_distance)
        {
            tls_registration.ndt.optimize(
                session.point_clouds_container.point_clouds, true, tls_registration.compute_mean_and_cov_for_bucket);
        }
        else if (tls_registration.use_lie_algebra_left_jacobian_ndt)
        {
            tls_registration.ndt.optimize_lie_algebra_left_jacobian(
                session.point_clouds_container.point_clouds, tls_registration.compute_mean_and_cov_for_bucket);
        }
        else if (tls_registration.use_lie_algebra_right_jacobian_ndt)
        {
            tls_registration.ndt.optimize_lie_algebra_right_jacobian(
                session.point_clouds_container.point_clouds, tls_registration.compute_mean_and_cov_for_bucket);
        }
        else
        {
            std::cout << "No additional optimization option selected for NDT: using default..." << std::endl;
            tls_registration.ndt.optimize(
                session.point_clouds_container.point_clouds, false, tls_registration.compute_mean_and_cov_for_bucket);
        }
    }

    if (tls_registration.use_icp)
    {
        if (tls_registration.point_to_point_source_to_target)
        {
            tls_registration.icp.optimization_point_to_point_source_to_target(session.point_clouds_container);
        }
        else if (tls_registration.use_lie_algebra_left_jacobian_icp)
        {
            tls_registration.icp.optimize_source_to_target_lie_algebra_left_jacobian(session.point_clouds_container);
        }
        else if (tls_registration.use_lie_algebra_right_jacobian_icp)
        {
            tls_registration.icp.optimize_source_to_target_lie_algebra_right_jacobian(session.point_clouds_container);
        }
        else if (tls_registration.point_to_point_source_to_target_compute_rms)
        {
            double rms = 0.0;
            tls_registration.icp.optimization_point_to_point_source_to_target_compute_rms(session.point_clouds_container, rms);
            std::cout << "rms(optimization_point_to_point_source_to_target): " << rms << std::endl;
        }
        else
        {
            std::cout << "No optimization option selected for ICP: skipping..." << std::endl;
        }
    }

    if (tls_registration.use_plane_features)
    {
        if (tls_registration.point_to_projection_onto_plane_source_to_target)
        {
            tls_registration.registration_plane_feature.optimize_point_to_projection_onto_plane_source_to_target(
                session.point_clouds_container);
        }
        else if (tls_registration.use_lie_algebra_left_jacobian_plane_features)
        {
            tls_registration.registration_plane_feature.optimize_point_to_projection_onto_plane_source_to_target_lie_algebra_left_jacobian(
                session.point_clouds_container);
        }
        else if (tls_registration.use_lie_algebra_right_jacobian_plane_features)
        {
            tls_registration.registration_plane_feature.optimize_point_to_projection_onto_plane_source_to_target_lie_algebra_right_jacobian(
                session.point_clouds_container);
        }
        else if (tls_registration.point_to_plane_source_to_target_dot_product)
        {
            tls_registration.registration_plane_feature.optimize_point_to_plane_source_to_target(session.point_clouds_container);
        }
        else if (tls_registration.point_to_plane_source_to_target)
        {
            tls_registration.registration_plane_feature.optimize_distance_point_to_plane_source_to_target(session.point_clouds_container);
        }
        else if (tls_registration.plane_to_plane_source_to_target)
        {
            tls_registration.registration_plane_feature.optimize_plane_to_plane_source_to_target(session.point_clouds_container);
        }
        else
        {
            std::cout << "No optimization option selected for the plane features: skipping..." << std::endl;
        }
    }

    // TODO: add with GTSAM and with MANIF
    if (tls_registration.use_pgslam)
    {
        tls_registration.pose_graph_slam.ndt_bucket_size[0] = tls_registration.ndt.bucket_size[0];
        tls_registration.pose_graph_slam.ndt_bucket_size[1] = tls_registration.ndt.bucket_size[1];
        tls_registration.pose_graph_slam.ndt_bucket_size[2] = tls_registration.ndt.bucket_size[2];
        tls_registration.pose_graph_slam.optimize(session.point_clouds_container);
    }

    if (output_dir.length() > 0)
    {
        if (session.point_clouds_container.initial_poses_file_name.empty() && tls_registration.save_initial_poses)
        {
            std::string initial_poses_file_name = (outwd / "session_step2_ini_poses.mri").string();
            std::cout << "saving initial poses to: " << initial_poses_file_name << std::endl;
            session.point_clouds_container.save_poses(initial_poses_file_name, false);
        }

        if (session.point_clouds_container.poses_file_name.empty() && tls_registration.save_poses)
        {
            std::string poses_file_name = (outwd / "session_step2_poses.mrp").string();
            std::cout << "saving poses to: " << poses_file_name << std::endl;
            session.point_clouds_container.save_poses(poses_file_name, false);
        }

        session.save(
            (outwd / "session_step2.mjs").string(),
            session.point_clouds_container.poses_file_name,
            session.point_clouds_container.initial_poses_file_name,
            false);
        std::cout << "saving result to: " << session.point_clouds_container.poses_file_name << std::endl;
        session.point_clouds_container.save_poses(fs::path(session.point_clouds_container.poses_file_name).string(), false);
    }

    if (tls_registration.save_laz)
    {
        save_all_to_las(session, (outwd / "all_step_2.laz").string(), false, true);
    }
    if (tls_registration.save_las)
    {
        save_all_to_las(session, (outwd / "all_step_2.las").string(), false, true);
    }
    if (tls_registration.save_as_separate_las)
    {
        save_separately_to_las(session, outwd, ".las");
    }
    if (tls_registration.save_as_separate_laz)
    {
        save_separately_to_las(session, outwd, ".laz");
    }

    if (tls_registration.save_trajectories_laz)
    {
        save_trajectories_to_laz(
            session,
            (outwd / "trajectories.laz").string(),
            tls_registration.curve_consecutive_distance_meters,
            tls_registration.not_curve_consecutive_distance_meters,
            tls_registration.is_trajectory_export_downsampling);
    }

    if (tls_registration.save_gnss_laz)
    {
        tls_registration.gnss.save_to_laz(
            (outwd / "gnss.laz").string(),
            session.point_clouds_container.offset.x(),
            session.point_clouds_container.offset.y(),
            session.point_clouds_container.offset.z());
    }

    if (tls_registration.save_scale_board_laz)
    {
        save_scale_board_to_laz(
            session, (outwd / "scale_board.laz").string(), tls_registration.scale_board_dec, tls_registration.scale_board_side_len);
    }

    if (tls_registration.save_trajectories_csv)
    {
        save_trajectories(
            session,
            (outwd / "trajectories.csv").string(),
            tls_registration.curve_consecutive_distance_meters,
            tls_registration.not_curve_consecutive_distance_meters,
            tls_registration.is_trajectory_export_downsampling,
            tls_registration.write_lidar_timestamp,
            tls_registration.write_unix_timestamp,
            tls_registration.use_quaternions,
            false);
    }
    if (tls_registration.save_trajectories_dxf)
    {
        save_trajectories(
            session,
            (outwd / "trajectories.dxf").string(),
            tls_registration.curve_consecutive_distance_meters,
            tls_registration.not_curve_consecutive_distance_meters,
            tls_registration.is_trajectory_export_downsampling,
            tls_registration.write_lidar_timestamp,
            tls_registration.write_unix_timestamp,
            tls_registration.use_quaternions,
            true);
    }
}