#include "step2_constraint_operations.h"

#include <Core/session.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace HDMapping::Processing
{
    using nlohmann::json;

    namespace
    {
        using Edge = PoseGraphLoopClosure::Edge;
        constexpr std::string_view add = "constraints.add";
        constexpr std::string_view update = "constraints.update";
        constexpr std::string_view remove = "constraints.remove";
        constexpr std::string_view replace = "constraints.replace";
        constexpr std::array components{ &TaitBryanPose::px, &TaitBryanPose::py, &TaitBryanPose::pz,
                                         &TaitBryanPose::om, &TaitBryanPose::fi, &TaitBryanPose::ka };
        constexpr std::array fixedFields{ &Edge::is_fixed_px, &Edge::is_fixed_py, &Edge::is_fixed_pz,
                                          &Edge::is_fixed_om, &Edge::is_fixed_fi, &Edge::is_fixed_ka };

        void checkKeys(const json& object, std::initializer_list<std::string_view> keys)
        {
            if (!object.is_object())
                throw std::invalid_argument("Expected an object");
            for (const auto& entry : object.items())
                if (std::ranges::find(keys, entry.key()) == keys.end())
                    throw std::invalid_argument("Unknown field: " + entry.key());
        }

        int index(const json& value, std::size_t size)
        {
            if (!value.is_number_integer() || value.get<std::uint64_t>() >= size ||
                value.get<std::uint64_t>() >= static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                throw std::invalid_argument("Index must be a zero-based integer in the target session");
            return value.get<int>();
        }

        std::vector<int> edgeIndices(const json& scope, std::size_t size, bool allowEmpty)
        {
            checkKeys(scope, { "edge_indices" });
            const auto& array = scope.at("edge_indices");
            if (!array.is_array() || (!allowEmpty && array.empty()) || array.size() > size)
                throw std::invalid_argument("Supply an explicit edge_indices array within the target graph");
            std::vector<int> result;
            std::vector<bool> seen(size);
            result.reserve(array.size());
            for (const auto& entry : array)
            {
                const int i = index(entry, size);
                if (seen[i])
                    throw std::invalid_argument("edge_indices must be distinct");
                seen[i] = true;
                result.push_back(i);
            }
            return result;
        }

        double finiteNumber(const json& value)
        {
            if (!value.is_number() || !std::isfinite(value.get<double>()))
                throw std::invalid_argument("Expected a finite number");
            return value.get<double>();
        }

        double weightFromSigma(double sigma)
        {
            const double weight = 1.0 / (sigma * sigma);
            if (sigma <= 0 || !std::isfinite(weight) || weight == 0)
                throw std::invalid_argument("Sigma must produce a positive finite information weight");
            return weight;
        }

        void requireSix(const json& array)
        {
            if (!array.is_array() || array.size() != components.size())
                throw std::invalid_argument("Expected six components in x, y, z, omega, phi, kappa order");
        }

        void checkEndpoints(const Edge& edge, std::size_t size)
        {
            if (edge.index_from < 0 || edge.index_to < 0 || edge.index_from == edge.index_to ||
                static_cast<std::size_t>(edge.index_from) >= size || static_cast<std::size_t>(edge.index_to) >= size)
                throw std::invalid_argument("The edge must join two distinct scans of the target session");
        }

        bool validPose(const Eigen::Affine3d& pose)
        {
            const double determinant = pose.linear().determinant();
            return pose.matrix().allFinite() && std::isfinite(determinant) && std::abs(determinant) > 1e-12;
        }

        Edge readEdge(const json& fields, const PointClouds& clouds, const Edge* prior = nullptr)
        {
            checkKeys(fields, { "index_from", "index_to", "relative_pose", "sigmas", "fixed" });
            if (fields.empty())
                throw std::invalid_argument("Specify at least one edge field");
            Edge edge = prior ? *prior : Edge{};
            const auto size = clouds.point_clouds.size();
            if (!prior || fields.contains("index_from"))
                edge.index_from = index(fields.at("index_from"), size);
            if (!prior || fields.contains("index_to"))
                edge.index_to = index(fields.at("index_to"), size);
            checkEndpoints(edge, size);
            if (!prior)
            {
                if (!validPose(clouds.point_clouds[edge.index_from].m_pose) || !validPose(clouds.point_clouds[edge.index_to].m_pose))
                    throw std::invalid_argument("Endpoint poses must be finite and invertible");
                edge = PoseGraphLoopClosure::make_edge(clouds, edge.index_from, edge.index_to);
            }
            if (const auto pose = fields.find("relative_pose"); pose != fields.end())
            {
                requireSix(*pose);
                for (std::size_t i = 0; i < components.size(); ++i)
                    edge.relative_pose_tb.*components[i] = finiteNumber(pose->at(i));
            }
            if (const auto sigmas = fields.find("sigmas"); sigmas != fields.end())
            {
                requireSix(*sigmas);
                for (std::size_t i = 0; i < components.size(); ++i)
                {
                    const auto& sigma = sigmas->at(i);
                    edge.relative_pose_tb_weights.*components[i] = sigma.is_null() ? 0 : weightFromSigma(finiteNumber(sigma));
                }
            }
            if (const auto fixed = fields.find("fixed"); fixed != fields.end())
            {
                requireSix(*fixed);
                for (std::size_t i = 0; i < components.size(); ++i)
                    edge.*fixedFields[i] = fixed->at(i).get<bool>();
            }
            return edge;
        }

        json objectSchema(json properties = json::object(), json required = json::array())
        {
            return { { "type", "object" },
                     { "properties", std::move(properties) },
                     { "required", std::move(required) },
                     { "additionalProperties", false } };
        }

        json sixValues(json items, std::string_view description)
        {
            return {
                { "type", "array" }, { "minItems", 6 }, { "maxItems", 6 }, { "items", std::move(items) }, { "description", description }
            };
        }

        void applyGraphSettings(ManualPoseGraphLoopClosure& graph, const json& settings)
        {
            checkKeys(settings, { "keep_initial_trajectory_curvature", "use_gnss_correspondences", "motion_sigmas" });
            const bool keep = settings.value("keep_initial_trajectory_curvature", graph.keep_initial_trajectory_curvature);
            const bool useGnss = settings.value("use_gnss_correspondences", graph.use_gnss_correspondences);
            if (const auto sigmas = settings.find("motion_sigmas"); sigmas != settings.end())
            {
                requireSix(*sigmas);
                const std::array fields{ &graph.motion_model_w_px_1_sigma_m,   &graph.motion_model_w_py_1_sigma_m,
                                         &graph.motion_model_w_pz_1_sigma_m,   &graph.motion_model_w_om_1_sigma_deg,
                                         &graph.motion_model_w_fi_1_sigma_deg, &graph.motion_model_w_ka_1_sigma_deg };
                std::array<double, 6> validated{};
                for (std::size_t i = 0; i < fields.size(); ++i)
                {
                    const auto sigma = finiteNumber(sigmas->at(i));
                    weightFromSigma(i < 3 ? sigma : sigma / 180.0 * M_PI);
                    validated[i] = sigma;
                }
                for (std::size_t i = 0; i < fields.size(); ++i)
                    *fields[i] = validated[i];
            }
            graph.keep_initial_trajectory_curvature = keep;
            graph.use_gnss_correspondences = useGnss;
        }

        // The existing graph consumes only the timestamp and ENU coordinates of stored GNSS measurements.
        GNSS graphGnss(const json& tracks)
        {
            GNSS gnss;
            if (const auto measurements = tracks.find("gnss_measurements"); measurements != tracks.end())
            {
                if (!measurements->is_array())
                    throw std::invalid_argument("GNSS measurements must be an array");
                for (const auto& measurement : *measurements)
                {
                    if (!measurement.is_object())
                        throw std::invalid_argument("GNSS measurement must be an object");
                    GNSS::GlobalPose pose{};
                    pose.timestamp = finiteNumber(measurement.value("timestamp", json(0.0)));
                    pose.enu_x = finiteNumber(measurement.value("enu_x", json(0.0)));
                    pose.enu_y = finiteNumber(measurement.value("enu_y", json(0.0)));
                    pose.enu_z = finiteNumber(measurement.value("enu_z", json(0.0)));
                    gnss.gnss_poses.push_back(pose);
                }
            }
            return gnss;
        }

        OperationError cancelled()
        {
            return { "cancelled", "Step 2 constraint job cancelled", {} };
        }
    } // namespace

    bool IsStep2ConstraintOperation(std::string_view operation)
    {
        return operation == list_constraints_operation || operation == add || operation == update || operation == remove ||
            operation == replace || operation == constraint_icp_operation || operation == optimize_graph_operation;
    }

    std::vector<OperationDescriptor> DescribeStep2Constraints()
    {
        const json idx{ { "type", "integer" }, { "minimum", 0 } };
        const json number{ { "type", "number" } };
        const json positive{ { "type", "number" }, { "exclusiveMinimum", 0 } };
        const json boolean{ { "type", "boolean" } };
        const auto edgeProperties =
            json{ { "index_from", idx },
                  { "index_to", idx },
                  { "relative_pose",
                    sixValues(number, "x, y, z in m and omega, phi, kappa in rad. Transform from index_to to index_from.") },
                  { "sigmas",
                    sixValues(
                        { { "anyOf", { positive, json{ { "type", "null" } } } } },
                        "Same component order and units as relative_pose. Default 0.001. Null is zero information weight.") },
                  { "fixed", sixValues(boolean, "Legacy edge metadata in the same order as relative_pose. Preserved when saving.") } };
        const auto newEdge = objectSchema(edgeProperties, { "index_from", "index_to" });
        const auto single = objectSchema({ { "edge_index", idx } }, { "edge_index" });
        const json edgeIndicesSchema{ { "type", "array" },
                                      { "uniqueItems", true },
                                      { "items", idx },
                                      { "description", "Explicit zero-based active edge indices in the requested opening and revision." } };
        const auto scope = objectSchema({ { "edge_indices", edgeIndicesSchema } }, { "edge_indices" });
        auto nonemptyScope = scope;
        nonemptyScope["properties"]["edge_indices"]["minItems"] = 1;
        const json result{ { "type", "object" } };
        const auto empty = objectSchema();
        return {
            { std::string(list_constraints_operation), empty, empty, result, OperationClass::Read },
            { std::string(add), empty, newEdge, result, OperationClass::Mutate },
            { std::string(update), single, objectSchema(edgeProperties), result, OperationClass::Mutate },
            { std::string(remove), single, empty, result, OperationClass::Mutate },
            { std::string(replace),
              nonemptyScope,
              objectSchema({ { "edges", { { "type", "array" }, { "items", newEdge } } } }, { "edges" }),
              result,
              OperationClass::Mutate },
            { std::string(constraint_icp_operation),
              nonemptyScope,
              objectSchema(
                  { { "radius_m", { { "type", "number" }, { "minimum", 0.01 } } },
                    { "iterations", { { "type", "integer" }, { "minimum", 1 } } },
                    { "extend_before", idx },
                    { "extend_after", idx },
                    { "coarse_to_fine", boolean } },
                  { "radius_m", "iterations" }),
              result,
              OperationClass::Heavy },
            { std::string(optimize_graph_operation),
              scope,
              objectSchema(
                  { { "keep_initial_trajectory_curvature", boolean },
                    { "use_gnss_correspondences", boolean },
                    { "motion_sigmas", sixValues(positive, "x, y, z in metres and omega, phi, kappa in degrees.") } }),
              result,
              OperationClass::Heavy },
        };
    }

    json DescribeActiveConstraints(const Session& session)
    {
        auto result = json::array();
        for (const auto& edge : session.pose_graph_loop_closure.edges)
        {
            auto pose = json::array();
            auto sigmas = json::array();
            auto fixed = json::array();
            for (std::size_t i = 0; i < components.size(); ++i)
            {
                pose.push_back(edge.relative_pose_tb.*components[i]);
                const double weight = edge.relative_pose_tb_weights.*components[i];
                sigmas.push_back(weight > 0 ? json(1.0 / std::sqrt(weight)) : json(nullptr));
                fixed.push_back(edge.*fixedFields[i]);
            }
            result.push_back(
                { { "edge_index", result.size() },
                  { "kind", "active" },
                  { "index_from", edge.index_from },
                  { "index_to", edge.index_to },
                  { "relative_pose", std::move(pose) },
                  { "sigmas", std::move(sigmas) },
                  { "fixed", std::move(fixed) } });
        }
        return result;
    }

    std::optional<OperationError> EditStep2Constraints(Session& session, const OperationRequest& request)
    {
        auto& graph = session.pose_graph_loop_closure;
        try
        {
            if (request.operation == list_constraints_operation)
            {
                checkKeys(request.scope, {});
                checkKeys(request.parameters, {});
                return {};
            }
            auto& edges = graph.edges;
            const auto& clouds = session.point_clouds_container;
            if (request.operation == add)
            {
                checkKeys(request.scope, {});
                edges.push_back(readEdge(request.parameters, clouds));
            }
            else if (request.operation == replace)
            {
                auto indices = edgeIndices(request.scope, edges.size(), false);
                checkKeys(request.parameters, { "edges" });
                const auto& replacements = request.parameters.at("edges");
                if (!replacements.is_array())
                    throw std::invalid_argument("Replacement edges must be an array");
                std::ranges::sort(indices);
                std::vector<Edge> staged;
                for (std::size_t i = 0; i < edges.size(); ++i)
                {
                    if (i == static_cast<std::size_t>(indices.front()))
                        for (const auto& fields : replacements)
                            staged.push_back(readEdge(fields, clouds));
                    if (!std::ranges::binary_search(indices, static_cast<int>(i)))
                        staged.push_back(edges[i]);
                }
                edges.swap(staged);
            }
            else if (request.operation == update || request.operation == remove)
            {
                checkKeys(request.scope, { "edge_index" });
                const auto i = index(request.scope.at("edge_index"), edges.size());
                if (request.operation == update)
                    edges[i] = readEdge(request.parameters, clouds, &edges[i]);
                else
                {
                    checkKeys(request.parameters, {});
                    edges.erase(edges.begin() + i);
                }
            }
            else
                return OperationError{ "unknown_operation", "Unknown short constraint operation", {} };
            graph.index_active_edge = std::clamp(graph.index_active_edge, 0, std::max(0, static_cast<int>(edges.size()) - 1));
            graph.gizmo = false;
            if (edges.empty())
                graph.manipulate_active_edge = false;
            graph.icp_all_edges_count = 0;
            graph.icp_all_edges_run.running = false;
            return {};
        } catch (const std::exception& error)
        {
            return OperationError{ "invalid_constraint", error.what(), {} };
        }
    }

    json DescribeManualGraphSettings(const ManualPoseGraphLoopClosure& graph)
    {
        return { { "keep_initial_trajectory_curvature", graph.keep_initial_trajectory_curvature },
                 { "use_gnss_correspondences", graph.use_gnss_correspondences },
                 { "motion_sigmas",
                   { graph.motion_model_w_px_1_sigma_m,
                     graph.motion_model_w_py_1_sigma_m,
                     graph.motion_model_w_pz_1_sigma_m,
                     graph.motion_model_w_om_1_sigma_deg,
                     graph.motion_model_w_fi_1_sigma_deg,
                     graph.motion_model_w_ka_1_sigma_deg } } };
    }

    std::optional<OperationError> UpdateManualGraphSettings(ManualPoseGraphLoopClosure& graph, const json& settings)
    {
        try
        {
            applyGraphSettings(graph, settings);
            return {};
        } catch (const std::exception& error)
        {
            return OperationError{ "invalid_constraint", error.what(), {} };
        }
    }

    std::optional<OperationError> RunStep2Graph(
        Session& session, const OperationRequest& request, json& parameters, const json& tracks, RunContext& context)
    {
        auto& clouds = session.point_clouds_container;
        auto& graph = session.pose_graph_loop_closure;
        const bool optimize = request.operation == optimize_graph_operation;
        try
        {
            const auto count = clouds.point_clouds.size();
            const auto indices = edgeIndices(request.scope, graph.edges.size(), optimize);
            for (const auto i : indices)
                checkEndpoints(graph.edges[i], count);
            if (count < 2 ||
                std::ranges::any_of(
                    clouds.point_clouds,
                    [optimize](const auto& scan)
                    {
                        return !validPose(scan.m_pose) || (optimize && !validPose(scan.m_initial_pose));
                    }))
                throw std::invalid_argument("Graph jobs require at least two scans with valid poses");

            json result{ { "operation", request.operation }, { "edge_indices", indices } };
            if (optimize)
            {
                ManualPoseGraphLoopClosure selected;
                if (parameters.contains("manual_graph"))
                    applyGraphSettings(selected, parameters.at("manual_graph"));
                applyGraphSettings(selected, request.parameters);
                for (const auto i : indices)
                    selected.edges.push_back(graph.edges[i]);

                for (const auto& cp : session.control_points.cps)
                    index(cp.index_to_pose, count);
                for (const auto& gcp : session.ground_control_points.gpcs)
                {
                    const auto scan = index(gcp.index_to_node_inner, count);
                    index(gcp.index_to_node_outer, clouds.point_clouds[scan].local_trajectory.size());
                }

                auto gnss = graphGnss(tracks);
                const bool solved = selected.compute_pose_graph_slam(
                    clouds,
                    gnss,
                    session.ground_control_points,
                    session.control_points,
                    [&](int iteration, int total)
                    {
                        if (context.progress)
                            context.progress(
                                { "optimize_graph", static_cast<std::uint64_t>(iteration), static_cast<std::uint64_t>(total), {} });
                        return !context.stop.stop_requested();
                    });
                if (context.stop.stop_requested())
                    return cancelled();
                if (!solved ||
                    std::ranges::any_of(
                        clouds.point_clouds,
                        [](const auto& scan)
                        {
                            return !validPose(scan.m_pose);
                        }))
                    return OperationError{ "optimization_failed", "The pose graph solver did not produce valid poses", {} };
                parameters["manual_graph"] = DescribeManualGraphSettings(selected);
            }
            else
            {
                const auto& settings = request.parameters;
                checkKeys(settings, { "radius_m", "iterations", "extend_before", "extend_after", "coarse_to_fine" });
                const auto radius = finiteNumber(settings.at("radius_m"));
                if (radius < 0.01 || radius > std::numeric_limits<float>::max())
                    throw std::invalid_argument("ICP radius must be at least 0.01 m and representable as a float");
                const int iterations = index(settings.at("iterations"), std::numeric_limits<int>::max());
                if (iterations == 0)
                    throw std::invalid_argument("ICP requires at least one iteration");
                const int before = index(settings.value("extend_before", json(0)), count);
                const int after = index(settings.value("extend_after", json(0)), count);
                const bool coarse = settings.value("coarse_to_fine", false);
                const auto radii = coarse ? PoseGraphLoopClosure::icp_search_radii(static_cast<float>(radius))
                                          : std::vector<float>{ static_cast<float>(radius) };
                std::vector<int> failed;
                std::uint64_t current = 0;
                const auto total = indices.size() * radii.size();
                for (const auto i : indices)
                {
                    for (std::size_t pass = 0; pass < radii.size(); ++pass)
                    {
                        const auto step = radii[pass];
                        if (context.progress)
                            context.progress(
                                { "constraint_icp",
                                  current,
                                  total,
                                  "Edge " + std::to_string(i) + ", radius " + std::to_string(step) + " m" });
                        if (context.stop.stop_requested())
                            return cancelled();
                        if (!graph.run_icp(clouds, i, step, iterations, before, after))
                        {
                            failed.push_back(i);
                            current += radii.size() - pass;
                            break;
                        }
                        ++current;
                    }
                }
                if (!coarse && failed.size() == indices.size())
                    return OperationError{ "icp_failed", "No selected edge could be refined", {} };
                result["failed_edge_indices"] = std::move(failed);
            }
            if (context.stop.stop_requested())
                return cancelled();
            parameters["constraints_result"] = std::move(result);
            return {};
        } catch (const std::exception& error)
        {
            return OperationError{ "invalid_constraint", error.what(), {} };
        }
    }
} // namespace HDMapping::Processing
