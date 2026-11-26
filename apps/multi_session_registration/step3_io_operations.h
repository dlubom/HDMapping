#pragma once

#include <HDMapping/Processing/Operation.h>

#include <string_view>

class Session;
struct Edge;

namespace HDMapping::Processing
{
    struct ProjectState;
    inline constexpr std::string_view project_export_operation = "project.export";
    inline constexpr std::string_view project_listing_operation = "project.list_items";

    //! Describe complete project, pose, trajectory and point-cloud exports.
    OperationDescriptor DescribeProjectExport();

    //! Describe bounded session, scan and constraint listings.
    OperationDescriptor DescribeProjectListing();

    //! Return one bounded page at an already verified project revision.
    nlohmann::json ProjectItemsPage(const std::vector<Session>& sessions, const std::vector<Edge>& edges, const nlohmann::json& request);

    //! Publish an export from an isolated project snapshot.
    RunResult ExportProject(const OperationRequest& request, ProjectState& state, RunContext& context);
} // namespace HDMapping::Processing
