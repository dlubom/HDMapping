#include "step2_result.h"

#include "NativeSessionResult.h"

#include <Core/export_laz.h>
#include <HDMapping/Processing/ResultPublisher.h>
#include <HDMapping/Processing/SnapshotStore.h>
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

        OperationError cancelled()
        {
            return { "cancelled", "Step 2 registration cancelled", {} };
        }

        //! Materialize worker-owned point clouds and poses in the existing MJS/LAZ formats.
        class RegistrationResultSource final : public CaptureSource
        {
        public:
            RegistrationResultSource(Session& session, const TargetRef& target, json parameters, const json& tracks)
                : session_(session)
                , target_(target)
                , parameters_(std::move(parameters))
                , tracks_(tracks)
            {
            }

            TargetRef Target() const override
            {
                return target_;
            }

            CaptureResult Materialize(const fs::path& directory, RunContext& context) override
            {
                auto& clouds = session_.point_clouds_container;
                for (std::size_t i = 0; i < clouds.point_clouds.size(); ++i)
                {
                    auto& scan = clouds.point_clouds[i];
                    scan.file_name = "scan_lio_" + std::to_string(i) + ".laz";
                    const auto path = directory / scan.file_name;
                    if (!writeCloud(path, scan, context.stop))
                        return context.stop.stop_requested()
                            ? cancelled()
                            : OperationError{ "publication_failed", "Could not write registered point cloud", { { "scan_index", i } } };
                    writeTrajectory(sessionTrajectoryPath(path), scan, context.stop);
                    if (context.stop.stop_requested())
                        return cancelled();
                    if (context.progress)
                        context.progress({ "write_session", i + 1, clouds.point_clouds.size(), {} });
                }

                constexpr auto poses = "poses.mrp";
                constexpr auto initial = "initial.mri";
                constexpr auto entry = "session.mjs";
                clouds.folder_name = ".";
                clouds.out_folder_name = ".";
                clouds.out_poses_file_name = poses;
                if (!clouds.save_poses(directory / poses, false, false, true) ||
                    !clouds.save_poses(directory / initial, false, true, true) ||
                    !session_.save_snapshot_metadata(directory / entry, poses, initial))
                    return OperationError{ "publication_failed", "Could not write registered session poses", {} };
                WriteNativeSessionTracks(directory / entry, tracks_);
                return SnapshotContents{ std::string(native_session_kind), entry, parameters_, {} };
            }

        private:
            Session& session_;
            TargetRef target_;
            json parameters_;
            const json& tracks_;
        };
    } // namespace

    RunResult PublishStep2Result(Session& session, const TargetRef& target, json parameters, const json& tracks, RunContext& context)
    {
        const auto stage = context.output_directory / ".hdmapping-result-stage-step2";
        const auto destination = context.output_directory / "native";
        RegistrationResultSource source(session, target, std::move(parameters), tracks);
        const auto captured = CaptureSnapshot(source, target.revision, stage, context);
        if (const auto* failure = std::get_if<OperationError>(&captured))
            return *failure;
        if (context.stop.stop_requested())
        {
            DiscardResultStaging(stage, destination);
            return cancelled();
        }
        return PublishResult(stage, destination);
    }
} // namespace HDMapping::Processing
