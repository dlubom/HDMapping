#include "step2_io_operations.h"

#include <Core/e57_utils.h>
#include <Core/session.h>
#include <HDMapping/Processing/ResultPublisher.h>
#include <HDMapping/Processing/SessionSnapshot.h>
#include <HDMapping/SessionPaths.h>
#include <laszip/laszip_api.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <fstream>

using nlohmann::json;
namespace fs = std::filesystem;

using namespace HDMapping::Processing;

TEST_CASE("Step 2 advertises the session IO contract")
{
    const auto operations = DescribeOperations();
    for (const std::string_view name : { "session.open", "session.save", "session.subsession", "export_result" })
        CHECK(std::ranges::find(operations, name, &OperationDescriptor::id) != operations.end());
}

namespace
{
    class Scratch
    {
    public:
        const fs::path path = fs::canonical(fs::temp_directory_path()) /
            ("hdmapping-session-io-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        Scratch()
        {
            REQUIRE(fs::create_directory(path));
        }
        ~Scratch()
        {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    };

    fs::path entry(const ArtifactManifest& result)
    {
        return std::get<SnapshotContents>(ReadSnapshot(result.snapshot)).entry;
    }

    struct LasPoint
    {
        std::uint16_t scan_id;
        double timestamp;
        double x;
    };

    std::vector<LasPoint> read_las(const fs::path& path)
    {
        laszip_POINTER reader{};
        REQUIRE(laszip_create(&reader) == 0);
        const auto close = [](void* handle)
        {
            laszip_destroy(handle);
        };
        std::unique_ptr<void, decltype(close)> owner(reader, close);
        std::ifstream stream(path, std::ios::binary);
        laszip_BOOL compressed{};
        REQUIRE(laszip_open_reader_stream(reader, stream, &compressed) == 0);
        laszip_header* header{};
        REQUIRE(laszip_get_header_pointer(reader, &header) == 0);
        laszip_point* point{};
        REQUIRE(laszip_get_point_pointer(reader, &point) == 0);
        std::vector<LasPoint> points;
        for (std::uint32_t i = 0; i < header->number_of_point_records; ++i)
        {
            REQUIRE(laszip_read_point(reader) == 0);
            points.push_back({ point->point_source_ID, point->gps_time, header->x_offset + point->X * header->x_scale_factor });
        }
        REQUIRE(laszip_close_reader(reader) == 0);
        return points;
    }
} // namespace

TEST_CASE("published Step 2 exports honor explicit hidden scans and snapshot visibility")
{
    const Scratch scratch;
    const TargetRef target{ "two-scans", 7 };
    auto session = std::make_shared<Session>();
    auto& clouds = session->point_clouds_container;
    clouds.offset = { 100, 200, 300 };
    for (int i = 0; i < 2; ++i)
    {
        auto& scan = clouds.point_clouds.emplace_back();
        scan.file_name = i == 0 ? "first.laz" : "hidden.laz";
        scan.visible = i == 0;
        scan.points_local.emplace_back(1.25, 2, 0);
        scan.intensities.push_back(123);
        scan.timestamps.push_back(1.125 + i);
        scan.m_pose = scan.m_initial_pose = Eigen::Affine3d::Identity();
        scan.m_pose.translation().x() = 4.0 * i;
        auto& node = scan.local_trajectory.emplace_back();
        node.m_pose = Eigen::Affine3d::Identity();
        node.timestamps = { 11.5 + i, 22.5 + i };
        node.imu_om_fi_ka = node.imu_diff_angle_om_fi_ka_deg = Eigen::Vector3d::Zero();
    }
    const auto observations = json::array(
        { json::array({ { { "scan_id", 0 }, { "local_coords", { 1, 2, 3 } } }, { { "scan_id", 1 }, { "local_coords", { 4, 5, 6 } } } }) });
    SessionCaptureSource source(
        session,
        [target]
        {
            return target;
        },
        { { "visible_scans", { 0 } }, { "observations", observations } });
    RunContext capture{ scratch.path / "input", {}, {} };
    const auto snapshot = std::get<SnapshotRef>(CaptureSnapshot(source, target.revision, capture.output_directory, capture));
    clouds.point_clouds[0].visible = false;
    clouds.point_clouds[1].visible = true;

    const json both = { { "scan_indices", { 0, 1 } } };
    const json visible = { { "visible", true } };
    const auto run = [&](std::string operation, std::string name, const json& scope, const json& options)
    {
        RunContext context{ scratch.path / name, {}, {} };
        REQUIRE(fs::create_directory(context.output_directory));
        auto result = ExecuteOperation({ std::move(operation), target, scope, options }, snapshot, context);
        if (const auto* error = std::get_if<OperationError>(&result))
            INFO(error->code, ": ", error->message);
        REQUIRE(std::holds_alternative<ArtifactManifest>(result));
        return std::get<ArtifactManifest>(std::move(result));
    };
    const auto all = run("export_result", "both", both, { { "format", "laz" } });
    CHECK(std::holds_alternative<ArtifactManifest>(ReadPublishedResult(scratch.path / "both" / "export")));
    const auto points = read_las(entry(all));
    REQUIRE(points.size() == 2);
    CHECK(points[0].scan_id == 0);
    CHECK(points[1].scan_id == 1);
    CHECK(points[1].x == doctest::Approx(105.25));
    CHECK(points[1].timestamp == doctest::Approx(2.125e9));

    const auto visible_result = run("export_result", "visible", visible, { { "format", "las" } });
    const auto visible_points = read_las(entry(visible_result));
    REQUIRE(visible_points.size() == 1);
    CHECK(visible_points.front().scan_id == 0);

    const auto separated = run("export_result", "separate", both, { { "format", "laz" }, { "layout", "separate" } });
    const auto scan1 = read_las(entry(separated).parent_path() / "1_processed.laz");
    REQUIRE(scan1.size() == 1);
    CHECK(scan1[0].scan_id == 1);
    CHECK(scan1[0].timestamp == doctest::Approx(2.125));

    const auto plane = run("export_result", "section", both, { { "format", "section_laz" }, { "plane", "xy" }, { "width_m", 0.25 } });
    const auto slab = read_las(entry(plane));
    REQUIRE(slab.size() == 2);
    CHECK(slab[1].scan_id == 1);

    const auto trajectory =
        run("export_result", "trajectory", both, { { "format", "trajectory_csv" }, { "timestamps", "both" }, { "pose", "quaternion" } });
    std::ifstream csv(entry(trajectory));
    std::string row;
    REQUIRE(std::getline(csv, row));
    CHECK(row.starts_with("11.5,22.5,100,200,300,"));
    REQUIRE(std::getline(csv, row));
    CHECK(row.starts_with("12.5,23.5,104,200,300,"));

    const auto e57 = run("export_result", "e57", both, { { "format", "e57" } });
    std::vector<mandeye::e57io::E57Scan> scans;
    std::string e57_error;
    REQUIRE(mandeye::e57io::load_e57(sessionPathText(entry(e57)), scans, e57_error));
    REQUIRE(scans.size() == 2);
    CHECK(scans[1].name == "hidden");
    REQUIRE(scans[1].timestamps.size() == 1);
    CHECK(scans[1].timestamps[0] == doctest::Approx(2.125));
    CHECK(scans[1].pose.translation().x() == doctest::Approx(104));

    // Open the published E57 through the same preparation contract as jobs.submit.
    OperationRequest open{ "session.open",
                           target,
                           json::object(),
                           { { "format", "e57" }, { "paths", { sessionPathText(entry(e57)) } }, { "fill_in_session", true } } };
    RunContext imported{ scratch.path / "imported", {}, {} };
    const auto again = std::get<SnapshotRef>(CaptureStep2Open(open, imported));
    const auto imported_session = std::get<SessionSnapshot>(LoadSessionSnapshot(again));
    CHECK(imported_session.session->point_clouds_container.point_clouds.size() == 2);
    CHECK(imported_session.session->point_clouds_container.point_clouds.at(1).timestamps.at(0) == doctest::Approx(2.125));

    const auto native = run("session.save", "native", json::object(), json::object());
    Session native_read{};
    REQUIRE(native_read.load_published(entry(native)));
    CHECK(native_read.point_clouds_container.point_clouds.size() == 2);
    CHECK(native_read.point_clouds_container.offset.x() == doctest::Approx(100));

    const auto sub = run("session.subsession", "sub", { { "scan_indices", { 1 } } }, json::object());
    Session sub_read{};
    REQUIRE(sub_read.load_published(entry(sub)));
    REQUIRE(sub_read.point_clouds_container.point_clouds.size() == 1);
    const auto& hidden = sub_read.point_clouds_container.point_clouds[0];
    CHECK(hidden.m_pose.translation().x() == doctest::Approx(4));
    CHECK(hidden.timestamps[0] == doctest::Approx(2.125));
    CHECK(hidden.local_trajectory[0].timestamps.second == doctest::Approx(23.5));

    RunContext derived{ scratch.path / "derived", {}, {} };
    REQUIRE(fs::create_directory(derived.output_directory));
    auto restored_observations = ExecuteOperation(
        { "export_result", target, { { "scan_indices", { 0 } } }, { { "format", "observations_json" } } }, sub.snapshot, derived);
    REQUIRE(std::holds_alternative<ArtifactManifest>(restored_observations));
    std::ifstream read_observations(entry(std::get<ArtifactManifest>(restored_observations)));
    const auto single_observation = json::parse(read_observations).at("observations").at(0);
    REQUIRE(single_observation.size() == 1);
    CHECK(single_observation.at(0).at("scan_id") == 0);
    CHECK(single_observation.at(0).at("local_coords") == json::array({ 4, 5, 6 }));

    const auto resso = run("export_result", "resso", both, { { "format", "resso" } });
    RunContext import{ scratch.path / "poses", {}, {} };
    REQUIRE(fs::create_directory(import.output_directory));
    auto inverse = ExecuteOperation(
        { "session.import_poses", target, both, { { "paths", { sessionPathText(entry(resso)) } }, { "mode", "inverse" } } },
        snapshot,
        import);
    REQUIRE(std::holds_alternative<ArtifactManifest>(inverse));
    Session updated{};
    REQUIRE(updated.load_published(entry(std::get<ArtifactManifest>(inverse))));
    CHECK(updated.point_clouds_container.point_clouds.at(1).m_pose.translation().x() == doctest::Approx(-4));
    CHECK(updated.point_clouds_container.point_clouds.at(1).m_initial_pose.translation().x() == doctest::Approx(0));

    RunContext invalid{ scratch.path / "invalid", {}, {} };
    REQUIRE(fs::create_directory(invalid.output_directory));
    const auto hidden_only = ExecuteOperation({ "export_result", target, visible, { { "format", "laz" } } }, sub.snapshot, invalid);
    CHECK(std::get<OperationError>(hidden_only).code == "invalid_scope");
    CHECK(fs::is_empty(invalid.output_directory));

    const auto missing_data3d = ExecuteOperation(
        { "export_result",
          target,
          both,
          { { "format", "e57_poses" }, { "paths", { sessionPathText(entry(e57)) } }, { "e57_scan_indices", { 0, 100 } } } },
        snapshot,
        invalid);
    CHECK(std::get<OperationError>(missing_data3d).code == "session_export_failed");
    CHECK(fs::is_empty(invalid.output_directory));
    for (const json& scope : { json::object(), json{ { "scan_indices", { 0, 0 } } }, json{ { "scan_indices", { 2 } } } })
    {
        const auto error = ExecuteOperation({ "export_result", target, scope, { { "format", "laz" } } }, snapshot, invalid);
        CHECK(std::get<OperationError>(error).code == "invalid_scope");
        CHECK(fs::is_empty(invalid.output_directory));
    }
}
