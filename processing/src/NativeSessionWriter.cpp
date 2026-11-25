#include "NativeSessionResult.h"

#include <Core/export_laz.h>
#include <Core/session.h>
#include <HDMapping/SessionPaths.h>

#include <fstream>
#include <iomanip>
#include <limits>

namespace HDMapping::Processing
{
    namespace fs = std::filesystem;
    using nlohmann::json;

    namespace
    {
        void writeTrajectory(const fs::path& path, const PointCloud& scan, std::stop_token stop)
        {
            if (scan.local_trajectory.empty())
                return;
            std::ofstream output(path);
            output.exceptions(std::ios::badbit | std::ios::failbit);
            output << "timestamp_nanoseconds pose00 pose01 pose02 pose03 pose10 pose11 pose12 pose13 pose20 pose21 pose22 pose23 "
                      "timestampUnix_nanoseconds om_rad fi_rad ka_rad\n"
                   << std::setprecision(std::numeric_limits<double>::max_digits10);
            for (const auto& node : scan.local_trajectory)
            {
                if (stop.stop_requested())
                    return;
                output << node.timestamps.first;
                for (int row = 0; row < 3; ++row)
                    for (int column = 0; column < 4; ++column)
                        output << ' ' << node.m_pose(row, column);
                output << ' ' << node.timestamps.second;
                for (int axis = 0; axis < 3; ++axis)
                    output << ' ' << node.imu_om_fi_ka(axis);
                output << '\n';
            }
            output.close();
        }

        bool writeCloud(const fs::path& path, const PointCloud& scan, std::stop_token stop)
        {
            const bool has_colors = scan.colors.size() == scan.points_local.size() && !scan.colors.empty();
            Eigen::Vector3d offset = Eigen::Vector3d::Zero();
            if (!scan.points_local.empty() && scan.points_local.front().allFinite())
                offset = scan.points_local.front().array().floor();
            constexpr double limit = std::numeric_limits<std::int32_t>::max() * LazWriter::coordinate_scale;
            LazWriter writer;
            if (!writer.open(sessionPathText(path), offset.x(), offset.y(), offset.z(), has_colors))
                return false;
            for (std::size_t i = 0; i < scan.points_local.size(); ++i)
            {
                if (stop.stop_requested())
                    return false;
                const auto& position = scan.points_local[i];
                if (!position.allFinite() || (position - offset).cwiseAbs().maxCoeff() > limit)
                    return false;
                std::array<std::uint16_t, 3> rgb{};
                if (has_colors)
                {
                    for (std::size_t channel = 0; channel < rgb.size(); ++channel)
                    {
                        const double component = scan.colors[i][static_cast<Eigen::Index>(channel)];
                        if (std::isfinite(component))
                            rgb[channel] = static_cast<std::uint16_t>(std::round(std::clamp(component * 256, 0.0, 65535.0)));
                    }
                }
                const auto intensity = i < scan.intensities.size() ? scan.intensities[i] : static_cast<unsigned short>(0);
                const auto timestamp = i < scan.timestamps.size() ? scan.timestamps[i] : 0.0;
                if (!writer.writePointWithGpsTime(
                        scan.points_local[i], intensity, timestamp, 0, 0, 0, 0, has_colors ? rgb.data() : nullptr))
                    return false;
            }
            return writer.close();
        }

    } // namespace

    CaptureResult WriteNativeSession(
        const Session& session, const fs::path& directory, json parameters, const json& tracks, RunContext& context)
    {
        Session metadata;
        metadata.is_ground_truth = session.is_ground_truth;
        metadata.pose_graph_loop_closure.edges = session.pose_graph_loop_closure.edges;
        metadata.control_points.cps = session.control_points.cps;
        metadata.ground_control_points.gpcs = session.ground_control_points.gpcs;
        auto& clouds = metadata.point_clouds_container;
        const auto& source = session.point_clouds_container;
        clouds.offset = source.offset;
        clouds.offset_to_apply = source.offset_to_apply;
        clouds.point_clouds.reserve(source.point_clouds.size());
        for (std::size_t i = 0; i < source.point_clouds.size(); ++i)
        {
            const auto& scan = source.point_clouds[i];
            auto& record = clouds.point_clouds.emplace_back();
            record.file_name = "scan_lio_" + std::to_string(i) + ".laz";
            const auto path = directory / record.file_name;
            if (!writeCloud(path, scan, context.stop))
                return context.stop.stop_requested()
                    ? OperationError{ "cancelled", "Native session write cancelled", {} }
                    : OperationError{ "publication_failed", "Could not write registered point cloud", { { "scan_index", i } } };
            writeTrajectory(sessionTrajectoryPath(path), scan, context.stop);
            if (context.stop.stop_requested())
                return OperationError{ "cancelled", "Native session write cancelled", {} };
            record.m_pose = scan.m_pose;
            record.m_initial_pose = scan.m_initial_pose;
            record.fuse_inclination_from_IMU = scan.fuse_inclination_from_IMU;
            record.fixed_x = scan.fixed_x;
            record.fixed_y = scan.fixed_y;
            record.fixed_z = scan.fixed_z;
            record.fixed_om = scan.fixed_om;
            record.fixed_fi = scan.fixed_fi;
            record.fixed_ka = scan.fixed_ka;
            if (context.progress)
                context.progress({ "write_session", i + 1, source.point_clouds.size(), {} });
        }

        constexpr auto poses = "poses.mrp";
        constexpr auto initial = "initial.mri";
        constexpr auto entry = "session.mjs";
        clouds.folder_name = ".";
        clouds.out_folder_name = ".";
        clouds.out_poses_file_name = poses;
        if (!clouds.save_poses(directory / poses, false, false, true) || !clouds.save_poses(directory / initial, false, true, true) ||
            !metadata.save_snapshot_metadata(directory / entry, poses, initial))
            return OperationError{ "publication_failed", "Could not write registered session poses", {} };
        WriteNativeSessionTracks(directory / entry, tracks);
        return SnapshotContents{ std::string(native_session_kind), entry, std::move(parameters), {} };
    }
} // namespace HDMapping::Processing
