#pragma once

#include <HDMapping/Processing/Operation.h>

#include <Eigen/Geometry>
#include <string_view>

class PointClouds;

namespace HDMapping::Processing
{
    inline constexpr std::string_view edit_segment_operation = "step2.edit_segment";
    inline constexpr std::string_view transform_scope_operation = "step2.transform_scope";
    inline constexpr std::string_view restore_poses_operation = "step2.restore_poses";

    using SegmentEditResult = std::variant<std::vector<std::size_t>, OperationError>;

    std::vector<OperationDescriptor> DescribeStep2Segments();
    bool IsStep2SegmentOperation(std::string_view operation);

    //! Calculate the gizmo's contiguous pose chain without modifying the session.
    //! An empty result indicates an invalid range or pose.
    std::vector<Eigen::Affine3d> PreviewStep2Segment(
        const PointClouds& clouds, std::size_t first, std::size_t count, const Eigen::Affine3d& anchor);

    //! Validate the entire pose edit before writing any scan, and return the edited indices.
    SegmentEditResult EditStep2Poses(PointClouds& clouds, const OperationRequest& request);

    //! Transfer only explicitly scoped poses from a published result into a live session.
    SegmentEditResult ApplyStep2PoseResult(PointClouds& live, const PointClouds& result, const nlohmann::json& scope);
} // namespace HDMapping::Processing
