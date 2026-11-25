#include "project_operations.h"

#include "project_capture.h"
#include "step3_operations.h"
#include <HDMapping/Processing/ResultPublisher.h>
#include <HDMapping/Processing/SessionSnapshot.h>
#include <HDMapping/SessionPaths.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace HDMapping::Processing
{
    namespace fs = std::filesystem;
    using nlohmann::json;

    namespace
    {
        constexpr std::string_view open = "project.open";
        constexpr std::string_view save = "project.save";
        constexpr std::string_view add = "project.add_session";
        constexpr std::string_view remove = "project.remove_session";
        constexpr std::string_view ground_truth = "project.set_ground_truth";

        OperationError failure(const RunContext& context, const std::exception& error)
        {
            return { context.stop.stop_requested() ? "cancelled" : "project_io_failed", error.what(), json::object() };
        }

        json object_schema(json properties, json required)
        {
            return { { "type", "object" },
                     { "properties", std::move(properties) },
                     { "required", std::move(required) },
                     { "additionalProperties", false } };
        }

        fs::path requested_path(const OperationRequest& request)
        {
            const auto name = request.parameters.at("path").get<std::string>();
            const auto path = sessionPathFromText(name);
            if (!request.scope.empty() || request.parameters.size() != 1 || !path.is_absolute() || name.find('\0') != std::string::npos)
                throw std::invalid_argument("Project input requires an absolute path and an empty scope");
            return path;
        }

        int requested_index(const OperationRequest& request)
        {
            const auto& index = request.scope.at("session_index");
            if (!request.parameters.empty() || request.scope.size() != 1 || !index.is_number_integer() ||
                index > std::numeric_limits<int>::max() || (!index.is_number_unsigned() && index.get<std::int64_t>() < -1))
                throw std::invalid_argument("Invalid project session index");
            return index.get<int>();
        }

        std::variant<SnapshotRef, OperationError> capture(const ProjectState& state, const TargetRef& target, RunContext& context)
        {
            ProjectCapture source(target, state.sessions, ProjectSettingsJson(state.settings), state.parameters, false, state.sources);
            return CaptureSnapshot(source, target.revision, context.output_directory, context);
        }

        //! Persist a project graph read result without publishing mutated project state.
        class ConstraintReport final : public CaptureSource
        {
        public:
            ConstraintReport(TargetRef target, json report)
                : target_(std::move(target))
                , report_(std::move(report))
            {
            }

            TargetRef Target() const override
            {
                return target_;
            }

            CaptureResult Materialize(const fs::path& directory, RunContext& context) override
            {
                if (context.stop.stop_requested())
                    return OperationError{ "cancelled", "Project constraint report cancelled", json::object() };
                std::ofstream file(directory / "constraints.json", std::ios::binary);
                file.exceptions(std::ios::failbit | std::ios::badbit);
                file << report_.dump(2);
                file.close();
                return SnapshotContents{ "project_constraints", "constraints.json", json::object(), {} };
            }

        private:
            TargetRef target_;
            json report_;
        };

        RunResult publish_constraints(const TargetRef& target, json report, RunContext& context)
        {
            ConstraintReport source(target, std::move(report));
            const auto captured = CaptureSnapshot(source, target.revision, context.output_directory / "project-constraints", context);
            if (const auto* error = std::get_if<OperationError>(&captured))
                return *error;
            const auto& snapshot = std::get<SnapshotRef>(captured);
            return ArtifactManifest{ "project-constraints",
                                     target,
                                     snapshot,
                                     { snapshot.manifest_path, snapshot.manifest_path.parent_path() / "constraints.json" },
                                     {} };
        }

        RunResult publish(const ProjectState& state, const TargetRef& target, RunContext& context)
        {
            const auto stage = context.output_directory / ".hdmapping-result-stage-project";
            const auto destination = context.output_directory / "native";
            ProjectCapture source(target, state.sessions, ProjectSettingsJson(state.settings), state.parameters, true, state.sources);
            const auto captured = CaptureSnapshot(source, target.revision, stage, context);
            if (const auto* failed = std::get_if<OperationError>(&captured))
                return *failed;
            if (context.stop.stop_requested())
            {
                DiscardResultStaging(stage, destination);
                return OperationError{ "cancelled", "Project publication cancelled", json::object() };
            }
            return PublishResult(stage, destination, {}, context.stop);
        }
    } // namespace

    TargetRef ProjectChildTarget(const TargetRef& project, std::size_t index)
    {
        return { project.state_id + "/session/" + std::to_string(index), project.revision };
    }

    std::vector<OperationDescriptor> DescribeProjectOperations()
    {
        const auto empty = object_schema(json::object(), json::array());
        const auto path = object_schema({ { "path", { { "type", "string" } } } }, { "path" });
        const auto index = [](int minimum)
        {
            return object_schema({ { "session_index", { { "type", "integer" }, { "minimum", minimum } } } }, { "session_index" });
        };
        const json artifact = { { "type", "object" } };
        std::vector<OperationDescriptor> operations{
            { std::string(open), empty, path, artifact, OperationClass::Heavy },
            { std::string(save), empty, empty, artifact, OperationClass::Heavy },
            { std::string(add), empty, path, artifact, OperationClass::Heavy },
            { std::string(remove), index(0), empty, artifact, OperationClass::Mutate },
            { std::string(ground_truth), index(-1), empty, artifact, OperationClass::Mutate },
        };
        auto graph = DescribeStep3Operations();
        operations.insert(operations.end(), std::make_move_iterator(graph.begin()), std::make_move_iterator(graph.end()));
        return operations;
    }

    bool IsProjectOperation(std::string_view operation)
    {
        return operation == open || operation == save || operation == add || operation == remove || operation == ground_truth ||
            IsStep3Operation(operation);
    }

    std::variant<SnapshotRef, OperationError> CaptureProjectOpen(const OperationRequest& request, RunContext& context)
    {
        try
        {
            const auto path = requested_path(request);
            ProjectState state{ ReadProjectSettings(path), {}, json::object(), {} };
            for (const auto& name : state.settings.session_file_names)
            {
                if (context.stop.stop_requested())
                    return OperationError{ "cancelled", "Project opening cancelled", json::object() };
                state.sessions.push_back(ReadProjectSession(name));
            }
            ValidateProjectScans(state.settings, state.sessions);
            OrderProjectGroundTruthFirst(state.settings, state.sessions);
            state.parameters["project_import"] = { { "operation", open }, { "path", sessionPathText(path) } };
            return capture(state, request.target, context);
        } catch (const std::exception& error)
        {
            return failure(context, error);
        }
    }

    std::variant<SnapshotRef, OperationError> CaptureProjectAdd(
        const OperationRequest& request, const SnapshotRef& base, RunContext& context)
    {
        try
        {
            if (request.target != base.target)
                throw std::invalid_argument("Project snapshot does not match the requested revision");
            const auto path = requested_path(request);
            auto loaded = LoadProjectSnapshot(base, context.stop);
            if (const auto* failed = std::get_if<OperationError>(&loaded))
                return *failed;
            auto& state = std::get<ProjectState>(loaded);
            if (std::ranges::any_of(
                    state.sources,
                    [&path](const auto& source)
                    {
                        return sessionPathFromText(source.second) == path.lexically_normal();
                    }) ||
                !AddProjectSessionPath(state.settings, sessionPathText(path)))
                throw std::invalid_argument("The project already contains the session");
            auto child = ReadProjectSession(state.settings.session_file_names.back());
            if (child.is_ground_truth)
                SetProjectGroundTruth(state.sessions, -1);
            state.sessions.push_back(std::move(child));
            OrderProjectGroundTruthFirst(state.settings, state.sessions);
            state.parameters["project_import"] = { { "operation", add }, { "path", sessionPathText(path) } };
            return capture(state, request.target, context);
        } catch (const std::exception& error)
        {
            return failure(context, error);
        }
    }

    std::variant<ProjectState, OperationError> LoadProjectSnapshot(const SnapshotRef& snapshot, std::stop_token stop)
    {
        try
        {
            ArtifactManifest inventory;
            auto result = ReadSnapshot(snapshot, stop, &inventory.files);
            if (const auto* failed = std::get_if<OperationError>(&result))
                return *failed;
            auto& contents = std::get<SnapshotContents>(result);
            const bool native = contents.kind == native_project_kind;
            if (native)
                ValidateNativeProject(contents, inventory, false);
            else if (contents.kind != "project" || !contents.dependencies.empty())
                throw std::invalid_argument("Expected a project snapshot");
            auto settings = ReadProjectSettings(contents.entry, true);
            const auto& children = contents.parameters.at("project_sessions");
            if (!children.is_array() || children.size() != settings.session_file_names.size())
                throw std::invalid_argument("Project and child revision inventory disagree");
            ProjectState state{ std::move(settings), {}, contents.parameters, {} };
            for (std::size_t i = 0; i < children.size(); ++i)
            {
                const auto& child = children.at(i);
                const auto target = ProjectChildTarget(snapshot.target, i);
                if (child.at("state_id") != target.state_id || child.at("revision") != target.revision)
                    throw std::invalid_argument("Project child revision mismatch");
                if (stop.stop_requested())
                    return OperationError{ "cancelled", "Project loading cancelled", json::object() };
                const auto& name = state.settings.session_file_names.at(i);
                if (native)
                    state.sessions.push_back(ReadProjectSession(name, { .strict = true }));
                else
                {
                    const auto entry = sessionPathFromText(name);
                    const auto child_snapshot = entry.parent_path() / "snapshot.json";
                    if (entry.parent_path() != contents.entry.parent_path() / std::to_string(i) ||
                        std::ranges::find(inventory.files, entry) == inventory.files.end() ||
                        std::ranges::find(inventory.files, child_snapshot) == inventory.files.end())
                        throw std::invalid_argument("Invalid project snapshot child path");
                    auto loaded = LoadSessionSnapshot({ child_snapshot, target }, stop);
                    if (const auto* failed = std::get_if<OperationError>(&loaded))
                        return *failed;
                    state.sessions.push_back(std::move(*std::get<SessionSnapshot>(loaded).session));
                    state.sessions.back().session_file_name = name;
                }
                auto& session = state.sessions.back();
                auto& scans = session.point_clouds_container.point_clouds;
                if (child.contains("previous_poses"))
                {
                    const auto& previous = child.at("previous_poses");
                    if (!previous.is_array() || previous.size() != scans.size())
                        throw std::invalid_argument("Project previous pose inventory disagrees with the session");
                    for (std::size_t scan = 0; scan < scans.size(); ++scan)
                    {
                        const auto matrix = previous.at(scan).get<std::array<double, 16>>();
                        scans[scan].m_pose_temp.matrix() = Eigen::Map<const Eigen::Matrix4d>(matrix.data());
                        if (!scans[scan].m_pose_temp.matrix().allFinite())
                            throw std::invalid_argument("Project previous pose is not finite");
                    }
                }
                else
                    for (auto& scan : scans)
                        scan.m_pose_temp = scan.m_pose;
                session.visible = child.at("visible").get<bool>();
                state.sources.emplace(state.sessions.back().session_file_name, child.at("source").get<std::string>());
            }
            ValidateProjectScans(state.settings, state.sessions);
            return state;
        } catch (const std::exception& error)
        {
            return { failure({ {}, stop, {} }, error) };
        }
    }

    void EditProject(const OperationRequest& request, ProjectSettings& settings, std::vector<Session>& sessions)
    {
        const auto index = requested_index(request);
        if (request.operation == ground_truth)
            SetProjectGroundTruth(sessions, index);
        else if (request.operation == remove && index >= 0)
            RemoveProjectSession(settings, sessions, static_cast<std::size_t>(index));
        else
            throw std::invalid_argument("Invalid project editing request");
    }

    RunResult ExecuteProjectOperation(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context)
    {
        try
        {
            if (request.target != snapshot.target)
                throw std::invalid_argument("Project snapshot does not match the requested revision");
            auto result = LoadProjectSnapshot(snapshot, context.stop);
            if (const auto* failed = std::get_if<OperationError>(&result))
                return *failed;
            auto& state = std::get<ProjectState>(result);
            if (request.operation == open || request.operation == add)
            {
                const auto path = requested_path(request);
                if (state.parameters.at("project_import") != json{ { "operation", request.operation }, { "path", sessionPathText(path) } })
                    throw std::invalid_argument("Project input was not captured for this operation");
            }
            else if (request.operation == remove || request.operation == ground_truth)
                EditProject(request, state.settings, state.sessions);
            else if (IsStep3Operation(request.operation))
            {
                auto report = RunStep3(request, state.settings, state.sessions, context);
                if (!report.is_null())
                    return publish_constraints(request.target, std::move(report), context);
            }
            else if (request.operation != save || !request.parameters.empty() || !request.scope.empty())
                throw std::invalid_argument("Invalid project save request");
            state.parameters.erase("project_import");
            return publish(state, request.target, context);
        } catch (const std::exception& error)
        {
            return failure(context, error);
        }
    }
} // namespace HDMapping::Processing
