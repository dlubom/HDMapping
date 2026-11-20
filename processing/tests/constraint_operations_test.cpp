#include "step2_constraint_operations.h"

#include <Core/session.h>
#include <HDMapping/Processing/Operation.h>
#include <HDMapping/Processing/SessionSnapshot.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <doctest/doctest.h>

using namespace HDMapping::Processing;

TEST_CASE("one-session constraints and optimization have public operation contracts")
{
    const auto operations = DescribeOperations();
    for (const auto* name : { "constraints.list",
                              "constraints.add",
                              "constraints.update",
                              "constraints.remove",
                              "constraints.replace",
                              "constraints.run_icp",
                              "step2.optimize_graph" })
        CHECK(std::ranges::find(operations, name, &OperationDescriptor::id) != operations.end());
}

TEST_CASE("a graph replacement requires the revision of the same opening")
{
    RunContext context{};
    const SnapshotRef input{ {}, { "opening", 3 } };
    for (const TargetRef& target : { TargetRef{ "opening", 2 }, TargetRef{ "different-opening", 3 } })
    {
        const auto result = ExecuteOperation({ "constraints.replace", target, {}, {} }, input, context);
        REQUIRE(std::holds_alternative<OperationError>(result));
        CHECK(std::get<OperationError>(result).code == "revision_conflict");
    }
}

namespace
{
    using nlohmann::json;
    namespace fs = std::filesystem;

    const json empty = json::object();
    const TargetRef target{ "synthetic-constraints", 7 };

    json edge(int from, int to)
    {
        return { { "index_from", from },
                 { "index_to", to },
                 { "relative_pose", { 0.035, -0.015, 0.025, 0.014, -0.021, 0.018 } },
                 { "sigmas", { 0.01, 0.02, 0.03, 0.004, 0.005, 0.006 } },
                 { "fixed", { true, false, false, false, true, false } } };
    }

    std::optional<OperationError> edit(Session& session, std::string operation, json scope, json parameters)
    {
        return EditStep2Constraints(session, { std::move(operation), target, std::move(scope), std::move(parameters) });
    }

    class ConstraintFixture
    {
    public:
        const fs::path directory = fs::canonical(fs::temp_directory_path()) /
            ("hdmapping-constraints-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::shared_ptr<Session> session = std::make_shared<Session>();
        SnapshotRef snapshot;

        ConstraintFixture()
        {
            REQUIRE(fs::create_directory(directory));
            auto& scans = session->point_clouds_container.point_clouds;
            scans.resize(3);
            for (auto& scan : scans)
            {
                scan.m_initial_pose = scan.m_pose;
                for (int i = 0; i < 196; ++i)
                {
                    scan.points_local.emplace_back(0.3 * (i % 7), 0.29 * ((i / 7) % 7), 0.32 * (i / 49) + 0.001 * i);
                    scan.timestamps.push_back(i + 0.5);
                    scan.intensities.push_back(17);
                }
            }
            REQUIRE_FALSE(edit(*session, "constraints.add", empty, edge(0, 1)).has_value());
            REQUIRE_FALSE(edit(*session, "constraints.add", empty, edge(1, 2)).has_value());
            SessionCaptureSource source(
                session,
                []
                {
                    return target;
                },
                empty);
            RunContext context{};
            auto capture = CaptureSnapshot(source, target.revision, directory / "input", context);
            REQUIRE(std::holds_alternative<SnapshotRef>(capture));
            snapshot = std::get<SnapshotRef>(std::move(capture));
        }

        ~ConstraintFixture()
        {
            std::error_code ignored;
            fs::remove_all(directory, ignored);
        }
    };
} // namespace

TEST_CASE("adding, updating and removing active edges preserves the unrelated graph")
{
    Session session;
    session.point_clouds_container.point_clouds.resize(3);
    REQUIRE_FALSE(edit(session, "constraints.add", empty, edge(0, 1)).has_value());
    const auto first = DescribeActiveConstraints(session).at(0);
    REQUIRE_FALSE(edit(session, "constraints.add", empty, edge(1, 2)).has_value());
    REQUIRE(DescribeActiveConstraints(session).size() == 2);
    CHECK(DescribeActiveConstraints(session).at(0) == first);
    CHECK(first.at("kind") == "active");

    REQUIRE_FALSE(edit(session, "constraints.update", { { "edge_index", 1 } }, { { "sigmas", { 1, 2, 3, 4, 5, 6 } } }).has_value());
    CHECK(DescribeActiveConstraints(session).at(0) == first);
    CHECK(DescribeActiveConstraints(session).at(1).at("relative_pose") == edge(1, 2).at("relative_pose"));
    REQUIRE_FALSE(edit(session, "constraints.remove", { { "edge_index", 1 } }, empty).has_value());
    CHECK(DescribeActiveConstraints(session) == json::array({ first }));
}

TEST_CASE("replace validates every endpoint and requires an explicit scope before committing")
{
    Session session;
    session.point_clouds_container.point_clouds.resize(3);
    REQUIRE_FALSE(edit(session, "constraints.add", empty, edge(0, 1)).has_value());
    REQUIRE_FALSE(edit(session, "constraints.add", empty, edge(1, 2)).has_value());
    const auto before = DescribeActiveConstraints(session);
    const json scope{ { "edge_indices", { 1 } } };
    const json proposed{ { "edges", json::array({ edge(0, 2) }) } };
    CHECK(edit(session, "constraints.replace", empty, proposed).has_value());
    CHECK(edit(session, "constraints.replace", { { "edge_indices", { 1, 1 } } }, proposed).has_value());
    CHECK(edit(session, "constraints.replace", scope, { { "edges", json::array({ edge(0, 2), edge(0, 3) }) } }).has_value());
    CHECK(edit(session, "constraints.update", { { "edge_index", 1 } }, { { "index_to", 1 } }).has_value());
    auto candidate = edge(0, 2);
    candidate["kind"] = "candidate";
    CHECK(edit(session, "constraints.add", empty, candidate).has_value());
    CHECK(DescribeActiveConstraints(session) == before);
    REQUIRE_FALSE(edit(session, "constraints.replace", scope, proposed).has_value());
    CHECK(DescribeActiveConstraints(session).at(0) == before.at(0));
    CHECK(DescribeActiveConstraints(session).at(1).at("index_from") == 0);
    CHECK(DescribeActiveConstraints(session).at(1).at("index_to") == 2);
}

TEST_CASE("native publication preserves transforms, sigmas and fixed flags for old and new edges")
{
    const ConstraintFixture fixture;
    RunContext context{ fixture.directory / "output", {}, {} };
    REQUIRE(fs::create_directory(context.output_directory));
    const auto result = ExecuteOperation({ "constraints.add", target, empty, edge(0, 2) }, fixture.snapshot, context);
    if (const auto* error = std::get_if<OperationError>(&result))
        FAIL(error->code << ": " << error->message);
    REQUIRE(std::holds_alternative<ArtifactManifest>(result));
    const auto& artifact = std::get<ArtifactManifest>(result);
    const auto checked = LoadSessionSnapshot(artifact.snapshot);
    REQUIRE(std::holds_alternative<SessionSnapshot>(checked));
    const auto& native = std::get<SessionSnapshot>(checked);
    const auto active = DescribeActiveConstraints(*native.session);
    const auto before = DescribeActiveConstraints(*fixture.session);
    REQUIRE(active.size() == 3);
    CHECK(active.at(0) == before.at(0));
    CHECK(active.at(1) == before.at(1));
    CHECK(active.at(2).at("relative_pose") == edge(0, 2).at("relative_pose"));
    CHECK(active.at(2).at("fixed") == edge(0, 2).at("fixed"));
    for (std::size_t i = 0; i < 6; ++i)
        CHECK(active.at(2).at("sigmas").at(i).get<double>() == doctest::Approx(edge(0, 2).at("sigmas").at(i).get<double>()).epsilon(1e-13));
}

TEST_CASE("edge ICP refines the selected active edge without touching unselected edges")
{
    const ConstraintFixture fixture;
    RunContext context{ fixture.directory / "output", {}, {} };
    REQUIRE(fs::create_directory(context.output_directory));
    const auto result = ExecuteOperation(
        { "constraints.run_icp", target, { { "edge_indices", { 0 } } }, { { "radius_m", 0.1 }, { "iterations", 10 } } },
        fixture.snapshot,
        context);
    if (const auto* error = std::get_if<OperationError>(&result))
        FAIL(error->code << ": " << error->message);
    REQUIRE(std::holds_alternative<ArtifactManifest>(result));
    const auto checked = LoadSessionSnapshot(std::get<ArtifactManifest>(result).snapshot);
    REQUIRE(std::holds_alternative<SessionSnapshot>(checked));
    const auto active = DescribeActiveConstraints(*std::get<SessionSnapshot>(checked).session);
    const auto before = DescribeActiveConstraints(*fixture.session);
    CHECK(active.at(1) == before.at(1));
    CHECK(active.at(0).at("sigmas") == before.at(0).at("sigmas"));
    for (const auto& component : active.at(0).at("relative_pose"))
        CHECK(std::abs(component.get<double>()) < 1e-6);
}

TEST_CASE("cancelling a graph job at its first optimization checkpoint suppresses native publication")
{
    const ConstraintFixture fixture;
    std::stop_source stop;
    bool checkpoint = false;
    RunContext context{ fixture.directory / "cancelled",
                        stop.get_token(),
                        [&](const ProgressEvent& progress)
                        {
                            if (progress.stage == "optimize_graph")
                            {
                                checkpoint = true;
                                stop.request_stop();
                            }
                        } };
    const auto result =
        ExecuteOperation({ "step2.optimize_graph", target, { { "edge_indices", { 0, 1 } } }, empty }, fixture.snapshot, context);
    CHECK(checkpoint);
    REQUIRE(std::holds_alternative<OperationError>(result));
    CHECK(std::get<OperationError>(result).code == "cancelled");
    CHECK_FALSE(fs::exists(context.output_directory / "native"));
    const auto input = LoadSessionSnapshot(fixture.snapshot);
    REQUIRE(std::holds_alternative<SessionSnapshot>(input));
    CHECK(DescribeActiveConstraints(*std::get<SessionSnapshot>(input).session) == DescribeActiveConstraints(*fixture.session));
}

TEST_CASE("the motion model alone accepts an already aligned graph")
{
    const ConstraintFixture fixture;
    RunContext context{ fixture.directory / "output", {}, {} };
    REQUIRE(fs::create_directory(context.output_directory));
    const auto result =
        ExecuteOperation({ "step2.optimize_graph", target, { { "edge_indices", json::array() } }, empty }, fixture.snapshot, context);
    if (const auto* error = std::get_if<OperationError>(&result))
        FAIL(error->code << ": " << error->message);
    REQUIRE(std::holds_alternative<ArtifactManifest>(result));
    const auto checked = LoadSessionSnapshot(std::get<ArtifactManifest>(result).snapshot);
    REQUIRE(std::holds_alternative<SessionSnapshot>(checked));
    const auto& output = std::get<SessionSnapshot>(checked);
    CHECK(DescribeActiveConstraints(*output.session) == DescribeActiveConstraints(*fixture.session));
    for (const auto& scan : output.session->point_clouds_container.point_clouds)
        CHECK(scan.m_pose.matrix().isIdentity(1e-12));
}
