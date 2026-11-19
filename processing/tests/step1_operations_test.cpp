#include "lidar_odometry.h"
#include "odometry_capture.h"
#include "recording_capture.h"
#include "step1_parameters.h"

#include <doctest.h>

#include <chrono>
#include <fstream>
#include <set>

namespace proc = HDMapping::Processing;
using nlohmann::json;

namespace
{
    struct Scratch
    {
        const fs::path path = fs::canonical(fs::temp_directory_path()) /
            ("hdmapping-step1-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        Scratch()
        {
            REQUIRE(fs::create_directory(path));
        }

        ~Scratch()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    const proc::TargetRef target{ "step1-test", 17 };

    template<class T>
    T checked(std::variant<T, proc::OperationError> result)
    {
        if (const auto* error = std::get_if<proc::OperationError>(&result))
        {
            const auto message = error->code + ": " + error->message;
            FAIL(message);
        }
        return std::get<T>(std::move(result));
    }
} // namespace

TEST_CASE("Step 1 provides seven callable operations")
{
    std::set<std::string> ids;
    for (const auto& entry : proc::DescribeOperations())
    {
        CHECK(ids.insert(entry.id).second);
        CHECK(entry.parameters_schema.at("type") == "object");
        CHECK(entry.scope_schema.at("type") == "object");
    }
    CHECK(
        ids ==
        std::set<std::string>{
            "step1.load", "step1.prepare", "step1.odometry", "step1.consistency", "step1.reference", "step1.save", "step1.export" });
}

TEST_CASE("The native TOML preset round-trips through the Step 1 parameter contract")
{
    Scratch scratch;
    LidarOdometryParams defaults{};
    set_lidar_odometry_default_params(defaults);
    const auto file = (scratch.path / "default.toml").string();
    TomlIO io;
    REQUIRE(io.SaveParametersToTomlFile(file, defaults));
    LidarOdometryParams loaded{};
    set_lidar_odometry_default_params(loaded);
    REQUIRE(io.LoadParametersFromTomlFile(file, loaded));
    const auto settings = Step1Parameters(defaults);
    CHECK(Step1Parameters(loaded) == settings);
    CHECK(settings.at("nr_iter") == 1000);
    CHECK(settings.at("in_out_params_indoor").at("resolution_X") == 0.1);

    const auto bad = ApplyStep1Parameters({ { "useMultithread", false }, { "nr_iter", "100" } }, loaded);
    REQUIRE(bad.has_value());
    CHECK(Step1Parameters(loaded) == settings);
}

TEST_CASE("Recording capture owns its files and reports invalid incomplete input")
{
    Scratch scratch;
    const auto original = scratch.path / "recording";
    const auto output = scratch.path / "output";
    REQUIRE(fs::create_directory(original));
    REQUIRE(fs::create_directory(output));
    proc::RunContext context{ output, {}, {} };
    proc::RecordingCapture empty(target, original);
    auto invalid = proc::CaptureSnapshot(empty, target.revision, scratch.path / "empty", context);
    REQUIRE(std::holds_alternative<proc::OperationError>(invalid));
    CHECK(std::get<proc::OperationError>(invalid).code == "invalid_input");

    std::ofstream(original / "imu0001.csv") << "timestamp,imu\n";
    const auto configuration = scratch.path / "bad.toml";
    std::ofstream(configuration) << "[not closed";
    proc::RecordingCapture capture(target, original, configuration);
    const auto snapshot = checked(proc::CaptureSnapshot(capture, target.revision, scratch.path / "input", context));
    fs::remove_all(original);
    fs::remove(configuration);
    CHECK(std::holds_alternative<proc::SnapshotContents>(proc::ReadSnapshot(snapshot)));
    const proc::OperationRequest request{ "step1.odometry", target, json::object(), json::object() };
    const auto result = proc::ExecuteOperation(request, snapshot, context);
    REQUIRE(std::holds_alternative<proc::OperationError>(result));
    CHECK(std::get<proc::OperationError>(result).code == "invalid_parameters");
    CHECK_FALSE(std::get<proc::OperationError>(result).message.empty());
    CHECK(fs::is_empty(output));

    REQUIRE(fs::create_directory(original));
    std::ofstream(original / "imu0001.csv") << "timestamp,imu";
    proc::RecordingCapture incomplete(target, original);
    const auto partial = checked(proc::CaptureSnapshot(incomplete, target.revision, scratch.path / "incomplete", context));
    const auto rejected = proc::ExecuteOperation(request, partial, context);
    REQUIRE(std::holds_alternative<proc::OperationError>(rejected));
    CHECK(std::get<proc::OperationError>(rejected).code == "invalid_input");
}

TEST_CASE("A cancelled Step 1 operation yields a structured error and no output")
{
    Scratch scratch;
    std::stop_source stop;
    stop.request_stop();
    proc::RunContext context{ scratch.path, stop.get_token(), {} };
    const auto result =
        proc::ExecuteOperation({ "step1.prepare", target, json::object(), json::object() }, { scratch.path / "absent", target }, context);
    REQUIRE(std::holds_alternative<proc::OperationError>(result));
    CHECK(std::get<proc::OperationError>(result).code == "cancelled");
    CHECK(fs::is_empty(scratch.path));
}

TEST_CASE("Preparation rejects an empty set of usable LiDAR windows")
{
    Scratch scratch;
    LidarOdometryParams params{};
    set_lidar_odometry_default_params(params);
    params.threshold_nr_poses = 2;
    params.working_directory_cache = (scratch.path / "cache").string();
    Point3Di point{};
    point.point = { 3, 4, 5 };
    point.timestamp = 1.5;
    std::vector<std::vector<Point3Di>> points{ { point } };
    Trajectory trajectory;
    const RawIMUData imu{ 1, Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero() };
    trajectory.emplace(1.0, std::make_tuple(Eigen::Matrix4d::Identity().eval(), 1.0, imu));
    trajectory.emplace(2.0, std::make_tuple(Eigen::Matrix4d::Identity().eval(), 2.0, imu));
    const std::atomic<bool> pause{ false };
    std::vector<WorkerData> workers;
    CHECK_FALSE(compute_step_1(points, params, trajectory, workers, pause));
    CHECK(workers.empty());
}

TEST_CASE("A reference submission freezes its external files in the next input")
{
    Scratch scratch;
    LidarOdometryParams params{};
    const std::vector<WorkerData> workers;
    OdometryCapture base(target, workers, params, json::object());
    proc::RunContext context{ scratch.path / "next", {}, {} };
    const auto prepared = checked(proc::CaptureSnapshot(base, target.revision, scratch.path / "prepared", context));
    const auto external = scratch.path / "reference.laz";
    std::ofstream(external) << "fixture";
    const auto next = checked(proc::CaptureStep1References(prepared, { external }, context));
    REQUIRE(fs::remove(external));
    const auto frozen = checked(proc::ReadSnapshot(next));
    CHECK(frozen.kind == "step1_prepared");
    const auto copy = frozen.entry.parent_path() / "external-reference" / "0.laz";
    std::ifstream entry(copy);
    std::string bytes;
    entry >> bytes;
    CHECK(bytes == "fixture");
}

TEST_CASE("Prepared state reloads copied caches, an edited pose and reference data")
{
    Scratch scratch;
    const auto cache = scratch.path / "original.points";
    Point3Di point{};
    point.point = { 3, 4, 5 };
    point.timestamp = 6;
    point.index_pose = 0;
    REQUIRE(save_vector_data(cache.string(), std::vector{ point }));
    LidarOdometryParams params{};
    params.reference_points.push_back(point);
    auto& bucket = params.buckets_indoor[1234];
    bucket.cov = Eigen::Matrix3d::Identity() * 4;
    bucket.normal_vector = Eigen::Vector3d::UnitY();
    std::vector<WorkerData> workers(1);
    auto& worker = workers.front();
    auto pose = Eigen::Affine3d::Identity();
    pose.translation().z() = 19.25;
    worker.intermediate_trajectory = { pose };
    worker.intermediate_trajectory_motion_model = { pose };
    worker.intermediate_trajectory_timestamps.emplace_back(6, 7);
    worker.imu_om_fi_ka = { Eigen::Vector3d::Zero() };
    worker.raw_imu_data = { { 6, Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero() } };
    worker.original_points_cache_file_name = cache;
    worker.intermediate_points_cache_file_name = cache;
    OdometryCapture source(target, workers, params, json::object());
    proc::RunContext context{ scratch.path, {}, {} };
    const auto snapshot = checked(proc::CaptureSnapshot(source, target.revision, scratch.path / "prepared", context));
    fs::remove(cache);
    std::vector<WorkerData> restored;
    LidarOdometryParams restored_params{};
    CHECK_FALSE(RestoreOdometry(snapshot, restored, restored_params, context).has_value());
    REQUIRE(restored.size() == 1);
    CHECK(restored.front().intermediate_trajectory.front().translation().z() == 19.25);
    std::vector<Point3Di> original;
    REQUIRE(load_vector_data(restored.front().original_points_cache_file_name.string(), original));
    REQUIRE(original.size() == 1);
    CHECK(original.front().point.x() == 3);
    REQUIRE(restored_params.reference_points.size() == 1);
    CHECK(restored_params.reference_points.front().point.y() == 4);
    REQUIRE(restored_params.buckets_indoor.contains(1234));
    CHECK(restored_params.buckets_indoor.at(1234).cov(1, 1) == 4);

    point.index_pose = 2;
    REQUIRE(save_vector_data(cache.string(), std::vector{ point }));
    const auto invalid = checked(proc::CaptureSnapshot(source, target.revision, scratch.path / "bad-poses", context));
    const auto rejected = RestoreOdometry(invalid, restored, restored_params, context);
    REQUIRE(rejected.has_value());
    CHECK(rejected->code == "invalid_snapshot");
    CHECK(restored.front().intermediate_trajectory.front().translation().z() == 19.25);
}
