#include "step2_segment_operations.h"

#include <Core/point_clouds.h>
#include <Core/structures.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace HDMapping::Processing
{
    using nlohmann::json;

    namespace
    {
        std::vector<std::size_t> readScope(const json& scope, std::size_t scan_count)
        {
            if (!scope.is_object() || scope.size() != 1 || !scope.contains("scan_indices"))
                return {};
            const auto& values = scope.at("scan_indices");
            if (!values.is_array() || values.size() > scan_count)
                return {};
            std::vector<std::size_t> indices;
            indices.reserve(values.size());
            std::set<std::size_t> seen;
            for (const auto& value : values)
            {
                if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
                    return {};
                const auto index = value.get<std::uint64_t>();
                if (index >= scan_count || !seen.insert(static_cast<std::size_t>(index)).second)
                    return {};
                indices.push_back(static_cast<std::size_t>(index));
            }
            return indices;
        }

        bool validPose(const Eigen::Affine3d& pose)
        {
            const double determinant = pose.linear().determinant();
            return pose.matrix().allFinite() && pose.translation().cwiseAbs().maxCoeff() <= std::numeric_limits<float>::max() &&
                std::abs(determinant) > 1e-12 && std::isfinite(determinant);
        }

        bool rigid(const Eigen::Matrix4d& matrix)
        {
            const auto rotation = matrix.topLeftCorner<3, 3>();
            return matrix.allFinite() && matrix.row(3) == Eigen::RowVector4d(0, 0, 0, 1) && rotation.isUnitary(1e-5) &&
                std::abs(rotation.determinant() - 1.0) <= 1e-5;
        }

        std::optional<Eigen::Affine3d> readMatrix(const json& parameters, const char* key)
        {
            if (!parameters.is_object() || parameters.size() != 1 || !parameters.contains(key))
                return {};
            const auto& values = parameters.at(key);
            if (!values.is_array() || values.size() != 4)
                return {};
            Eigen::Matrix4d matrix;
            for (int row = 0; row < 4; ++row)
            {
                const auto& line = values.at(row);
                if (!line.is_array() || line.size() != 4)
                    return {};
                for (int col = 0; col < 4; ++col)
                {
                    if (!line.at(col).is_number())
                        return {};
                    matrix(row, col) = line.at(col).get<double>();
                }
            }
            if (!rigid(matrix))
                return {};
            return Eigen::Affine3d(matrix);
        }

        SegmentEditResult commit(
            PointClouds& clouds, std::vector<std::size_t> indices, const std::vector<Eigen::Affine3d>& poses, bool initial = false)
        {
            if (poses.size() != indices.size() || !std::ranges::all_of(poses, validPose))
                return OperationError{ "invalid_pose", "The pose edit requires finite, invertible poses", {} };
            for (std::size_t i = 0; i < indices.size(); ++i)
            {
                auto& scan = clouds.point_clouds[indices[i]];
                if (initial)
                {
                    scan.m_initial_pose = poses[i];
                    continue;
                }
                scan.m_pose = poses[i];
                scan.pose = pose_tait_bryan_from_affine_matrix(scan.m_pose);
                scan.gui_translation[0] = static_cast<float>(scan.pose.px);
                scan.gui_translation[1] = static_cast<float>(scan.pose.py);
                scan.gui_translation[2] = static_cast<float>(scan.pose.pz);
                scan.gui_rotation[0] = static_cast<float>(rad2deg(scan.pose.om));
                scan.gui_rotation[1] = static_cast<float>(rad2deg(scan.pose.fi));
                scan.gui_rotation[2] = static_cast<float>(rad2deg(scan.pose.ka));
            }
            return indices;
        }

        json matrixSchema(const char* key, const char* description)
        {
            const json row = { { "type", "array" }, { "minItems", 4 }, { "maxItems", 4 }, { "items", { { "type", "number" } } } };
            const json matrix = {
                { "type", "array" }, { "minItems", 4 }, { "maxItems", 4 }, { "items", row }, { "description", description }
            };
            return {
                { "type", "object" }, { "properties", { { key, matrix } } }, { "required", { key } }, { "additionalProperties", false }
            };
        }
    } // namespace

    bool IsStep2SegmentOperation(std::string_view operation)
    {
        return operation == edit_segment_operation || operation == transform_scope_operation || operation == restore_poses_operation;
    }

    std::vector<OperationDescriptor> DescribeStep2Segments()
    {
        const json scope = { { "type", "object" },
                             { "properties",
                               { { "scan_indices",
                                   { { "type", "array" },
                                     { "minItems", 1 },
                                     { "uniqueItems", true },
                                     { "items", { { "type", "integer" }, { "minimum", 0 } } },
                                     { "description", "Explicit zero-based scan indices, including hidden scans when named." } } } } },
                             { "required", { "scan_indices" } },
                             { "additionalProperties", false } };
        auto segment = scope;
        segment["properties"]["scan_indices"]["description"] =
            "Contiguous ascending zero-based scan indices. The first index receives the supplied anchor pose.";
        const json result = { { "type", "object" } };
        return { { std::string(edit_segment_operation),
                   std::move(segment),
                   matrixSchema("anchor_pose", "Row-major rigid 4x4 world-from-scan pose for the first scan. Translation in metres."),
                   result,
                   OperationClass::Mutate },
                 { std::string(transform_scope_operation),
                   scope,
                   matrixSchema(
                       "transform", "Row-major rigid 4x4 transform left-multiplied with each selected pose. Translation in metres."),
                   result,
                   OperationClass::Mutate },
                 { std::string(restore_poses_operation),
                   scope,
                   { { "type", "object" }, { "additionalProperties", false } },
                   result,
                   OperationClass::Mutate } };
    }

    std::vector<Eigen::Affine3d> PreviewStep2Segment(
        const PointClouds& clouds, std::size_t first, std::size_t count, const Eigen::Affine3d& anchor)
    {
        const auto& scans = clouds.point_clouds;
        if (count == 0 || first >= scans.size() || count > scans.size() - first || !rigid(anchor.matrix()) || !validPose(anchor))
            return {};
        std::vector<Eigen::Affine3d> poses{ anchor };
        poses.reserve(count);
        for (std::size_t i = first + 1; i < first + count; ++i)
        {
            const auto& before = scans[i - 1].m_pose;
            const auto& next = scans[i].m_pose;
            if (!validPose(before) || !validPose(next))
                return {};
            poses.push_back(poses.back() * (before.inverse() * next));
            if (!validPose(poses.back()))
                return {};
        }
        return poses;
    }

    SegmentEditResult EditStep2Poses(PointClouds& clouds, const OperationRequest& request)
    {
        if (!IsStep2SegmentOperation(request.operation))
            return OperationError{ "unknown_operation", "Unknown Step 2 pose edit", {} };
        auto indices = readScope(request.scope, clouds.point_clouds.size());
        if (indices.empty())
            return OperationError{ "invalid_scope", "Specify distinct scan indices from the current session", {} };
        std::vector<Eigen::Affine3d> poses;
        if (request.operation == edit_segment_operation)
        {
            const auto first = indices.front();
            for (std::size_t i = 0; i < indices.size(); ++i)
                if (indices[i] != first + i)
                    return OperationError{ "invalid_scope", "A segment requires contiguous ascending scan indices", {} };
            const auto anchor = readMatrix(request.parameters, "anchor_pose");
            if (!anchor)
                return OperationError{ "invalid_parameters", "Specify a rigid 4x4 anchor_pose matrix in row order", {} };
            poses = PreviewStep2Segment(clouds, first, indices.size(), *anchor);
        }
        else if (request.operation == transform_scope_operation)
        {
            const auto transform = readMatrix(request.parameters, "transform");
            if (!transform)
                return OperationError{ "invalid_parameters", "Specify a rigid 4x4 transform matrix in row order", {} };
            poses.reserve(indices.size());
            for (const auto i : indices)
                poses.push_back(*transform * clouds.point_clouds[i].m_pose);
        }
        else
        {
            if (!request.parameters.is_object() || !request.parameters.empty())
                return OperationError{ "invalid_parameters", "Restoring initial poses takes no parameters", {} };
            poses.reserve(indices.size());
            for (const auto i : indices)
                poses.push_back(clouds.point_clouds[i].m_initial_pose);
        }
        return commit(clouds, std::move(indices), poses);
    }

    SegmentEditResult ApplyStep2PoseResult(PointClouds& live, const PointClouds& result, const json& scope, bool initial)
    {
        auto indices = readScope(scope, live.point_clouds.size());
        if (indices.empty() || live.point_clouds.size() != result.point_clouds.size())
            return OperationError{ "invalid_result", "The published pose edit does not match the live scan scope", {} };
        std::vector<Eigen::Affine3d> poses;
        poses.reserve(indices.size());
        for (const auto i : indices)
            poses.push_back(initial ? result.point_clouds[i].m_initial_pose : result.point_clouds[i].m_pose);
        return commit(live, std::move(indices), poses, initial);
    }
} // namespace HDMapping::Processing
