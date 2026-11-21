#include "step1_operations.h"

#include "ResultPublisherIO.h"
#include "SnapshotStorage.h"
#include "lidar_odometry.h"
#include "odometry_capture.h"
#include "step1_parameters.h"

#include <HDMapping/Processing/ResultPublisher.h>
#include <HDMapping/SessionPaths.h>

#include <array>
#include <fstream>
#include <random>
#include <stdexcept>

namespace proc = HDMapping::Processing;
using json = nlohmann::json;

namespace
{
    using proc::ArtifactManifest;
    using proc::OperationError;

    constexpr std::array operations{ std::string_view{ "step1.load" },      std::string_view{ "step1.prepare" },
                                     std::string_view{ "step1.odometry" },  std::string_view{ "step1.consistency" },
                                     std::string_view{ "step1.reference" }, std::string_view{ "step1.save" },
                                     std::string_view{ "step1.export" } };
    constexpr std::string_view recording_kind = "step1_recording";
    constexpr std::string_view prepared_kind = "step1_prepared";
    constexpr std::string_view native_kind = "native_session";

    OperationError failure(std::string code, std::string message)
    {
        return { std::move(code), std::move(message), json::object() };
    }

    void check_stop(const std::stop_token& stop)
    {
        if (stop.stop_requested())
            throw std::runtime_error("Step 1 operation cancelled");
    }

    json scope_properties(std::string_view operation)
    {
        const json mode{ { "type", "string" } };
        const json flag{ { "type", "boolean" } };
        const json index{ { "type", "integer" }, { "minimum", 0 } };
        const json path{ { "type", "string" } };
        if (operation == "step1.load" || operation == "step1.prepare")
            return { { "debug", flag }, { "recording_directory", path } };
        if (operation == "step1.odometry")
            return { { "save", { { "type", "boolean" }, { "default", true } } },
                     { "debug", flag },
                     { "destination", path },
                     { "recording_directory", path } };
        if (operation == "step1.reference")
        {
            return { { "mode", { { "type", "string" }, { "enum", { "load", "filter", "align" } } } },
                     { "files", { { "type", "array" }, { "items", path }, { "minItems", 1 } } } };
        }
        if (operation == "step1.save")
            return { { "destination", path } };
        if (operation == "step1.export")
        {
            return { { "mode", { { "type", "string" }, { "enum", { "complete", "las", "laz", "trajectory", "poses" } } } },
                     { "begin", index },
                     { "end", index },
                     { "selected", flag },
                     { "destination", path } };
        }
        return json::object();
    }

    void validate_scope(const proc::OperationRequest& request)
    {
        if (!request.scope.is_object())
            throw std::invalid_argument("Step 1 scope must be an object");
        const auto properties = scope_properties(request.operation);
        for (const auto& [key, value] : request.scope.items())
        {
            const auto definition = properties.find(key);
            if (definition == properties.end())
                throw std::invalid_argument("Unknown Step 1 scope field " + key);
            const auto& type = definition->at("type").get_ref<const std::string&>();
            const bool valid = (type == "integer" && value.is_number_integer() && value >= 0) ||
                (type == "boolean" && value.is_boolean()) || (type == "string" && value.is_string()) ||
                (type == "array" && value.is_array() && !value.empty() &&
                 std::ranges::all_of(
                     value,
                     [](const json& item)
                     {
                         return item.is_string();
                     }));
            const auto choices = definition->find("enum");
            if (!valid || (choices != definition->end() && std::ranges::find(*choices, value) == choices->end()))
                throw std::invalid_argument("Invalid Step 1 scope field " + key);
        }
        if ((request.operation == "step1.reference" || request.operation == "step1.export") && !request.scope.contains("mode"))
            throw std::invalid_argument("Step 1 operation requires a mode");
        if (request.scope.contains("begin") != request.scope.contains("end"))
            throw std::invalid_argument("A scan range needs both begin and end");
        if (request.operation == "step1.reference" && (request.scope.at("mode") == "load") != request.scope.contains("files"))
            throw std::invalid_argument("Reference files are required for reference load");
        if (request.operation == "step1.odometry" && request.scope.contains("destination") && !request.scope.value("save", true))
            throw std::invalid_argument("A native destination requires odometry save");
    }

    fs::path path_from_json(const json& value)
    {
        const auto& text = value.get_ref<const std::string&>();
        if (text.empty() || text.find('\0') != std::string::npos)
            throw std::invalid_argument("Invalid Step 1 path");
        return sessionPathFromText(text);
    }

    fs::path owned_path(const fs::path& root, const json& value)
    {
        const auto relative = path_from_json(value);
        if (relative.has_root_path() || std::ranges::find(relative, fs::path("..")) != relative.end())
            throw std::invalid_argument("Snapshot entry must reference an owned file or directory");
        const auto path = fs::canonical(root / relative);
        const auto local = path.lexically_relative(root);
        if (local.empty() || local == "." || *local.begin() == "..")
            throw std::invalid_argument("Snapshot entry leaves its directory");
        return path;
    }

    fs::path output_destination(const json& scope)
    {
        const auto value = scope.find("destination");
        if (value == scope.end())
            return {};
        const auto path = path_from_json(*value);
        if (!path.is_absolute() || path.filename().empty() || path.filename() == "." || path.filename() == "..")
            throw std::invalid_argument("Step 1 output destination must be an absolute path");
        const auto parent = fs::canonical(path.parent_path());
        for (auto ancestor = parent; ancestor != ancestor.root_path(); ancestor = ancestor.parent_path())
        {
            if (fs::exists(ancestor / "snapshot.json") || fs::exists(ancestor / "result.json"))
                throw std::invalid_argument("Output destination cannot overwrite snapshot data");
        }
        return parent / path.filename();
    }

    //! Owns only a private directory created for the current operation.
    class TemporaryDirectory
    {
    public:
        explicit TemporaryDirectory(fs::path path)
            : path_(std::move(path))
        {
            if (!fs::create_directory(path_))
                throw std::runtime_error("Step 1 temporary directory already exists");
        }

        ~TemporaryDirectory()
        {
            std::error_code ignored;
            if (!path_.empty())
                fs::remove_all(path_, ignored);
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
        void release() noexcept
        {
            path_.clear();
        }

    private:
        fs::path path_;
    };

    void copy_output_file(const fs::path& source, const fs::path& output, std::stop_token stop)
    {
        std::ifstream input(source, std::ios::binary);
        std::ofstream destination(output, std::ios::binary);
        destination.exceptions(std::ios::failbit | std::ios::badbit);
        std::vector<char> buffer(256 * 1024);
        while (input)
        {
            check_stop(stop);
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            destination.write(buffer.data(), input.gcount());
        }
        if (input.bad() || !input.eof())
            throw std::runtime_error("Cannot read the Step 1 output file: " + sessionPathText(source));
        destination.close();
    }

    void deliver(const ArtifactManifest& artifact, const fs::path& destination, bool native, std::stop_token stop)
    {
        if (destination.empty())
            return;
        const auto root = artifact.snapshot.manifest_path.parent_path();
        const auto type = fs::symlink_status(destination).type();
        if (type != fs::file_type::not_found && (native || type != fs::file_type::regular))
            throw std::invalid_argument("Step 1 output destination already exists");

        // Use a sibling on the destination's filesystem so readers only see the complete output.
        std::random_device random;
        const auto staging = destination.parent_path() / (".hdmapping-step1-" + std::to_string(random()) + "-" + std::to_string(random()));
        TemporaryDirectory stage(staging);
        fs::path leaf;
        for (const auto& file : artifact.files)
        {
            check_stop(stop);
            if (file == artifact.snapshot.manifest_path)
                continue;
            leaf = file.lexically_relative(root);
            fs::create_directories((staging / leaf).parent_path());
            copy_output_file(file, staging / leaf, stop);
            proc::SyncSnapshotPath(staging / leaf, false);
        }
        check_stop(stop);
        if (native)
        {
            std::vector<fs::path> directories{ staging };
            for (const auto& entry : fs::recursive_directory_iterator(staging))
                if (entry.is_directory())
                    directories.push_back(entry.path());
            for (auto it = directories.rbegin(); it != directories.rend(); ++it)
                proc::SyncSnapshotPath(*it, true);
        }
        check_stop(stop);
        proc::MovePublishedPath(native ? staging : staging / leaf, destination, !native);
        if (native)
            stage.release();
        proc::SyncSnapshotPath(destination.parent_path(), true);
    }

    //! Captures an operation's newly generated files in a private result snapshot.
    class OutputCapture final : public proc::CaptureSource
    {
    public:
        using Writer = std::function<bool(const fs::path&)>;

        OutputCapture(proc::TargetRef target, proc::SnapshotContents contents, Writer writer)
            : target_(std::move(target))
            , contents_(std::move(contents))
            , writer_(std::move(writer))
        {
        }

        proc::TargetRef Target() const override
        {
            return target_;
        }

        proc::CaptureResult Materialize(const fs::path& directory, proc::RunContext&) override
        {
            if (!writer_(directory))
                return failure("write_failed", "Could not write the Step 1 result");
            return contents_;
        }

    private:
        proc::TargetRef target_;
        proc::SnapshotContents contents_;
        Writer writer_;
    };

    proc::RunResult capture_artifact(proc::CaptureSource& source, const fs::path& path, const fs::path& leaf, proc::RunContext& context)
    {
        const auto target = source.Target();
        auto capture = proc::CaptureSnapshot(source, target.revision, path, context);
        if (const auto* error = std::get_if<OperationError>(&capture))
            return *error;
        auto ref = std::get<proc::SnapshotRef>(std::move(capture));
        std::vector<fs::path> files{ ref.manifest_path };
        if (!leaf.empty())
            files.push_back(ref.manifest_path.parent_path() / leaf);
        return ArtifactManifest{ sessionPathText(path.filename()), target, std::move(ref), std::move(files), {} };
    }

    proc::RunResult native_result(
        const proc::TargetRef& target,
        std::vector<WorkerData>& workers,
        LidarOdometryParams& params,
        const json& settings,
        const OdometryControl& control,
        proc::RunContext& context)
    {
        const auto stage = context.output_directory / ".hdmapping-result-stage-step1";
        OutputCapture source(
            target,
            { std::string(native_kind), "session.mjs", settings, {} },
            [&](const fs::path& directory)
            {
                const bool saved = save_result(workers, params, directory, 0.0, control);
                remove_cache(params); // update cleanup in the report before sealing its checksum
                return saved;
            });
        auto staged = proc::CaptureSnapshot(source, target.revision, stage, context);
        if (const auto* error = std::get_if<OperationError>(&staged))
            return *error;
        if (context.stop.stop_requested())
        {
            proc::DiscardResultStaging(stage, context.output_directory / "native");
            return failure("cancelled", "Step 1 operation cancelled");
        }
        return proc::PublishResult(stage, context.output_directory / "native");
    }

    std::vector<std::string> reference_files(const fs::path& root, const json& files, std::stop_token stop)
    {
        std::vector<fs::path> requested;
        requested.reserve(files.size());
        for (const auto& value : files)
            requested.push_back(path_from_json(value));
        const auto captured = ReferenceCapturePaths(requested);
        std::vector<std::string> result;
        result.reserve(captured.size());
        for (size_t i = 0; i < captured.size(); ++i)
        {
            check_stop(stop);
            const auto path = requested[i].is_absolute() ? root / captured[i] : owned_path(root, files.at(i));
            if (!fs::is_regular_file(path))
                throw std::invalid_argument("Reference file is missing from the captured snapshot");
            result.push_back(path.string());
        }
        return result;
    }

    void apply_reference(const fs::path& root, const json& scope, LidarOdometryParams& params, const OdometryControl& control)
    {
        const auto& mode = scope.at("mode");
        if (mode == "load")
        {
            if (!load_reference_point_clouds(reference_files(root, scope.at("files"), control.stop), params, control))
                throw std::invalid_argument("Reference files contain no usable points");
        }
        else
        {
            if (params.reference_points.empty())
                throw std::invalid_argument("Load reference points before alignment or filtering");
            if (mode == "filter")
            {
                control.report("reference", 0, 1);
                filter_reference_buckets(params, control.stop);
            }
            else
            {
                if (params.initial_points.empty() || params.buckets_indoor.empty())
                    throw std::invalid_argument("Alignment needs initial points and reference buckets");
                for (size_t i = 0; i < 30; ++i)
                {
                    check_stop(control.stop);
                    control.report("reference", i, 30);
                    align_to_reference(params.in_out_params_indoor, params.initial_points, params.m_g, params.buckets_indoor);
                }
            }
        }
    }

    proc::RunResult export_file(
        const proc::OperationRequest& request,
        const fs::path& destination,
        std::vector<WorkerData>& workers,
        const LidarOdometryParams& params,
        proc::RunContext& context)
    {
        const auto& mode = request.scope.at("mode").get_ref<const std::string&>();
        const bool range = request.scope.contains("begin");
        const bool selected = request.scope.value("selected", false);
        size_t first_scan = 0;
        if (range)
        {
            if (selected || request.scope.at("begin") >= request.scope.at("end") || request.scope.at("end") > workers.size())
                throw std::invalid_argument("Invalid Step 1 scan range");
            const auto begin = request.scope.at("begin").get<std::ptrdiff_t>();
            first_scan = static_cast<size_t>(begin);
            const auto end = request.scope.at("end").get<std::ptrdiff_t>();
            workers.erase(workers.begin() + end, workers.end());
            workers.erase(workers.begin(), workers.begin() + begin);
        }

        const fs::path leaf = mode == "trajectory" ? "trajectory.txt" : mode == "poses" ? "poses.mrp" : "point_cloud." + mode;
        OutputCapture source(
            request.target,
            { "step1_export", leaf, json::object(), {} },
            [&](const fs::path& directory)
            {
                const auto output = directory / leaf;
                check_stop(context.stop);
                if (context.progress)
                    context.progress({ "export", {}, {}, "Writing Step 1 export" });
                if (mode == "trajectory")
                    return save_trajectory_to_ascii(workers, output, context.stop);
                if (selected && std::ranges::none_of(workers, &WorkerData::show))
                    throw std::invalid_argument("Select at least one scan to export");
                if (mode == "poses")
                {
                    std::vector<Eigen::Affine3d> poses;
                    std::vector<std::string> names;
                    for (size_t i = 0; i < workers.size(); ++i)
                    {
                        if (selected && !workers[i].show)
                            continue;
                        poses.push_back(workers[i].intermediate_trajectory.front());
                        names.push_back("scan_" + std::to_string(first_scan + i) + ".laz");
                    }
                    return save_poses(output.string(), poses, names);
                }
                Session session;
                session.fill_session_from_worker_data(workers, selected, !selected, !selected, params.threshould_output_filter);
                check_stop(context.stop);
                return save_all_to_las(session, output.string(), !selected, true);
            });
        auto result = capture_artifact(source, context.output_directory / "export", leaf, context);
        if (const auto* artifact = std::get_if<ArtifactManifest>(&result))
            deliver(*artifact, destination, false, context.stop);
        return result;
    }
} // namespace

std::vector<proc::OperationDescriptor> DescribeStep1Operations()
{
    const auto settings = DescribeStep1Parameters();
    const json result{ { "type", "object" }, { "required", { "artifact_id", "target", "snapshot", "files", "dependencies" } } };
    std::vector<proc::OperationDescriptor> descriptors;
    descriptors.reserve(operations.size());
    for (const auto id : operations)
    {
        json scope{ { "type", "object" }, { "properties", scope_properties(id) }, { "additionalProperties", false } };
        if (id == "step1.reference" || id == "step1.export")
            scope["required"] = { "mode" };
        descriptors.push_back({ std::string(id), std::move(scope), settings, result, proc::OperationClass::Heavy });
    }
    return descriptors;
}

proc::RunResult ExecuteStep1Operation(const proc::OperationRequest& request, const proc::SnapshotRef& snapshot, proc::RunContext& context)
{
    try
    {
        if (std::ranges::find(operations, request.operation) == operations.end())
            return failure("unsupported_operation", "Unknown processing operation " + request.operation);
        if (request.target != snapshot.target)
            return failure("revision_conflict", "Step 1 request and snapshot revisions differ");
        check_stop(context.stop);
        validate_scope(request);
        const auto destination = output_destination(request.scope);
        const auto read = proc::ReadSnapshot(snapshot, context.stop);
        if (const auto* error = std::get_if<OperationError>(&read))
            return *error;
        const auto& contents = std::get<proc::SnapshotContents>(read);
        const auto snapshot_root = fs::canonical(snapshot.manifest_path).parent_path();
        const bool load = request.operation == "step1.load" || request.operation == "step1.prepare";
        const bool compute = request.operation == "step1.odometry";
        const bool consistency = request.operation == "step1.consistency";
        const bool reference = request.operation == "step1.reference";
        const bool single_file = request.operation == "step1.export" && request.scope.at("mode") != "complete";
        const bool save = request.operation == "step1.save" || (request.operation == "step1.export" && !single_file) ||
            (compute && request.scope.value("save", true));
        if (save && !destination.empty() && fs::symlink_status(destination).type() != fs::file_type::not_found)
            return failure("publication_conflict", "Step 1 native output destination already exists");
        if (contents.kind == native_kind && request.operation == "step1.export" && !single_file)
        {
            const auto generation = sessionPathText(snapshot_root.filename());
            if (!generation.starts_with("result-") || !request.parameters.empty() || request.scope.contains("begin") ||
                request.scope.contains("selected"))
                return failure("invalid_input", "Complete export requires an unmodified published native session");
            const auto previous = proc::ReadPublishedGeneration(snapshot_root.parent_path(), generation.substr(7));
            if (const auto* error = std::get_if<OperationError>(&previous))
                return *error;
            const auto& original = std::get<ArtifactManifest>(previous);
            if (original.snapshot.target != request.target)
                return failure("revision_conflict", "Published native session revision differs");
            auto exported = proc::ExportComplete(original, context.output_directory / "native", context.stop);
            if (const auto* artifact = std::get_if<ArtifactManifest>(&exported))
                deliver(*artifact, destination, true, context.stop);
            return exported;
        }

        const bool raw = contents.kind == recording_kind;
        if (const auto recording_source = request.scope.find("recording_directory"); recording_source != request.scope.end())
        {
            const auto selected = path_from_json(*recording_source);
            const auto captured = contents.parameters.find("recording_directory");
            if (!raw || !selected.is_absolute() || captured == contents.parameters.end() ||
                selected.lexically_normal() != path_from_json(*captured).lexically_normal())
                return failure("invalid_input", "The selected recording directory does not match the captured recording");
        }
        if ((raw && !load && !compute) || (!raw && (load || contents.kind != prepared_kind)))
            return failure("invalid_snapshot", "Unsupported snapshot kind for " + request.operation);
        if (!request.parameters.is_object())
            return failure("invalid_parameters", "Step 1 parameters must be an object");
        for (const auto* name :
             { "prepared", "processed", "selected_chunks", "range", "recording_directory", "initial_transform", "apply_to_gui" })
        {
            if (request.parameters.contains(name))
                return failure("invalid_parameters", std::string("Step 1 parameter is read-only: ") + name);
        }

        const OdometryControl control{ context.stop,
                                       [&](const char* stage, size_t current, size_t total)
                                       {
                                           if (context.progress)
                                               context.progress({ stage, current, total, {} });
                                       } };
        LidarOdometryParams params{};
        set_lidar_odometry_default_params(params);
        std::vector<WorkerData> workers;
        fs::path recording;
        auto settings = contents.parameters;
        bool processed = settings.value("processed", false);
        bool prepared = settings.value("prepared", contents.kind == prepared_kind);
        const auto workspace = fs::absolute(context.output_directory) / "step1-work";
        const TemporaryDirectory scratch(workspace);
        if (raw)
        {
            if (!fs::is_regular_file(contents.entry) || fs::file_size(contents.entry) > 64 * 1024)
                throw std::invalid_argument("Invalid recording snapshot entry");
            std::ifstream in(contents.entry, std::ios::binary);
            const auto source = json::parse(in);
            recording = owned_path(snapshot_root, source.at("directory"));
            if (!fs::is_directory(recording))
                throw std::invalid_argument("Expected a recording directory");
            if (const auto seed = source.find("seed"); seed != source.end())
            {
                if (auto error = RestoreOdometry({ owned_path(snapshot_root, *seed), snapshot.target }, workers, params, context))
                    return *error;
                workers.clear();
                params.initial_points.clear();
                params.consecutive_distance = 0;
                params.total_length_of_calculated_trajectory = 0;
            }
            if (const auto toml = source.find("toml"); toml != source.end())
            {
                std::string validation_error;
                if (!TomlIO{}.LoadParametersFromTomlFile(owned_path(snapshot_root, *toml).string(), params, &validation_error))
                    return failure("invalid_parameters", validation_error);
            }
        }
        else
        {
            if (auto error = RestoreOdometry(contents, workers, params, context))
                return *error;
        }

        if (auto error = ApplyStep1Parameters(settings, params))
            return *error;
        if (auto error = ApplyStep1Parameters(request.parameters, params))
            return *error;

        const auto original_name = settings.value("recording_directory", std::string{});
        const auto original = original_name.empty() ? recording : sessionPathFromText(original_name);
        params.run_report = std::make_shared<RunReport>(original, params.points_global_spill_directory, "step1");
        if (raw)
        {
            std::vector<std::string> files;
            for (const auto& file : fs::directory_iterator(recording))
            {
                if (file.is_regular_file())
                    files.push_back(file.path().string());
            }
            const bool debug = request.scope.value("debug", false);
            std::vector<std::vector<Point3Di>> points;
            Trajectory trajectory;
            Imu imu;
            const std::atomic<bool> pause{ false };
            control.report("loading", 0, 1);
            if (!load_data(files, params, points, imu, debug, control, workspace))
            {
                check_stop(context.stop);
                return failure("invalid_input", "Could not load the Mandeye recording");
            }
            control.report("loading", 1, 1);
            control.report("initialization", 0, 1);
            calculate_trajectory(trajectory, imu, params, debug, control);
            check_stop(context.stop);
            control.report("initialization", 1, 1);
            if (!compute_step_1(points, params, trajectory, workers, pause, control))
            {
                check_stop(context.stop);
                return failure("invalid_input", "The recording could not be prepared for odometry");
            }
            release_raw_clouds(points, params);
            prepared = true;
            processed = false;
            settings["range"] = { 0, workers.size() - 1 };
        }
        else
        {
            params.working_directory_cache = (workspace / "cache").string();
            params.working_directory_preview = (workspace / "preview").string();
            fs::create_directory(workspace / "cache");
        }

        if (!reference && (!prepared || workers.empty()))
            return failure("invalid_input", "Step 1 operation requires prepared scans");
        if (single_file && !destination.empty() && !original.empty() && fs::exists(destination))
        {
            const auto recording_root = fs::weakly_canonical(original);
            const auto relative = destination.lexically_relative(recording_root);
            if (!relative.empty() && *relative.begin() != "..")
                return failure("invalid_input", "Cannot replace a file in the source recording");
        }
        if (request.operation == "step1.export" && !single_file && (request.scope.contains("begin") || request.scope.contains("selected")))
            return failure("invalid_input", "Complete export needs all scans");
        if ((save || consistency) && !compute && !processed)
            return failure("invalid_input", "Compute odometry before saving or running consistency");

        if (compute)
        {
            if (processed)
                return failure("invalid_input", "Odometry is already computed");
            fs::create_directory(workspace / "preview");
            std::atomic<float> progress{ 0 };
            const std::atomic<bool> pause{ false };
            std::atomic<bool> parked{ false };
            double timestamp = 0;
            if (!compute_step_2(workers, params, timestamp, progress, pause, request.scope.value("debug", false), parked, control))
            {
                check_stop(context.stop);
                return OperationError{ "odometry_failed", "LiDAR odometry failed", { { "timestamp", timestamp } } };
            }
            processed = true;
        }
        else if (consistency)
            run_consistency(workers, params, control);
        else if (reference)
            apply_reference(snapshot_root, request.scope, params, control);

        check_stop(context.stop);
        if (single_file)
            return export_file(request, destination, workers, params, context);

        auto state = Step1Parameters(params);
        state["prepared"] = prepared;
        state["processed"] = processed;
        state["recording_directory"] = sessionPathText(original);
        if (settings.contains("range"))
            state["range"] = settings.at("range");

        const bool return_prepared = !save || (compute && (!destination.empty() || settings.value("apply_to_gui", false)));
        proc::RunResult prepared_result;
        if (return_prepared)
        {
            OdometryCapture capture(request.target, workers, params, state);
            prepared_result = capture_artifact(capture, context.output_directory / "prepared", "odometry.json", context);
            if (std::holds_alternative<OperationError>(prepared_result))
                return prepared_result;
        }
        if (!save)
        {
            remove_cache(params);
            return prepared_result;
        }

        auto result = native_result(request.target, workers, params, state, control, context);
        if (auto* artifact = std::get_if<ArtifactManifest>(&result))
        {
            deliver(*artifact, destination, true, context.stop);
            if (return_prepared)
            {
                auto& primary = std::get<ArtifactManifest>(prepared_result);
                primary.files.insert(primary.files.end(), artifact->files.begin(), artifact->files.end());
                return prepared_result;
            }
        }
        return result;
    } catch (const std::exception& error)
    {
        return failure(
            context.stop.stop_requested()                                                                              ? "cancelled"
                : (dynamic_cast<const std::invalid_argument*>(&error) || dynamic_cast<const json::exception*>(&error)) ? "invalid_input"
                                                                                                                       : "step1_failed",
            error.what());
    }
}
