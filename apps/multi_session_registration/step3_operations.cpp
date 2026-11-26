#include "step3_operations.h"

#include <Core/ndt.h>
#include <Core/pair_wise_iterative_closest_point.h>

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>

namespace HDMapping::Processing
{
    using nlohmann::json;

    namespace
    {
        constexpr std::string_view transform = "project.transform_scope";
        constexpr std::string_view restore = "project.restore_poses";
        constexpr std::string_view registration = "project.register_sessions";
        constexpr std::string_view optimize_graph = "optimize_project";
        constexpr std::string_view list = "project.constraints.list";
        constexpr std::string_view add = "project.constraints.add";
        constexpr std::string_view update = "project.constraints.update";
        constexpr std::string_view remove = "project.constraints.remove";
        constexpr std::string_view replace = "project.constraints.replace";
        constexpr std::string_view detect = "project.constraints.detect_candidates";
        constexpr std::string_view icp = "project.constraints.run_icp";

        using SessionPairs = std::vector<std::pair<int, int>>;

        void keys(
            const json& values, std::initializer_list<std::string_view> required, std::initializer_list<std::string_view> optional = {})
        {
            if (!values.is_object() ||
                !std::ranges::all_of(
                    required,
                    [&values](std::string_view key)
                    {
                        return values.contains(key);
                    }))
                throw std::invalid_argument("Missing required Step 3 arguments");
            for (const auto& item : values.items())
                if (std::ranges::find(required, item.key()) == required.end() && std::ranges::find(optional, item.key()) == optional.end())
                    throw std::invalid_argument("Unexpected Step 3 argument: " + item.key());
        }

        int integer(const json& value, int minimum, int maximum)
        {
            if (!value.is_number_integer() ||
                (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(maximum)) ||
                (!value.is_number_unsigned() && (value.get<std::int64_t>() < minimum || value.get<std::int64_t>() > maximum)) ||
                (value.is_number_unsigned() && value.get<std::uint64_t>() < static_cast<std::uint64_t>(minimum)))
                throw std::invalid_argument("Integer argument is out of range");
            return value.get<int>();
        }

        double number(const json& value, double minimum, double maximum = std::numeric_limits<double>::max())
        {
            if (!value.is_number())
                throw std::invalid_argument("Expected a numeric argument");
            const auto result = value.get<double>();
            if (!std::isfinite(result) || result < minimum || result > maximum)
                throw std::invalid_argument("Numeric argument is out of range");
            return result;
        }

        int index(const json& value, std::size_t count)
        {
            const auto result = integer(value, 0, std::numeric_limits<int>::max());
            if (static_cast<std::size_t>(result) >= count)
                throw std::out_of_range("Step 3 index is out of range");
            return result;
        }

        std::vector<int> indices(const json& values, std::size_t count)
        {
            if (!values.is_array())
                throw std::invalid_argument("Expected an array of explicit indices");
            std::vector<int> result;
            std::set<int> seen;
            for (const auto& value : values)
            {
                const auto item = index(value, count);
                if (!seen.insert(item).second)
                    throw std::invalid_argument("Duplicate Step 3 index");
                result.push_back(item);
            }
            return result;
        }

        TaitBryanPose pose(const json& values, double minimum = -std::numeric_limits<double>::max())
        {
            if (!values.is_array() || values.size() != 6)
                throw std::invalid_argument("Expected six pose components ordered px, py, pz, om, fi, ka");
            return { number(values[0], minimum), number(values[1], minimum), number(values[2], minimum),
                     number(values[3], minimum), number(values[4], minimum), number(values[5], minimum) };
        }

        json pose_json(const TaitBryanPose& value)
        {
            return { value.px, value.py, value.pz, value.om, value.fi, value.ka };
        }

        const auto& scans(const std::vector<Session>& sessions, int session)
        {
            return sessions.at(session).point_clouds_container.point_clouds;
        }

        json edge_json(const Edge& edge)
        {
            return { { "from", { { "session_index", edge.index_session_from }, { "scan_index", edge.index_from } } },
                     { "to", { { "session_index", edge.index_session_to }, { "scan_index", edge.index_to } } },
                     { "relative_pose", pose_json(edge.relative_pose_tb) },
                     { "weights", pose_json(edge.relative_pose_tb_weights) },
                     { "fixed",
                       { edge.is_fixed_px, edge.is_fixed_py, edge.is_fixed_pz, edge.is_fixed_om, edge.is_fixed_fi, edge.is_fixed_ka } } };
        }

        Edge read_edge(const json& value, const std::vector<Session>& sessions)
        {
            keys(value, { "from", "to", "relative_pose", "weights" }, { "fixed" });
            const auto& from = value.at("from");
            const auto& to = value.at("to");
            keys(from, { "session_index", "scan_index" });
            keys(to, { "session_index", "scan_index" });
            Edge edge{};
            edge.index_session_from = index(from.at("session_index"), sessions.size());
            edge.index_session_to = index(to.at("session_index"), sessions.size());
            edge.index_from = index(from.at("scan_index"), scans(sessions, edge.index_session_from).size());
            edge.index_to = index(to.at("scan_index"), scans(sessions, edge.index_session_to).size());
            if (edge.index_session_from == edge.index_session_to && edge.index_from == edge.index_to)
                throw std::invalid_argument("An edge must connect different scan endpoints");
            edge.relative_pose_tb = pose(value.at("relative_pose"));
            edge.relative_pose_tb_weights = pose(value.at("weights"), 0.0);
            if (value.contains("fixed"))
            {
                const auto& fixed = value.at("fixed");
                if (!fixed.is_array() || fixed.size() != 6)
                    throw std::invalid_argument("Expected six fixed-axis flags");
                edge.is_fixed_px = fixed[0].get<bool>();
                edge.is_fixed_py = fixed[1].get<bool>();
                edge.is_fixed_pz = fixed[2].get<bool>();
                edge.is_fixed_om = fixed[3].get<bool>();
                edge.is_fixed_fi = fixed[4].get<bool>();
                edge.is_fixed_ka = fixed[5].get<bool>();
            }
            return edge;
        }

        void set_pose(PointCloud& scan, const Eigen::Affine3d& value)
        {
            scan.m_pose = value;
            scan.pose = pose_tait_bryan_from_affine_matrix(value);
            scan.gui_translation[0] = static_cast<float>(scan.pose.px);
            scan.gui_translation[1] = static_cast<float>(scan.pose.py);
            scan.gui_translation[2] = static_cast<float>(scan.pose.pz);
            scan.gui_rotation[0] = static_cast<float>(rad2deg(scan.pose.om));
            scan.gui_rotation[1] = static_cast<float>(rad2deg(scan.pose.fi));
            scan.gui_rotation[2] = static_cast<float>(rad2deg(scan.pose.ka));
        }

        void transform_session(Session& session, const Eigen::Affine3d& delta)
        {
            if (!session.is_ground_truth)
                for (auto& scan : session.point_clouds_container.point_clouds)
                    set_pose(scan, delta * scan.m_pose);
        }

        SessionPairs session_pairs(const json& values, const std::vector<Session>& sessions, bool scan_scope = false)
        {
            if (!values.is_array() || values.empty())
                throw std::invalid_argument("Expected non-empty explicit session pairs");
            SessionPairs result;
            std::set<std::pair<int, int>> seen;
            for (const auto& value : values)
            {
                if (scan_scope)
                    keys(value, { "from", "to", "from_scan", "to_scan" });
                else
                    keys(value, { "from", "to" });
                const auto from = index(value.at("from"), sessions.size());
                const auto to = index(value.at("to"), sessions.size());
                if (from == to || !seen.emplace(std::min(from, to), std::max(from, to)).second)
                    throw std::invalid_argument("Session pairs must be distinct");
                result.emplace_back(from, to);
            }
            return result;
        }

        Edge proximity_edge(const std::vector<Session>& sessions, int from, int to, int from_scan, int to_scan)
        {
            Edge edge{};
            edge.index_session_from = from;
            edge.index_session_to = to;
            edge.index_from = from_scan;
            edge.index_to = to_scan;
            edge.relative_pose_tb =
                pose_tait_bryan_from_affine_matrix(scans(sessions, from)[from_scan].m_pose.inverse() * scans(sessions, to)[to_scan].m_pose);
            edge.relative_pose_tb_weights = { 1, 1, 1, 1, 1, 1 };
            return edge;
        }

        void check_stop(const RunContext& context)
        {
            if (context.stop.stop_requested())
                throw std::runtime_error("Step 3 operation cancelled");
        }

        struct ICPSettings
        {
            float radius;
            int iterations;
            int before;
            int after;
            bool crop_ground_truth;
        };

        ICPSettings icp_settings(const json& parameters)
        {
            return { static_cast<float>(number(parameters.value("search_radius_m", json(0.1)), 0.01, std::numeric_limits<float>::max())),
                     integer(parameters.value("iterations", json(10)), 1, std::numeric_limits<int>::max()),
                     integer(parameters.value("scans_before", json(0)), 0, std::numeric_limits<int>::max()),
                     integer(parameters.value("scans_after", json(0)), 0, std::numeric_limits<int>::max()),
                     parameters.value("crop_ground_truth", true) };
        }

        std::vector<Eigen::Vector3d> extended_points(const std::vector<PointCloud>& clouds, int centre, const ICPSettings& params)
        {
            if (params.before == 0 && params.after == 0)
                return clouds.at(centre).points_local;
            const auto middle = static_cast<std::size_t>(centre);
            const auto start = middle - std::min(middle, static_cast<std::size_t>(params.before));
            const auto end = std::min(clouds.size(), middle + static_cast<std::size_t>(params.after) + 1);
            std::vector<Eigen::Vector3d> points;
            for (auto i = start; i < end; ++i)
                for (const auto& point : clouds[i].points_local)
                    points.push_back(clouds[i].m_pose * point);
            const Eigen::Affine3d inverse = clouds[middle].m_pose.inverse();
            for (auto& point : points)
                point = inverse * point;
            return points;
        }

        bool refine_edge(const std::vector<Session>& sessions, Edge& edge, const ICPSettings& params)
        {
            const bool from_truth = sessions[edge.index_session_from].is_ground_truth;
            const bool to_truth = sessions[edge.index_session_to].is_ground_truth;
            if (from_truth && to_truth)
                return false;
            const bool reverse = params.crop_ground_truth && to_truth;
            const auto& source_scans = scans(sessions, reverse ? edge.index_session_from : edge.index_session_to);
            const auto& target_scans = scans(sessions, reverse ? edge.index_session_to : edge.index_session_from);
            const int source_index = reverse ? edge.index_from : edge.index_to;
            const int target_index = reverse ? edge.index_to : edge.index_from;
            const auto& source_scan = source_scans.at(source_index);
            const auto& target_scan = target_scans.at(target_index);
            std::vector<Eigen::Vector3d> source;
            std::vector<Eigen::Vector3d> target;
            if (params.crop_ground_truth && (from_truth || to_truth))
            {
                source = source_scan.points_local;
                Eigen::AlignedBox3d region;
                for (const auto& point : source)
                    region.extend(source_scan.m_pose * point);
                for (const auto& point : target_scan.points_local)
                {
                    const Eigen::Vector3d global = target_scan.m_pose * point;
                    if ((global.array() > region.min().array()).all() && (global.array() < region.max().array()).all())
                        target.push_back(point);
                }
            }
            else
            {
                source = extended_points(source_scans, source_index, params);
                target = extended_points(target_scans, target_index, params);
            }
            auto relative = affine_matrix_from_pose_tait_bryan(edge.relative_pose_tb);
            if (reverse)
                relative = relative.inverse();
            PairWiseICP engine;
            if (source.empty() || target.empty() || !engine.compute(source, target, params.radius, params.iterations, relative) ||
                !relative.matrix().allFinite())
                return false;
            edge.relative_pose_tb = pose_tait_bryan_from_affine_matrix(reverse ? Eigen::Affine3d(relative.inverse()) : relative);
            return true;
        }

        std::vector<int> ndt_indices(const json& scope, const std::vector<Session>& sessions)
        {
            if (scope.contains("session_indices"))
            {
                keys(scope, { "session_indices" });
                const auto selected = indices(scope.at("session_indices"), sessions.size());
                if (selected.size() != 1)
                    throw std::invalid_argument("Single-session NDT requires exactly one session index");
                return selected;
            }
            keys(scope, { "session_pairs" });
            const auto pairs = session_pairs(scope.at("session_pairs"), sessions);
            std::set<int> unique;
            for (const auto& [from, to] : pairs)
            {
                unique.insert(from);
                unique.insert(to);
            }
            if (pairs.size() != unique.size() * (unique.size() - 1) / 2)
                throw std::invalid_argument("Joint NDT requires an explicit pair for every two participating sessions");
            std::vector<int> selected(unique.begin(), unique.end());
            std::ranges::stable_partition(
                selected,
                [&sessions](int i)
                {
                    return sessions[i].is_ground_truth;
                });
            return selected;
        }

        void run_ndt(const OperationRequest& request, std::vector<Session>& sessions)
        {
            keys(request.parameters, { "method" }, { "bucket_size_m", "threads", "iterations" });
            const auto selected = ndt_indices(request.scope, sessions);
            if (std::ranges::count_if(
                    selected,
                    [&sessions](int i)
                    {
                        return sessions[i].is_ground_truth;
                    }) > 1)
                throw std::invalid_argument("Joint NDT accepts at most one ground truth session");
            NDT engine;
            const auto buckets =
                request.parameters.value("bucket_size_m", json{ engine.bucket_size[0], engine.bucket_size[1], engine.bucket_size[2] });
            if (!buckets.is_array() || buckets.size() != 3)
                throw std::invalid_argument("Expected three NDT bucket dimensions in metres");
            for (int i = 0; i < 3; ++i)
                engine.bucket_size[i] = static_cast<float>(number(buckets[i], 0.01, std::numeric_limits<float>::max()));
            engine.number_of_threads =
                integer(request.parameters.value("threads", json(engine.number_of_threads)), 1, std::numeric_limits<int>::max());
            engine.number_of_iterations =
                integer(request.parameters.value("iterations", json(engine.number_of_iterations)), 1, std::numeric_limits<int>::max());
            if (selected.size() == 1 && sessions[selected.front()].is_ground_truth)
                return;
            for (const auto i : selected)
                if (scans(sessions, i).empty() ||
                    std::ranges::all_of(
                        scans(sessions, i),
                        [](const PointCloud& cloud)
                        {
                            return cloud.points_local.empty();
                        }))
                    throw std::invalid_argument("NDT needs points in each selected session");

            // NDT uses only the scans and ground-truth flag of each selected session.
            std::vector<Session> working(selected.size());
            for (std::size_t i = 0; i < selected.size(); ++i)
            {
                auto& session = sessions[selected[i]];
                working[i].is_ground_truth = session.is_ground_truth;
                working[i].point_clouds_container.point_clouds.swap(session.point_clouds_container.point_clouds);
            }
            const auto restore_sessions = [&]
            {
                for (std::size_t i = 0; i < working.size(); ++i)
                    sessions[selected[i]].point_clouds_container.point_clouds.swap(working[i].point_clouds_container.point_clouds);
            };
            bool solved;
            try
            {
                solved = engine.optimize(working, false, false, true);
            } catch (...)
            {
                restore_sessions();
                throw;
            }
            restore_sessions();
            if (!solved)
                throw std::runtime_error("NDT could not solve the selected registration");
        }

        void validate_graph_sessions(const std::vector<Session>& sessions)
        {
            if (sessions.empty())
                throw std::invalid_argument("Graph optimization needs at least one loaded project session");
            for (const auto& session : sessions)
            {
                const auto& clouds = session.point_clouds_container.point_clouds;
                if (clouds.empty())
                    throw std::invalid_argument("Graph optimization needs scans in each loaded session");
                for (const auto& edge : session.pose_graph_loop_closure.edges)
                {
                    static_cast<void>(index(edge.index_from, clouds.size()));
                    static_cast<void>(index(edge.index_to, clouds.size()));
                }
                for (const auto& cp : session.control_points.cps)
                    static_cast<void>(index(cp.index_to_pose, clouds.size()));
                for (const auto& gcp : session.ground_control_points.gpcs)
                {
                    const auto scan = index(gcp.index_to_node_inner, clouds.size());
                    static_cast<void>(index(gcp.index_to_node_outer, clouds[scan].local_trajectory.size()));
                }
            }
        }

        void register_icp(const OperationRequest& request, std::vector<Session>& sessions, const RunContext& context)
        {
            keys(request.parameters, { "method" }, { "search_radius_m", "iterations", "scans_before", "scans_after", "crop_ground_truth" });
            keys(request.scope, { "session_pairs" });
            const auto& scope = request.scope.at("session_pairs");
            const auto pairs = session_pairs(scope, sessions, true);
            const auto params = icp_settings(request.parameters);
            std::vector<Edge> endpoints;
            std::set<int> moving;
            for (std::size_t i = 0; i < pairs.size(); ++i)
            {
                const auto [from, to] = pairs[i];
                if (sessions[to].is_ground_truth || !moving.insert(to).second)
                    throw std::invalid_argument("Each ICP pair needs a distinct movable 'to' session");
                endpoints.push_back(proximity_edge(
                    sessions,
                    from,
                    to,
                    index(scope[i].at("from_scan"), scans(sessions, from).size()),
                    index(scope[i].at("to_scan"), scans(sessions, to).size())));
            }
            std::vector<std::pair<int, Eigen::Affine3d>> changes;
            for (auto& edge : endpoints)
            {
                check_stop(context);
                if (moving.contains(edge.index_session_from))
                    throw std::invalid_argument("ICP reference sessions must be outside the moving scope");
                if (!refine_edge(sessions, edge, params))
                    throw std::runtime_error("ICP did not find a solution for a session pair");
                const auto& from = scans(sessions, edge.index_session_from)[edge.index_from];
                const auto& to = scans(sessions, edge.index_session_to)[edge.index_to];
                changes.emplace_back(
                    edge.index_session_to, from.m_pose * affine_matrix_from_pose_tait_bryan(edge.relative_pose_tb) * to.m_pose.inverse());
            }
            check_stop(context);
            for (const auto& [to, delta] : changes)
                transform_session(sessions[to], delta);
        }
    } // namespace

    bool IsStep3Edit(std::string_view operation)
    {
        return operation == transform || operation == restore || operation == add || operation == update || operation == remove ||
            operation == replace;
    }

    bool IsStep3Operation(std::string_view operation)
    {
        return IsStep3Edit(operation) || operation == registration || operation == optimize_graph || operation == list ||
            operation == detect || operation == icp;
    }

    json ProjectEdgesJson(std::span<const Edge> edges)
    {
        auto result = json::array();
        for (const auto& edge : edges)
            result.push_back(edge_json(edge));
        return result;
    }

    void EditStep3(const OperationRequest& request, ProjectSettings& settings, std::vector<Session>& sessions)
    {
        const auto& scope = request.scope;
        const auto& parameters = request.parameters;
        if (request.operation == transform || request.operation == restore)
        {
            keys(scope, { "session_indices" });
            const auto selected = indices(scope.at("session_indices"), sessions.size());
            if (selected.empty())
                throw std::invalid_argument("Select at least one project session");
            if (request.operation == transform)
            {
                keys(parameters, { "transform" });
                const auto delta = affine_matrix_from_pose_tait_bryan(pose(parameters.at("transform")));
                for (const auto i : selected)
                    transform_session(sessions[i], delta);
            }
            else
            {
                keys(parameters, { "source" });
                const auto source = parameters.at("source").get<std::string>();
                if (source != "initial" && source != "previous")
                    throw std::invalid_argument("Pose source must be initial or previous");
                for (const auto i : selected)
                    if (!sessions[i].is_ground_truth)
                        for (auto& scan : sessions[i].point_clouds_container.point_clouds)
                            set_pose(scan, source == "initial" ? scan.m_initial_pose : scan.m_pose_temp);
            }
        }
        else if (request.operation == add)
        {
            keys(scope, {});
            keys(parameters, { "edge" });
            settings.edges.push_back(read_edge(parameters.at("edge"), sessions));
        }
        else if (request.operation == update || request.operation == remove)
        {
            keys(scope, { "edge_index" });
            const auto i = index(scope.at("edge_index"), settings.edges.size());
            if (request.operation == update)
            {
                keys(parameters, { "edge" });
                settings.edges[i] = read_edge(parameters.at("edge"), sessions);
            }
            else
            {
                keys(parameters, {});
                settings.edges.erase(settings.edges.begin() + i);
            }
        }
        else if (request.operation == replace)
        {
            keys(scope, { "edge_indices" });
            keys(parameters, { "edges" });
            const auto selected = indices(scope.at("edge_indices"), settings.edges.size());
            const auto& replacements = parameters.at("edges");
            if (selected.empty() || !replacements.is_array())
                throw std::invalid_argument("Replace requires an explicit non-empty active edge scope and an array of new edges");
            std::vector<Edge> inserted;
            for (const auto& edge : replacements)
                inserted.push_back(read_edge(edge, sessions));
            const std::set<int> removed(selected.begin(), selected.end());
            std::vector<Edge> result;
            for (std::size_t i = 0; i < settings.edges.size(); ++i)
            {
                if (static_cast<int>(i) == *removed.begin())
                    result.insert(result.end(), inserted.begin(), inserted.end());
                if (!removed.contains(static_cast<int>(i)))
                    result.push_back(settings.edges[i]);
            }
            settings.edges = std::move(result);
        }
        else
            throw std::invalid_argument("Unknown Step 3 edit");
    }

    json RunStep3(const OperationRequest& request, ProjectSettings& settings, std::vector<Session>& sessions, const RunContext& context)
    {
        check_stop(context);
        if (IsStep3Edit(request.operation))
            EditStep3(request, settings, sessions);
        else if (request.operation == list)
        {
            keys(request.scope, {});
            keys(request.parameters, {});
            return { { "active_edges", ProjectEdgesJson(settings.edges) } };
        }
        else if (request.operation == detect)
        {
            keys(request.scope, { "session_pairs" });
            keys(request.parameters, { "max_distance_m" });
            const double distance = number(request.parameters.at("max_distance_m"), std::numeric_limits<double>::min());
            std::vector<Edge> candidates;
            for (const auto& [from, to] : session_pairs(request.scope.at("session_pairs"), sessions))
                for (std::size_t i = 0; i < scans(sessions, from).size(); ++i)
                {
                    check_stop(context);
                    for (std::size_t j = 0; j < scans(sessions, to).size(); ++j)
                        if ((scans(sessions, from)[i].m_pose.translation() - scans(sessions, to)[j].m_pose.translation()).norm() < distance)
                            candidates.push_back(proximity_edge(sessions, from, to, static_cast<int>(i), static_cast<int>(j)));
                }
            return { { "candidates", ProjectEdgesJson(candidates) } };
        }
        else if (request.operation == registration)
        {
            if (context.progress)
                context.progress({ "register_sessions", {}, {}, {} });
            const auto method = request.parameters.at("method").get<std::string>();
            if (method == "ndt")
                run_ndt(request, sessions);
            else if (method == "icp")
                register_icp(request, sessions, context);
            else
                throw std::invalid_argument("Unknown project registration method");
        }
        else if (request.operation == icp)
        {
            keys(request.scope, { "edge_indices" });
            keys(request.parameters, {}, { "search_radius_m", "iterations", "scans_before", "scans_after", "crop_ground_truth" });
            const auto selected = indices(request.scope.at("edge_indices"), settings.edges.size());
            if (selected.empty())
                throw std::invalid_argument("Select at least one project edge");
            ValidateProjectScans(settings, sessions);
            const auto params = icp_settings(request.parameters);
            auto refined = settings.edges;
            for (std::size_t i = 0; i < selected.size(); ++i)
            {
                check_stop(context);
                if (context.progress)
                    context.progress({ "refine_project_edges", i, selected.size(), {} });
                if (!refine_edge(sessions, refined[selected[i]], params))
                    throw std::runtime_error("ICP did not refine project edge " + std::to_string(selected[i]));
            }
            check_stop(context);
            settings.edges = std::move(refined);
        }
        else if (request.operation == optimize_graph)
        {
            keys(request.scope, { "edge_indices" });
            keys(request.parameters, {}, { "motion_model_sigmas", "iterations" });
            std::vector<Edge> active;
            for (const auto i : indices(request.scope.at("edge_indices"), settings.edges.size()))
                active.push_back(settings.edges[i]);
            ValidateProjectScans(settings, sessions);
            const auto sigmas = pose(request.parameters.value("motion_model_sigmas", json{ 0.01, 0.01, 0.01, 0.1, 0.1, 0.1 }), 0.0);
            if (sigmas.om < 1e-12 || sigmas.fi < 1e-12 || sigmas.ka < 1e-12)
                throw std::invalid_argument("Angular motion model sigmas must be positive");
            const auto iterations = integer(request.parameters.value("iterations", json(100)), 1, std::numeric_limits<int>::max());
            validate_graph_sessions(sessions);

            std::vector<Eigen::Affine3d> before;
            for (const auto& session : sessions)
                for (const auto& scan : session.point_clouds_container.point_clouds)
                    before.push_back(scan.m_pose);
            try
            {
                for (int i = 0; i < iterations; ++i)
                {
                    check_stop(context);
                    if (context.progress)
                        context.progress({ "optimize_project", i, iterations, {} });
                    if (!optimize(sessions, active, sigmas))
                        throw std::runtime_error("The project graph could not be solved");
                }
                check_stop(context);
            } catch (...)
            {
                std::size_t i = 0;
                for (auto& session : sessions)
                    for (auto& scan : session.point_clouds_container.point_clouds)
                        set_pose(scan, before[i++]);
                throw;
            }
            std::size_t i = 0;
            for (auto& session : sessions)
                for (auto& scan : session.point_clouds_container.point_clouds)
                    scan.m_pose_temp = before[i++];
        }
        else
            throw std::invalid_argument("Unknown Step 3 operation");
        check_stop(context);
        return {};
    }

    std::vector<OperationDescriptor> DescribeStep3Operations()
    {
        const auto object = [](json properties, json required)
        {
            return json{ { "type", "object" },
                         { "properties", std::move(properties) },
                         { "required", std::move(required) },
                         { "additionalProperties", false } };
        };
        const auto array = [](const json& item, int minimum, bool unique = false)
        {
            return json{ { "type", "array" }, { "items", item }, { "minItems", minimum }, { "uniqueItems", unique } };
        };
        const auto tuple = [&array](const json& item, int size, std::string_view description)
        {
            auto schema = array(item, size);
            schema["maxItems"] = size;
            schema["description"] = description;
            return schema;
        };
        const json index_schema{ { "type", "integer" }, { "minimum", 0 } };
        const json positive_int{ { "type", "integer" }, { "minimum", 1 } };
        const json number_schema{ { "type", "number" } };
        const json metres{ { "type", "number" }, { "minimum", 0.01 } };
        const json boolean{ { "type", "boolean" } };
        const auto empty = object(json::object(), json::array());
        const auto session_indices = object({ { "session_indices", array(index_schema, 1, true) } }, { "session_indices" });
        const auto edge_indices = [&](int minimum)
        {
            return object({ { "edge_indices", array(index_schema, minimum, true) } }, { "edge_indices" });
        };
        const auto edge_index = object({ { "edge_index", index_schema } }, { "edge_index" });
        const auto endpoint =
            object({ { "session_index", index_schema }, { "scan_index", index_schema } }, { "session_index", "scan_index" });
        const auto relative = tuple(number_schema, 6, "Tait-Bryan px, py, pz in metres and om, fi, ka in radians");
        const auto edge = object(
            { { "from", endpoint },
              { "to", endpoint },
              { "relative_pose", relative },
              { "weights", tuple({ { "type", "number" }, { "minimum", 0 } }, 6, "Axis information weights") },
              { "fixed", tuple(boolean, 6, "Fixed-axis flags") } },
            { "from", "to", "relative_pose", "weights" });
        const auto edge_argument = object({ { "edge", edge } }, { "edge" });
        const auto pair = object({ { "from", index_schema }, { "to", index_schema } }, { "from", "to" });
        const auto icp_pair = object(
            { { "from", index_schema }, { "to", index_schema }, { "from_scan", index_schema }, { "to_scan", index_schema } },
            { "from", "to", "from_scan", "to_scan" });
        const auto pair_scope = [&](const json& schema)
        {
            return object({ { "session_pairs", array(schema, 1, true) } }, { "session_pairs" });
        };
        const json artifact{ { "type", "object" } };
        const auto icp_properties = json{ { "search_radius_m", metres },
                                          { "iterations", positive_int },
                                          { "scans_before", index_schema },
                                          { "scans_after", index_schema },
                                          { "crop_ground_truth", boolean } };
        auto registration_icp = icp_properties;
        registration_icp["method"] = { { "const", "icp" } };
        return {
            { std::string(transform),
              session_indices,
              object({ { "transform", relative } }, { "transform" }),
              artifact,
              OperationClass::Mutate },
            { std::string(restore),
              session_indices,
              object({ { "source", { { "enum", { "initial", "previous" } } } } }, { "source" }),
              artifact,
              OperationClass::Mutate },
            { std::string(registration),
              { { "oneOf", { session_indices, pair_scope(pair), pair_scope(icp_pair) } } },
              { { "oneOf",
                  { object(
                        { { "method", { { "const", "ndt" } } },
                          { "bucket_size_m", tuple(metres, 3, "NDT bucket dimensions in metres") },
                          { "threads", positive_int },
                          { "iterations", positive_int } },
                        { "method" }),
                    object(registration_icp, { "method" }) } } },
              artifact,
              OperationClass::Heavy },
            { std::string(optimize_graph),
              edge_indices(0),
              object(
                  { { "motion_model_sigmas", tuple(number_schema, 6, "XYZ in metres, roll/pitch/yaw in degrees") },
                    { "iterations", positive_int } },
                  json::array()),
              artifact,
              OperationClass::Heavy },
            { std::string(list), empty, empty, object({ { "active_edges", array(edge, 0) } }, { "active_edges" }), OperationClass::Read },
            { std::string(add), empty, edge_argument, artifact, OperationClass::Mutate },
            { std::string(update), edge_index, edge_argument, artifact, OperationClass::Mutate },
            { std::string(remove), edge_index, empty, artifact, OperationClass::Mutate },
            { std::string(replace),
              edge_indices(1),
              object({ { "edges", array(edge, 0) } }, { "edges" }),
              artifact,
              OperationClass::Mutate },
            { std::string(detect),
              pair_scope(pair),
              object({ { "max_distance_m", { { "type", "number" }, { "exclusiveMinimum", 0 } } } }, { "max_distance_m" }),
              object({ { "candidates", array(edge, 0) } }, { "candidates" }),
              OperationClass::Read },
            { std::string(icp), edge_indices(1), object(icp_properties, json::array()), artifact, OperationClass::Heavy },
        };
    }
} // namespace HDMapping::Processing
