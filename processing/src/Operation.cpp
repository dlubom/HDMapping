#include <HDMapping/Processing/Operation.h>

#include "project_operations.h"
#include "step1_operations.h"
#include "step2_constraint_operations.h"
#include "step2_georeference_operations.h"
#include "step2_io_operations.h"
#include "step2_loop_candidate_operations.h"
#include "step2_operations.h"
#include "step2_segment_operations.h"

namespace HDMapping::Processing
{
    std::vector<OperationDescriptor> DescribeOperations()
    {
        auto operations = DescribeStep1Operations();
        operations.push_back(DescribeStep2Registration());
        auto constraints = DescribeStep2Constraints();
        operations.insert(operations.end(), std::make_move_iterator(constraints.begin()), std::make_move_iterator(constraints.end()));
        auto georeference = DescribeStep2Georeference();
        operations.insert(operations.end(), std::make_move_iterator(georeference.begin()), std::make_move_iterator(georeference.end()));
        operations.push_back(DescribeLoopCandidateDetection());
        auto segments = DescribeStep2Segments();
        operations.insert(operations.end(), std::make_move_iterator(segments.begin()), std::make_move_iterator(segments.end()));
        auto io = DescribeStep2IO();
        operations.insert(operations.end(), std::make_move_iterator(io.begin()), std::make_move_iterator(io.end()));
        auto projects = DescribeProjectOperations();
        operations.insert(operations.end(), std::make_move_iterator(projects.begin()), std::make_move_iterator(projects.end()));
        using nlohmann::json;
        const json text = { { "type", "string" } };
        const json target = { { "type", "object" },
                              { "required", { "state_id", "revision" } },
                              { "properties", { { "state_id", text }, { "revision", { { "type", "integer" }, { "minimum", 0 } } } } } };
        const json snapshot = { { "type", "object" },
                                { "required", { "manifest_path", "target" } },
                                { "properties", { { "manifest_path", text }, { "target", target } } } };
        const json paths = { { "type", "array" }, { "items", text } };
        const json artifact = {
            { "type", "object" },
            { "required", { "artifact_id", "target", "snapshot", "files", "dependencies" } },
            { "properties",
              { { "artifact_id", text }, { "target", target }, { "snapshot", snapshot }, { "files", paths }, { "dependencies", paths } } }
        };
        for (auto& operation : operations)
            if (!operation.result_schema.contains("properties"))
                operation.result_schema = artifact;
        return operations;
    }

    RunResult ExecuteOperation(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context)
    {
        if (request.operation == register_scans_operation)
            return ExecuteStep2Registration(request, snapshot, context);
        if (request.operation == detect_candidates_operation)
            return ExecuteLoopCandidateDetection(request, snapshot, context);
        if (IsStep2ConstraintOperation(request.operation))
            return ExecuteStep2Constraints(request, snapshot, context);
        if (IsStep2GeoreferenceOperation(request.operation))
            return ExecuteStep2Georeference(request, snapshot, context);
        if (IsStep2SegmentOperation(request.operation))
            return ExecuteStep2Segments(request, snapshot, context);
        if (IsStep2IOOperation(request.operation))
            return ExecuteStep2IO(request, snapshot, context);
        if (IsProjectOperation(request.operation))
            return ExecuteProjectOperation(request, snapshot, context);
        return ExecuteStep1Operation(request, snapshot, context);
    }
} // namespace HDMapping::Processing
