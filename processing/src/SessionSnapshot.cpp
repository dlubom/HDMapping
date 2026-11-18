#include <HDMapping/Processing/SessionSnapshot.h>

#include <Core/session.h>

#include "SessionSnapshotPaths.h"

#include <array>
#include <bit>
#include <fstream>
#include <span>
#include <stdexcept>

namespace HDMapping::Processing
{
    namespace fs = std::filesystem;
    using nlohmann::json;

    namespace
    {
        static_assert(std::endian::native == std::endian::little);

        // The cache stores scalar fields, never the bytes of an Eigen object or a C++ struct.
        // This keeps the layout independent of padding and preserves the full precision in RAM.
        class CloudWriter
        {
        public:
            explicit CloudWriter(const fs::path& path, std::stop_token stop)
                : file_(path, std::ios::binary)
                , stop_(stop)
            {
                file_.exceptions(std::ios::failbit | std::ios::badbit);
            }

            template<typename T>
            void Scalars(std::span<const T> values)
            {
                constexpr std::size_t chunk_elements = 4096;
                while (!values.empty())
                {
                    if (stop_.stop_requested())
                    {
                        throw std::runtime_error("Snapshot capture cancelled");
                    }
                    const auto chunk = values.first(std::min(chunk_elements, values.size()));
                    file_.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size_bytes()));
                    values = values.subspan(chunk.size());
                }
            }

            void Size(std::uint64_t value)
            {
                Scalars(std::span<const std::uint64_t>(&value, 1));
            }
            void Close()
            {
                file_.close();
            }

        private:
            std::ofstream file_;
            std::stop_token stop_;
        };

        class CloudReader
        {
        public:
            explicit CloudReader(const fs::path& path)
                : file_(path, std::ios::binary)
                , remaining_(fs::file_size(path))
            {
                file_.exceptions(std::ios::failbit | std::ios::badbit);
            }

            template<typename T>
            void Scalars(std::span<T> values)
            {
                const auto size = values.size_bytes();
                if (size > remaining_)
                {
                    throw std::runtime_error("Truncated snapshot cloud");
                }
                if (!values.empty())
                {
                    file_.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(size));
                }
                remaining_ -= size;
            }

            std::size_t Size(std::size_t element_bytes)
            {
                std::uint64_t count = 0;
                Scalars(std::span<std::uint64_t>(&count, 1));
                if (count > remaining_ / element_bytes || count > std::numeric_limits<std::size_t>::max())
                {
                    throw std::runtime_error("Invalid snapshot cloud count");
                }
                return static_cast<std::size_t>(count);
            }

            bool AtEnd() const
            {
                return remaining_ == 0;
            }

        private:
            std::ifstream file_;
            std::uintmax_t remaining_;
        };

        template<typename T>
        void write_vector(CloudWriter& writer, const std::vector<T>& values)
        {
            writer.Size(values.size());
            if constexpr (std::is_same_v<T, Eigen::Vector3d>)
            {
                for (const auto& value : values)
                {
                    writer.Scalars(std::span<const double>(value.data(), 3));
                }
            }
            else
            {
                writer.Scalars(std::span<const T>(values));
            }
        }

        template<typename T>
        void read_vector(CloudReader& reader, std::vector<T>& values)
        {
            if constexpr (std::is_same_v<T, Eigen::Vector3d>)
            {
                values.resize(reader.Size(3 * sizeof(double)));
                for (auto& value : values)
                {
                    reader.Scalars(std::span<double>(value.data(), 3));
                }
            }
            else
            {
                values.resize(reader.Size(sizeof(T)));
                reader.Scalars(std::span<T>(values));
            }
        }

        void write_cloud(const fs::path& path, const PointCloud& cloud, const PointCloud& geometry, std::stop_token stop)
        {
            CloudWriter writer(path, stop);
            write_vector(writer, geometry.points_local);
            write_vector(writer, cloud.normal_vectors_local);
            write_vector(writer, geometry.colors);
            write_vector(writer, cloud.points_type);
            write_vector(writer, geometry.intensities);
            write_vector(writer, geometry.timestamps);
            writer.Size(cloud.local_trajectory.size());
            for (const auto& node : cloud.local_trajectory)
            {
                const std::array times{ node.timestamps.first, node.timestamps.second };
                writer.Scalars(std::span<const double>(times));
                writer.Scalars(std::span<const double>(node.m_pose.data(), 16));
                writer.Scalars(std::span<const double>(node.imu_om_fi_ka.data(), 3));
                writer.Scalars(std::span<const double>(node.imu_diff_angle_om_fi_ka_deg.data(), 3));
            }
            const std::array flags{
                cloud.visible ? 1 : 0, cloud.num_threads, cloud.number_points_vertical, cloud.number_points_horizontal
            };
            writer.Scalars(std::span<const int>(flags));
            writer.Close();
        }

        void read_cloud(const fs::path& path, PointCloud& cloud)
        {
            CloudReader reader(path);
            read_vector(reader, cloud.points_local);
            read_vector(reader, cloud.normal_vectors_local);
            read_vector(reader, cloud.colors);
            read_vector(reader, cloud.points_type);
            read_vector(reader, cloud.intensities);
            read_vector(reader, cloud.timestamps);
            cloud.local_trajectory.resize(reader.Size(24 * sizeof(double)));
            for (auto& node : cloud.local_trajectory)
            {
                std::array<double, 2> times{};
                reader.Scalars(std::span<double>(times));
                node.timestamps = { times[0], times[1] };
                reader.Scalars(std::span<double>(node.m_pose.data(), 16));
                reader.Scalars(std::span<double>(node.imu_om_fi_ka.data(), 3));
                reader.Scalars(std::span<double>(node.imu_diff_angle_om_fi_ka_deg.data(), 3));
            }
            std::array<int, 4> flags{};
            reader.Scalars(std::span<int>(flags));
            cloud.visible = flags[0] != 0;
            cloud.num_threads = flags[1];
            cloud.number_points_vertical = flags[2];
            cloud.number_points_horizontal = flags[3];
            if (!reader.AtEnd())
            {
                throw std::runtime_error("Unexpected data after snapshot cloud");
            }
        }
    } // namespace

    SessionCaptureSource::SessionCaptureSource(std::shared_ptr<const Session> session, std::function<TargetRef()> target, json parameters)
        : session_(std::move(session))
        , target_(std::move(target))
        , parameters_(std::move(parameters))
    {
    }

    TargetRef SessionCaptureSource::Target() const
    {
        return target_();
    }

    CaptureResult SessionCaptureSource::Materialize(const fs::path& directory, RunContext& context)
    {
        if (!session_ || session_->point_clouds_container.point_clouds.empty())
        {
            return OperationError{ "invalid_snapshot_source", "A loaded session with scans is required", json::object() };
        }
        Session metadata;
        metadata.is_ground_truth = session_->is_ground_truth;
        metadata.pose_graph_loop_closure.edges = session_->pose_graph_loop_closure.edges;
        metadata.control_points.cps = session_->control_points.cps;
        metadata.ground_control_points.gpcs = session_->ground_control_points.gpcs;
        auto& output = metadata.point_clouds_container;
        const auto& input = session_->point_clouds_container;
        output.offset = input.offset;
        output.offset_to_apply = input.offset_to_apply;
        output.folder_name = ".";
        output.out_folder_name = ".";
        output.out_poses_file_name = SessionSnapshotPaths::poses;
        output.point_clouds.reserve(input.point_clouds.size());
        for (const auto& scan : input.point_clouds)
        {
            const auto path = directory / SessionSnapshotPaths::cloud(output.point_clouds.size());
            if (context.stop.stop_requested())
            {
                return OperationError{ "cancelled", "Snapshot capture cancelled", json::object() };
            }
            if (scan.deferred_points)
            {
                const auto& deferred = *scan.deferred_points;
                if (deferred.format == PointCloud::DeferredPoints::Format::Ply)
                {
                    return OperationError{ "snapshot_capture_failed",
                                           "Load the RESSO point clouds before capturing a snapshot",
                                           json::object() };
                }
                PointCloud cached;
                const auto file_name = deferred.source.string();
                if (!cached.load_pc(file_name, true))
                {
                    return OperationError{ "snapshot_capture_failed", "Could not read cached cloud: " + file_name, json::object() };
                }
                write_cloud(path, scan, cached, context.stop);
            }
            else
            {
                write_cloud(path, scan, scan, context.stop);
            }
            auto& record = output.point_clouds.emplace_back();
            record.file_name = path.filename().string();
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
            {
                context.progress({ "materialize_clouds", output.point_clouds.size(), input.point_clouds.size(), {} });
            }
        }
        if (!output.save_poses(directory / SessionSnapshotPaths::poses, false, false, true) ||
            !output.save_poses(directory / SessionSnapshotPaths::initial_poses, false, true, true) ||
            !metadata.save_snapshot_metadata(
                directory / SessionSnapshotPaths::entry, output.out_poses_file_name, std::string(SessionSnapshotPaths::initial_poses)))
        {
            return OperationError{ "snapshot_capture_failed", "Could not write native session metadata", json::object() };
        }
        return SnapshotContents{ std::string(SessionSnapshotPaths::kind), SessionSnapshotPaths::entry, parameters_, {} };
    }

    std::variant<SessionSnapshot, OperationError> LoadSessionSnapshot(const SnapshotRef& snapshot)
    {
        auto verified = ReadSnapshot(snapshot);
        if (auto* failure = std::get_if<OperationError>(&verified))
        {
            return std::move(*failure);
        }
        auto& stored = std::get<SnapshotContents>(verified);
        try
        {
            if (stored.kind != SessionSnapshotPaths::kind || stored.entry.filename() != SessionSnapshotPaths::entry)
            {
                throw std::runtime_error("Expected a session snapshot");
            }
            auto session = std::make_unique<Session>();
            if (!session->load_snapshot_metadata(stored.entry))
            {
                throw std::runtime_error("Could not read session snapshot metadata");
            }
            const auto root = stored.entry.parent_path();
            auto& scans = session->point_clouds_container.point_clouds;
            if (scans.empty())
            {
                throw std::runtime_error("Snapshot session has no scans");
            }
            for (std::size_t i = 0; i < scans.size(); ++i)
            {
                const auto path = root / SessionSnapshotPaths::cloud(i);
                if (scans[i].file_name != path.filename().string())
                {
                    throw std::runtime_error("Snapshot cloud path does not match session");
                }
                read_cloud(path, scans[i]);
            }
            return SessionSnapshot{ std::move(session), std::move(stored.parameters) };
        } catch (const std::exception& failure)
        {
            return OperationError{ "invalid_snapshot", failure.what(), json::object() };
        }
    }
} // namespace HDMapping::Processing
