#include "step2_segment_operations.h"

#include <Core/session.h>
#include <HDMapping/Processing/GuiStateBridge.h>
#include <HDMapping/Processing/SessionSnapshot.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

#include <doctest/doctest.h>

using namespace HDMapping::Processing;
using nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    const json anchor = { { 0, -1, 0, 10 }, { 1, 0, 0, 20 }, { 0, 0, 1, 30 }, { 0, 0, 0, 1 } };
    const TargetRef target{ "segment-test", 4 };

    OperationRequest edit(std::string_view operation, json indices, json parameters = json::object())
    {
        return { std::string(operation), target, { { "scan_indices", std::move(indices) } }, std::move(parameters) };
    }

    PointClouds makeClouds()
    {
        PointClouds clouds;
        auto& scans = clouds.point_clouds;
        scans.resize(4);
        for (std::size_t i = 0; i < scans.size(); ++i)
        {
            auto& scan = scans[i];
            scan.m_pose = Eigen::Affine3d::Identity();
            scan.m_pose.translation() = Eigen::Vector3d{ 2 * static_cast<double>(i), -1, 5 };
            scan.m_initial_pose = Eigen::Affine3d::Identity();
            scan.m_initial_pose.translation().x() = static_cast<double>(i);
            scan.points_local.emplace_back(1, 2, 3);
            scan.intensities.push_back(8);
            scan.timestamps.push_back(9);
        }
        scans[2].visible = false;
        return clouds;
    }
} // namespace

TEST_CASE("a segment sets its anchor and carries only the specified relative poses forward")
{
    auto clouds = makeClouds();
    const auto before = clouds.point_clouds;
    const auto& scans = clouds.point_clouds;
    const auto result = EditStep2Poses(clouds, edit(edit_segment_operation, { 1, 2 }, { { "anchor_pose", anchor } }));
    REQUIRE(std::holds_alternative<std::vector<std::size_t>>(result));
    CHECK(scans[1].m_pose.translation().isApprox(Eigen::Vector3d(10, 20, 30)));
    CHECK(scans[2].m_pose.translation().isApprox(Eigen::Vector3d(10, 22, 30)));
    CHECK(scans[2].m_pose.linear().isApprox(scans[1].m_pose.linear()));
    CHECK(scans[2].gui_rotation[2] == doctest::Approx(90));
    CHECK(scans[2].gui_translation[1] == doctest::Approx(22));
    CHECK_FALSE(scans[2].visible);
    for (const auto i : { 0U, 3U })
        CHECK(scans[i].m_pose.matrix() == before[i].m_pose.matrix());
    for (std::size_t i = 0; i < scans.size(); ++i)
    {
        CHECK(scans[i].m_initial_pose.matrix() == before[i].m_initial_pose.matrix());
        CHECK(scans[i].points_local == before[i].points_local);
    }
}

TEST_CASE("a world transform left-multiplies an explicit scope and restore is selective")
{
    auto clouds = makeClouds();
    auto& scans = clouds.point_clouds;
    scans[2].m_pose.rotate(Eigen::AngleAxisd(0.4, Eigen::Vector3d::UnitX()));
    const auto before = scans;
    const auto result = EditStep2Poses(clouds, edit(transform_scope_operation, { 2, 0 }, { { "transform", anchor } }));
    REQUIRE(std::holds_alternative<std::vector<std::size_t>>(result));
    CHECK(scans[2].m_pose.translation().isApprox(Eigen::Vector3d(11, 24, 35)));
    const Eigen::Matrix3d expected =
        (Eigen::AngleAxisd(std::acos(-1.0) / 2, Eigen::Vector3d::UnitZ()) * Eigen::AngleAxisd(0.4, Eigen::Vector3d::UnitX()))
            .toRotationMatrix();
    CHECK(scans[2].m_pose.linear().isApprox(expected));
    CHECK(scans[1].m_pose.matrix() == before[1].m_pose.matrix());
    CHECK(scans[3].m_pose.matrix() == before[3].m_pose.matrix());

    const auto restored = EditStep2Poses(clouds, edit(restore_poses_operation, { 2 }));
    REQUIRE(std::holds_alternative<std::vector<std::size_t>>(restored));
    CHECK(scans[2].m_pose.matrix() == before[2].m_initial_pose.matrix());
    CHECK(scans[0].m_pose.translation().isApprox(Eigen::Vector3d(11, 20, 35)));
}

TEST_CASE("invalid inputs leave every scan untouched")
{
    auto clouds = makeClouds();
    // An invalid second pose must not leave a partially restored scope.
    clouds.point_clouds[2].m_initial_pose(0, 3) = std::numeric_limits<double>::infinity();
    const auto before = clouds.point_clouds;
    for (const auto& scope : { json::array(),
                               json{ 0, 0 },
                               json{ 0, -1 },
                               json{ 0, 4 },
                               json{ 0, 1.25 },
                               json{ 0, std::numeric_limits<std::uint64_t>::max() } })
    {
        const auto rejected = EditStep2Poses(clouds, edit(restore_poses_operation, scope));
        REQUIRE(std::holds_alternative<OperationError>(rejected));
        CHECK(std::get<OperationError>(rejected).code == "invalid_scope");
    }
    auto rejected = EditStep2Poses(clouds, edit(edit_segment_operation, { 0, 2 }, { { "anchor_pose", anchor } }));
    CHECK(std::get<OperationError>(rejected).code == "invalid_scope");
    auto reflection = anchor;
    reflection[2][2] = -1;
    rejected = EditStep2Poses(clouds, edit(transform_scope_operation, { 0 }, { { "transform", reflection } }));
    CHECK(std::get<OperationError>(rejected).code == "invalid_parameters");
    auto nonfinite = anchor;
    nonfinite[0][3] = std::numeric_limits<double>::infinity();
    rejected = EditStep2Poses(clouds, edit(transform_scope_operation, { 0 }, { { "transform", nonfinite } }));
    CHECK(std::get<OperationError>(rejected).code == "invalid_parameters");
    rejected = EditStep2Poses(clouds, edit(restore_poses_operation, { 0, 2 }));
    CHECK(std::get<OperationError>(rejected).code == "invalid_pose");
    for (std::size_t i = 0; i < before.size(); ++i)
        CHECK(clouds.point_clouds[i].m_pose.matrix() == before[i].m_pose.matrix());
}

TEST_CASE("a drag preview leaves committed poses and its GUI opening revision intact")
{
    auto clouds = makeClouds();
    GuiStateBridge bridge(
        { []
          {
              return StateSummary{};
          },
          [&](const OperationRequest& request) -> MutationResult
          {
              auto edited = EditStep2Poses(clouds, request);
              if (const auto* error = std::get_if<OperationError>(&edited))
                  return *error;
              return StateSummary{};
          },
          {},
          {},
          {},
          {} });
    bridge.Open();
    const auto initial = bridge.Target();
    const auto original = clouds.point_clouds[1].m_pose;
    const auto preview = PreviewStep2Segment(clouds, 1, 2, Eigen::Affine3d::Identity());
    REQUIRE(preview.size() == 2);
    CHECK(preview.front().translation().isZero());
    CHECK(clouds.point_clouds[1].m_pose.matrix() == original.matrix());
    CHECK(bridge.Target() == initial);
    auto execute = [&](const OperationRequest& request)
    {
        GuiResponse response;
        bridge.Dispatch(
            "states.execute",
            { { "target", { { "state_id", initial.state_id }, { "revision", initial.revision } } },
              { "operation", request.operation },
              { "scope", request.scope },
              { "parameters", request.parameters } },
            [&](GuiResponse value)
            {
                response = std::move(value);
            });
        return response;
    };
    const auto invalid = execute(edit(restore_poses_operation, json::array()));
    CHECK(std::get<OperationError>(invalid).code == "invalid_scope");
    CHECK(bridge.Target() == initial);
    const auto committed = execute(edit(transform_scope_operation, { 1 }, { { "transform", anchor } }));
    CHECK(std::holds_alternative<json>(committed));
    CHECK(bridge.Target().revision == initial.revision + 1);
    const auto pose = clouds.point_clouds[1].m_pose;
    const auto stale = execute(edit(restore_poses_operation, { 1 }));
    CHECK(std::get<OperationError>(stale).code == "stale_revision");
    CHECK(clouds.point_clouds[1].m_pose.matrix() == pose.matrix());
}

TEST_CASE("the worker persists scoped edits in a native session")
{
    const auto operations = DescribeOperations();
    for (const auto id : { edit_segment_operation, transform_scope_operation, restore_poses_operation })
    {
        const auto operation = std::ranges::find(operations, id, &OperationDescriptor::id);
        REQUIRE(operation != operations.end());
        CHECK(operation->operation_class == OperationClass::Mutate);
    }
    struct Scratch
    {
        fs::path path = fs::canonical(fs::temp_directory_path()) /
            ("hdmapping-segments-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~Scratch()
        {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } scratch;
    REQUIRE(fs::create_directory(scratch.path));
    auto session = std::make_shared<Session>();
    session->point_clouds_container = makeClouds();
    SessionCaptureSource source(
        session,
        []
        {
            return target;
        },
        json::object());
    RunContext context{ scratch.path / "output", {}, {} };
    REQUIRE(fs::create_directory(context.output_directory));
    const auto snapshot = CaptureSnapshot(source, target.revision, scratch.path / "input", context);
    REQUIRE(std::holds_alternative<SnapshotRef>(snapshot));
    auto request = edit(edit_segment_operation, { 1, 2 }, { { "anchor_pose", anchor } });
    const auto& input = std::get<SnapshotRef>(snapshot);
    auto stale = request;
    ++stale.target.revision;
    CHECK(std::get<OperationError>(ExecuteOperation(stale, input, context)).code == "revision_conflict");
    CHECK(std::get<OperationError>(ExecuteOperation(edit(restore_poses_operation, { 4 }), input, context)).code == "invalid_scope");
    CHECK(fs::is_empty(context.output_directory));
    const auto edited = ExecuteOperation(request, input, context);
    REQUIRE(std::holds_alternative<ArtifactManifest>(edited));
    const auto loaded = LoadSessionSnapshot(std::get<ArtifactManifest>(edited).snapshot);
    REQUIRE(std::holds_alternative<SessionSnapshot>(loaded));
    const auto& output = std::get<SessionSnapshot>(loaded);
    auto& clouds = session->point_clouds_container;
    const auto& published = output.session->point_clouds_container;
    CHECK(published.point_clouds[2].m_pose.translation().isApprox(Eigen::Vector3d(10, 22, 30)));
    CHECK(published.point_clouds[0].m_pose.matrix() == clouds.point_clouds[0].m_pose.matrix());
    CHECK(published.point_clouds[3].m_pose.matrix() == clouds.point_clouds[3].m_pose.matrix());
    CHECK(clouds.point_clouds[2].m_pose.translation().isApprox(Eigen::Vector3d(4, -1, 5)));

    // Apply a result to a live session without replacing its scans or unrelated poses.
    clouds.point_clouds[3].m_pose.translation().x() = 100;
    const auto applied = ApplyStep2PoseResult(clouds, published, output.parameters.at("segment_result").at("scope"));
    REQUIRE(std::holds_alternative<std::vector<std::size_t>>(applied));
    CHECK(clouds.point_clouds[2].m_pose.matrix() == published.point_clouds[2].m_pose.matrix());
    CHECK(clouds.point_clouds[3].m_pose.translation().x() == 100);
}
