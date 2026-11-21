#pragma once

#include <HDMapping/Processing/Operation.h>

namespace HDMapping::Processing
{
    //! Apply explicit pose edits to a snapshot and publish a native session.
    RunResult ExecuteStep2Segments(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context);

    inline constexpr std::string_view register_scans_operation = "step2.register_scans";

    //! Describe the ordered scan selection and engine settings for registration.
    OperationDescriptor DescribeStep2Registration();

    //! Register scans from a captured GUI session or a published native session.
    RunResult ExecuteStep2Registration(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context);
} // namespace HDMapping::Processing
