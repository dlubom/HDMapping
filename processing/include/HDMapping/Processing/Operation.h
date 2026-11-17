#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace HDMapping::Processing
{
    struct TargetRef
    {
        std::string state_id;
        std::uint64_t revision{};

        bool operator==(const TargetRef&) const = default;
    };

    struct OperationRequest
    {
        std::string operation;
        TargetRef target;
        nlohmann::json scope;
        nlohmann::json parameters;
    };

    struct SnapshotRef
    {
        std::filesystem::path manifest_path;
        TargetRef target;
    };

    struct ProgressEvent
    {
        std::string stage;
        std::optional<std::uint64_t> current;
        std::optional<std::uint64_t> total;
        std::string message;
    };

    using ProgressSink = std::function<void(const ProgressEvent&)>;

    struct RunContext
    {
        std::filesystem::path output_directory;
        std::stop_token stop;
        ProgressSink progress;
    };

    struct OperationError
    {
        std::string code;
        std::string message;
        nlohmann::json details;
    };

    struct ArtifactManifest
    {
        std::string artifact_id;
        TargetRef target;
        SnapshotRef snapshot;
        std::vector<std::filesystem::path> files;
        std::vector<std::filesystem::path> dependencies;
    };

    enum class OperationClass
    {
        Read,
        Mutate,
        Heavy,
    };

    struct OperationDescriptor
    {
        std::string id;
        nlohmann::json scope_schema;
        nlohmann::json parameters_schema;
        nlohmann::json result_schema;
        OperationClass operation_class;
    };

    struct StateSummary
    {
        TargetRef target;
        nlohmann::json parameters;
        nlohmann::json metadata;
    };

    using MutationResult = std::variant<StateSummary, OperationError>;
    using RunResult = std::variant<ArtifactManifest, OperationError>;
    using OperationExecutor = std::function<RunResult(const OperationRequest&, const SnapshotRef&, RunContext&)>;

    //! List the operations executable by this build and their schemas.
    std::vector<OperationDescriptor> DescribeOperations();

    //! Execute against a materialized snapshot, with progress and cooperative cancellation.
    RunResult ExecuteOperation(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context);
} // namespace HDMapping::Processing
