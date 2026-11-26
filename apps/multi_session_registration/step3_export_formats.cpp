#include "step3_export_formats.h"

#include <Core/export_laz.h>
#include <HDMapping/SessionPaths.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>

namespace HDMapping::Processing
{
    bool WriteProjectTrajectory(
        const Session& session,
        const std::filesystem::path& path,
        std::string_view format,
        std::string_view timestamps,
        std::string_view pose,
        bool sort_tum,
        std::stop_token stop)
    {
        const auto& clouds = session.point_clouds_container;
        if (format == "trajectory_las" || format == "trajectory_laz")
        {
            const auto& offset = clouds.offset;
            LazWriter writer;
            if (!writer.open(sessionPathText(path), offset.x(), offset.y(), offset.z(), false, format == "trajectory_laz"))
                return false;
            for (const auto& scan : clouds.point_clouds)
            {
                if (!scan.visible)
                    continue;
                for (const auto& node : scan.local_trajectory)
                {
                    if (stop.stop_requested() ||
                        !writer.writePoint(
                            scan.m_pose * node.m_pose.translation(), 0, node.timestamps.first, offset.x(), offset.y(), offset.z()))
                        return false;
                }
            }
            return writer.close();
        }

        const bool tum = format == "trajectory_tum";
        const bool lidar = timestamps != "unix";
        const bool unix_time = timestamps != "lidar";
        const bool quaternion = tum || pose == "quaternion";
        std::ofstream file(path);
        file.imbue(std::locale::classic());
        if (!file)
            return false;
        if (!tum)
        {
            if (lidar)
                file << "timestampLidar,";
            if (unix_time)
                file << "timestampUnix,";
            file << "x,y,z," << (quaternion ? "qx,qy,qz,qw\n" : "r00,r01,r02,r10,r11,r12,r20,r21,r22\n");
        }
        const auto separator = tum ? ' ' : ',';
        const auto write = [&](double lidar_stamp, double unix_stamp, const Eigen::Affine3d& transform)
        {
            file << std::fixed;
            if (tum)
                file << std::setprecision(9) << lidar_stamp / 1e9 << separator;
            else
            {
                file << std::setprecision(0);
                if (lidar)
                    file << lidar_stamp << separator;
                if (unix_time)
                    file << unix_stamp << separator;
            }
            const auto position = transform.translation();
            file << std::setprecision(10) << position.x() << separator << position.y() << separator << position.z();
            const Eigen::Matrix3d rotation = transform.rotation();
            if (quaternion)
            {
                const Eigen::Quaterniond q(rotation);
                file << separator << q.x() << separator << q.y() << separator << q.z() << separator << q.w();
            }
            else
                for (Eigen::Index row = 0; row < 3; ++row)
                    for (Eigen::Index col = 0; col < 3; ++col)
                        file << separator << rotation(row, col);
            file << '\n';
        };

        std::vector<std::pair<double, Eigen::Affine3d>> ordered;
        for (const auto& scan : clouds.point_clouds)
        {
            if (!scan.visible)
                continue;
            for (const auto& node : scan.local_trajectory)
            {
                if (stop.stop_requested())
                    return false;
                if (tum && sort_tum)
                {
                    if (!std::isfinite(node.timestamps.first))
                        return false;
                    ordered.emplace_back(node.timestamps.first, scan.m_pose * node.m_pose);
                }
                else
                    write(node.timestamps.first, node.timestamps.second, scan.m_pose * node.m_pose);
            }
        }
        std::stable_sort(
            ordered.begin(),
            ordered.end(),
            [](const auto& first, const auto& second)
            {
                return first.first < second.first;
            });
        for (std::size_t i = 0; i < ordered.size(); ++i)
        {
            if (stop.stop_requested())
                return false;
            if (i == 0 || ordered[i].first != ordered[i - 1].first)
                write(ordered[i].first, 0.0, ordered[i].second);
        }
        file.close();
        return file.good();
    }
} // namespace HDMapping::Processing
