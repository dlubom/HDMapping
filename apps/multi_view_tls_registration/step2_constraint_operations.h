#pragma once

#include <HDMapping/Processing/Operation.h>

class Session;
class ManualPoseGraphLoopClosure;

namespace HDMapping::Processing
{
    inline constexpr std::string_view list_constraints_operation = "constraints.list";
    inline constexpr std::string_view constraint_icp_operation = "constraints.run_icp";
    inline constexpr std::string_view optimize_graph_operation = "step2.optimize_graph";

    //! Scope indices identify active edges in the exact requested opening and revision.
    std::vector<OperationDescriptor> DescribeStep2Constraints();
    bool IsStep2ConstraintOperation(std::string_view operation);

    //! Read only active edges. A proposed relation becomes active through an explicit add or replace.
    nlohmann::json DescribeActiveConstraints(const Session& session);

    //! Apply a short edit atomically to an already validated live opening or worker-owned session.
    std::optional<OperationError> EditStep2Constraints(Session& session, const OperationRequest& request);

    //! Preserve the existing motion model settings when pinning a session for the worker.
    nlohmann::json DescribeManualGraphSettings(const ManualPoseGraphLoopClosure& graph);

    //! Validate and apply the effective settings from a completed optimization.
    std::optional<OperationError> UpdateManualGraphSettings(ManualPoseGraphLoopClosure& graph, const nlohmann::json& settings);

    //! Refine precisely the requested active edges or optimize the worker-owned graph.
    std::optional<OperationError> RunStep2Graph(
        Session& session, const OperationRequest& request, nlohmann::json& parameters, const nlohmann::json& tracks, RunContext& context);

    //! Run a graph job against a snapshot, publishing a native session only on success.
    RunResult ExecuteStep2Constraints(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context);
} // namespace HDMapping::Processing
