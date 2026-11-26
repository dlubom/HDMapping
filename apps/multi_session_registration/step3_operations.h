#pragma once

#include <span>

#include "project_io.h"

#include <HDMapping/Processing/Operation.h>

namespace HDMapping::Processing
{
    //! Describe explicit Step 3 registration, graph and pose operations.
    std::vector<OperationDescriptor> DescribeStep3Operations();
    bool IsStep3Operation(std::string_view operation);
    bool IsStep3Edit(std::string_view operation);

    //! Serialize active edges in index order for the editor and MCP.
    nlohmann::json ProjectEdgesJson(std::span<const Edge> edges);

    //! Validate and apply a short edit to a loaded project.
    void EditStep3(const OperationRequest& request, ProjectSettings& settings, std::vector<Session>& sessions);

    //! Execute against a loaded project. Read operations return a JSON report.
    nlohmann::json RunStep3(
        const OperationRequest& request, ProjectSettings& settings, std::vector<Session>& sessions, const RunContext& context);
} // namespace HDMapping::Processing
