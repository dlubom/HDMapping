#include "odometry_capture.h"

#include <fstream>
#include <span>

namespace proc = HDMapping::Processing;
using json = nlohmann::json;

namespace
{
    // Eigen stores these matrices column-major, as in the original WorkerData.
    template<class Matrix>
    json coefficients(const Matrix& matrix)
    {
        return std::span(matrix.data(), static_cast<std::size_t>(matrix.size()));
    }

    json poses(const std::vector<Eigen::Affine3d>& trajectory)
    {
        auto values = json::array();
        for (const auto& pose : trajectory)
            values.push_back(coefficients(pose.matrix()));
        return values;
    }

    void write_points(const fs::path& path, const std::vector<Point3Di>& points)
    {
        std::ofstream out(path, std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        for (const auto& point : points)
        {
            Point3DiDisk disk{};
            convert_to_disk(point, disk);
            out.write(reinterpret_cast<const char*>(&disk), sizeof(disk));
        }
        out.close();
    }

    void write_buckets(const fs::path& path, const NDTBucketMapType& buckets, std::stop_token stop)
    {
        std::ofstream out(path, std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        for (const auto& [id, b] : buckets)
        {
            if (stop.stop_requested())
                return;
            out << json::array(
                       { id,
                         coefficients(b.mean),
                         coefficients(b.cov),
                         coefficients(b.cov_inverse),
                         coefficients(b.normal_vector),
                         b.number_of_points,
                         b.index_begin,
                         b.index_end,
                         b.number_of_hits,
                         b.intensity })
                       .dump()
                << '\n';
        }
        out.close();
    }
} // namespace

OdometryCapture::OdometryCapture(
    proc::TargetRef target, const std::vector<WorkerData>& workers, const LidarOdometryParams& params, json settings)
    : target_(std::move(target))
    , workers_(workers)
    , params_(params)
    , settings_(std::move(settings))
{
}

proc::TargetRef OdometryCapture::Target() const
{
    return target_;
}

proc::CaptureResult OdometryCapture::Materialize(const fs::path& directory, proc::RunContext& context)
{
    write_points(directory / "initial.points", params_.initial_points);
    write_points(directory / "reference.points", params_.reference_points);
    write_buckets(directory / "indoor.ndjson", params_.buckets_indoor, context.stop);
    write_buckets(directory / "outdoor.ndjson", params_.buckets_outdoor, context.stop);
    auto chunks = json::array();
    for (const auto& worker : workers_)
    {
        if (context.stop.stop_requested())
            return proc::OperationError{ "cancelled", "Odometry capture cancelled", json::object() };
        const auto name = std::to_string(chunks.size());
        const auto chunk = directory / name;
        fs::create_directory(chunk);
        json row{ { "poses", poses(worker.intermediate_trajectory) },
                  { "motion_model", poses(worker.intermediate_trajectory_motion_model) },
                  { "prediction", poses(worker.intermediate_trajectory_prediction) },
                  { "timestamps", worker.intermediate_trajectory_timestamps },
                  { "prediction_vector", coefficients(worker.imu_prediction_vector) },
                  { "selected", worker.show } };
        auto angles = json::array();
        for (const auto& a : worker.imu_om_fi_ka)
            angles.push_back(coefficients(a));
        row["angles"] = std::move(angles);
        auto imu = json::array();
        for (const auto& sample : worker.raw_imu_data)
            imu.push_back({ sample.timestamp, coefficients(sample.accelerometers), coefficients(sample.guroscopes) });
        row["imu"] = std::move(imu);
        for (const auto& [source, leaf] : { std::pair{ worker.intermediate_points_cache_file_name, "intermediate.points" },
                                            std::pair{ worker.original_points_cache_file_name, "original.points" },
                                            std::pair{ worker.original_points_to_save_cache_file_name, "export.points" } })
            if (!source.empty())
            {
                fs::copy_file(source, chunk / leaf);
                row[leaf] = leaf;
            }
        std::ofstream stream(chunk / "worker.json", std::ios::binary);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream << row;
        stream.close();
        chunks.push_back(name);
    }
    std::ofstream out(directory / "odometry.json", std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << json{ { "chunks", chunks },
                 { "transform", coefficients(params_.m_g.matrix()) },
                 { "gyro_bias_dps", coefficients(params_.estimated_gyro_bias_dps) },
                 { "consecutive_distance", params_.consecutive_distance },
                 { "trajectory_length", params_.total_length_of_calculated_trajectory } };
    out.close();
    return proc::SnapshotContents{ "step1_prepared", "odometry.json", settings_, {} };
}
