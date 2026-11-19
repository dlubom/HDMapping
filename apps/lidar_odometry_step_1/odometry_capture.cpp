#include "odometry_capture.h"
#include "recording_capture.h"

#include <HDMapping/SessionPaths.h>

#include <cctype>
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

    bool write_points(const fs::path& path, const std::vector<Point3Di>& points, std::stop_token stop)
    {
        std::ofstream out(path, std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        for (const auto& point : points)
        {
            if (stop.stop_requested())
                return false;
            Point3DiDisk disk{};
            convert_to_disk(point, disk);
            out.write(reinterpret_cast<const char*>(&disk), sizeof(disk));
        }
        out.close();
        return true;
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

    proc::OperationError cancelled()
    {
        return { "cancelled", "Odometry snapshot transfer cancelled", json::object() };
    }

    template<class Matrix>
    Matrix matrix(const json& values)
    {
        Matrix result;
        if (!values.is_array() || values.size() != static_cast<std::size_t>(result.size()))
            throw std::invalid_argument("Unexpected matrix size in odometry snapshot");
        for (Eigen::Index i = 0; i < result.size(); ++i)
            result.data()[i] = values.at(static_cast<std::size_t>(i)).get<double>();
        if (!result.allFinite())
            throw std::invalid_argument("Non-finite matrix in odometry snapshot");
        return result;
    }

    std::vector<Eigen::Affine3d> read_poses(const json& values)
    {
        if (!values.is_array())
            throw std::invalid_argument("Expected a list of poses in odometry snapshot");
        std::vector<Eigen::Affine3d> result;
        result.reserve(values.size());
        for (const auto& pose : values)
            result.emplace_back(matrix<Eigen::Matrix4d>(pose));
        return result;
    }

    std::vector<Point3Di> read_points(const fs::path& path, std::stop_token stop)
    {
        std::ifstream input(path, std::ios::binary);
        const auto size = fs::file_size(path);
        if (!input || size % sizeof(Point3DiDisk) != 0)
            throw std::invalid_argument("Invalid odometry point data");
        std::vector<Point3Di> result;
        result.reserve(size / sizeof(Point3DiDisk));
        Point3DiDisk point{};
        while (!stop.stop_requested() && input.read(reinterpret_cast<char*>(&point), sizeof(point)))
        {
            convert_from_disk(point, result.emplace_back());
            if (!result.back().point.allFinite() || !std::isfinite(point.timestamp))
                throw std::invalid_argument("Non-finite point in odometry snapshot");
        }
        if (!stop.stop_requested() && (!input.eof() || input.bad() || result.size() != size / sizeof(Point3DiDisk)))
            throw std::runtime_error("Could not read odometry point data");
        return result;
    }

    NDTBucketMapType read_buckets(const fs::path& path, std::stop_token stop)
    {
        std::ifstream input(path, std::ios::binary);
        NDTBucketMapType result;
        std::string line;
        while (!stop.stop_requested() && std::getline(input, line))
        {
            const auto values = json::parse(line);
            if (!values.is_array() || values.size() != 10)
                throw std::invalid_argument("Invalid bucket record in odometry snapshot");
            NDT::Bucket bucket;
            bucket.mean = matrix<Eigen::Vector3d>(values.at(1));
            bucket.cov = matrix<Eigen::Matrix3d>(values.at(2));
            bucket.cov_inverse = matrix<Eigen::Matrix3d>(values.at(3));
            bucket.normal_vector = matrix<Eigen::Vector3d>(values.at(4));
            bucket.number_of_points = values.at(5).get<std::uint64_t>();
            bucket.index_begin = values.at(6).get<std::uint64_t>();
            bucket.index_end = values.at(7).get<std::uint64_t>();
            bucket.number_of_hits = values.at(8).get<char>();
            bucket.intensity = values.at(9).get<float>();
            if (!std::isfinite(bucket.intensity) || !result.emplace(values.at(0).get<std::uint64_t>(), bucket).second)
                throw std::invalid_argument("Invalid or duplicate bucket in odometry snapshot");
        }
        if (!stop.stop_requested() && (!input.eof() || input.bad()))
            throw std::runtime_error("Could not read odometry buckets");
        return result;
    }

    void validate_cache(const fs::path& file, std::size_t poses, std::stop_token stop)
    {
        using DiskPoint = std::conditional_t<std::is_trivially_copyable_v<Point3Di>, Point3Di, Point3DiDisk>;
        std::ifstream input(file, std::ios::binary);
        const auto size = fs::file_size(file);
        if (!input || size == 0 || size % sizeof(DiskPoint) != 0)
            throw std::invalid_argument("Invalid scan data in odometry snapshot");
        std::vector<DiskPoint> buffer(4096);
        std::uintmax_t remaining = size / sizeof(DiskPoint);
        while (remaining > 0 && !stop.stop_requested())
        {
            const auto count = static_cast<std::size_t>(std::min<std::uintmax_t>(remaining, buffer.size()));
            if (!input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count * sizeof(DiskPoint))))
                throw std::runtime_error("Could not read odometry scan data");
            for (const auto& point : std::span(buffer).first(count))
                if (point.index_pose < 0 || static_cast<std::size_t>(point.index_pose) >= poses || !std::isfinite(point.timestamp))
                    throw std::invalid_argument("Scan data refers to an invalid pose in odometry snapshot");
            remaining -= count;
        }
    }

    WorkerData read_worker(const fs::path& directory, std::stop_token stop)
    {
        std::ifstream input(directory / "worker.json", std::ios::binary);
        const auto row = json::parse(input);
        WorkerData worker;
        worker.intermediate_trajectory = read_poses(row.at("poses"));
        worker.intermediate_trajectory_motion_model = read_poses(row.at("motion_model"));
        worker.intermediate_trajectory_prediction = read_poses(row.at("prediction"));
        worker.intermediate_trajectory_timestamps = row.at("timestamps").get<std::vector<std::pair<double, double>>>();
        worker.imu_prediction_vector = matrix<Eigen::Vector3d>(row.at("prediction_vector"));
        worker.show = row.at("selected").get<bool>();
        for (const auto& a : row.at("angles"))
            worker.imu_om_fi_ka.push_back(matrix<Eigen::Vector3d>(a));
        for (const auto& sample : row.at("imu"))
            worker.raw_imu_data.push_back(
                { sample.at(0).get<double>(), matrix<Eigen::Vector3d>(sample.at(1)), matrix<Eigen::Vector3d>(sample.at(2)) });
        const auto count = worker.intermediate_trajectory.size();
        if (count == 0 || worker.intermediate_trajectory_motion_model.size() != count ||
            worker.intermediate_trajectory_timestamps.size() != count || worker.imu_om_fi_ka.size() != count ||
            worker.raw_imu_data.size() != count ||
            (!worker.intermediate_trajectory_prediction.empty() && worker.intermediate_trajectory_prediction.size() != count))
            throw std::invalid_argument("Incomplete trajectory in odometry snapshot");
        if (std::ranges::any_of(
                worker.intermediate_trajectory_timestamps,
                [](const auto& timestamp)
                {
                    return !std::isfinite(timestamp.first) || !std::isfinite(timestamp.second);
                }) ||
            std::ranges::any_of(
                worker.raw_imu_data,
                [](const auto& sample)
                {
                    return !std::isfinite(sample.timestamp);
                }))
            throw std::invalid_argument("Invalid timestamps in odometry snapshot");
        for (const auto& [leaf, cache] : { std::pair{ "intermediate.points", &worker.intermediate_points_cache_file_name },
                                           std::pair{ "original.points", &worker.original_points_cache_file_name },
                                           std::pair{ "export.points", &worker.original_points_to_save_cache_file_name } })
        {
            if (!row.contains(leaf) && cache == &worker.original_points_to_save_cache_file_name)
                continue;
            const auto file = directory / leaf;
            if (row.at(leaf) != leaf || !fs::is_regular_file(file))
                throw std::invalid_argument("Invalid cache path in odometry snapshot");
            if (cache != &worker.original_points_to_save_cache_file_name)
                validate_cache(file, count, stop);
            *cache = file;
        }
        return worker;
    }
} // namespace

OdometryCapture::OdometryCapture(
    proc::TargetRef target,
    const std::vector<WorkerData>& workers,
    const LidarOdometryParams& params,
    json settings,
    std::vector<fs::path> reference_files)
    : target_(std::move(target))
    , workers_(workers)
    , params_(params)
    , settings_(std::move(settings))
    , reference_files_(std::move(reference_files))
{
}

proc::TargetRef OdometryCapture::Target() const
{
    return target_;
}

proc::CaptureResult OdometryCapture::Materialize(const fs::path& directory, proc::RunContext& context)
{
    if (!write_points(directory / "initial.points", params_.initial_points, context.stop) ||
        !write_points(directory / "reference.points", params_.reference_points, context.stop))
        return cancelled();
    write_buckets(directory / "indoor.ndjson", params_.buckets_indoor, context.stop);
    write_buckets(directory / "outdoor.ndjson", params_.buckets_outdoor, context.stop);
    auto chunks = json::array();
    for (const auto& worker : workers_)
    {
        if (context.stop.stop_requested())
            return cancelled();
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
    if (!reference_files_.empty())
    {
        const auto reference_paths = ReferenceCapturePaths(reference_files_);
        fs::create_directory(directory / "external-reference");
        for (std::size_t i = 0; i < reference_paths.size(); ++i)
        {
            if (context.stop.stop_requested())
                return cancelled();
            if (!proc::CopyStep1Input(reference_files_[i], directory / reference_paths[i], context.stop))
                return cancelled();
        }
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

std::vector<fs::path> ReferenceCapturePaths(const std::vector<fs::path>& sources)
{
    std::vector<fs::path> paths;
    paths.reserve(sources.size());
    for (const auto& source : sources)
    {
        auto ext = sessionPathText(source.extension());
        std::ranges::transform(
            ext,
            ext.begin(),
            [](unsigned char ch)
            {
                return static_cast<char>(std::tolower(ch));
            });
        if (ext != ".las" && ext != ".laz")
            throw std::invalid_argument("Expected a LAS or LAZ reference file");
        paths.push_back(fs::path("external-reference") / (std::to_string(paths.size()) + ext));
    }
    return paths;
}

namespace
{
    //! Freeze external reference files before admitting an otherwise prepared headless job.
    class ReferenceInput final : public proc::CaptureSource
    {
    public:
        ReferenceInput(const proc::SnapshotRef& base, const std::vector<fs::path>& references)
            : base_(base)
            , references_(references)
        {
        }

        proc::TargetRef Target() const override
        {
            return base_.target;
        }

        proc::CaptureResult Materialize(const fs::path& directory, proc::RunContext& context) override
        {
            auto read = proc::ReadSnapshot(base_, context.stop);
            if (auto* error = std::get_if<proc::OperationError>(&read))
                return std::move(*error);
            auto contents = std::get<proc::SnapshotContents>(std::move(read));
            if (contents.kind != "step1_prepared")
                return proc::OperationError{ "invalid_snapshot", "Reference loading needs a prepared Step 1 snapshot", json::object() };
            const auto root = base_.manifest_path.parent_path();
            const auto paths = ReferenceCapturePaths(references_);
            const fs::path reference_directory("external-reference");
            for (const auto& file : fs::recursive_directory_iterator(root))
            {
                const auto relative = file.path().lexically_relative(root);
                if (relative == "snapshot.json" || *relative.begin() == reference_directory || file.is_directory())
                    continue;
                fs::create_directories((directory / relative).parent_path());
                if (!proc::CopyStep1Input(file.path(), directory / relative, context.stop))
                    return cancelled();
            }
            fs::create_directory(directory / reference_directory);
            for (std::size_t i = 0; i < paths.size(); ++i)
            {
                auto source = references_[i];
                if (!source.is_absolute())
                {
                    if (source.has_root_path() || std::ranges::find(source, fs::path("..")) != source.end())
                        return proc::OperationError{ "invalid_input", "Reference input is outside the prepared snapshot", json::object() };
                    source = root / source;
                }
                if (!proc::CopyStep1Input(source, directory / paths[i], context.stop))
                    return cancelled();
                if (context.progress)
                    context.progress({ "capture_reference", i + 1, paths.size(), "Copying reference point clouds" });
            }
            if (auto failure = proc::ValidateSnapshot(base_, context.stop))
                return *failure;
            contents.entry = contents.entry.lexically_relative(root);
            return contents;
        }

    private:
        const proc::SnapshotRef& base_;
        const std::vector<fs::path>& references_;
    };
} // namespace

std::variant<proc::SnapshotRef, proc::OperationError> proc::CaptureStep1References(
    const proc::SnapshotRef& base, const std::vector<fs::path>& references, proc::RunContext& context)
{
    ReferenceInput source(base, references);
    return proc::CaptureSnapshot(source, base.target.revision, context.output_directory, context);
}

std::optional<proc::OperationError> RestoreOdometry(
    const proc::SnapshotContents& contents, std::vector<WorkerData>& workers, LidarOdometryParams& params, proc::RunContext& context)
{
    try
    {
        if (contents.kind != "step1_prepared" || contents.entry.filename() != "odometry.json")
            throw std::invalid_argument("Expected a prepared odometry snapshot");
        if (context.stop.stop_requested())
            return cancelled();
        std::ifstream input(contents.entry, std::ios::binary);
        const auto data = json::parse(input);
        const auto directory = contents.entry.parent_path();
        auto initial = read_points(directory / "initial.points", context.stop);
        auto reference = read_points(directory / "reference.points", context.stop);
        auto indoor = read_buckets(directory / "indoor.ndjson", context.stop);
        auto outdoor = read_buckets(directory / "outdoor.ndjson", context.stop);
        const Eigen::Affine3d transform(matrix<Eigen::Matrix4d>(data.at("transform")));
        const auto bias = matrix<Eigen::Vector3d>(data.at("gyro_bias_dps"));
        const auto distance = data.at("consecutive_distance").get<double>();
        const auto length = data.at("trajectory_length").get<double>();
        const auto& rows = data.at("chunks");
        if (!rows.is_array() || !std::isfinite(distance) || !std::isfinite(length))
            throw std::invalid_argument("Invalid odometry snapshot metadata");

        std::vector<WorkerData> restored;
        restored.reserve(rows.size());
        for (const auto& name : rows)
        {
            if (context.stop.stop_requested())
                return cancelled();
            const auto expected = std::to_string(restored.size());
            if (name != expected)
                throw std::invalid_argument("Invalid odometry chunk index");
            restored.push_back(read_worker(directory / expected, context.stop));
        }
        if (context.stop.stop_requested())
            return cancelled();

        workers = std::move(restored);
        params.initial_points = std::move(initial);
        params.reference_points = std::move(reference);
        params.buckets_indoor = std::move(indoor);
        params.buckets_outdoor = std::move(outdoor);
        params.m_g = transform;
        params.estimated_gyro_bias_dps = bias;
        params.consecutive_distance = distance;
        params.total_length_of_calculated_trajectory = length;
        ++params.initial_points_revision;
        ++params.reference_points_revision;
        ++params.buckets_indoor_revision;
        ++params.buckets_outdoor_revision;
        return std::nullopt;
    } catch (const std::bad_alloc&)
    {
        throw;
    } catch (const std::exception& failure)
    {
        return proc::OperationError{ "invalid_snapshot", failure.what(), json::object() };
    }
}

std::optional<proc::OperationError> RestoreOdometry(
    const proc::SnapshotRef& snapshot, std::vector<WorkerData>& workers, LidarOdometryParams& params, proc::RunContext& context)
{
    auto contents = proc::ReadSnapshot(snapshot, context.stop);
    if (auto* failure = std::get_if<proc::OperationError>(&contents))
        return std::move(*failure);
    return RestoreOdometry(std::get<proc::SnapshotContents>(contents), workers, params, context);
}
