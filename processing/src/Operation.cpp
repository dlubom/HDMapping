#include <HDMapping/Processing/Operation.h>

namespace HDMapping::Processing
{
    std::vector<OperationDescriptor> DescribeOperations()
    {
        return {};
    }

    RunResult ExecuteOperation(const OperationRequest& request, const SnapshotRef&, RunContext&)
    {
        return OperationError{ "unsupported_operation", "Unknown processing operation", { { "operation", request.operation } } };
    }
} // namespace HDMapping::Processing
