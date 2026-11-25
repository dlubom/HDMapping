#include <Core/point_cloud.h>
#include <Core/session.h>
#include <HDMapping/Processing/Operation.h>
#include <HDMapping/Processing/SessionSnapshot.h>

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>

using namespace HDMapping::Processing;

TEST_CASE("the regular grid retains complete bucket ranges across worker boundaries")
{
    PointCloud cloud;
    cloud.points_local = { { 2.0, 0.0, 0.0 }, { 0.2, 0.0, 0.0 }, { 0.3, 0.0, 0.0 }, { 0.0, 0.0, 0.0 }, { 0.1, 0.0, 0.0 } };
    cloud.rgd_params.bounding_box_extension = 0.0;
    cloud.rgd_params.resolution_X = 1.0;
    cloud.num_threads = 5;

    REQUIRE(cloud.build_rgd());
    REQUIRE(cloud.buckets.size() == 3);
    CHECK(cloud.buckets[0].index_begin == 0);
    CHECK(cloud.buckets[0].index_end == 4);
    CHECK(cloud.buckets[0].number_of_points == 4);
    CHECK(cloud.buckets[1].number_of_points == 0);
    CHECK(cloud.buckets[2].index_begin == 4);
    CHECK(cloud.buckets[2].index_end == 5);
    CHECK(cloud.buckets[2].number_of_points == 1);
}

TEST_CASE("the worker publishes its Step 2 registration contract and rejects stale revisions")
{
    const auto operations = DescribeOperations();
    const auto operation = std::ranges::find(operations, "step2.register_scans", &OperationDescriptor::id);
    REQUIRE(operation != operations.end());
    CHECK(operation->operation_class == OperationClass::Heavy);
    CHECK(operation->parameters_schema.at("oneOf").size() == 3);

    RunContext context{};
    OperationRequest request{ "step2.register_scans", { "open-session", 2 }, {}, {} };
    SnapshotRef snapshot{ {}, { "open-session", 1 } };
    const auto result = ExecuteOperation(request, snapshot, context);
    REQUIRE(std::holds_alternative<OperationError>(result));
    CHECK(std::get<OperationError>(result).code == "revision_conflict");
}

namespace
{
    namespace fs = std::filesystem;
    using nlohmann::json;

    class RegistrationFixture
    {
    public:
        const fs::path directory = fs::canonical(fs::temp_directory_path()) /
            ("hdmapping-step2-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const TargetRef target{ "synthetic-session", 4 };
        Eigen::Affine3d expected = Eigen::Affine3d::Identity();
        const json tracks{ { "tum_trajectory", json::array({ { { "timestamp", 42 }, { "qx", 0 }, { "qw", 1 } } }) } };
        SnapshotRef snapshot;

        RegistrationFixture()
        {
            REQUIRE(fs::create_directory(directory));
            auto session = std::make_shared<Session>();
            auto& scans = session->point_clouds_container.point_clouds;
            scans.resize(3);
            expected.translate(Eigen::Vector3d{ 0.03, -0.018, 0.012 });
            expected.rotate(Eigen::AngleAxisd(0.032, Eigen::Vector3d{ 1, 2, 3 }.normalized()));
            for (int i = 0; i < 96; ++i)
            {
                const Eigen::Vector3d point{ 0.27 * (i % 7) + 0.002 * i, 0.29 * ((i / 7) % 5), 0.26 * (i / 35) + 0.004 * (i % 5) };
                scans[0].points_local.push_back(point);
                scans[1].points_local.push_back(expected.inverse() * point);
                scans[2].points_local.push_back(point + Eigen::Vector3d{ 500000, 5700000, 150 });
                scans[2].colors.emplace_back(21.125, 75.25, 255.5);
                for (auto& scan : scans)
                {
                    scan.timestamps.push_back(30000000000.0 + i);
                    scan.intensities.push_back(15);
                }
            }
            scans[1].visible = false;
            scans[2].m_pose.translate(Eigen::Vector3d{ 20, -2, 1 });
            for (auto& scan : scans)
                scan.m_initial_pose = scan.m_pose;
            SessionCaptureSource source(
                session,
                [this]
                {
                    return target;
                },
                json::object(),
                tracks);
            RunContext capture{};
            auto captured = CaptureSnapshot(source, target.revision, directory / "input", capture);
            REQUIRE(std::holds_alternative<SnapshotRef>(captured));
            snapshot = std::get<SnapshotRef>(captured);
        }

        ~RegistrationFixture()
        {
            std::error_code ignored;
            fs::remove_all(directory, ignored);
        }

        RunResult run(json scope, json parameters) const
        {
            RunContext context{ directory / "output", {}, {} };
            fs::create_directory(context.output_directory);
            return ExecuteOperation({ "step2.register_scans", target, std::move(scope), std::move(parameters) }, snapshot, context);
        }
    };
} // namespace

TEST_CASE("Step 2 rejects duplicate, missing and out-of-range scan indices")
{
    const RegistrationFixture fixture;
    for (const auto& indices :
         { json::array({ 0 }), json::array({ 1, 1 }), json::array({ 0, 3 }), json::array({ -1, 0 }), json::array({ 0, 1.5 }) })
    {
        const auto result = fixture.run({ { "scan_indices", indices } }, { { "algorithm", "icp" } });
        REQUIRE(std::holds_alternative<OperationError>(result));
        CHECK(std::get<OperationError>(result).code == "invalid_scope");
    }
    const auto result = fixture.run({ { "visible", true } }, { { "algorithm", "icp" } });
    REQUIRE(std::holds_alternative<OperationError>(result));
    CHECK(std::get<OperationError>(result).code == "invalid_scope");
}

TEST_CASE("Step 2 rejects unknown variants and invalid engine settings")
{
    const RegistrationFixture fixture;
    const json scope{ { "scan_indices", { 0, 1 } } };
    const auto unknown = fixture.run(scope, { { "algorithm", "invalid" } });
    REQUIRE(std::holds_alternative<OperationError>(unknown));
    CHECK(std::get<OperationError>(unknown).code == "invalid_parameters");
    for (const auto& settings : { json({ { "search_radius", 0 } }),
                                  json({ { "number_of_threads", "2" } }),
                                  json({ { "unknown", 1 } }),
                                  json({ { "bucket_size", { 1, 1, 1 } } }) })
    {
        const auto result = fixture.run(scope, { { "algorithm", "icp" }, { "settings", settings } });
        REQUIRE(std::holds_alternative<OperationError>(result));
        CHECK(std::get<OperationError>(result).code == "invalid_parameters");
    }
}

TEST_CASE("Step 2 NDT rejects an empty correspondence set with Levenberg-Marquardt")
{
    const RegistrationFixture fixture;
    const auto result = fixture.run(
        { { "scan_indices", { 0, 1 } } },
        { { "algorithm", "ndt" }, { "settings", { { "optimizer", "levenberg_marquardt" }, { "number_of_threads", 1 } } } });
    REQUIRE(std::holds_alternative<OperationError>(result));
    CHECK(std::get<OperationError>(result).code == "registration_failed");
    CHECK_FALSE(fs::exists(fixture.directory / "output" / "native"));
}

TEST_CASE("Step 2 ICP aligns a hidden moving scan and keeps the anchor and excluded scan in place")
{
    const RegistrationFixture fixture;
    const auto result = fixture.run(
        { { "scan_indices", { 0, 1 } } },
        { { "algorithm", "icp" },
          { "settings",
            { { "search_radius", 0.12 },
              { "number_of_threads", 1 },
              { "number_of_iterations", 16 },
              { "is_fix_first_node", true },
              { "is_adaptive_robust_kernel", false } } } });
    const auto* error = std::get_if<OperationError>(&result);
    INFO((error ? error->code + ": " + error->message : ""));
    REQUIRE(std::holds_alternative<ArtifactManifest>(result));
    auto restored = LoadSessionSnapshot(std::get<ArtifactManifest>(result).snapshot);
    REQUIRE(std::holds_alternative<SessionSnapshot>(restored));
    const auto& scans = std::get<SessionSnapshot>(restored).session->point_clouds_container.point_clouds;
    REQUIRE(scans.size() == 3);
    CHECK(scans[0].m_pose.matrix() == Eigen::Matrix4d::Identity());
    CHECK(scans[2].m_pose.translation() == Eigen::Vector3d{ 20, -2, 1 });
    CHECK((scans[1].m_pose.translation() - fixture.expected.translation()).norm() < 1e-5);
    CHECK(Eigen::AngleAxisd(scans[1].m_pose.linear() * fixture.expected.linear().transpose()).angle() < 1e-5);
    CHECK(scans[1].timestamps.front() == 30000000000.0);
    CHECK(scans[1].intensities.front() == 15);
    CHECK((scans[2].points_local.front() - Eigen::Vector3d{ 500000, 5700000, 150 }).norm() < 0.0001);
    CHECK(scans[2].colors.front() == Eigen::Vector3d{ 21.125, 75.25, 255.5 });
    CHECK(std::get<SessionSnapshot>(restored).tracks == fixture.tracks);
}
