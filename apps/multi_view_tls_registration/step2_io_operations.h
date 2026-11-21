#pragma once

#include <HDMapping/Processing/Operation.h>

#include <string_view>

class Session;

namespace HDMapping::Processing
{
    inline constexpr std::string_view session_open_operation = "session.open";
    inline constexpr std::string_view session_save_operation = "session.save";
    inline constexpr std::string_view session_subsession_operation = "session.subsession";
    inline constexpr std::string_view import_session_poses_operation = "session.import_poses";
    inline constexpr std::string_view import_session_observations_operation = "session.import_observations";
    inline constexpr std::string_view export_session_operation = "export_result";

    std::vector<OperationDescriptor> DescribeStep2IO();
    bool IsStep2IOOperation(std::string_view operation);

    //! Open and materialize external session data before the worker starts the durable job.
    std::variant<SnapshotRef, OperationError> CaptureStep2Open(const OperationRequest& request, RunContext& context);

    //! Save or export from a verified snapshot in the existing native formats.
    RunResult ExecuteStep2IO(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context);
} // namespace HDMapping::Processing
