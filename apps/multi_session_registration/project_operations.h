#pragma once

#include "project_io.h"

#include <HDMapping/Processing/SnapshotStore.h>

#include <map>

namespace HDMapping::Processing
{
    inline constexpr std::string_view native_project_kind = "native_project";

    using ProjectSessionSources = std::map<std::string, std::string>;

    //! Identify a child in the owning project revision.
    TargetRef ProjectChildTarget(const TargetRef& project, std::size_t index);

    //! Loaded Step 3 project and its dependent sessions.
    struct ProjectState
    {
        ProjectSettings settings;
        std::vector<Session> sessions;
        nlohmann::json parameters;
        ProjectSessionSources sources;
    };

    std::vector<OperationDescriptor> DescribeProjectOperations();
    bool IsProjectOperation(std::string_view operation);
    std::variant<SnapshotRef, OperationError> CaptureProjectOpen(const OperationRequest& request, RunContext& context);
    std::variant<SnapshotRef, OperationError> CaptureProjectAdd(
        const OperationRequest& request, const SnapshotRef& base, RunContext& context);
    std::variant<ProjectState, OperationError> LoadProjectSnapshot(const SnapshotRef& snapshot, std::stop_token stop = {});
    //! Apply one short project edit using the same index validation for GUI and snapshots.
    void EditProject(const OperationRequest& request, ProjectSettings& settings, std::vector<Session>& sessions);
    RunResult ExecuteProjectOperation(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context);

    //! Check the native MJP and all child MJS references against the publication inventory.
    void ValidateNativeProject(
        const SnapshotContents& contents, const ArtifactManifest& manifest, bool read_sessions, std::stop_token stop = {});
} // namespace HDMapping::Processing
