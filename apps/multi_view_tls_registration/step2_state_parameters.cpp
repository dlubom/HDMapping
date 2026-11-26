#include "step2_state_parameters.h"
#include "step2_solver_selection.h"

#include <cmath>
#include <limits>

using nlohmann::json;

namespace
{
    template<typename Solver>
    json solverSettings(const Solver& solver)
    {
        return { { "number_of_threads", solver.number_of_threads },
                 { "number_of_iterations", solver.number_of_iterations },
                 { "is_fix_first_node", solver.is_fix_first_node },
                 { "optimizer", solver.is_gauss_newton ? "gauss_newton" : "levenberg_marquardt" },
                 { "pose_convention", solver.is_cw ? "cw" : "wc" },
                 { "rotation", solver.is_tait_bryan_angles ? "tait_bryan" : (solver.is_quaternion ? "quaternion" : "rodrigues") } };
    }

    bool positiveInteger(const json& value)
    {
        return value.is_number_integer() && value >= 1 && value <= std::numeric_limits<int>::max();
    }

    bool finiteNumber(const json& value)
    {
        return value.is_number() && std::isfinite(value.get<double>());
    }

    bool validSolver(const json& s)
    {
        return positiveInteger(s.at("number_of_threads")) && positiveInteger(s.at("number_of_iterations")) &&
            s.at("is_fix_first_node").is_boolean() && (s.at("optimizer") == "gauss_newton" || s.at("optimizer") == "levenberg_marquardt") &&
            (s.at("pose_convention") == "cw" || s.at("pose_convention") == "wc") &&
            (s.at("rotation") == "tait_bryan" || s.at("rotation") == "quaternion" || s.at("rotation") == "rodrigues");
    }

    template<typename Solver>
    void applySolver(Solver& solver, const json& s)
    {
        solver.number_of_threads = s.at("number_of_threads");
        solver.number_of_iterations = s.at("number_of_iterations");
        solver.is_fix_first_node = s.at("is_fix_first_node");
        const Step2SolverSelection selection{ s.at("optimizer") == "gauss_newton" ? 0 : 1,
                                              s.at("pose_convention") == "cw" ? 0 : 1,
                                              s.at("rotation") == "tait_bryan" ? 0 : (s.at("rotation") == "quaternion" ? 1 : 2) };
        selection.write(solver);
    }
} // namespace

nlohmann::json describeStep2Parameters(const TLSRegistration& registration)
{
    const auto& ndt = registration.ndt;
    auto n = solverSettings(ndt);
    n.update(
        json{ { "bucket_size", ndt.bucket_size },
              { "is_generalized", ndt.is_generalized },
              { "sigma_r", ndt.sigma_r },
              { "sigma_polar_angle", ndt.sigma_polar_angle },
              { "sigma_azimuthal_angle", ndt.sigma_azimuthal_angle },
              { "num_extended_points", ndt.num_extended_points },
              { "compute_mean_and_cov_for_bucket", registration.compute_mean_and_cov_for_bucket } });
    auto i = solverSettings(registration.icp);
    i["search_radius"] = registration.icp.search_radius;
    i["is_adaptive_robust_kernel"] = registration.icp.is_adaptive_robust_kernel;
    auto plane = solverSettings(registration.registration_plane_feature);
    plane["search_radius"] = registration.registration_plane_feature.search_radius;
    plane["is_adaptive_robust_kernel"] = registration.registration_plane_feature.is_adaptive_robust_kernel;
    return { { "ndt", std::move(n) }, { "icp", std::move(i) }, { "plane_features", std::move(plane) } };
}

std::optional<HDMapping::Processing::OperationError> updateStep2Parameters(TLSRegistration& registration, const nlohmann::json& patch)
{
    const HDMapping::Processing::OperationError invalid{ "invalid_parameters", "Invalid Step 2 registration settings", {} };
    auto next = describeStep2Parameters(registration);
    if (!patch.is_object() || patch.empty())
        return invalid;
    for (const auto& [solver, settings] : patch.items())
    {
        if (!next.contains(solver) || !settings.is_object() || settings.empty())
            return invalid;
        for (const auto& [name, value] : settings.items())
        {
            if (!next.at(solver).contains(name))
                return invalid;
            next.at(solver).at(name) = value;
        }
    }
    const auto& n = next.at("ndt");
    const auto& i = next.at("icp");
    const auto& plane = next.at("plane_features");
    const auto validRadius = [](const json& solver)
    {
        const auto& radius = solver.at("search_radius");
        return finiteNumber(radius) && radius >= 0.01f && radius <= 2 && solver.at("is_adaptive_robust_kernel").is_boolean();
    };
    const auto& bucket = n.at("bucket_size");
    if (!validSolver(n) || !validSolver(i) || !validSolver(plane) || !bucket.is_array() || bucket.size() != 3 ||
        !n.at("is_generalized").is_boolean() || !n.at("compute_mean_and_cov_for_bucket").is_boolean() ||
        !positiveInteger(n.at("num_extended_points")) || (!finiteNumber(n.at("sigma_r")) || n.at("sigma_r") < 0) ||
        (!finiteNumber(n.at("sigma_polar_angle")) || n.at("sigma_polar_angle") < 0) ||
        (!finiteNumber(n.at("sigma_azimuthal_angle")) || n.at("sigma_azimuthal_angle") < 0) || !validRadius(i) || !validRadius(plane))
        return invalid;
    for (const auto& dimension : bucket)
        if (!finiteNumber(dimension) || dimension < 0.01f || dimension > std::numeric_limits<float>::max())
            return invalid;
    applySolver(registration.ndt, n);
    applySolver(registration.icp, i);
    for (std::size_t d = 0; d < 3; ++d)
        registration.ndt.bucket_size[d] = bucket.at(d);
    registration.ndt.is_generalized = n.at("is_generalized");
    registration.ndt.sigma_r = n.at("sigma_r");
    registration.ndt.sigma_polar_angle = n.at("sigma_polar_angle");
    registration.ndt.sigma_azimuthal_angle = n.at("sigma_azimuthal_angle");
    registration.ndt.num_extended_points = n.at("num_extended_points");
    registration.compute_mean_and_cov_for_bucket = n.at("compute_mean_and_cov_for_bucket");
    registration.icp.search_radius = i.at("search_radius");
    registration.icp.is_adaptive_robust_kernel = i.at("is_adaptive_robust_kernel");
    applySolver(registration.registration_plane_feature, plane);
    registration.registration_plane_feature.search_radius = plane.at("search_radius");
    registration.registration_plane_feature.is_adaptive_robust_kernel = plane.at("is_adaptive_robust_kernel");
    return {};
}
