#pragma once

#include <HDMapping/Processing/Operation.h>

#include <string_view>

class Session;

namespace HDMapping::Processing
{
    //! Describe the distinct CP, GCP, trajectory import, projection and rigid alignment contracts.
    std::vector<OperationDescriptor> DescribeStep2Georeference();
    bool IsStep2GeoreferenceOperation(std::string_view operation);
    nlohmann::json DescribeControlPoints(const Session& session);
    nlohmann::json DescribeGroundControlPoints(const Session& session);

    //! Apply a validated point edit on the owner thread or to a worker-owned snapshot.
    std::optional<OperationError> EditStep2Georeference(Session& session, const OperationRequest& request);

    //! Run a reference import, projection or alignment against worker-owned state.
    std::optional<OperationError> RunStep2Georeference(
        Session& session, const OperationRequest& request, nlohmann::json& tracks, RunContext& context);

    RunResult ExecuteStep2Georeference(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context);
} // namespace HDMapping::Processing
