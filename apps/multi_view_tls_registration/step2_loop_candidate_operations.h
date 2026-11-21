#pragma once

#include <HDMapping/Processing/Operation.h>

namespace HDMapping::Processing
{
    inline constexpr std::string_view detect_candidates_operation = "constraints.detect_candidates";
    inline constexpr std::string_view loop_candidates_kind = "step2_loop_candidates";

    //! Describe the input scope, camera folders and published candidate report.
    OperationDescriptor DescribeLoopCandidateDetection();

    //! Publish scored candidate edges and review images from an unchanged session snapshot.
    RunResult ExecuteLoopCandidateDetection(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context);

    //! Load the shipped vocabulary and confirm that it accepts ORB descriptors.
    void CheckLoopCandidateVocabulary();
} // namespace HDMapping::Processing
