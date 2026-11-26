#include "project_capture.h"
#include "project_operations.h"
#include "step3_operations.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <fstream>

using namespace HDMapping::Processing;

namespace
{
    namespace fs = std::filesystem;
    using nlohmann::json;
    const auto empty = json::object();

    //! An isolated published project and its two session scans.
    class Step3Fixture
    {
    public:
        const fs::path directory = fs::canonical(fs::temp_directory_path()) /
            ("processing-step3-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const TargetRef target{ "step3-fixture", 42 };
        std::vector<Session> sessions = std::vector<Session>(2);
        ProjectSettings settings;

        Step3Fixture()
        {
            REQUIRE(fs::create_directory(directory));
            for (std::size_t i = 0; i < sessions.size(); ++i)
            {
                auto& session = sessions[i];
                session.visible = i == 0;
                session.is_ground_truth = i == 0;
                for (int scan = 0; scan < 2; ++scan)
                {
                    auto& cloud = session.point_clouds_container.point_clouds.emplace_back();
                    cloud.m_pose = Eigen::Affine3d::Identity();
                    cloud.m_pose.translation().x() = 2.0 * scan + static_cast<double>(i);
                    cloud.m_pose.translation().y() = 0.03 * static_cast<double>(i);
                    cloud.m_initial_pose = cloud.m_pose_temp = cloud.m_pose;
                    cloud.points_local = { { 1.25, 2.5, 3.75 }, { -1.2, -3.1, 1.4 }, { 2, -2.5, -1.3 },
                                           { 0.5, 0.25, 3.3 },  { -0.4, 2.3, 1.5 },  { 3.2, 0.9, 0.6 } };
                    cloud.intensities.resize(cloud.points_local.size(), 42);
                }
            }
            settings.session_file_names = { "fixed.mjs", "hidden.mjs" };
            for (int scan = 0; scan < 2; ++scan)
            {
                Edge edge{};
                edge.index_session_from = 0;
                edge.index_session_to = 1;
                edge.index_from = scan;
                edge.index_to = scan;
                edge.relative_pose_tb_weights = { 100, 100, 100, 100, 100, 100 };
                settings.edges.push_back(edge);
            }
        }

        ~Step3Fixture()
        {
            std::error_code error;
            fs::remove_all(directory, error);
        }

        Step3Fixture(const Step3Fixture&) = delete;
        Step3Fixture& operator=(const Step3Fixture&) = delete;

        OperationRequest request(std::string operation, json scope, json parameters) const
        {
            return { std::move(operation), target, std::move(scope), std::move(parameters) };
        }

        SnapshotRef snapshot() const
        {
            ProjectCapture writer(target, sessions, ProjectSettingsJson(settings), empty, true);
            RunContext context{};
            const auto result = CaptureSnapshot(writer, target.revision, directory / "input", context);
            if (const auto* error = std::get_if<OperationError>(&result))
                FAIL(error->code << " " << error->message);
            return std::get<SnapshotRef>(result);
        }

        ArtifactManifest execute(const OperationRequest& operation, const SnapshotRef& snapshot, const std::string& name) const
        {
            RunContext context{ directory / name, {}, {} };
            REQUIRE(fs::create_directory(context.output_directory));
            auto result = ExecuteOperation(operation, snapshot, context);
            if (const auto* error = std::get_if<OperationError>(&result))
                FAIL(error->code << " " << error->message);
            return std::get<ArtifactManifest>(std::move(result));
        }
    };

    auto first_pose(const Session& session)
    {
        return session.point_clouds_container.point_clouds[0].m_pose.matrix();
    }

    const json pairs{ { "session_pairs", { { { "from", 0 }, { "to", 1 } } } } };
} // namespace

TEST_CASE("Step 3 advertises the registration, pose and graph operations")
{
    const auto operations = DescribeOperations();
    for (const std::string_view name : { "project.transform_scope",
                                         "project.restore_poses",
                                         "project.register_sessions",
                                         "project.constraints.list",
                                         "project.constraints.add",
                                         "project.constraints.update",
                                         "project.constraints.remove",
                                         "project.constraints.replace",
                                         "project.constraints.run_icp",
                                         "project.constraints.detect_candidates",
                                         "optimize_project" })
        CHECK(std::ranges::count(operations, name, &OperationDescriptor::id) == 1);
}

TEST_CASE("Step 3 validates both constraint endpoints and only replaces named active edges")
{
    Step3Fixture project;
    const auto before = ProjectEdgesJson(project.settings.edges);
    for (const auto* side : { "from", "to" })
        for (const auto* field : { "session_index", "scan_index" })
        {
            auto invalid = before[0];
            invalid[side][field] = 9;
            CHECK_THROWS(
                EditStep3(project.request("project.constraints.add", empty, { { "edge", invalid } }), project.settings, project.sessions));
        }
    auto invalid = before[0];
    invalid["to"]["scan_index"] = std::numeric_limits<std::uint64_t>::max();
    CHECK_THROWS(EditStep3(
        project.request("project.constraints.replace", { { "edge_indices", { 0 } } }, { { "edges", { before[1], invalid } } }),
        project.settings,
        project.sessions));
    CHECK(ProjectEdgesJson(project.settings.edges) == before);

    auto replacement = before[0];
    replacement["relative_pose"][0] = 2.5;
    EditStep3(
        project.request("project.constraints.replace", { { "edge_indices", { 0 } } }, { { "edges", { replacement } } }),
        project.settings,
        project.sessions);
    CHECK(ProjectEdgesJson(project.settings.edges) == json::array({ replacement, before[1] }));
    CHECK_THROWS(EditStep3(
        project.request("project.constraints.replace", { { "edge_indices", { 1, 1 } } }, { { "edges", json::array() } }),
        project.settings,
        project.sessions));
}

TEST_CASE("Step 3 publishes a scoped candidate report without changing the active graph")
{
    const Step3Fixture project;
    const auto snapshot = project.snapshot();
    const auto result =
        project.execute(project.request("project.constraints.detect_candidates", pairs, { { "max_distance_m", 1.1 } }), snapshot, "job");
    CHECK(result.snapshot.target == snapshot.target);
    CHECK(result.files.size() == 2);
    const auto report = json::parse(std::ifstream(result.snapshot.manifest_path.parent_path() / "constraints.json"));
    REQUIRE(report.at("candidates").size() == 3);
    CHECK(report.at("candidates").at(0).at("from").at("session_index") == 0);
    CHECK(report.at("candidates").at(0).at("to").at("session_index") == 1);
    const auto active = project.execute(project.request("project.constraints.list", empty, empty), snapshot, "list");
    const auto listed = json::parse(std::ifstream(active.snapshot.manifest_path.parent_path() / "constraints.json"));
    CHECK(listed.at("active_edges") == ProjectEdgesJson(project.settings.edges));
}

TEST_CASE("Step 3 publishes graph poses with ground truth fixed and restores pre-optimization poses")
{
    const Step3Fixture project;
    const auto input = project.snapshot();
    const auto result = project.execute(
        project.request("optimize_project", { { "edge_indices", { 0, 1 } } }, { { "iterations", 50 } }), input, "optimization");
    const auto mjp = result.snapshot.manifest_path.parent_path() / "project.mjp";
    const auto native = ReadProjectSettings(mjp, true);
    REQUIRE(native.session_file_names.size() == 2);
    const auto fixed = ReadProjectSession(native.session_file_names[0], { .strict = true });
    const auto moving = ReadProjectSession(native.session_file_names[1], { .strict = true });
    CHECK(first_pose(fixed) == first_pose(project.sessions[0]));
    CHECK(first_pose(moving)(0, 3) < 0.02);
    CHECK(LoadProjectSnapshot(result.snapshot).index() == 0);

    const auto restored = project.execute(
        project.request("project.restore_poses", { { "session_indices", { 1 } } }, { { "source", "previous" } }),
        result.snapshot,
        "restore");
    const auto loaded = LoadProjectSnapshot(restored.snapshot);
    REQUIRE(std::holds_alternative<ProjectState>(loaded));
    const auto& sessions = std::get<ProjectState>(loaded).sessions;
    CHECK(first_pose(sessions[1]) == first_pose(project.sessions[1]));
    CHECK_FALSE(sessions[1].visible);
    CHECK(first_pose(sessions[0]) == first_pose(project.sessions[0]));
}

TEST_CASE("Step 3 transforms an explicitly selected hidden session and keeps ground truth fixed")
{
    Step3Fixture project;
    const auto fixed = first_pose(project.sessions[0]);
    const auto before = first_pose(project.sessions[1]);
    EditStep3(
        project.request("project.transform_scope", { { "session_indices", { 0, 1 } } }, { { "transform", { 1, 2, 3, 0, 0, 0 } } }),
        project.settings,
        project.sessions);
    CHECK(first_pose(project.sessions[0]) == fixed);
    CHECK(first_pose(project.sessions[1])(0, 3) == before(0, 3) + 1);
    CHECK(first_pose(project.sessions[1])(1, 3) == doctest::Approx(before(1, 3) + 2));
    CHECK_FALSE(project.sessions[1].visible);
}

TEST_CASE("Step 3 NDT records its computed pose for an explicitly selected hidden session")
{
    Step3Fixture project;
    const Eigen::Vector3d offset(0.025, 0.015, -0.02);
    for (auto& session : project.sessions)
    {
        auto& clouds = session.point_clouds_container.point_clouds;
        clouds.resize(1);
        auto& points = clouds[0].points_local;
        points.clear();
        for (int cell = 0; cell < 30; ++cell)
        {
            const Eigen::Vector3d centre(1.0 + cell % 5, 1.0 + cell / 5 % 3, 1.0 + cell / 15);
            for (int sample = 0; sample < 37; ++sample)
                points.push_back(
                    centre +
                    Eigen::Vector3d(
                        0.2 + 0.003 * ((sample * 7 + cell) % 37),
                        0.2 + 0.003 * ((sample * 13 + 2 * cell) % 37),
                        0.2 + 0.003 * ((sample * 19 + 3 * cell) % 37)));
        }
        clouds[0].m_pose = Eigen::Affine3d::Identity();
    }
    project.sessions[1].point_clouds_container.point_clouds[0].m_pose.translation() = offset;

    RunContext context{};
    RunStep3(
        project.request(
            "project.register_sessions",
            pairs,
            { { "method", "ndt" }, { "bucket_size_m", { 0.5, 0.5, 0.5 } }, { "threads", 1 }, { "iterations", 1 } }),
        project.settings,
        project.sessions,
        context);
    CHECK(first_pose(project.sessions[0]) == Eigen::Matrix4d::Identity());
    CHECK(first_pose(project.sessions[1]).topRightCorner<3, 1>().norm() < offset.norm() * 0.5);
    CHECK_FALSE(project.sessions[1].visible);
}

TEST_CASE("Step 3 graph keeps saved control points and ground control points active")
{
    for (const bool ground_control : { false, true })
    {
        Step3Fixture project;
        auto& moving = project.sessions[1];
        auto& cloud = moving.point_clouds_container.point_clouds[0];
        const auto expected = cloud.m_pose.translation().x() + 0.2;
        if (ground_control)
        {
            auto& node = cloud.local_trajectory.emplace_back();
            node.m_pose = Eigen::Affine3d::Identity();
            node.timestamps = { 0, 0 };
            node.imu_om_fi_ka = node.imu_diff_angle_om_fi_ka_deg = Eigen::Vector3d::Zero();
            auto& gcp = moving.ground_control_points.gpcs.emplace_back();
            gcp = {};
            gcp.x = expected;
            gcp.y = cloud.m_pose.translation().y();
            gcp.sigma_x = gcp.sigma_y = gcp.sigma_z = 0.05;
        }
        else
        {
            auto& cp = moving.control_points.cps.emplace_back();
            cp = {};
            cp.x_target_global = expected;
            cp.y_target_global = cloud.m_pose.translation().y();
            cp.sigma_x = cp.sigma_y = cp.sigma_z = 0.05;
        }

        const auto input = project.snapshot();
        const auto result = project.execute(
            project.request("optimize_project", { { "edge_indices", json::array() } }, { { "iterations", 30 } }), input, "control");
        const auto native = ReadProjectSettings(result.snapshot.manifest_path.parent_path() / "project.mjp", true);
        const auto fitted = ReadProjectSession(native.session_file_names[1], { .strict = true });
        CHECK(first_pose(fitted)(0, 3) == doctest::Approx(expected).epsilon(0.01));
        CHECK(fitted.ground_control_points.gpcs.size() == (ground_control ? 1 : 0));
        CHECK(fitted.control_points.cps.size() == (ground_control ? 0 : 1));
    }
}

TEST_CASE("Step 3 ICP registers a hidden session from explicit pair endpoints")
{
    Step3Fixture project;
    const auto make_points = []
    {
        std::vector<Eigen::Vector3d> points;
        for (int i = 0; i < 240; ++i)
            points.emplace_back(0.5 * (i % 8) + 0.002 * (i % 13), 0.4 * (i / 8 % 6) + 0.002 * (i % 17), 0.3 * (i / 48) + 0.002 * (i % 11));
        return points;
    };
    auto& fixed = project.sessions[0].point_clouds_container.point_clouds[0];
    auto& moving = project.sessions[1].point_clouds_container.point_clouds[0];
    fixed.points_local = moving.points_local = make_points();
    const TaitBryanPose shift{ 0.035, -0.026, 0.021, 0.003, 0.004, -0.006 };
    moving.m_pose = affine_matrix_from_pose_tait_bryan(shift);
    const double before = moving.m_pose.translation().norm();
    const json scope{ { "session_pairs", { { { "from", 0 }, { "to", 1 }, { "from_scan", 0 }, { "to_scan", 0 } } } } };
    RunContext context{};
    RunStep3(
        project.request(
            "project.register_sessions",
            scope,
            { { "method", "icp" }, { "search_radius_m", 0.2 }, { "iterations", 1 }, { "crop_ground_truth", false } }),
        project.settings,
        project.sessions,
        context);
    CHECK(moving.m_pose.translation().norm() < before * 0.1);
    CHECK(first_pose(project.sessions[0]) == Eigen::Matrix4d::Identity());
    CHECK_FALSE(project.sessions[1].visible);
}
