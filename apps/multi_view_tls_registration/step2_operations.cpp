#include "step2_operations.h"
#include "step2_constraint_operations.h"
#include "step2_georeference_operations.h"

#include "step2_result.h"
#include "step2_solver_selection.h"
#include "step2_state_parameters.h"

#include <HDMapping/Processing/SessionSnapshot.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace HDMapping::Processing
{
    using nlohmann::json;

    namespace
    {
        const std::vector<std::string_view> ndtVariants{ "ndt", "ndt_lie_left", "ndt_lie_right", "ndt_mahalanobis" };
        const std::vector<std::string_view> icpVariants{ "icp", "icp_lie_left", "icp_lie_right", "icp_rms" };
        const std::map<std::string_view, bool (RegistrationPlaneFeature::*)(PointClouds&)> planeVariants{
            { "plane_projection", &RegistrationPlaneFeature::optimize_point_to_projection_onto_plane_source_to_target },
            { "plane_projection_lie_left",
              &RegistrationPlaneFeature::optimize_point_to_projection_onto_plane_source_to_target_lie_algebra_left_jacobian },
            { "plane_projection_lie_right",
              &RegistrationPlaneFeature::optimize_point_to_projection_onto_plane_source_to_target_lie_algebra_right_jacobian },
            { "plane_point", &RegistrationPlaneFeature::optimize_point_to_plane_source_to_target },
            { "plane_distance", &RegistrationPlaneFeature::optimize_distance_point_to_plane_source_to_target },
            { "plane_to_plane", &RegistrationPlaneFeature::optimize_plane_to_plane_source_to_target }
        };

        OperationError failure(std::string code, std::string message)
        {
            return { std::move(code), std::move(message), json::object() };
        }

        TLSRegistration registrationDefaults()
        {
            TLSRegistration defaults{};
            // These radio selections are the initial values in the raylib Step 2 GUI.
            const Step2SolverSelection selection{ 0, 0, 0 };
            selection.write(defaults.ndt);
            selection.write(defaults.icp);
            selection.write(defaults.registration_plane_feature);
            return defaults;
        }

        json settingsSchema(const json& defaults)
        {
            const json count{
                { "type", "integer" }, { "minimum", 1 }, { "maximum", std::numeric_limits<int>::max() }, { "unit", "count" }
            };
            json properties{ { "number_of_threads", count },
                             { "number_of_iterations", count },
                             { "is_fix_first_node", { { "type", "boolean" } } },
                             { "optimizer", { { "enum", { "gauss_newton", "levenberg_marquardt" } } } },
                             { "pose_convention",
                               { { "enum", { "cw", "wc" } }, { "description", "cw: camera from world. wc: world from camera." } } },
                             { "rotation", { { "enum", { "tait_bryan", "quaternion", "rodrigues" } } } } };
            if (defaults.contains("bucket_size"))
            {
                properties["bucket_size"] = {
                    { "type", "array" },
                    { "minItems", 3 },
                    { "maxItems", 3 },
                    { "unit", "m" },
                    { "items", { { "type", "number" }, { "minimum", 0.01 }, { "maximum", std::numeric_limits<float>::max() } } }
                };
                properties["is_generalized"] = { { "type", "boolean" } };
                properties["compute_mean_and_cov_for_bucket"] = { { "type", "boolean" } };
                properties["num_extended_points"] = count;
                properties["sigma_r"] = { { "type", "number" }, { "minimum", 0 }, { "unit", "m" } };
                properties["sigma_polar_angle"] = { { "type", "number" }, { "minimum", 0 }, { "unit", "rad" } };
                properties["sigma_azimuthal_angle"] = properties.at("sigma_polar_angle");
            }
            else
            {
                properties["search_radius"] = { { "type", "number" }, { "minimum", 0.01 }, { "maximum", 2 }, { "unit", "m" } };
                properties["is_adaptive_robust_kernel"] = { { "type", "boolean" } };
            }
            for (const auto& field : defaults.items())
                properties.at(field.key())["default"] = field.value();
            return { { "type", "object" }, { "properties", std::move(properties) }, { "additionalProperties", false } };
        }

        json algorithmSchema(const std::vector<std::string_view>& variants, const json& defaults)
        {
            return { { "type", "object" },
                     { "properties", { { "algorithm", { { "enum", variants } } }, { "settings", settingsSchema(defaults) } } },
                     { "required", { "algorithm" } },
                     { "additionalProperties", false } };
        }

        std::string_view solverFamily(std::string_view variant)
        {
            if (std::ranges::find(ndtVariants, variant) != ndtVariants.end())
                return "ndt";
            if (std::ranges::find(icpVariants, variant) != icpVariants.end())
                return "icp";
            if (planeVariants.contains(variant))
                return "plane_features";
            return {};
        }

        std::vector<std::size_t> scanIndices(const json& indices, std::size_t scanCount)
        {
            std::vector<std::size_t> ordered;
            std::set<std::size_t> seen;
            for (const auto& index : indices)
            {
                if (!index.is_number_integer() || index < 0 || index >= scanCount || !seen.insert(index.get<std::size_t>()).second)
                    return {};
                ordered.push_back(index.get<std::size_t>());
            }
            return ordered;
        }

        bool runSolver(
            std::string_view variant, std::string_view solver, TLSRegistration& registration, PointClouds& selected, double& metric)
        {
            if (solver == "ndt")
            {
                auto& ndt = registration.ndt;
                const bool computeCovariance = registration.compute_mean_and_cov_for_bucket;
                if (variant == "ndt_lie_left")
                    return ndt.optimize_lie_algebra_left_jacobian(selected.point_clouds, computeCovariance);
                if (variant == "ndt_lie_right")
                    return ndt.optimize_lie_algebra_right_jacobian(selected.point_clouds, computeCovariance);
                return ndt.optimize(selected.point_clouds, variant == "ndt_mahalanobis", computeCovariance, &metric);
            }
            if (solver == "icp")
            {
                auto& icp = registration.icp;
                if (variant == "icp_lie_left")
                    return icp.optimize_source_to_target_lie_algebra_left_jacobian(selected);
                if (variant == "icp_lie_right")
                    return icp.optimize_source_to_target_lie_algebra_right_jacobian(selected);
                if (variant == "icp_rms")
                    return icp.optimization_point_to_point_source_to_target_compute_rms(selected, metric);
                return icp.optimization_point_to_point_source_to_target(selected);
            }
            return (registration.registration_plane_feature.*planeVariants.at(variant))(selected);
        }

        void report(RunContext& context, const std::string& stage, std::string_view message = {})
        {
            if (context.progress)
                context.progress({ stage, {}, {}, std::string(message) });
        }
    } // namespace

    OperationDescriptor DescribeStep2Registration()
    {
        const auto defaults = describeStep2Parameters(registrationDefaults());
        std::vector<std::string_view> planeNames;
        for (const auto& entry : planeVariants)
            planeNames.push_back(entry.first);
        const json indices{ { "type", "array" },
                            { "minItems", 2 },
                            { "uniqueItems", true },
                            { "items", { { "type", "integer" }, { "minimum", 0 } } },
                            { "description",
                              "Ordered zero-based indices in the target session. The first index is the anchor when the engine's fix-first "
                              "setting is enabled." } };
        return { std::string(register_scans_operation),
                 { { "type", "object" },
                   { "properties", { { "scan_indices", indices } } },
                   { "required", { "scan_indices" } },
                   { "additionalProperties", false } },
                 { { "type", "object" },
                   { "oneOf",
                     { algorithmSchema(ndtVariants, defaults.at("ndt")),
                       algorithmSchema(icpVariants, defaults.at("icp")),
                       algorithmSchema(planeNames, defaults.at("plane_features")) } } },
                 { { "type", "object" }, { "required", { "artifact_id", "target", "snapshot", "files", "dependencies" } } },
                 OperationClass::Heavy };
    }

    RunResult ExecuteStep2Registration(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context)
    {
        if (request.target != snapshot.target)
            return failure("revision_conflict", "Step 2 request and snapshot revisions differ");
        if (!request.scope.is_object() || request.scope.size() != 1 || !request.scope.contains("scan_indices") ||
            !request.scope.at("scan_indices").is_array() || request.scope.at("scan_indices").size() < 2)
            return failure("invalid_scope", "Select at least two distinct scan indices explicitly");
        const auto& parameters = request.parameters;
        if (!parameters.is_object() || !parameters.contains("algorithm") || !parameters.at("algorithm").is_string() ||
            parameters.size() != 1 + static_cast<std::size_t>(parameters.contains("settings")) ||
            (parameters.contains("settings") && !parameters.at("settings").is_object()))
            return failure("invalid_parameters", "Supply an algorithm and optional settings object");

        try
        {
            const auto& variant = parameters.at("algorithm").get_ref<const std::string&>();
            const auto family = solverFamily(variant);
            if (family.empty())
                return failure("invalid_parameters", "Unknown Step 2 registration algorithm");
            report(context, "read_session");
            auto loaded = LoadSessionSnapshot(snapshot, context.stop);
            if (const auto* error = std::get_if<OperationError>(&loaded))
                return *error;
            auto& state = std::get<SessionSnapshot>(loaded);
            auto& scans = state.session->point_clouds_container.point_clouds;
            const auto ordered = scanIndices(request.scope.at("scan_indices"), scans.size());
            if (ordered.empty())
                return failure("invalid_scope", "Scan indices must be distinct integers in the target session");

            auto registration = registrationDefaults();
            auto saved = describeStep2Parameters(registration);
            for (const auto& setting : saved.items())
                if (state.parameters.contains(setting.key()))
                    setting.value() = state.parameters.at(setting.key());
            if (auto error = updateStep2Parameters(registration, saved))
                return *error;
            if (parameters.contains("settings") && !parameters.at("settings").empty())
            {
                if (auto error = updateStep2Parameters(registration, { { family, parameters.at("settings") } }))
                    return *error;
            }

            // Move only selected clouds into the legacy engine and restore their original slots after solving.
            // The engine sees the requested order, including hidden scans, without copying any point buffers.
            PointClouds selected;
            selected.point_clouds.reserve(ordered.size());
            for (const auto index : ordered)
            {
                const auto& scan = scans[index];
                const double determinant = scan.m_pose.linear().determinant();
                if (scan.points_local.size() < 3 || !scan.m_pose.matrix().allFinite() || !std::isfinite(determinant) ||
                    std::abs(determinant) < std::numeric_limits<double>::epsilon() ||
                    !std::ranges::all_of(
                        scan.points_local,
                        [](const auto& point)
                        {
                            return point.allFinite();
                        }))
                    return failure("invalid_input", "Selected scans require a finite, invertible pose and at least three finite points");
                selected.point_clouds.push_back(std::move(scans[index]));
            }
            if (context.stop.stop_requested())
                return failure("cancelled", "Step 2 registration cancelled");
            auto output = describeStep2Parameters(registration);
            report(context, "register_scans", variant);
            double metric = std::numeric_limits<double>::quiet_NaN();
            std::vector<Eigen::Affine3d> before;
            const bool diagnostic = variant == "icp_rms" || variant == "ndt_mahalanobis";
            if (diagnostic)
                for (const auto& scan : selected.point_clouds)
                    before.push_back(scan.m_pose);
            if (!runSolver(variant, family, registration, selected, metric) || (diagnostic && !std::isfinite(metric)))
                return failure("registration_failed", "The registration engine could not compute a result");
            for (std::size_t i = 0; i < ordered.size(); ++i)
            {
                auto& scan = selected.point_clouds[i];
                if (diagnostic)
                    scan.m_pose = before[i];
                if (!scan.m_pose.matrix().allFinite())
                    return failure("registration_failed", "The registration engine produced a non-finite pose");
                scans[ordered[i]] = std::move(scan);
            }

            if (context.stop.stop_requested())
                return failure("cancelled", "Step 2 registration cancelled");
            output["registration"] = { { "algorithm", variant }, { "scan_indices", ordered } };
            if (diagnostic)
                output["registration"]["metric"] = metric;
            state.parameters.erase("constraints_result");
            state.parameters.erase("georeference_result");
            state.parameters.update(output);
            auto result = PublishStep2Result(*state.session, request.target, std::move(state.parameters), state.tracks, context);
            if (diagnostic && std::holds_alternative<ArtifactManifest>(result))
            {
                const std::string label = variant == "icp_rms" ? "ICP RMS [m]: " : "Mean Mahalanobis distance: ";
                report(context, "registration_metric", label + std::to_string(metric));
            }
            return result;
        } catch (const std::exception& error)
        {
            return failure("registration_failed", error.what());
        }
    }

    RunResult ExecuteStep2Constraints(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context)
    {
        if (request.target != snapshot.target)
            return failure("revision_conflict", "Constraint request and snapshot must name the same opening and revision");
        if (context.stop.stop_requested())
            return failure("cancelled", "Step 2 constraint job cancelled");
        try
        {
            report(context, "read_session");
            auto loaded = LoadSessionSnapshot(snapshot, context.stop);
            if (const auto* error = std::get_if<OperationError>(&loaded))
                return *error;
            auto& state = std::get<SessionSnapshot>(loaded);
            state.parameters.erase("registration");
            state.parameters.erase("georeference_result");
            if (request.operation == constraint_icp_operation || request.operation == optimize_graph_operation)
            {
                if (auto error = RunStep2Graph(*state.session, request, state.parameters, state.tracks, context))
                    return *error;
            }
            else
            {
                if (auto error = EditStep2Constraints(*state.session, request))
                    return *error;
                state.parameters["constraints_result"] = { { "operation", request.operation } };
            }
            state.parameters["constraints_result"]["active_edges"] = DescribeActiveConstraints(*state.session);
            return PublishStep2Result(*state.session, request.target, std::move(state.parameters), state.tracks, context);
        } catch (const std::exception& error)
        {
            return failure("constraint_operation_failed", error.what());
        }
    }
    RunResult ExecuteStep2Georeference(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context)
    {
        if (request.target != snapshot.target)
            return failure("revision_conflict", "Reference request and snapshot must name the same opening and revision");
        if (context.stop.stop_requested())
            return failure("cancelled", "Step 2 reference operation cancelled");
        try
        {
            report(context, "read_session");
            auto loaded = LoadSessionSnapshot(snapshot, context.stop);
            if (const auto* error = std::get_if<OperationError>(&loaded))
                return *error;
            auto& state = std::get<SessionSnapshot>(loaded);
            if (auto error = RunStep2Georeference(*state.session, request, state.tracks, context))
                return *error;
            state.parameters.erase("registration");
            state.parameters.erase("constraints_result");
            state.parameters["georeference_result"] = { { "operation", request.operation } };
            if (request.operation == "gnss.project")
                state.parameters["georeference_result"]["projection"] = request.parameters;
            return PublishStep2Result(*state.session, request.target, std::move(state.parameters), state.tracks, context);
        } catch (const std::exception& error)
        {
            return failure("georeference_failed", error.what());
        }
    }
} // namespace HDMapping::Processing
