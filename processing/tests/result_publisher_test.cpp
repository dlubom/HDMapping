#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include "../src/ResultPublisherIO.h"

#include <Core/export_laz.h>
#include <Core/session.h>
#include <HDMapping/Processing/ResultPublisher.h>
#include <HDMapping/Processing/SnapshotStore.h>
#include <HDMapping/SessionPaths.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <span>

using namespace HDMapping::Processing;
namespace fs = std::filesystem;
using nlohmann::json;

namespace
{
    class Scratch
    {
    public:
        const fs::path path = fs::canonical(fs::temp_directory_path()) /
            ("hdmapping-publication-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        Scratch()
        {
            if (!fs::create_directory(path))
            {
                throw std::runtime_error("Test directory already exists");
            }
        }

        ~Scratch()
        {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    };

    void write_json(const fs::path& path, const json& value)
    {
        std::ofstream file(path);
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file << value;
        file.close();
    }

    json read_json(const fs::path& path)
    {
        std::ifstream file(path);
        return json::parse(file);
    }

    class NativeSource final : public CaptureSource
    {
    public:
        explicit NativeSource(fs::path input)
            : entry(std::move(input))
        {
        }

        fs::path entry;
        bool include_trajectories = true;
        std::function<void()> poses_written;

        TargetRef Target() const override
        {
            return { "native-session", 42 };
        }

        CaptureResult Materialize(const fs::path& directory, RunContext&) override
        {
            auto metadata = read_json(entry);
            auto& settings = metadata.at("Session Settings");
            const auto source = entry.parent_path();
            for (const auto* key : { "poses_file_name", "initial_poses_file_name", "point_sizes_path", "index_poses_path" })
            {
                if (settings.contains(key))
                {
                    const auto input = source / sessionPathFromText(settings.at(key).get<std::string>());
                    fs::copy_file(input, directory / input.filename());
                    settings[key] = sessionPathText(input.filename());
                }
            }
            if (poses_written)
            {
                poses_written();
            }
            settings["folder_name"] = ".";
            settings["out_folder_name"] = ".";
            settings["out_poses_file_name"] = settings.at("poses_file_name");
            std::vector<fs::path> dependencies;
            for (auto& scan : metadata.at("laz_file_names"))
            {
                const auto cloud = fs::canonical(source / sessionPathFromText(scan.at("file_name").get<std::string>()));
                scan["file_name"] = sessionPathText(cloud);
                dependencies.push_back(cloud);
                const auto trajectory = sessionTrajectoryPath(cloud);
                if (!trajectory.empty() && fs::exists(trajectory) && include_trajectories)
                {
                    dependencies.push_back(trajectory);
                }
            }
            write_json(directory / "session.mjs", metadata);
            return SnapshotContents{ "native_session", "session.mjs", { { "method", "fixture" } }, dependencies };
        }
    };

    fs::path make_source(const fs::path& root, int pose = 7)
    {
        fs::create_directory(root);
        const auto cloud = root / "scan room_lio_0.laz";
        if (!exportLaz(cloud.string(), { { 1, 2, 3 } }, { 4 }, { 5 }))
        {
            throw std::runtime_error("Could not write test LAZ");
        }
        for (const auto* name : { "poses.mrp", "initial.mri" })
        {
            std::ofstream(root / name) << "1\nscan room_lio_0.laz\n1 0 0 " << pose << "\n0 1 0 0\n0 0 1 0\n0 0 0 1\n";
        }
        std::ofstream(root / "trajectory_lio_0.csv") << "header\n12 1 0 0 2 0 1 0 3 0 0 1 4\n";
        std::ofstream(root / "index_poses.txt") << "0 0\n";
        const auto entry = root / "session.mjs";
        write_json(
            entry,
            { { "Session Settings",
                { { "poses_file_name", "poses.mrp" },
                  { "initial_poses_file_name", "initial.mri" },
                  { "index_poses_path", "index_poses.txt" },
                  { "offset_to_apply_x", 123.45 } } },
              { "laz_file_names", json::array({ { { "file_name", "scan room_lio_0.laz" }, { "fixed_x", true } } }) } });
        return entry;
    }

    fs::path prepare(NativeSource& source, const fs::path& root, const std::string& id)
    {
        const auto stage = root / (".hdmapping-result-stage-" + id);
        RunContext context{};
        auto snapshot = CaptureSnapshot(source, source.Target().revision, stage, context);
        if (const auto* error = std::get_if<OperationError>(&snapshot))
        {
            throw std::runtime_error(error->message);
        }
        return stage;
    }

    ArtifactManifest checked_result(RunResult result)
    {
        if (const auto* error = std::get_if<OperationError>(&result))
        {
            throw std::runtime_error(error->code + ": " + error->message);
        }
        return std::get<ArtifactManifest>(std::move(result));
    }
} // namespace

TEST_CASE("Publishing and exporting preserves native files and immutable shared inputs")
{
    const Scratch scratch;
    const auto source_path = make_source(scratch.path / "source");
    {
        std::ofstream trajectory(source_path.parent_path() / "trajectory_lio_0.csv", std::ios::app);
        trajectory << "13 1 0 0 3 0 1 0 4 0 0 1 5\n";
    }
    auto original_metadata = read_json(source_path);
    original_metadata["ground_control_points"] = json::array({ { { "index_to_node_inner", 0 }, { "index_to_node_outer", 1 } } });
    write_json(source_path, original_metadata);
    const auto unicode_source = scratch.path / fs::path(u8"źródło chmur");
    fs::rename(source_path.parent_path(), unicode_source);
    NativeSource source(unicode_source / source_path.filename());
    const auto destination = scratch.path / "published";
    const auto stage = prepare(source, scratch.path, "original");
    const auto published = checked_result(PublishResult(stage, destination));
    CHECK_FALSE(fs::exists(stage));
    REQUIRE(published.dependencies.size() == 2);
    CHECK(checked_result(ReadPublishedResult(destination)).artifact_id == published.artifact_id);
    const auto export_path = scratch.path / fs::path(u8"pełny zestaw");
    const auto immutable = unicode_source / "scan room_lio_0.laz";
    fs::permissions(immutable, fs::perms::owner_write, fs::perm_options::remove);
    auto exported_result = ExportComplete(published, export_path);
    fs::permissions(immutable, fs::perms::owner_write, fs::perm_options::add);
    const auto exported = checked_result(std::move(exported_result));
    CHECK(exported.dependencies.empty());
    fs::rename(source.entry.parent_path(), scratch.path / "offline");
    CHECK(ValidateSnapshot(published.snapshot).has_value());
    CHECK_FALSE(ValidateSnapshot(exported.snapshot).has_value());
    const auto native_entry = exported.snapshot.manifest_path.parent_path() / "session.mjs";
    CHECK(read_json(native_entry).at("Session Settings").at("offset_to_apply_x") == 123.45);
    CHECK(fs::exists(scratch.path / "offline" / "trajectory_lio_0.csv"));
    CHECK(fs::exists(native_entry.parent_path() / "index_poses.txt"));
    Session session;
    REQUIRE(session.load(sessionPathText(native_entry), false, 0, 0, 0, false));
    const auto& scans = session.point_clouds_container.point_clouds;
    REQUIRE(scans.size() == 1);
    CHECK(scans[0].m_pose.translation().x() == 7.0);
    CHECK(scans[0].m_initial_pose.translation().x() == 7.0);
    CHECK(scans[0].fixed_x);
    REQUIRE(scans[0].points_local.size() == 1);
    CHECK(scans[0].points_local[0].z() == 3.0);
    REQUIRE(scans[0].local_trajectory.size() == 2);
    CHECK(scans[0].local_trajectory[0].m_pose.translation().y() == 3.0);
    REQUIRE(session.ground_control_points.gpcs.size() == 1);
    CHECK(session.ground_control_points.gpcs[0].index_to_node_outer == 1);
}

TEST_CASE("Existing results and unrelated directories require an exact replacement identity")
{
    const Scratch scratch;
    NativeSource source(make_source(scratch.path / "source"));
    const auto destination = scratch.path / "published";
    const auto before = checked_result(PublishResult(prepare(source, scratch.path, "before"), destination));
    const auto stage = prepare(source, scratch.path, "rejected");
    CHECK(std::holds_alternative<OperationError>(PublishResult(stage, destination)));
    CHECK_FALSE(fs::exists(stage));
    CHECK(std::holds_alternative<OperationError>(PublishResult(prepare(source, scratch.path, "stale"), destination, { true, "stale" })));
    auto unrelated = scratch.path / "unrelated";
    fs::create_directory(unrelated);
    CHECK(
        std::holds_alternative<OperationError>(
            PublishResult(prepare(source, scratch.path, "unrelated"), unrelated, { true, before.artifact_id })));
    CHECK(fs::is_empty(unrelated));
    CHECK(checked_result(ReadPublishedResult(destination)).artifact_id == before.artifact_id);
    const auto after = checked_result(PublishResult(prepare(source, scratch.path, "after"), destination, { true, before.artifact_id }));
    CHECK(before.artifact_id != after.artifact_id);
    CHECK(
        std::holds_alternative<OperationError>(
            PublishResult(prepare(source, scratch.path, "before"), destination, { true, after.artifact_id })));
    CHECK(!ValidateSnapshot(before.snapshot));
    CHECK(checked_result(ReadPublishedResult(destination)).artifact_id == after.artifact_id);
}

TEST_CASE("Native read rejects unusable poses, invalid clouds and unlisted sidecars before commit")
{
    const Scratch scratch;
    NativeSource source(make_source(scratch.path / "source"));
    const auto destination = scratch.path / "published";
    source.include_trajectories = false;
    CHECK(std::holds_alternative<OperationError>(PublishResult(prepare(source, scratch.path, "no-trajectory"), destination)));
    source.include_trajectories = true;
    std::ofstream(source.entry.parent_path() / "poses.mrp") << "1\nother.laz\n1 0 0 7\n0 1 0 0\n0 0 1 0\n0 0 0 1\n";
    CHECK(std::holds_alternative<OperationError>(PublishResult(prepare(source, scratch.path, "wrong-pose"), destination)));
    std::ofstream(source.entry.parent_path() / "poses.mrp") << "1\nscan room_lio_0.laz\n1 0 0 7\n0 1 0 0\n0 0 1 0\n0 0 0 1\n";
    const auto trajectory = source.entry.parent_path() / "trajectory_lio_0.csv";
    std::ofstream(trajectory) << "header\ninvalid 1 0 0 2 0 1 0 3 0 0 1 4\n";
    CHECK(std::holds_alternative<OperationError>(PublishResult(prepare(source, scratch.path, "bad-trajectory"), destination)));
    std::ofstream(trajectory) << "header\n12 1 0 0 2 0 1 0 3 0 0 1 4\n";
    auto metadata = read_json(source.entry);
    metadata["ground_control_points"] = json::array({ { { "index_to_node_outer", 0 }, { "index_to_node_inner", 9 } } });
    write_json(source.entry, metadata);
    CHECK(std::holds_alternative<OperationError>(PublishResult(prepare(source, scratch.path, "bad-gcp"), destination)));
    metadata["ground_control_points"] = json::array({ { { "index_to_node_inner", 0 }, { "index_to_node_outer", 9 } } });
    write_json(source.entry, metadata);
    CHECK(std::holds_alternative<OperationError>(PublishResult(prepare(source, scratch.path, "bad-gcp-node"), destination)));
    metadata.erase("ground_control_points");
    write_json(source.entry, metadata);
    std::ofstream(source.entry.parent_path() / "scan room_lio_0.laz") << "not a point cloud";
    CHECK(std::holds_alternative<OperationError>(PublishResult(prepare(source, scratch.path, "bad-laz"), destination)));
    CHECK_FALSE(fs::exists(destination));
    CHECK(fs::exists(source.entry.parent_path() / "scan room_lio_0.laz"));
}

TEST_CASE("Failed writes and failed commit rename preserve the old result and remove uncommitted files")
{
    const Scratch scratch;
    NativeSource source(make_source(scratch.path / "source"));
    const auto destination = scratch.path / "published";
    const auto before = checked_result(PublishResult(prepare(source, scratch.path, "original"), destination));
    const PublishOptions replace{ true, before.artifact_id };
    auto io =
        PublicationIO{ [](const fs::path& path, const std::string&)
                       {
                           std::ofstream(path) << "{";
                           throw fs::filesystem_error("write commit manifest", path, std::make_error_code(std::errc::no_space_on_device));
                       },
                       [](const fs::path& from, const fs::path& to, bool)
                       {
                           fs::rename(from, to);
                       } };
    const auto no_space = prepare(source, scratch.path, "no-space");
    CHECK(std::holds_alternative<OperationError>(PublishResult(no_space, destination, replace, io)));
    CHECK_FALSE(fs::exists(no_space));
    CHECK(checked_result(ReadPublishedResult(destination)).artifact_id == before.artifact_id);
    io.write_manifest = [](const fs::path& path, const std::string& text)
    {
        std::ofstream(path) << text;
    };
    io.move = [](const fs::path& from, const fs::path& to, bool commits)
    {
        if (commits)
        {
            throw fs::filesystem_error("rename commit manifest", from, to, std::make_error_code(std::errc::permission_denied));
        }
        fs::rename(from, to);
    };
    const auto denied = prepare(source, scratch.path, "denied");
    CHECK(std::holds_alternative<OperationError>(PublishResult(denied, destination, replace, io)));
    CHECK_FALSE(fs::exists(denied));
    CHECK_FALSE(fs::exists(destination / "result-denied"));
    CHECK(checked_result(ReadPublishedResult(destination)).artifact_id == before.artifact_id);
    CHECK(std::distance(fs::directory_iterator(destination), fs::directory_iterator()) == 2);
    io.move = [](const fs::path& from, const fs::path& to, bool)
    {
        if (from.filename().string().starts_with(".hdmapping-result-commit-"))
        {
            fs::create_directory(to);
            std::ofstream(to / "unrelated") << "preserve";
        }
        fs::rename(from, to);
    };
    const auto collision = scratch.path / "late-collision";
    CHECK(std::holds_alternative<OperationError>(PublishResult(prepare(source, scratch.path, "late-collision"), collision, {}, io)));
    CHECK(fs::exists(collision / "unrelated"));
    CHECK_FALSE(fs::exists(collision / "result.json"));
    for (const auto& entry : fs::directory_iterator(scratch.path))
    {
        CHECK_FALSE(entry.path().filename().string().starts_with(".hdmapping-result-"));
    }
}

namespace
{
    void print_result(const ArtifactManifest& manifest)
    {
        const auto contents = std::get<SnapshotContents>(ReadSnapshot(manifest.snapshot));
        Session session;
        if (!session.load(sessionPathText(contents.entry), false, 0, 0, 0, false))
        {
            throw std::runtime_error("Native MJS read failed");
        }
        const auto& scans = session.point_clouds_container.point_clouds;
        std::size_t points = 0;
        std::size_t trajectories = 0;
        for (const auto& scan : scans)
        {
            points += scan.points_local.size();
            trajectories += scan.local_trajectory.size();
        }
        const auto& first = scans.at(0);
        const auto report = json{
            { "entry", sessionPathText(contents.entry) },
            { "artifact_id", manifest.artifact_id },
            { "dependencies", manifest.dependencies.size() },
            { "scans", scans.size() },
            { "points", points },
            { "trajectory_nodes", trajectories },
            { "first_pose_x", first.m_pose.translation().x() },
            { "first_initial_x", first.m_initial_pose.translation().x() },
            { "first_point_z", first.points_local.at(0).z() },
            { "offset_to_apply_x", session.point_clouds_container.offset_to_apply.x() },
        };
        std::cout << "READOUT " << report.dump() << '\n';
    }

    int run_probe(std::span<char*> args)
    {
        const std::string_view command(args[1]);
        if (command == "--make-input" && args.size() == 4)
        {
            make_source(args[2], std::stoi(args[3]));
        }
        else if (
            (command == "--publish" || command == "--pause-after-poses" || command == "--pause-before-head" ||
             command == "--race-publish") &&
            args.size() >= 5)
        {
            NativeSource source(fs::canonical(args[2]));
            if (command == "--pause-after-poses")
            {
                source.poses_written = []
                {
                    std::cout << "POSES_WRITTEN" << std::endl;
                    std::string resume;
                    std::getline(std::cin, resume);
                };
            }
            fs::path destination(args[3]);
            const auto stage = prepare(source, destination.parent_path(), args[4]);
            if (command == "--race-publish")
            {
                std::cout << "READY" << std::endl;
                std::string start;
                if (!std::getline(std::cin, start))
                {
                    throw std::runtime_error("Race controller disconnected");
                }
            }
            const PublishOptions options = args.size() == 6 ? PublishOptions{ true, args[5] } : PublishOptions{};
            if (command == "--pause-before-head")
            {
                PublicationIO io{ [](const fs::path& path, const std::string& json)
                                  {
                                      std::ofstream output(path);
                                      output.exceptions(std::ios::failbit | std::ios::badbit);
                                      output << json;
                                      output.close();
                                  },
                                  [](const fs::path& from, const fs::path& to, bool replacement)
                                  {
                                      if (replacement)
                                      {
                                          std::cout << "READY_TO_COMMIT" << std::endl;
                                          std::string resume;
                                          std::getline(std::cin, resume);
                                          throw std::runtime_error("Publication test lost its controller");
                                      }
                                      fs::rename(from, to);
                                  } };
                checked_result(PublishResult(stage, destination, options, io));
                return 0;
            }
            const auto artifact = checked_result(PublishResult(stage, destination, options));
            std::cout << "PUBLISHED " << artifact.artifact_id << '\n';
        }
        else if (command == "--read" && args.size() == 3)
        {
            print_result(checked_result(ReadPublishedResult(args[2])));
        }
        else if (command == "--export" && args.size() == 4)
        {
            checked_result(ExportComplete(checked_result(ReadPublishedResult(args[2])), args[3]));
        }
        else if (command == "--cleanup" && args.size() == 5)
        {
            if (const auto error = DiscardResultStaging(fs::path(args[2]) / (".hdmapping-result-stage-" + std::string(args[3])), args[4]))
            {
                throw std::runtime_error(error->message);
            }
        }
        else
        {
            return 2;
        }
        return 0;
    }
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::string_view(argv[1]).starts_with("--") && std::string_view(argv[1]).find("--dt-") != 0)
        {
            return run_probe(std::span(argv, static_cast<std::size_t>(argc)));
        }
        return doctest::Context(argc, argv).run();
    } catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
