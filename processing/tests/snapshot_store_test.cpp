#include <doctest.h>

#include <Core/export_laz.h>
#include <Core/session.h>
#include <HDMapping/Processing/SessionSnapshot.h>
#include <HDMapping/Processing/SnapshotStore.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>

using namespace HDMapping::Processing;
namespace fs = std::filesystem;

namespace
{
    class Scratch
    {
    public:
        const fs::path path = fs::temp_directory_path() /
            ("hdmapping-snapshot-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        Scratch()
        {
            REQUIRE(fs::create_directory(path));
        }
        ~Scratch()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    };

    class Source final : public CaptureSource
    {
    public:
        std::atomic<std::uint64_t> revision{ 7 };
        fs::path cache;
        fs::path archive;
        std::function<void()> on_write;

        TargetRef Target() const override
        {
            return { "room", revision.load() };
        }

        CaptureResult Materialize(const fs::path& directory, RunContext&) override
        {
            fs::copy_file(cache, directory / "cache.bin");
            std::ofstream(directory / "state.json") << "{}";
            if (on_write)
                on_write();
            return SnapshotContents{ "test", "state.json", { { "radius", 0.3 } }, { archive } };
        }
    };

    void create_inputs(Source& source, const Scratch& scratch)
    {
        source.cache = scratch.path / "temporary.bin";
        source.archive = scratch.path / "immutable.laz";
        std::ofstream(source.cache) << "cached points";
        std::ofstream(source.archive) << "archive";
    }
} // namespace

TEST_CASE("Snapshot commits owned inputs and detects changes to an archive")
{
    const Scratch scratch;
    auto source = std::make_unique<Source>();
    create_inputs(*source, scratch);
    RunContext context{};
    auto one = CaptureSnapshot(*source, 7, scratch.path / fs::path(u8"zadanie żółć"), context);
    auto two = CaptureSnapshot(*source, 7, scratch.path / "second", context);
    REQUIRE(std::holds_alternative<SnapshotRef>(one));
    REQUIRE(std::holds_alternative<SnapshotRef>(two));
    fs::remove(source->cache);
    const auto archive = source->archive;
    source.reset();
    fs::remove_all(std::get<SnapshotRef>(two).manifest_path.parent_path());
    const auto& snapshot = std::get<SnapshotRef>(one);
    REQUIRE_FALSE(ValidateSnapshot(snapshot));
    CHECK(fs::file_size(snapshot.manifest_path.parent_path() / "cache.bin") == 13);
    auto wrong = snapshot;
    wrong.target.revision++;
    CHECK(ValidateSnapshot(wrong).has_value());
    const auto timestamp = fs::last_write_time(archive);
    std::ofstream(archive, std::ios::trunc) << "replace";
    fs::last_write_time(archive, timestamp);
    CHECK(ValidateSnapshot(snapshot).has_value());
}

TEST_CASE("Revision changes and cancellation leave no published snapshot")
{
    const Scratch scratch;
    Source source;
    create_inputs(source, scratch);
    const auto destination = scratch.path / "job";
    std::stop_source stop;
    RunContext context{ {}, stop.get_token(), {} };
    SUBCASE("revision changes during handoff")
    {
        source.on_write = [&]
        {
            ++source.revision;
        };
        const auto result = CaptureSnapshot(source, 7, destination, context);
        REQUIRE(std::holds_alternative<OperationError>(result));
        CHECK(std::get<OperationError>(result).code == "revision_conflict");
    }
    SUBCASE("request cancelled during handoff")
    {
        source.on_write = [&]
        {
            stop.request_stop();
        };
        const auto result = CaptureSnapshot(source, 7, destination, context);
        REQUIRE(std::holds_alternative<OperationError>(result));
        CHECK(std::get<OperationError>(result).code == "cancelled");
    }
    CHECK_FALSE(fs::exists(destination));
    CHECK(std::distance(fs::directory_iterator(scratch.path), fs::directory_iterator()) == 2);
}

TEST_CASE("Session snapshot retains unsaved poses, constraints, CP/GCP and a RAM cloud")
{
    const Scratch scratch;
    auto session = std::make_shared<Session>();
    auto& scan = session->point_clouds_container.point_clouds.emplace_back();
    scan.m_pose.translation().x() = 1.23456789012345;
    scan.m_initial_pose = Eigen::Affine3d::Identity();
    scan.m_initial_pose.translation().x() = 7.3210987654321;
    scan.laz_in_memory_only = true;
    scan.file_name = (scratch.path / "never-saved.laz").string();
    scan.visible = false;
    scan.fixed_x = true;
    scan.points_local.emplace_back(1.000000001, 2, 3);
    scan.intensities.push_back(42);
    scan.timestamps.push_back(131.573123);
    scan.colors.emplace_back(5, 19, 255);
    scan.local_trajectory.emplace_back().timestamps = { 123, 456 };
    auto& edge = session->pose_graph_loop_closure.edges.emplace_back();
    edge.index_from = 0;
    edge.index_to = 0;
    edge.relative_pose_tb.pz = 3.25;
    auto& cp = session->control_points.cps.emplace_back();
    cp.index_to_pose = 0;
    cp.x_target_global = 99.5;
    cp.is_z_0 = true;
    auto& gcp = session->ground_control_points.gpcs.emplace_back();
    gcp.index_to_node_inner = 0;
    gcp.index_to_node_outer = 0;
    gcp.lidar_height_above_ground = 0.177;
    const auto cache = scratch.path / "transient-cache.laz";
    REQUIRE(exportLaz(cache.string(), { Eigen::Vector3d(10, 20, 30) }, { 31 }, { 151.25 }));
    auto& cached_scan = session->point_clouds_container.point_clouds.emplace_back();
    cached_scan.file_name = cache.string();
    cached_scan.m_initial_pose = Eigen::Affine3d::Identity();
    session->load_cache_mode = true;
    const TargetRef target{ "session-test", 51 };
    const nlohmann::json parameters{ { "iterations", 179 }, { "radius", 0.681 } };
    RunContext context{};
    SnapshotRef snapshot;
    {
        SessionCaptureSource source(
            session,
            [target]
            {
                return target;
            },
            parameters);
        session.reset();
        const auto result = CaptureSnapshot(source, target.revision, scratch.path / fs::path(u8"sesja żółć"), context);
        REQUIRE_MESSAGE(
            std::holds_alternative<SnapshotRef>(result),
            (std::holds_alternative<OperationError>(result) ? std::get<OperationError>(result).message : ""));
        snapshot = std::get<SnapshotRef>(result);
    }
    CHECK_FALSE(fs::exists(scratch.path / "never-saved.laz"));
    REQUIRE(fs::remove(cache));
    auto result = LoadSessionSnapshot(snapshot);
    REQUIRE_MESSAGE(
        std::holds_alternative<SessionSnapshot>(result),
        (std::holds_alternative<OperationError>(result) ? std::get<OperationError>(result).message : ""));
    const auto& restored = std::get<SessionSnapshot>(result);
    CHECK(restored.parameters == parameters);
    REQUIRE(restored.session->point_clouds_container.point_clouds.size() == 2);
    const auto& cloud = restored.session->point_clouds_container.point_clouds.front();
    CHECK(cloud.m_pose.translation().x() == 1.23456789012345);
    CHECK(cloud.m_initial_pose.translation().x() == 7.3210987654321);
    CHECK_FALSE(cloud.visible);
    CHECK(cloud.fixed_x);
    REQUIRE(cloud.points_local.size() == 1);
    CHECK(cloud.points_local.front() == Eigen::Vector3d(1.000000001, 2, 3));
    REQUIRE(cloud.colors.size() == 1);
    CHECK(cloud.colors.front() == Eigen::Vector3d(5, 19, 255));
    CHECK(cloud.intensities == std::vector<unsigned short>{ 42 });
    CHECK(cloud.timestamps == std::vector<double>{ 131.573123 });
    REQUIRE(cloud.local_trajectory.size() == 1);
    CHECK(cloud.local_trajectory.front().timestamps.second == 456);
    REQUIRE(restored.session->pose_graph_loop_closure.edges.size() == 1);
    CHECK(restored.session->pose_graph_loop_closure.edges.front().relative_pose_tb.pz == 3.25);
    REQUIRE(restored.session->control_points.cps.size() == 1);
    CHECK(restored.session->control_points.cps.front().x_target_global == 99.5);
    CHECK(restored.session->control_points.cps.front().is_z_0);
    REQUIRE(restored.session->ground_control_points.gpcs.size() == 1);
    CHECK(restored.session->ground_control_points.gpcs.front().lidar_height_above_ground == 0.177);
    const auto& restored_cache = restored.session->point_clouds_container.point_clouds.at(1);
    REQUIRE(restored_cache.points_local.size() == 1);
    CHECK(restored_cache.points_local.front() == Eigen::Vector3d(10, 20, 30));
    CHECK(restored_cache.intensities == std::vector<unsigned short>{ 31 });
    CHECK(restored_cache.timestamps == std::vector<double>{ 151.25e9 });
    fs::remove(snapshot.manifest_path.parent_path() / "cloud_0.bin");
    CHECK(ValidateSnapshot(snapshot).has_value());
}

TEST_CASE("Preparing and another job's owned files cannot be submitted as snapshots")
{
    const Scratch scratch;
    Source source;
    create_inputs(source, scratch);
    RunContext context{};
    const auto one = CaptureSnapshot(source, 7, scratch.path / "job", context);
    REQUIRE(std::holds_alternative<SnapshotRef>(one));
    auto snapshot = std::get<SnapshotRef>(one);
    source.archive = snapshot.manifest_path.parent_path() / "cache.bin";
    CHECK(std::holds_alternative<OperationError>(CaptureSnapshot(source, 7, scratch.path / "other", context)));

    const auto staging = scratch.path / "job.preparing";
    fs::rename(snapshot.manifest_path.parent_path(), staging);
    snapshot.manifest_path = staging / snapshot.manifest_path.filename();
    CHECK(ValidateSnapshot(snapshot).has_value());
}

TEST_CASE("Session with an omitted cloud is incomplete even if the file inventory agrees")
{
    const Scratch scratch;
    auto session = std::make_shared<Session>();
    session->point_clouds_container.point_clouds.emplace_back().m_initial_pose = Eigen::Affine3d::Identity();
    const TargetRef target{ "incomplete", 7 };
    SessionCaptureSource source(
        session,
        [target]
        {
            return target;
        },
        nlohmann::json::object());
    RunContext context{};
    const auto result = CaptureSnapshot(source, 7, scratch.path / fs::path(u8"sesja żółć"), context);
    REQUIRE(std::holds_alternative<SnapshotRef>(result));
    const auto& snapshot = std::get<SnapshotRef>(result);
    auto manifest = nlohmann::json::parse(std::ifstream(snapshot.manifest_path));
    REQUIRE(manifest.at("files").erase("cloud_0.bin") == 1);
    fs::remove(snapshot.manifest_path.parent_path() / "cloud_0.bin");
    std::ofstream(snapshot.manifest_path, std::ios::trunc) << manifest.dump();
    CHECK(ValidateSnapshot(snapshot).has_value());
}
