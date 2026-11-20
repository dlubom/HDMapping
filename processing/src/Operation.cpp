#include <HDMapping/Processing/Operation.h>

#include "step1_operations.h"
#include "step2_operations.h"

namespace HDMapping::Processing
{
    std::vector<OperationDescriptor> DescribeOperations()
    {
        auto operations = DescribeStep1Operations();
        operations.push_back(DescribeStep2Registration());
        return operations;
    }

    RunResult ExecuteOperation(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context)
    {
        if (request.operation == register_scans_operation)
            return ExecuteStep2Registration(request, snapshot, context);
        return ExecuteStep1Operation(request, snapshot, context);
    }
} // namespace HDMapping::Processing
