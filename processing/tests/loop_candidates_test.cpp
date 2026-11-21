#include "step2_constraint_operations.h"

#include <Core/session.h>
#include <HDMapping/Processing/SessionSnapshot.h>
#include <HDMapping/SessionPaths.h>

#include <algorithm>
#include <chrono>
#include <fstream>

#include <doctest/doctest.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

using namespace HDMapping::Processing;

namespace
{
    using nlohmann::json;
    namespace fs = std::filesystem;
    const TargetRef target{ "loop-revisit", 7 };
    const json empty = json::object();

    struct LoopFixture
    {
        fs::path root = fs::canonical(fs::temp_directory_path()) /
            ("hdmapping-bow-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::path camera = root / sessionPathFromText("kamera żółć");
        std::shared_ptr<Session> session = std::make_shared<Session>();

        LoopFixture()
        {
            REQUIRE(fs::create_directories(camera));
            auto& scans = session->point_clouds_container.point_clouds;
            scans.resize(3);
            scans[1].m_pose.translation().x() = 25.0;
            scans[2].m_pose.translation().x() = 0.25;
            for (std::size_t i = 0; i < scans.size(); ++i)
            {
                scans[i].m_initial_pose = scans[i].m_pose;
                scans[i].points_local.emplace_back(0.0, 0.0, 0.0);
                scans[i].timestamps.push_back(1000000000.0 * (static_cast<double>(i) + 1));
                scans[i].intensities.push_back(0);
            }
            const auto err =
                EditStep2Constraints(*session, { "constraints.add", target, empty, { { "index_from", 0 }, { "index_to", 1 } } });
            REQUIRE_FALSE(err.has_value());

            cv::Mat image(480, 640, CV_8UC1);
            cv::RNG random(971);
            random.fill(image, cv::RNG::UNIFORM, 0, 255);
            cv::GaussianBlur(image, image, cv::Size(3, 3), 0);
            cv::Mat shifted;
            const cv::Matx23d transform(1.0, 0.0, 9.0, 0.0, 1.0, 5.0);
            cv::warpAffine(image, shifted, transform, image.size());
            for (const auto& [name, frame] : { std::pair{ "cam0_1000000000.jpg", image }, std::pair{ "cam0_3000000000.jpg", shifted } })
            {
                std::vector<unsigned char> bytes;
                REQUIRE(cv::imencode(".jpg", frame, bytes));
                std::ofstream out(camera / name, std::ios::binary);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                REQUIRE(out.good());
            }
        }

        ~LoopFixture()
        {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
    };
} // namespace

TEST_CASE("BoW detection publishes a scored revisit separately from the active graph")
{
#ifdef HDMAPPING_LOOP_CANDIDATES_TSAN
    // The distro OpenCV parallel backend crosses into the instrumented debug oneTBB runtime.
    // Keep that external backend serial while TSan checks the operation and the snapshot code.
    cv::setNumThreads(0);
#endif
    LoopFixture fixture;
    SessionCaptureSource source(
        fixture.session,
        []
        {
            return target;
        },
        empty);
    RunContext context{ fixture.root, {}, {} };
    const auto graph = DescribeActiveConstraints(*fixture.session);
    const auto input = CaptureSnapshot(source, target.revision, fixture.root / "input", context);
    REQUIRE(std::holds_alternative<SnapshotRef>(input));
    const auto& snapshot = std::get<SnapshotRef>(input);
    OperationRequest request{ "constraints.detect_candidates",
                              target,
                              { { "scan_indices", { 0, 2 } } },
                              { { "camera_dirs", { sessionPathText(fixture.camera) } } } };
    const auto executed = ExecuteOperation(request, snapshot, context);
    if (const auto* error = std::get_if<OperationError>(&executed))
        INFO(error->code, " ", error->message);
    REQUIRE(std::holds_alternative<ArtifactManifest>(executed));
    const auto& artifact = std::get<ArtifactManifest>(executed);
    CHECK(artifact.target == target);
    const auto published = ReadSnapshot(artifact.snapshot);
    REQUIRE(std::holds_alternative<SnapshotContents>(published));
    const auto& contents = std::get<SnapshotContents>(published);
    CHECK(contents.kind == "step2_loop_candidates");
    std::ifstream file(contents.entry);
    const auto detection = json::parse(file);
    const auto& candidates = detection.at("candidates");
    REQUIRE(candidates.size() == 1);
    const auto& candidate = candidates.at(0);
    CHECK(candidate.at("edge").at("index_from") == 0);
    CHECK(candidate.at("edge").at("index_to") == 2);
    CHECK(candidate.at("inliers").get<int>() >= 30);
    CHECK(candidate.at("bow_score").get<double>() >= 0.005);
    CHECK(candidate.at("arc_gap_m") == 49.75);
    CHECK(candidate.at("xy_dist_m") == 0.25);
    CHECK(candidate.at("frame_from").at("timestamp_ns") == "1000000000");
    CHECK(candidate.at("frame_to").at("timestamp_ns") == "3000000000");
    for (const auto* frame : { "frame_from", "frame_to" })
    {
        const auto path = contents.entry.parent_path() / sessionPathFromText(candidate.at(frame).at("file").get<std::string>());
        CHECK(fs::is_regular_file(path));
        CHECK(std::ranges::find(artifact.files, path) != artifact.files.end());
    }
    CHECK(DescribeActiveConstraints(*fixture.session) == graph);
    auto loaded = LoadSessionSnapshot(snapshot);
    REQUIRE(std::holds_alternative<SessionSnapshot>(loaded));
    auto& reopened = *std::get<SessionSnapshot>(loaded).session;
    CHECK(DescribeActiveConstraints(reopened) == graph);

    REQUIRE_FALSE(EditStep2Constraints(reopened, { "constraints.add", target, empty, candidate.at("edge") }).has_value());
    CHECK(DescribeActiveConstraints(reopened).at(0) == graph.at(0));
    CHECK(DescribeActiveConstraints(reopened).at(1).at("relative_pose") == candidate.at("edge").at("relative_pose"));
    REQUIRE_FALSE(
        EditStep2Constraints(
            *fixture.session, { "constraints.replace", target, { { "edge_indices", { 0 } } }, { { "edges", { candidate.at("edge") } } } })
            .has_value());
    CHECK(DescribeActiveConstraints(*fixture.session).size() == 1);
    CHECK(DescribeActiveConstraints(*fixture.session).at(0).at("index_to") == 2);

    request.scope["scan_indices"] = { 0, 0 };
    const auto duplicate = ExecuteOperation(request, snapshot, context);
    REQUIRE(std::holds_alternative<OperationError>(duplicate));
    CHECK(std::get<OperationError>(duplicate).code == "invalid_scope");

    std::stop_source stop;
    stop.request_stop();
    RunContext cancelledContext{ fixture.root / "cancelled", stop.get_token(), {} };
    const auto cancelled = ExecuteOperation(request, snapshot, cancelledContext);
    REQUIRE(std::holds_alternative<OperationError>(cancelled));
    CHECK(std::get<OperationError>(cancelled).code == "cancelled");
    CHECK_FALSE(fs::exists(cancelledContext.output_directory / "loop-candidates"));

    request.target.revision++;
    const auto stale = ExecuteOperation(request, snapshot, context);
    REQUIRE(std::holds_alternative<OperationError>(stale));
    CHECK(std::get<OperationError>(stale).code == "revision_conflict");
}
