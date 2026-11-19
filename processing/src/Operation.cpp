#include <HDMapping/Processing/Operation.h>

#include "step1_operations.h"

namespace HDMapping::Processing
{
    std::vector<OperationDescriptor> DescribeOperations()
    {
        return DescribeStep1Operations();
    }

    RunResult ExecuteOperation(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context)
    {
        return ExecuteStep1Operation(request, snapshot, context);
    }
} // namespace HDMapping::Processing
