#include <doctest.h>

#include <Core/export_laz.h>
#include <HDMapping/Processing/GuiStateBridge.h>
#include <HDMapping/Processing/ResultPublisher.h>

#include "../src/RuntimeJson.h"

#include <chrono>
#include <fstream>
#include <future>

using namespace HDMapping::Processing;
using nlohmann::json;
using RuntimeJson::target_to_json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{
    struct Scratch
    {
        const fs::path root = fs::canonical(fs::temp_directory_path()) /
            ("hdmapping-state-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        Scratch()
        {
            REQUIRE(fs::create_directory(root));
        }
        ~Scratch()
        {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
    };

    GuiResponse call(GuiStateBridge& bridge, std::string_view method, const json& params)
    {
        std::promise<GuiResponse> response;
        auto future = response.get_future();
        bridge.Dispatch(
            method,
            params,
            [&](GuiResponse value)
            {
                response.set_value(std::move(value));
            });
        REQUIRE(future.wait_for(5s) == std::future_status::ready);
        return future.get();
    }

    std::string failure_code(const GuiResponse& response)
    {
        return std::get<OperationError>(response).code;
    }

    class Source final : public CaptureSource
    {
    public:
        explicit Source(TargetRef target, std::function<void()> before_write = {})
            : target_(std::move(target))
            , before_write_(std::move(before_write))
        {
        }
        TargetRef Target() const override
        {
            return target_;
        }
        CaptureResult Materialize(const fs::path& directory, RunContext&) override
        {
            if (before_write_)
            {
                before_write_();
            }
            if (!exportLaz((directory / "scan.laz").string(), { { 1, 2, 3 } }, { 4 }, { 5 }))
            {
                throw std::runtime_error("Could not materialize test cloud");
            }
            for (const auto* name : { "poses.mrp", "initial.mri" })
            {
                std::ofstream(directory / name) << "1\nscan.laz\n1 0 0 3\n0 1 0 0\n0 0 1 0\n";
            }
            const json metadata{
                { "Session Settings", { { "poses_file_name", "poses.mrp" }, { "initial_poses_file_name", "initial.mri" } } },
                { "laz_file_names", json::array({ { { "file_name", "scan.laz" } } }) },
            };
            std::ofstream(directory / "session.mjs") << metadata;
            return SnapshotContents{ "native_session", "session.mjs", json::object(), {} };
        }

    private:
        TargetRef target_;
        std::function<void()> before_write_;
    };

    json capture_params(const TargetRef& target, std::string phase)
    {
        return { { "target", target_to_json(target) }, { "job_id", "job" }, { "phase", std::move(phase) }, { "access", "mutate" } };
    }
} // namespace

TEST_CASE("Opening identity and revision protect mutations on the window thread")
{
    std::string path = "one.mjp";
    int child_pose = 0;
    int camera_angle = 0;
    const auto owner = std::this_thread::get_id();
    GuiStateCallbacks callbacks;
    callbacks.DescribeState = [&]
    {
        return StateSummary{ {}, { { "child_pose", child_pose } }, { { "path", path }, { "camera_angle", camera_angle } } };
    };
    callbacks.ApplyMutation = [&](const OperationRequest& request) -> MutationResult
    {
        CHECK(std::this_thread::get_id() == owner);
        if (request.operation != "child.pose")
        {
            return OperationError{ "unsupported_operation", "", nullptr };
        }
        child_pose = request.parameters.at("pose").get<int>();
        return callbacks.DescribeState();
    };
    GuiStateBridge first(callbacks);
    GuiStateBridge second(callbacks);
    const auto ref = first.Target();
    CHECK(ref.state_id != second.Target().state_id);
    const json original{ { "target", target_to_json(ref) } };
    CHECK(failure_code(call(second, "states.describe", original)) == "state_closed");
    camera_angle = 45;
    CHECK(std::get<json>(call(first, "states.describe", original)).at("metadata").at("camera_angle") == 45);
    path = "saved-as.mjp";
    CHECK(std::get<json>(call(first, "states.describe", original)).at("metadata").at("path") == path);
    const json edit{
        { "target", target_to_json(ref) }, { "operation", "child.pose" }, { "parameters", { { "pose", 5 } } }, { "scope", json::object() }
    };
    auto wrong_thread = std::async(
        std::launch::async,
        [&]
        {
            return call(first, "states.execute", edit);
        });
    CHECK(failure_code(wrong_thread.get()) == "wrong_thread");
    CHECK(first.Target() == ref);
    const auto summary = std::get<json>(call(first, "states.execute", edit));
    CHECK(summary.at("target").at("revision") == ref.revision + 1);
    CHECK(child_pose == 5);
    CHECK(failure_code(call(first, "states.execute", edit)) == "stale_revision");
    CHECK(failure_code(call(first, "states.apply_result", original)) == "stale_revision");
    first.Open();
    CHECK(first.Target().state_id != ref.state_id);
    CHECK(failure_code(call(first, "states.apply_result", original)) == "state_closed");
}

TEST_CASE("A reserved capture outlives the window and writes its pinned RAM data in the background")
{
    Scratch scratch;
    std::promise<void> entered, resume;
    auto gate = resume.get_future();
    const auto owner = std::this_thread::get_id();
    std::thread::id materializer;
    GuiStateCallbacks callbacks;
    callbacks.PinCapture = [&](TargetRef ref)
    {
        CHECK(std::this_thread::get_id() == owner);
        return std::make_unique<Source>(
            ref,
            [&]
            {
                materializer = std::this_thread::get_id();
                entered.set_value();
                if (gate.wait_for(5s) != std::future_status::ready)
                {
                    throw std::runtime_error("Timed out waiting for test");
                }
            });
    };
    GuiStateBridge bridge(std::move(callbacks));
    const auto target = bridge.Target();
    CHECK(std::get<json>(call(bridge, "states.capture", capture_params(target, "reserve"))) == json::object());
    CHECK(bridge.Reserved());
    CHECK(failure_code(call(bridge, "states.execute", { { "target", target_to_json(target) } })) == "state_busy");
    bridge.Close();
    CHECK(std::get<json>(call(bridge, "states.list", json::object())).at("states").empty());
    CHECK(bridge.Preparing());
    auto write = capture_params(target, "write");
    write["directory"] = RuntimeJson::path_to_utf8(scratch.root / "snapshot");
    std::promise<GuiResponse> done;
    bridge.Dispatch(
        "states.capture",
        write,
        [&](GuiResponse response)
        {
            done.set_value(std::move(response));
        });
    REQUIRE(entered.get_future().wait_for(5s) == std::future_status::ready);
    CHECK(bridge.Preparing());
    CHECK(materializer != owner);
    CHECK(failure_code(call(bridge, "states.capture", write)) == "invalid_request");
    resume.set_value();
    const auto snapshot = std::get<json>(done.get_future().get());
    CHECK_FALSE(bridge.Preparing());
    CHECK(bridge.Reserved());
    CHECK(std::get<json>(call(bridge, "states.capture", capture_params(target, "release"))) == json::object());
    CHECK_FALSE(bridge.Reserved());
    CHECK_FALSE(ValidateSnapshot({ scratch.root / "snapshot" / "snapshot.json", target }));
    CHECK(snapshot.at("target") == target_to_json(target));
}

TEST_CASE("Opening external session input reserves an empty Step 2 without a snapshot")
{
    GuiStateCallbacks callbacks;
    callbacks.AllowExternalSessionOpen = true;
    GuiStateBridge bridge(std::move(callbacks));
    const auto target = bridge.Target();
    const auto reserve = capture_params(target, "reserve_external_session");
    GuiStateBridge unsupported(GuiStateCallbacks{});
    CHECK(
        failure_code(call(unsupported, "states.capture", capture_params(unsupported.Target(), "reserve_external_session"))) ==
        "invalid_request");
    REQUIRE(std::holds_alternative<json>(call(bridge, "states.capture", reserve)));
    CHECK(bridge.Reserved());
    CHECK_FALSE(bridge.Preparing());
    CHECK(failure_code(call(bridge, "states.capture", reserve)) == "state_busy");
    CHECK(failure_code(call(bridge, "states.capture", capture_params(target, "write"))) == "invalid_request");
    bridge.Close();
    CHECK(std::holds_alternative<json>(call(bridge, "states.capture", capture_params(target, "release"))));
    CHECK_FALSE(bridge.Reserved());
    CHECK(failure_code(call(bridge, "states.capture", reserve)) == "state_closed");
}

TEST_CASE("Only a verified result receipt updates the exact current opening")
{
    Scratch scratch;
    std::optional<ArtifactManifest> applied;
    GuiStateCallbacks callbacks;
    callbacks.ApplyResult = [&](const ArtifactManifest& artifact)
    {
        applied = artifact;
        return std::optional<OperationError>{};
    };
    GuiStateBridge bridge(std::move(callbacks));
    const auto target = bridge.Target();
    Source source(target);
    RunContext context{};
    const auto stage = scratch.root / ".hdmapping-result-stage-state-test";
    REQUIRE(std::holds_alternative<SnapshotRef>(CaptureSnapshot(source, target.revision, stage, context)));
    auto publication = PublishResult(stage, scratch.root / "publication");
    if (const auto* error = std::get_if<OperationError>(&publication))
    {
        FAIL(error->message);
    }
    const auto& manifest = std::get<ArtifactManifest>(publication);
    const auto replacement = scratch.root / ".hdmapping-result-stage-newer";
    REQUIRE(std::holds_alternative<SnapshotRef>(CaptureSnapshot(source, target.revision, replacement, context)));
    REQUIRE(
        std::holds_alternative<ArtifactManifest>(PublishResult(replacement, scratch.root / "publication", { true, manifest.artifact_id })));
    const auto path = scratch.root / "result.json";
    json receipt{
        { "version", HDMAPPING_GUI_VERSION }, { "wire", 1 }, { "job_id", "job" }, { "manifest", RuntimeJson::manifest_to_json(manifest) }
    };
    auto& relative = receipt.at("manifest").at("snapshot").at("manifest_path");
    relative = RuntimeJson::path_to_utf8(fs::relative(manifest.snapshot.manifest_path, scratch.root));
    std::ofstream(path) << receipt;
    json params{ { "job_id", "other-job" }, { "target", target_to_json(target) }, { "manifest_path", RuntimeJson::path_to_utf8(path) } };
    CHECK(failure_code(call(bridge, "states.apply_result", params)) == "invalid_published_result");
    CHECK_FALSE(applied);
    params["job_id"] = "job";
    auto result = call(bridge, "states.apply_result", params);
    if (const auto* error = std::get_if<OperationError>(&result))
    {
        FAIL(error->message);
    }
    CHECK(std::get<json>(result) == json::object());
    REQUIRE(applied.has_value());
    CHECK(applied->artifact_id == manifest.artifact_id);
    CHECK(failure_code(call(bridge, "states.apply_result", params)) == "stale_revision");
    bridge.Close();
    CHECK(failure_code(call(bridge, "states.apply_result", params)) == "state_closed");
    CHECK(std::holds_alternative<ArtifactManifest>(ReadPublishedResult(scratch.root / "publication")));
}

TEST_CASE("Shared reads release their GUI pins after handoff and disconnect frees outstanding reservations")
{
    Scratch scratch;
    GuiStateCallbacks callbacks;
    callbacks.PinCapture = [](TargetRef ref)
    {
        return std::make_unique<Source>(std::move(ref));
    };
    GuiStateBridge bridge(std::move(callbacks));
    const auto original = bridge.Target();
    auto first = capture_params(original, "reserve");
    first["access"] = "read";
    auto second = first;
    second["job_id"] = "another-read";
    CHECK(std::get<json>(call(bridge, "states.capture", first)) == json::object());
    CHECK(std::get<json>(call(bridge, "states.capture", second)) == json::object());
    auto writer = capture_params(original, "reserve");
    writer["job_id"] = "writer";
    CHECK(failure_code(call(bridge, "states.capture", writer)) == "state_busy");
    first["phase"] = "write";
    first["directory"] = RuntimeJson::path_to_utf8(scratch.root / "read");
    CHECK(std::get<json>(call(bridge, "states.capture", first)).at("target") == target_to_json(original));
    CHECK(bridge.Reserved());
    second["phase"] = "release";
    CHECK(std::get<json>(call(bridge, "states.capture", second)) == json::object());
    CHECK_FALSE(bridge.Reserved());
    CHECK(std::get<json>(call(bridge, "states.capture", writer)) == json::object());
    CHECK(bridge.Reserved());
    bridge.Disconnect();
    CHECK_FALSE(bridge.Preparing());
    CHECK_FALSE(bridge.Reserved());
    CHECK(bridge.Target() == original);
    writer["phase"] = "write";
    CHECK(failure_code(call(bridge, "states.capture", writer)) == "reservation_missing");
}
