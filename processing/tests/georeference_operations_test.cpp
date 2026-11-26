#include "step2_georeference_operations.h"

#include <Core/reference_tracks.h>
#include <Core/session.h>
#include <HDMapping/Processing/SessionSnapshot.h>

#include <chrono>
#include <fstream>

#include <algorithm>
#include <array>
#include <doctest/doctest.h>

TEST_CASE("Step 2 publishes distinct control point and reference operations")
{
    const auto operations = HDMapping::Processing::DescribeOperations();
    for (const auto* id : { "cp.import", "gcp.import", "step2.align_reference", "gnss.import", "tum.import" })
        CHECK(
            std::ranges::any_of(
                operations,
                [id](const auto& operation)
                {
                    return operation.id == id;
                }));
}

using namespace HDMapping::Processing;

namespace
{
    using nlohmann::json;
    namespace fs = std::filesystem;

    const auto empty = json::object();
    const TargetRef target{ "reference-test", 4 };

    struct Fixture
    {
        fs::path directory = fs::canonical(fs::temp_directory_path()) /
            ("hdmapping-georeference-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::shared_ptr<Session> session = std::make_shared<Session>();
        json control = json::array();
        json ground = json::array();
        Eigen::Vector3d shift{ 3.2, -2.4, 5.1 };

        Fixture()
        {
            REQUIRE(fs::create_directory(directory));
            auto& scans = session->point_clouds_container.point_clouds;
            scans.resize(4);
            const std::array<Eigen::Vector3d, 4> sources{ Eigen::Vector3d(0.0, 0.0, 0.0),
                                                          Eigen::Vector3d(3.0, 0.0, 0.0),
                                                          Eigen::Vector3d(0.0, 4.0, 0.0),
                                                          Eigen::Vector3d(0.0, 0.0, 5.0) };
            const Eigen::Vector3d local(0.2, -0.4, 0.3);
            for (std::size_t i = 0; i < scans.size(); ++i)
            {
                auto& scan = scans[i];
                scan.m_pose = Eigen::Affine3d::Identity();
                scan.m_pose.translation() = sources[i];
                scan.m_initial_pose = scan.m_pose;
                scan.timestamps.push_back(2e9 + static_cast<double>(i) * 1e9);
                scan.points_local.push_back(local);
                scan.intensities.push_back(2);
                auto& node = scan.local_trajectory.emplace_back();
                node.m_pose = Eigen::Affine3d::Identity();
                node.m_pose.translation() = local;
                node.timestamps = { scan.timestamps.front(), 0.0 };
                const Eigen::Vector3d observed = scan.m_pose * local + shift;
                const auto name = "point-" + std::to_string(i);
                control.push_back(
                    { { "name", name },
                      { "scan_index", i },
                      { "local_m", { local.x(), local.y(), local.z() } },
                      { "target_m", { observed.x(), observed.y(), observed.z() } },
                      { "sigma_m", { 0.05, 0.1, 0.02 } } });
                ground.push_back(
                    { { "name", name },
                      { "scan_index", i },
                      { "trajectory_index", 0 },
                      { "target_m", { observed.x(), observed.y(), observed.z() - 1.65 } },
                      { "sigma_m", { 0.9, 0.6, 0.3 } },
                      { "lidar_height_m", 1.65 } });
            }
        }

        ~Fixture()
        {
            std::error_code ignored;
            fs::remove_all(directory, ignored);
        }

        SnapshotRef capture(json tracks)
        {
            SessionCaptureSource source(
                session,
                []
                {
                    return target;
                },
                empty,
                std::move(tracks));
            RunContext context{};
            auto result = CaptureSnapshot(source, target.revision, directory / "input", context);
            REQUIRE(std::holds_alternative<SnapshotRef>(result));
            return std::get<SnapshotRef>(std::move(result));
        }
    };
} // namespace

TEST_CASE("CP and GCP preserve coordinates, weights and sensor height in rigid adjustments")
{
    for (const std::string source : { "cp", "gcp" })
    {
        Fixture fixture;
        auto& session = *fixture.session;
        const auto points = source == "cp" ? fixture.control : fixture.ground;
        REQUIRE_FALSE(EditStep2Georeference(session, { source + ".import", target, empty, { { "points", points } } }));
        const auto described = source == "cp" ? DescribeControlPoints(session) : DescribeGroundControlPoints(session);
        CHECK(described[0]["sigma_m"] == points[0]["sigma_m"]);
        CHECK(described[0]["target_m"] == points[0]["target_m"]);
        if (source == "gcp")
            CHECK(described[0]["lidar_height_m"] == 1.65);
        std::vector<Eigen::Affine3d> before;
        for (const auto& scan : session.point_clouds_container.point_clouds)
            before.push_back(scan.m_pose);
        json tracks = empty;
        RunContext context{};
        REQUIRE_FALSE(RunStep2Georeference(session, { "step2.align_reference", target, empty, { { "source", source } } }, tracks, context));
        for (std::size_t i = 0; i < before.size(); ++i)
        {
            const auto& pose = session.point_clouds_container.point_clouds[i].m_pose;
            CHECK(pose.translation().isApprox(before[i].translation() + fixture.shift, 1e-10));
            CHECK(pose.linear().isApprox(before[i].linear(), 1e-10));
        }
    }
}

TEST_CASE("CP and GCP apply the same small rigid rotation around a known axis")
{
    for (const std::string source : { "cp", "gcp" })
    {
        Fixture fixture;
        auto& session = *fixture.session;
        auto points = source == "cp" ? fixture.control : fixture.ground;
        const Eigen::AngleAxisd rotation(1e-4, Eigen::Vector3d::UnitZ());
        for (auto& point : points)
        {
            auto& xyz = point["target_m"];
            const Eigen::Vector3d translated(xyz[0].get<double>(), xyz[1].get<double>(), xyz[2].get<double>());
            const Eigen::Vector3d rotated = rotation * (translated - fixture.shift) + fixture.shift;
            xyz = { rotated.x(), rotated.y(), rotated.z() };
        }
        REQUIRE_FALSE(EditStep2Georeference(session, { source + ".import", target, empty, { { "points", points } } }));
        json tracks = empty;
        RunContext context{};
        REQUIRE_FALSE(RunStep2Georeference(session, { "step2.align_reference", target, empty, { { "source", source } } }, tracks, context));
        CHECK(session.point_clouds_container.point_clouds[0].m_pose.linear().isApprox(rotation.toRotationMatrix(), 1e-7));
    }
}

TEST_CASE("CP weight controls a conflicting observation")
{
    Fixture fixture;
    auto& session = *fixture.session;
    auto competing = fixture.control[0];
    competing["target_m"][0] = competing["target_m"][0].get<double>() + 40.0;
    competing["sigma_m"] = { 10000.0, 10000.0, 10000.0 };
    fixture.control.push_back(competing);
    REQUIRE_FALSE(EditStep2Georeference(session, { "cp.import", target, empty, { { "points", fixture.control } } }));
    json tracks = empty;
    RunContext context{};
    REQUIRE_FALSE(RunStep2Georeference(session, { "step2.align_reference", target, empty, { { "source", "cp" } } }, tracks, context));
    CHECK(session.point_clouds_container.point_clouds[0].m_pose.translation().isApprox(fixture.shift, 1e-8));
}

TEST_CASE("Invalid pose and node associations leave the entire import or edit unchanged")
{
    Fixture fixture;
    auto& session = *fixture.session;
    fixture.control.back()["scan_index"] = 4;
    CHECK(EditStep2Georeference(session, { "cp.import", target, empty, { { "points", fixture.control } } }));
    CHECK(session.control_points.cps.empty());
    REQUIRE_FALSE(EditStep2Georeference(session, { "gcp.import", target, empty, { { "points", fixture.ground } } }));
    const json index{ { "point_index", 0 } };
    CHECK(EditStep2Georeference(session, { "gcp.update", target, index, { { "point", { { "trajectory_index", 1 } } } } }));
    CHECK(DescribeGroundControlPoints(session) == fixture.ground);
    CHECK(EditStep2Georeference(session, { "gcp.update", target, index, { { "point", { { "sigma_m", { 0.0, 1.0, 1.0 } } } } } }));
    REQUIRE_FALSE(EditStep2Georeference(session, { "gcp.update", target, index, { { "point", { { "lidar_height_m", 1.4 } } } } }));
    CHECK(DescribeGroundControlPoints(session)[0]["lidar_height_m"] == 1.4);
    REQUIRE_FALSE(EditStep2Georeference(session, { "gcp.remove", target, index, empty }));
    CHECK(session.ground_control_points.gpcs.size() == 3);
}

TEST_CASE("Unsupported CRS and unavailable geoid are explicit reference errors")
{
    Session session;
    json tracks = empty;
    RunContext context{};
    auto run = [&](json parameters)
    {
        const auto result = RunStep2Georeference(session, { "gnss.project", target, empty, std::move(parameters) }, tracks, context);
        REQUIRE(result);
        return result->code;
    };
    CHECK(run({ { "crs", "unavailable-crs" } }) == "unsupported_crs");
    CHECK(run({ { "crs", "PUWG92" }, { "geoid", "hdmapping-missing-geoid.gtx" } }) == "missing_geoid");
    CHECK(tracks.empty());
}

TEST_CASE("GNSS CRS projection persists its datum and preserves the WGS84 fix")
{
    GNSS gnss;
    GNSS::GlobalPose fix{};
    fix.lat = 52.0;
    fix.lon = 21.0;
    fix.alt = 120.0;
    fix.undulation = 34.0;
    fix.h_wgs84 = 154.0;
    gnss.gnss_poses.push_back(fix);
    for (const std::string crs : { "PUWG92", "UTM_AUTO", "WebMercator" })
    {
        Session session;
        RunContext context{};
        auto tracks = HDMapping::EncodeReferenceTracks(gnss, TUM{});
        REQUIRE_FALSE(RunStep2Georeference(session, { "gnss.project", target, empty, { { "crs", crs } } }, tracks, context));
        TUM unused;
        GNSS decoded;
        HDMapping::DecodeReferenceTracks(tracks, decoded, unused);
        CHECK(decoded.projected_crs == crs);
        CHECK(decoded.projected_vertical_datum == "WGS84Ellipsoid");
        REQUIRE(decoded.gnss_poses.size() == 1);
        const auto& position = decoded.gnss_poses.front();
        CHECK(position.lat == fix.lat);
        CHECK(position.lon == fix.lon);
        CHECK(position.h_wgs84 == fix.h_wgs84);
        CHECK(position.enu_z == doctest::Approx(fix.h_wgs84).epsilon(1e-7));
        if (crs == "WebMercator")
            CHECK(position.enu_x == doctest::Approx(2337709.30666).epsilon(1e-7));
    }
}

TEST_CASE("Published alignment uses unsaved control points and TUM from the reserved snapshot")
{
    Fixture fixture;
    auto& session = *fixture.session;
    REQUIRE_FALSE(EditStep2Georeference(session, { "cp.import", target, empty, { { "points", fixture.control } } }));
    REQUIRE_FALSE(EditStep2Georeference(session, { "gcp.import", target, empty, { { "points", fixture.ground } } }));

    TUM tum;
    tum.tum_poses.push_back({ 0.5, -1, 0, 0, 0, 0, 0, 1 });
    for (const auto& scan : session.point_clouds_container.point_clouds)
    {
        const Eigen::Vector3d position = scan.m_pose.translation() + fixture.shift;
        tum.tum_poses.push_back({ scan.timestamps.front() * 1e-9 - 0.1, position.x(), position.y(), position.z(), 0, 0, 0, 1 });
    }
    const auto tracks = HDMapping::EncodeReferenceTracks(GNSS{}, tum);
    const auto snapshot = fixture.capture(tracks);
    auto liveTracks = tracks;
    RunContext offline{ fixture.directory / "result", {}, {} };
    REQUIRE(fs::create_directory(offline.output_directory));
    const OperationRequest request{ "step2.align_reference", target, empty, { { "source", "tum" } } };
    REQUIRE_FALSE(RunStep2Georeference(session, request, liveTracks, offline));
    const auto result = ExecuteOperation(request, snapshot, offline);
    if (const auto* failure = std::get_if<OperationError>(&result))
        FAIL(failure->code << ": " << failure->message);
    REQUIRE(std::holds_alternative<ArtifactManifest>(result));
    auto published = LoadSessionSnapshot(std::get<ArtifactManifest>(result).snapshot);
    REQUIRE(std::holds_alternative<SessionSnapshot>(published));
    const auto& restored = std::get<SessionSnapshot>(published);
    CHECK(restored.tracks == tracks);
    CHECK(DescribeControlPoints(*restored.session) == DescribeControlPoints(session));
    CHECK(DescribeGroundControlPoints(*restored.session) == DescribeGroundControlPoints(session));
    for (std::size_t i = 0; i < session.point_clouds_container.point_clouds.size(); ++i)
        CHECK(restored.session->point_clouds_container.point_clouds[i].m_pose.matrix().isApprox(
            session.point_clouds_container.point_clouds[i].m_pose.matrix(), 1e-10));
}
