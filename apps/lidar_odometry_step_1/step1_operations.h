#pragma once

#include <HDMapping/Processing/Operation.h>

//! Describe the Step 1 commands supported by the managed worker.
std::vector<HDMapping::Processing::OperationDescriptor> DescribeStep1Operations();

//! Execute a Step 1 command against an immutable recording or odometry snapshot.
HDMapping::Processing::RunResult ExecuteStep1Operation(
    const HDMapping::Processing::OperationRequest& request,
    const HDMapping::Processing::SnapshotRef& snapshot,
    HDMapping::Processing::RunContext& context);
