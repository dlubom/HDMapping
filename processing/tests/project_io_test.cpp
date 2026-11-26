#include <HDMapping/Processing/Operation.h>

#include <doctest/doctest.h>
#include <laszip/laszip_api.h>

#include <algorithm>

using namespace HDMapping::Processing;

TEST_CASE("Step 3 advertises the project IO contract")
{
    const auto operations = DescribeOperations();
    for (const std::string_view name :
         { "project.open", "project.save", "project.add_session", "project.remove_session", "project.set_ground_truth" })
        CHECK(std::ranges::find(operations, name, &OperationDescriptor::id) != operations.end());
}

#include "project_capture.h"
#include "project_operations.h"
#include "step3_io_operations.h"

#include "../src/ResultPublisherIO.h"

#include <HDMapping/SessionPaths.h>

#include <chrono>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace
{
    //! Private files for each project contract test, including its source data.
    class ProjectFiles
    {
    public:
        const fs::path root = fs::canonical(fs::temp_directory_path()) /
            ("project-io-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        ProjectFiles()
        {
            REQUIRE(fs::create_directory(root));
        }

        ~ProjectFiles()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }

        ProjectFiles(const ProjectFiles&) = delete;
        ProjectFiles& operator=(const ProjectFiles&) = delete;
    };

    std::string bytes(const fs::path& path)
    {
        REQUIRE(fs::is_regular_file(path));
        std::ifstream stream(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
    }

    template<class T>
    T checked(std::variant<T, OperationError> result)
    {
        if (const auto* error = std::get_if<OperationError>(&result))
            FAIL(error->code << ": " << error->message);
        return std::get<T>(std::move(result));
    }

    std::vector<Session> make_sessions()
    {
        std::vector<Session> sessions(2);
        for (std::size_t i = 0; i < sessions.size(); ++i)
        {
            auto& child = sessions[i];
            child.is_ground_truth = i == 1;
            auto& scan = child.point_clouds_container.point_clouds.emplace_back();
            scan.points_local.emplace_back(1.25, 2.5, 3.75);
            scan.intensities = { 42 };
            scan.m_pose = scan.m_initial_pose = Eigen::Affine3d::Identity();
            scan.m_pose.translation().x() = 9.125 + static_cast<double>(i);
            scan.m_initial_pose.translation().y() = 20.5 + static_cast<double>(i);
        }
        return sessions;
    }

    ProjectSettings make_project()
    {
        Edge link{};
        link.index_session_from = 0;
        link.index_session_to = 1;
        link.relative_pose_tb = { 1, 2, 3, 0, 0, 0 };
        link.relative_pose_tb_weights = { 1, 2, 3, 4, 5, 6 };
        link.is_fixed_om = true;
        return { { "scan.mjs", "reference.mjs" }, { link } };
    }

    fs::path stage(const TargetRef& target, const fs::path& path, const ProjectSettings& project, const std::vector<Session>& children)
    {
        RunContext context{ {}, {}, {} };
        ProjectCapture writer(target, children, ProjectSettingsJson(project), json::object(), true);
        static_cast<void>(checked(CaptureSnapshot(writer, target.revision, path, context)));
        return path;
    }

    fs::path entry(const ArtifactManifest& artifact)
    {
        return checked(ReadSnapshot(artifact.snapshot)).entry;
    }
} // namespace

TEST_CASE("a native project save owns the whole graph and leaves the source poses byte-identical")
{
    const ProjectFiles files;
    const TargetRef target{ "project-fixture", 42 };
    const auto sessions = make_sessions();
    const auto project = make_project();
    const auto baseline =
        checked(PublishResult(stage(target, files.root / ".hdmapping-result-stage-input", project, sessions), files.root / "input"));
    const auto input_mjp = entry(baseline);
    const auto input_poses_0 = input_mjp.parent_path() / "0" / "poses.mrp";
    const auto input_poses_1 = input_mjp.parent_path() / "1" / "poses.mrp";
    const auto before_0 = bytes(input_poses_0);
    const auto before_1 = bytes(input_poses_1);
    const auto old_manifest = bytes(input_mjp);

    const OperationRequest open{ "project.open", target, json::object(), { { "path", sessionPathText(input_mjp) } } };
    RunContext preparation{ files.root / "snapshot", {}, {} };
    const auto snapshot = checked(CaptureProjectOpen(open, preparation));
    const auto manifest = checked(ReadSnapshot(snapshot));
    REQUIRE(manifest.parameters.at("project_sessions").size() == 2);
    CHECK(manifest.parameters.at("project_sessions").at(1).at("revision") == target.revision);

    RunContext save{ files.root / "save", {}, {} };
    REQUIRE(fs::create_directory(save.output_directory));
    const auto result = checked(ExecuteOperation({ "project.save", target, json::object(), json::object() }, snapshot, save));
    const auto saved_mjp = entry(result);
    const auto saved = ReadProjectSettings(saved_mjp, true);
    REQUIRE(saved.session_file_names.size() == 2);
    REQUIRE(saved.edges.size() == 1);
    CHECK(saved.edges.front().index_session_from == 1);
    CHECK(saved.edges.front().index_session_to == 0);
    CHECK(saved.edges.front().is_fixed_om);
    CHECK(saved.edges.front().relative_pose_tb.pz == 3);
    CHECK(saved.edges.front().relative_pose_tb_weights.ka == 6);
    for (std::size_t i = 0; i < saved.session_file_names.size(); ++i)
    {
        const auto child = ReadProjectSession(saved.session_file_names.at(i));
        const auto path = sessionPathFromText(saved.session_file_names.at(i));
        CHECK(path.parent_path().parent_path() == saved_mjp.parent_path());
        CHECK(path.extension() == ".mjs");
        CHECK(fs::is_regular_file(path.parent_path() / "poses.mrp"));
        CHECK(fs::is_regular_file(path.parent_path() / "initial.mri"));
        CHECK(child.is_ground_truth == (i == 0));
        const auto& scan = child.point_clouds_container.point_clouds.at(0);
        CHECK(scan.m_pose.translation().x() == (i == 0 ? 10.125 : 9.125));
        CHECK(scan.m_initial_pose.translation().y() == (i == 0 ? 21.5 : 20.5));
    }
    CHECK(bytes(input_mjp) == old_manifest);
    CHECK(bytes(input_poses_0) == before_0);
    CHECK(bytes(input_poses_1) == before_1);

    const OperationRequest duplicate{
        "project.add_session", target, json::object(), { { "path", sessionPathText(input_mjp.parent_path() / "0" / "session.mjs") } }
    };
    RunContext reject{ files.root / "duplicate", {}, {} };
    CHECK(std::holds_alternative<OperationError>(CaptureProjectAdd(duplicate, snapshot, reject)));
    CHECK(std::holds_alternative<OperationError>(CaptureProjectAdd(duplicate, result.snapshot, reject)));
    CHECK_FALSE(fs::exists(reject.output_directory));

    RunContext edits{ files.root / "edit", {}, {} };
    REQUIRE(fs::create_directory(edits.output_directory));
    const auto edited = checked(
        ExecuteOperation({ "project.set_ground_truth", target, { { "session_index", -1 } }, json::object() }, result.snapshot, edits));
    const auto state = checked(LoadProjectSnapshot(edited.snapshot));
    CHECK_FALSE(state.sessions.at(0).is_ground_truth);
    CHECK_FALSE(state.sessions.at(1).is_ground_truth);

    RunContext removal{ files.root / "remove", {}, {} };
    REQUIRE(fs::create_directory(removal.output_directory));
    const auto removed = checked(
        ExecuteOperation({ "project.remove_session", target, { { "session_index", 0 } }, json::object() }, edited.snapshot, removal));
    const auto remaining = checked(LoadProjectSnapshot(removed.snapshot));
    REQUIRE(remaining.sessions.size() == 1);
    CHECK(remaining.settings.edges.empty());

    RunContext addition{ files.root / "add", {}, {} };
    const OperationRequest add{ "project.add_session", target, json::object(), { { "path", saved.session_file_names.at(0) } } };
    const auto added = checked(CaptureProjectAdd(add, removed.snapshot, addition));
    const auto expanded = checked(LoadProjectSnapshot(added));
    CHECK(expanded.sessions.size() == 2);
    CHECK(expanded.sessions.at(0).is_ground_truth);

    const auto cloud = files.root / fs::path(u8"źródłowy skan.laz");
    fs::copy_file(saved_mjp.parent_path() / "0" / "scan_lio_0.laz", cloud);
    RunContext cloud_input{ files.root / "add-cloud", {}, {} };
    const auto with_cloud = checked(CaptureProjectAdd(
        { "project.add_session", target, json::object(), { { "path", sessionPathText(cloud) } } }, removed.snapshot, cloud_input));
    CHECK(checked(LoadProjectSnapshot(with_cloud)).sessions.at(1).point_clouds_container.point_clouds.size() == 1);
}

TEST_CASE("interrupting a project write keeps the previous graph readable")
{
    const ProjectFiles files;
    const TargetRef target{ "project-fixture", 19 };
    auto sessions = make_sessions();
    const auto project = make_project();
    const auto destination = files.root / "published";
    const auto original =
        checked(PublishResult(stage(target, files.root / ".hdmapping-result-stage-original", project, sessions), destination));
    const auto previous_entry = entry(original);
    const auto original_head = bytes(destination / "result.json");
    const auto previous_poses = bytes(previous_entry.parent_path() / "0" / "poses.mrp");

    sessions[0].point_clouds_container.point_clouds[0].m_pose.translation().x() += 100;
    const auto interrupted_stage = files.root / ".hdmapping-result-stage-cancelled";
    std::stop_source stop;
    RunContext context{ {},
                        stop.get_token(),
                        [&stop](const ProgressEvent& event)
                        {
                            if (event.stage == "write_project_sessions" && event.current == 1)
                                stop.request_stop();
                        } };
    ProjectCapture writer(target, sessions, ProjectSettingsJson(project), json::object(), true);
    const auto cancelled = CaptureSnapshot(writer, target.revision, interrupted_stage, context);
    CHECK(std::get<OperationError>(cancelled).code == "cancelled");
    CHECK_FALSE(fs::exists(interrupted_stage));

    const auto replacement = stage(target, files.root / ".hdmapping-result-stage-replacement", project, sessions);
    const PublicationIO io{ [](const fs::path& path, const std::string& value)
                            {
                                std::ofstream(path) << value;
                            },
                            [](const fs::path& from, const fs::path& to, bool replace)
                            {
                                if (replace)
                                    throw fs::filesystem_error(
                                        "commit head", from, to, std::make_error_code(std::errc::no_space_on_device));
                                MovePublishedPath(from, to, false);
                            } };
    const auto failed = PublishResult(replacement, destination, { true, original.artifact_id }, io);
    CHECK(std::get<OperationError>(failed).code == "publication_failed");
    CHECK_FALSE(fs::exists(destination / "result-replacement"));

    CHECK(bytes(destination / "result.json") == original_head);
    const auto restored = checked(LoadProjectSnapshot(checked(ReadPublishedResult(destination)).snapshot));
    REQUIRE(restored.sessions.size() == 2);
    CHECK(restored.sessions[1].is_ground_truth);
    CHECK(restored.settings.edges.size() == 1);
    CHECK(bytes(previous_entry.parent_path() / "0" / "poses.mrp") == previous_poses);
}

TEST_CASE("P16 project export owns its selected exchange files and relocates a complete project")
{
    const ProjectFiles files;
    const TargetRef target{ "project-export", 8 };
    auto sessions = make_sessions();
    auto& scan = sessions.front().point_clouds_container.point_clouds.front();
    scan.visible = false;
    scan.file_name = "original-scan.laz";
    scan.m_pose_temp = scan.m_pose;
    scan.timestamps = { 1000000000.0 };
    for (const double time : { 2000000000.0, 1000000000.0, 1000000000.0 })
    {
        auto& node = scan.local_trajectory.emplace_back();
        node.m_pose = Eigen::Affine3d::Identity();
        node.m_pose.translation().x() = 1;
        node.timestamps = { time, time + 50 };
    }
    const auto input =
        checked(PublishResult(stage(target, files.root / ".hdmapping-result-stage-input", make_project(), sessions), files.root / "input"));
    ProjectCapture capture(target, sessions, ProjectSettingsJson(make_project()), json::object());
    RunContext capture_context{ {}, {}, {} };
    const auto live = checked(CaptureSnapshot(capture, target.revision, files.root / "live", capture_context));
    const auto poses = entry(input).parent_path() / "0" / "poses.mrp";
    const auto original = bytes(poses);
    const auto selected = json{ { "sessions", { { { "session_index", 0 }, { "scan_indices", { 0 } } } } } };
    const auto run = [&](const std::string& directory, const json& scope, const json& options)
    {
        RunContext context{ files.root / directory, {}, {} };
        REQUIRE(fs::create_directory(context.output_directory));
        return ExecuteOperation({ "project.export", target, scope, options }, live, context);
    };

    const auto catalog = DescribeOperations();
    CHECK(std::ranges::find(catalog, "project.export", &OperationDescriptor::id) != catalog.end());
    const auto tum = checked(run("tum", selected, { { "format", "trajectory_tum" } }));
    CHECK(tum.dependencies.empty());
    auto tum_text = bytes(entry(tum));
    std::erase(tum_text, '\r');
    CHECK(
        tum_text ==
        "1.000000000 10.1250000000 0.0000000000 0.0000000000 0.0000000000 0.0000000000 0.0000000000 1.0000000000\n"
        "2.000000000 10.1250000000 0.0000000000 0.0000000000 0.0000000000 0.0000000000 0.0000000000 1.0000000000\n");
    const auto csv = checked(run("csv", selected, { { "format", "trajectory_csv" }, { "timestamps", "both" }, { "pose", "quaternion" } }));
    auto csv_text = bytes(entry(csv));
    std::erase(csv_text, '\r');
    CHECK(csv_text.starts_with("timestampLidar,timestampUnix,x,y,z,qx,qy,qz,qw\n2000000000,2000000050,10.125"));
    for (const std::string_view format : { "las", "laz", "trajectory_las", "trajectory_laz", "poses" })
    {
        const auto result = checked(run("parent.laz-" + std::string(format), selected, { { "format", format } }));
        CHECK(result.files.size() == 2);
        CHECK(result.dependencies.empty());
        CHECK(fs::file_size(entry(result)) > 0);
        if (format == "poses")
        {
            REQUIRE(bytes(entry(result)).find("original-scan.laz") != std::string::npos);
            continue;
        }
        laszip_POINTER handle = nullptr;
        REQUIRE(laszip_create(&handle) == 0);
        const auto output = sessionPathText(entry(result));
        laszip_BOOL compressed = 0;
        REQUIRE(laszip_open_reader(handle, output.c_str(), &compressed) == 0);
        CHECK((compressed != 0) == format.ends_with("laz"));
        laszip_header* header = nullptr;
        REQUIRE(laszip_get_header_pointer(handle, &header) == 0);
        CHECK(header->number_of_point_records == (format.starts_with("trajectory") ? 3U : 1U));
        REQUIRE(laszip_read_point(handle) == 0);
        if (!format.starts_with("trajectory"))
        {
            laszip_point* point = nullptr;
            REQUIRE(laszip_get_point_pointer(handle, &point) == 0);
            CHECK(point->gps_time == scan.timestamps.front());
            CHECK(point->intensity == 42);
            double coordinates[3]{};
            REQUIRE(laszip_get_coordinates(handle, coordinates) == 0);
            CHECK(coordinates[0] == doctest::Approx(10.375));
        }
        CHECK(laszip_close_reader(handle) == 0);
        CHECK(laszip_destroy(handle) == 0);
    }
    CHECK(bytes(poses) == original);
    const auto bad = run("bad", { { "sessions", { { { "session_index", 1 }, { "scan_indices", { 999 } } } } } }, { { "format", "laz" } });
    REQUIRE(std::holds_alternative<OperationError>(bad));
    CHECK(std::get<OperationError>(bad).code == "invalid_parameter");
    CHECK_FALSE(fs::exists(files.root / "bad" / "export"));

    RunContext page_context{ files.root / "page", {}, {} };
    REQUIRE(fs::create_directory(page_context.output_directory));
    const auto page = checked(ExecuteOperation(
        { "project.list_items", target, { { "kind", "scans" }, { "session_index", 0 } }, json::object() }, live, page_context));
    CHECK(json::parse(bytes(entry(page))).at("items").front().at("trajectory_nodes") == 3);
    const auto full = checked(run("full", json::object(), { { "format", "project" } }));
    const auto old_root = full.snapshot.manifest_path.parent_path();
    const auto moved = files.root / "relocated";
    fs::rename(old_root, moved);
    fs::rename(files.root / "input", files.root / "offline");
    fs::rename(files.root / "live", files.root / "offline-live");
    const auto reopened = checked(LoadProjectSnapshot({ moved / full.snapshot.manifest_path.filename(), target }));
    REQUIRE(reopened.sessions.size() == 2);
    CHECK(reopened.sessions.at(1).is_ground_truth);
    CHECK(reopened.settings.edges.size() == 1);
    CHECK(reopened.sessions.at(0).point_clouds_container.point_clouds.at(0).local_trajectory.size() == 3);
    CHECK(full.dependencies.empty());
}

TEST_CASE("P16 project pages preserve indices and are bounded for large projects")
{
    std::vector<Session> sessions(2001);
    std::vector<Edge> edges(2001, make_project().edges.front());
    sessions.at(2000).is_ground_truth = true;
    const auto first = ProjectItemsPage(sessions, edges, { { "kind", "sessions" }, { "limit", 100 } });
    CHECK(first.at("total") == 2001);
    CHECK(first.at("next_offset") == 100);
    CHECK(first.at("items").size() == 100);
    CHECK(first.at("items").back().at("index") == 99);
    const auto last = ProjectItemsPage(sessions, edges, { { "kind", "sessions" }, { "offset", 2000 }, { "limit", 100 } });
    CHECK_FALSE(last.contains("next_offset"));
    CHECK(last.at("items").front().at("ground_truth") == true);
    CHECK(last.at("items").front().at("index") == 2000);
    const auto constraints = ProjectItemsPage(sessions, edges, { { "kind", "constraints" }, { "offset", 1000 }, { "limit", 25 } });
    CHECK(constraints.at("next_offset") == 1025);
    CHECK(constraints.at("items").front().at("index") == 1000);
    CHECK(constraints.at("items").back().at("to").at("session_index") == 1);
    CHECK_THROWS_AS(ProjectItemsPage(sessions, edges, { { "kind", "sessions" }, { "offset", 2002 } }), std::invalid_argument);
}
