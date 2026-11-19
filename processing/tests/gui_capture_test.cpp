#include <Core/session.h>
#include <HDMapping/SessionPaths.h>
#include <doctest.h>
#include <odometry_capture.h>
#include <project_capture.h>

#include <chrono>
#include <fstream>

namespace proc = HDMapping::Processing;
using nlohmann::json;

namespace
{
    struct Directory
    {
        const fs::path path = fs::canonical(fs::temp_directory_path()) /
            ("hdmapping-gui-capture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        Directory()
        {
            REQUIRE(fs::create_directory(path));
        }
        ~Directory()
        {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    };

    template<class T>
    T checked(std::variant<T, proc::OperationError> result)
    {
        if (const auto* failure = std::get_if<proc::OperationError>(&result))
        {
            FAIL(failure->message);
        }
        return std::get<T>(std::move(result));
    }

    json read_json(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return json::parse(stream);
    }

    Point3Di point(double x, float intensity)
    {
        Point3Di result{};
        result.point = { x, -4, 8 };
        result.timestamp = 178.5;
        result.intensity = intensity;
        result.rgb[2] = 255;
        return result;
    }
} // namespace

TEST_CASE("A prepared odometry capture owns RAM points, cache files and the live chunk trajectory")
{
    Directory scratch;
    const auto cache = scratch.path / "working-cache";
    REQUIRE(fs::create_directory(cache));
    const proc::TargetRef target{ "step1-opening", 3 };
    proc::SnapshotRef snapshot;
    {
        LidarOdometryParams model;
        model.initial_points = { point(2.375, 90) };
        model.reference_points = { point(3, 56) };
        model.total_length_of_calculated_trajectory = 34.6;
        std::vector<WorkerData> workers(1);
        auto& worker = workers.front();
        worker.intermediate_trajectory.push_back(Eigen::Affine3d::Identity());
        worker.intermediate_trajectory.front().translation().y() = 19.25;
        worker.intermediate_trajectory_timestamps.emplace_back(1.25, 1.5);
        worker.intermediate_points_cache_file_name = cache / "cloud";
        REQUIRE(save_vector_data(worker.intermediate_points_cache_file_name.string(), std::vector{ point(7.25, 110) }));
        OdometryCapture source(target, workers, model, { { "iterations", 58 } });
        proc::RunContext context{};
        snapshot = checked(proc::CaptureSnapshot(source, target.revision, scratch.path / "prepared", context));
    }
    REQUIRE(fs::remove_all(cache) > 0);
    const auto data = checked(proc::ReadSnapshot(snapshot));
    CHECK(data.kind == "step1_prepared");
    CHECK(data.parameters.at("iterations") == 58);
    CHECK(data.dependencies.empty());
    const auto directory = data.entry.parent_path();
    const auto manifest = read_json(data.entry);
    CHECK(manifest.at("trajectory_length") == 34.6);
    const auto chunk = directory / manifest.at("chunks").at(0).get<std::string>();
    const auto worker = read_json(chunk / "worker.json");
    CHECK(worker.at("poses").at(0).at(13) == 19.25);
    CHECK(worker.at("timestamps").at(0).at(1) == 1.5);
    std::vector<Point3Di> cloud;
    REQUIRE(load_vector_data((chunk / worker.at("intermediate.points").get<std::string>()).string(), cloud));
    REQUIRE(cloud.size() == 1);
    CHECK(cloud[0].point.x() == 7.25);
    CHECK(cloud[0].rgb[2] == 255);
    REQUIRE(load_vector_data((directory / "initial.points").string(), cloud));
    REQUIRE(cloud.size() == 1);
    CHECK(cloud[0].point.x() == 2.375);
    CHECK(cloud[0].intensity == 90);
    REQUIRE(load_vector_data((directory / "reference.points").string(), cloud));
    REQUIRE(cloud.size() == 1);
    CHECK(cloud[0].point.x() == 3);
}

TEST_CASE("A project capture restores each unsaved child pose and cloud after releasing the project")
{
    Directory scratch;
    const proc::TargetRef target{ "project-opening", 12 };
    const json edges = json::array({ { { "index_session_from", 0 }, { "index_session_to", 1 }, { "pz", 1.375 } } });
    proc::SnapshotRef snapshot;
    {
        std::vector<Session> children(2);
        for (std::size_t i = 0; i < children.size(); ++i)
        {
            auto& cloud = children[i].point_clouds_container.point_clouds.emplace_back();
            cloud.file_name = (scratch.path / "never-saved.laz").string();
            cloud.laz_in_memory_only = true;
            cloud.m_initial_pose = Eigen::Affine3d::Identity();
            cloud.m_pose = Eigen::Affine3d::Identity();
            cloud.m_pose.translation().z() = 21.375 + static_cast<double>(i);
            cloud.points_local.emplace_back(6.75 + static_cast<double>(i), 9, 12);
        }
        ProjectCapture source(target, children, { { "loop_closure_edges", edges } }, { { "radius", 0.75 } });
        proc::RunContext context{};
        snapshot = checked(proc::CaptureSnapshot(source, target.revision, scratch.path / "project", context));
    }
    const auto project = checked(proc::ReadSnapshot(snapshot));
    CHECK(project.kind == "project");
    CHECK(project.parameters.at("radius") == 0.75);
    const auto document = read_json(project.entry);
    CHECK(document.at("loop_closure_edges") == edges);
    const auto& entries = document.at("session_file_names");
    REQUIRE(entries.size() == 2);
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const auto entry = project.entry.parent_path() / sessionPathFromText(entries.at(i).at("session_file_name").get<std::string>());
        auto restored = checked(proc::LoadSessionSnapshot({ entry.parent_path() / "snapshot.json", target }));
        const auto& scans = restored.session->point_clouds_container.point_clouds;
        REQUIRE(scans.size() == 1);
        CHECK(scans[0].m_pose.translation().z() == 21.375 + static_cast<double>(i));
        REQUIRE(scans[0].points_local.size() == 1);
        CHECK(scans[0].points_local[0].x() == 6.75 + static_cast<double>(i));
    }
}
