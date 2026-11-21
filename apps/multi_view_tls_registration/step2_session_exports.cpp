#include "step2_session_exports.h"

#include <Core/export_laz.h>
#include <fstream>
#include <iomanip>

bool save_trajectories_to_laz(
    const Session& session,
    std::string output_file_name,
    float curve_consecutive_distance_meters,
    float not_curve_consecutive_distance_meters,
    bool is_trajectory_export_downsampling)
{
    std::vector<Eigen::Vector3d> pointcloud;
    std::vector<unsigned short> intensity;
    std::vector<double> timestamps;
    std::vector<unsigned short> point_source_ids;

    const auto& clouds = session.point_clouds_container.point_clouds;
    if (clouds.size() > 65535)
    {
        std::cerr << "warning: more than 65535 scans, point_source_ID capped at 65535\n";
    }

    float consecutive_distance = 0;
    for (size_t scan_idx = 0; scan_idx < clouds.size(); ++scan_idx)
    {
        const auto& p = clouds[scan_idx];
        if (p.visible)
        {
            const unsigned short psid = static_cast<unsigned short>(std::min<size_t>(scan_idx, 65535));
            for (std::size_t i = 0; i < p.local_trajectory.size(); i++)
            {
                const auto& pp = p.local_trajectory[i].m_pose.translation();
                Eigen::Vector3d vp;
                vp = p.m_pose * pp; // + session.point_clouds_container.offset;

                if (i > 0)
                {
                    double dist = (p.local_trajectory[i].m_pose.translation() - p.local_trajectory[i - 1].m_pose.translation()).norm();
                    consecutive_distance = static_cast<float>(consecutive_distance + dist);
                }

                bool is_curve = false;

                if (i > 100 && i < p.local_trajectory.size() - 100)
                {
                    Eigen::Vector3d position_prev = p.local_trajectory[i - 100].m_pose.translation();
                    Eigen::Vector3d position_curr = p.local_trajectory[i].m_pose.translation();
                    Eigen::Vector3d position_next = p.local_trajectory[i + 100].m_pose.translation();

                    Eigen::Vector3d v1 = position_curr - position_prev;
                    Eigen::Vector3d v2 = position_next - position_curr;

                    if (v1.norm() > 0 && v2.norm() > 0)
                    {
                        double angle_deg = fabs(acos(v1.dot(v2) / (v1.norm() * v2.norm())) * 180.0 / M_PI);

                        if (angle_deg > 10.0)
                        {
                            is_curve = true;
                        }
                    }
                }
                double tol = not_curve_consecutive_distance_meters;

                if (is_curve)
                {
                    tol = curve_consecutive_distance_meters;
                }

                if (!is_trajectory_export_downsampling)
                {
                    pointcloud.push_back(vp);
                    intensity.push_back(0);
                    timestamps.push_back(p.local_trajectory[i].timestamps.first);
                    point_source_ids.push_back(psid);
                }
                else
                {
                    if (consecutive_distance >= tol)
                    {
                        consecutive_distance = 0;
                        pointcloud.push_back(vp);
                        intensity.push_back(0);
                        timestamps.push_back(p.local_trajectory[i].timestamps.first);
                        point_source_ids.push_back(psid);
                    }
                }
            }
        }
    }
    return exportLaz(
        output_file_name,
        pointcloud,
        intensity,
        timestamps,
        session.point_clouds_container.offset.x(),
        session.point_clouds_container.offset.y(),
        session.point_clouds_container.offset.z(),
        &point_source_ids);
}

static bool createDXFPolyline(const std::string& filename, const std::vector<Eigen::Vector3d>& points)
{
    std::ofstream dxfFile(filename);
    dxfFile << std::setprecision(20);
    if (!dxfFile.is_open())
    {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return false;
    }

    // DXF header
    dxfFile << "0\nSECTION\n2\nHEADER\n0\nENDSEC\n";
    dxfFile << "0\nSECTION\n2\nTABLES\n0\nENDSEC\n";

    // Start the ENTITIES section
    dxfFile << "0\nSECTION\n2\nENTITIES\n";

    // Start the POLYLINE entity
    dxfFile << "0\nPOLYLINE\n";
    dxfFile << "8\n0\n"; // Layer 0
    dxfFile << "66\n1\n"; // Indicates the presence of vertices
    dxfFile << "70\n8\n"; // 1 = Open polyline

    // Write the VERTEX entities
    for (const auto& point : points)
    {
        dxfFile << "0\nVERTEX\n";
        dxfFile << "8\n0\n"; // Layer 0
        dxfFile << "10\n" << point.x() << "\n"; // X coordinate
        dxfFile << "20\n" << point.y() << "\n"; // Y coordinate
        dxfFile << "30\n" << point.z() << "\n"; // Z coordinate
    }

    // End the POLYLINE
    dxfFile << "0\nSEQEND\n";

    // End the ENTITIES section
    dxfFile << "0\nENDSEC\n";

    // End the DXF file
    dxfFile << "0\nEOF\n";

    dxfFile.close();
    if (!dxfFile.good())
    {
        std::cerr << "Failed to write file: " << filename << std::endl;
        return false;
    }
    std::cout << "DXF file created: " << filename << std::endl;
    return true;
}

bool save_trajectories(
    const Session& session,
    std::string output_file_name,
    float curve_consecutive_distance_meters,
    float not_curve_consecutive_distance_meters,
    bool is_trajectory_export_downsampling,
    bool write_lidar_timestamp,
    bool write_unix_timestamp,
    bool use_quaternions,
    bool save_to_dxf)
{
    std::ofstream outfile;
    if (!save_to_dxf)
    {
        outfile.open(output_file_name);
    }
    if (save_to_dxf || outfile.good())
    {
        float consecutive_distance = 0;
        std::vector<Eigen::Vector3d> polylinePoints;
        for (const auto& p : session.point_clouds_container.point_clouds)
        {
            if (p.visible)
            {
                for (std::size_t i = 0; i < p.local_trajectory.size(); i++)
                {
                    const auto& m = p.local_trajectory[i].m_pose;
                    Eigen::Affine3d pose = p.m_pose * m;
                    pose.translation() += session.point_clouds_container.offset;

                    if (i > 0)
                    {
                        double dist = (p.local_trajectory[i].m_pose.translation() - p.local_trajectory[i - 1].m_pose.translation()).norm();
                        consecutive_distance = static_cast<float>(consecutive_distance + dist);
                    }

                    bool is_curve = false;

                    if (i > 100 && i < p.local_trajectory.size() - 100)
                    {
                        Eigen::Vector3d position_prev = p.local_trajectory[i - 100].m_pose.translation();
                        Eigen::Vector3d position_curr = p.local_trajectory[i].m_pose.translation();
                        Eigen::Vector3d position_next = p.local_trajectory[i + 100].m_pose.translation();

                        Eigen::Vector3d v1 = position_curr - position_prev;
                        Eigen::Vector3d v2 = position_next - position_curr;

                        if (v1.norm() > 0 && v2.norm() > 0)
                        {
                            double angle_deg = fabs(acos(v1.dot(v2) / (v1.norm() * v2.norm())) * 180.0 / M_PI);

                            if (angle_deg > 10.0)
                            {
                                is_curve = true;
                            }
                        }
                    }
                    double tol = not_curve_consecutive_distance_meters;

                    if (is_curve)
                    {
                        tol = curve_consecutive_distance_meters;
                    }

                    if (!is_trajectory_export_downsampling || (is_trajectory_export_downsampling && consecutive_distance >= tol))
                    {
                        if (is_trajectory_export_downsampling)
                        {
                            consecutive_distance = 0;
                        }
                        if (save_to_dxf)
                        {
                            polylinePoints.push_back(pose.translation());
                        }
                        else
                        {
                            outfile << std::setprecision(20);
                            if (write_lidar_timestamp)
                            {
                                outfile << p.local_trajectory[i].timestamps.first << ",";
                            }
                            if (write_unix_timestamp)
                            {
                                outfile << p.local_trajectory[i].timestamps.second << ",";
                            }
                            outfile << pose(0, 3) << "," << pose(1, 3) << "," << pose(2, 3) << ",";
                            if (use_quaternions)
                            {
                                Eigen::Quaterniond q(pose.rotation());
                                outfile << q.x() << "," << q.y() << "," << q.z() << "," << q.w() << std::endl;
                            }
                            else
                            {
                                outfile << pose(0, 0) << "," << pose(0, 1) << "," << pose(0, 2) << "," << pose(1, 0) << "," << pose(1, 1)
                                        << "," << pose(1, 2) << "," << pose(2, 0) << "," << pose(2, 1) << "," << pose(2, 2) << std::endl;
                            }
                        }
                    }
                }
            }
        }
        if (!save_to_dxf)
        {
            outfile.close();
            if (!outfile.good())
            {
                std::cerr << "Failed to write file: " << output_file_name << std::endl;
                return false;
            }
            return true;
        }
        else
        {
            return createDXFPolyline(output_file_name, polylinePoints);
        }
    }
    std::cerr << "Failed to open file: " << output_file_name << std::endl;
    return false;
}

bool save_scale_board_to_laz(const Session& session, std::string output_file_name, float dec, float side_len)
{
    float min_x = 1000000000.0;
    float max_x = -1000000000.0;
    float min_y = 1000000000.0;
    float max_y = -1000000000.0;
    float min_z = 1000000000.0;
    float max_z = -1000000000.0;
    float xy_incr = 0.001f;
    float z = 0.0f;
    if (side_len < 0.0f)
    {
        for (const auto& p : session.point_clouds_container.point_clouds)
        {
            if (p.visible)
            {
                for (std::size_t i = 0; i < p.local_trajectory.size(); i++)
                {
                    const auto& pp = p.local_trajectory[i].m_pose.translation();
                    Eigen::Vector3d vp;
                    vp = p.m_pose * pp;
                    if (vp.x() < min_x)
                    {
                        min_x = static_cast<float>(vp.x());
                    }
                    if (vp.x() > max_x)
                    {
                        max_x = static_cast<float>(vp.x());
                    }
                    if (vp.y() < min_y)
                    {
                        min_y = static_cast<float>(vp.y());
                    }
                    if (vp.y() > max_y)
                    {
                        max_y = static_cast<float>(vp.y());
                    }
                    if (vp.z() < min_z)
                    {
                        min_z = static_cast<float>(vp.z());
                    }
                    if (vp.z() > max_z)
                    {
                        max_z = static_cast<float>(vp.z());
                    }
                }
            }
        }
        min_x -= 100.0f;
        min_y -= 100.0f;
        max_x += 100.0f;
        max_y += 100.0f;
        z = (max_z + min_z) / 2.0f;
    }
    else
    {
        min_x = -side_len / 2.0f;
        min_y = -side_len / 2.0f;
        max_x = side_len / 2.0f;
        max_y = side_len / 2.0f;
        xy_incr = 0.2f;
        z = 0.0;
    }

    const auto advances = [](float minimum, float maximum, float increment)
    {
        const auto limit = std::max(std::abs(minimum), std::abs(maximum));
        return std::isfinite(limit) && limit + increment > limit;
    };
    if (!advances(min_x, max_x, dec) || !advances(min_x, max_x, xy_incr) || !advances(min_y, max_y, dec) ||
        !advances(min_y, max_y, xy_incr))
        return false;

    const auto grid = [](float minimum, float maximum, float increment)
    {
        std::vector<float> coordinates;
        while (minimum <= maximum)
        {
            coordinates.push_back(minimum);
            minimum += increment;
        }
        return coordinates;
    };
    const auto coarse_x = grid(min_x, max_x, dec);
    const auto fine_x = grid(min_x, max_x, xy_incr);
    const auto coarse_y = grid(min_y, max_y, dec);
    const auto fine_y = grid(min_y, max_y, xy_incr);

    const auto& offset = session.point_clouds_container.offset;
    LazWriter writer;
    if (!writer.open(output_file_name, offset.x(), offset.y(), offset.z()))
        return false;
    for (const auto x : coarse_x)
        for (const auto y : fine_y)
            if (!writer.writePoint({ x, y, z }, 0, 0, offset.x(), offset.y(), offset.z()))
                return false;
    for (const auto y : coarse_y)
        for (const auto x : fine_x)
            if (!writer.writePoint({ x, y, z }, 0, 0, offset.x(), offset.y(), offset.z()))
                return false;
    return writer.close();
}
