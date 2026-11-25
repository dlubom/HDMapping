#include "project_io.h"

#include <HDMapping/SessionPaths.h>

#include <algorithm>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace fs = std::filesystem;

nlohmann::json ProjectSettingsJson(const ProjectSettings& settings)
{
    nlohmann::json jj;

    nlohmann::json jsession_file_names;
    for (const auto& pc : settings.session_file_names)
    {
        nlohmann::json jfn{ { "session_file_name", pc } };
        jsession_file_names.push_back(jfn);
    }
    jj["session_file_names"] = jsession_file_names;

    nlohmann::json jloop_closure_edges;
    for (const auto& edge : settings.edges)
    {
        nlohmann::json jloop_closure_edge{
            { "px", edge.relative_pose_tb.px },
            { "py", edge.relative_pose_tb.py },
            { "pz", edge.relative_pose_tb.pz },
            { "om", edge.relative_pose_tb.om },
            { "fi", edge.relative_pose_tb.fi },
            { "ka", edge.relative_pose_tb.ka },
            { "w_px", edge.relative_pose_tb_weights.px },
            { "w_py", edge.relative_pose_tb_weights.py },
            { "w_pz", edge.relative_pose_tb_weights.pz },
            { "w_om", edge.relative_pose_tb_weights.om },
            { "w_fi", edge.relative_pose_tb_weights.fi },
            { "w_ka", edge.relative_pose_tb_weights.ka },
            { "index_from", edge.index_from },
            { "index_to", edge.index_to },
            { "is_fixed_px", edge.is_fixed_px },
            { "is_fixed_py", edge.is_fixed_py },
            { "is_fixed_pz", edge.is_fixed_pz },
            { "is_fixed_om", edge.is_fixed_om },
            { "is_fixed_fi", edge.is_fixed_fi },
            { "is_fixed_ka", edge.is_fixed_ka },
            { "index_session_from", edge.index_session_from },
            { "index_session_to", edge.index_session_to },
        };
        jloop_closure_edges.push_back(jloop_closure_edge);
    }
    jj["loop_closure_edges"] = jloop_closure_edges;

    return jj;
}

ProjectSettings ReadProjectSettings(const fs::path& path, bool strict)
{
    std::ifstream stream(path);
    const auto data = nlohmann::json::parse(stream);
    ProjectSettings loaded;
    const auto& children = data.at("session_file_names");
    if (!children.is_array() && !children.is_null())
        throw std::invalid_argument("Invalid project session list");
    for (const auto& item : children)
    {
        auto name = item.at("session_file_name").get<std::string>();
        if (name.empty() || name.find('\0') != std::string::npos)
            throw std::invalid_argument("Invalid project session path");
        std::ranges::replace(name, '\\', '/');
        const auto stored = sessionPathFromText(name);
        if (strict && (stored.has_root_path() || stored != stored.lexically_normal() || *stored.begin() == ".."))
            throw std::invalid_argument("A project snapshot must use relative session references");
        auto resolved = (fs::absolute(path).parent_path() / stored).lexically_normal();
        // Older projects could store a path relative to the process working directory.
        if (!strict && stored.is_relative() && !fs::exists(resolved) && fs::exists(stored))
            resolved = fs::absolute(stored).lexically_normal();
        loaded.session_file_names.push_back(sessionPathText(resolved));
    }

    const auto edges = data.value("loop_closure_edges", nlohmann::json{});
    if (!edges.is_array() && !edges.is_null())
        throw std::invalid_argument("Invalid project constraint list");
    for (const auto& edge_json : edges)
    {
        Edge edge;
        edge.index_from = edge_json.at("index_from");
        edge.index_to = edge_json.at("index_to");
        edge.is_fixed_fi = edge_json.at("is_fixed_fi");
        edge.is_fixed_ka = edge_json.at("is_fixed_ka");
        edge.is_fixed_om = edge_json.at("is_fixed_om");
        edge.is_fixed_px = edge_json.at("is_fixed_px");
        edge.is_fixed_py = edge_json.at("is_fixed_py");
        edge.is_fixed_pz = edge_json.at("is_fixed_pz");
        edge.relative_pose_tb.fi = edge_json.at("fi");
        edge.relative_pose_tb.ka = edge_json.at("ka");
        edge.relative_pose_tb.om = edge_json.at("om");
        edge.relative_pose_tb.px = edge_json.at("px");
        edge.relative_pose_tb.py = edge_json.at("py");
        edge.relative_pose_tb.pz = edge_json.at("pz");
        edge.relative_pose_tb_weights.fi = edge_json.at("w_fi");
        edge.relative_pose_tb_weights.ka = edge_json.at("w_ka");
        edge.relative_pose_tb_weights.om = edge_json.at("w_om");
        edge.relative_pose_tb_weights.px = edge_json.at("w_px");
        edge.relative_pose_tb_weights.py = edge_json.at("w_py");
        edge.relative_pose_tb_weights.pz = edge_json.at("w_pz");
        edge.index_session_from = edge_json.at("index_session_from");
        edge.index_session_to = edge_json.at("index_session_to");
        loaded.edges.push_back(edge);
    }

    for (const auto& edge : loaded.edges)
    {
        if (edge.index_session_from < 0 || edge.index_session_to < 0 ||
            static_cast<std::size_t>(edge.index_session_from) >= loaded.session_file_names.size() ||
            static_cast<std::size_t>(edge.index_session_to) >= loaded.session_file_names.size())
            throw std::invalid_argument("Invalid project constraint session index");
    }
    return loaded;
}

void WriteProjectSettings(const fs::path& path, const ProjectSettings& settings)
{
    std::ofstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream << ProjectSettingsJson(settings).dump(2);
    stream.close();
}

Session ReadProjectSession(const std::string& path, const ProjectLoadOptions& options)
{
    Session session;
    const bool loaded = options.strict
        ? session.load_published(sessionPathFromText(path))
        : session.load(path, options.decimate, options.buckets[0], options.buckets[1], options.buckets[2], options.calculate_offset);
    if (!loaded || session.point_clouds_container.point_clouds.empty())
        throw std::runtime_error("Could not load project session: " + path);
    for (auto& scan : session.point_clouds_container.point_clouds)
        scan.m_pose_temp = scan.m_pose;
    return session;
}

bool AddProjectSessionPath(ProjectSettings& settings, const std::string& path)
{
    const auto absolute = sessionPathText(fs::absolute(sessionPathFromText(path)).lexically_normal());
    if (std::ranges::find(settings.session_file_names, absolute) != settings.session_file_names.end())
        return false;
    settings.session_file_names.push_back(absolute);
    return true;
}

void RemoveProjectSession(ProjectSettings& settings, std::vector<Session>& sessions, std::size_t index)
{
    if (index >= settings.session_file_names.size())
        throw std::out_of_range("Project session index is out of range");
    settings.session_file_names.erase(settings.session_file_names.begin() + static_cast<std::ptrdiff_t>(index));
    if (index < sessions.size())
        sessions.erase(sessions.begin() + static_cast<std::ptrdiff_t>(index));
    const auto removed = static_cast<int>(index);
    std::erase_if(
        settings.edges,
        [removed](const Edge& edge)
        {
            return edge.index_session_from == removed || edge.index_session_to == removed;
        });
    for (auto& edge : settings.edges)
    {
        edge.index_session_from -= edge.index_session_from > removed;
        edge.index_session_to -= edge.index_session_to > removed;
    }
}

void SetProjectGroundTruth(std::vector<Session>& sessions, int index)
{
    if (index < -1 || (index >= 0 && static_cast<std::size_t>(index) >= sessions.size()))
        throw std::out_of_range("Ground truth session index is out of range");
    for (std::size_t i = 0; i < sessions.size(); ++i)
        sessions[i].is_ground_truth = index >= 0 && i == static_cast<std::size_t>(index);
}

void ValidateProjectScanIndices(const ProjectSettings& settings, const std::vector<std::size_t>& scan_counts)
{
    for (const auto& edge : settings.edges)
    {
        if (edge.index_session_from < 0 || edge.index_session_to < 0 ||
            static_cast<std::size_t>(edge.index_session_from) >= scan_counts.size() ||
            static_cast<std::size_t>(edge.index_session_to) >= scan_counts.size())
            throw std::invalid_argument("Invalid project constraint session index");
        if (edge.index_from < 0 || edge.index_to < 0 ||
            static_cast<std::size_t>(edge.index_from) >= scan_counts[static_cast<std::size_t>(edge.index_session_from)] ||
            static_cast<std::size_t>(edge.index_to) >= scan_counts[static_cast<std::size_t>(edge.index_session_to)])
            throw std::invalid_argument("Invalid project constraint scan index");
    }
}

void ValidateProjectScans(const ProjectSettings& settings, const std::vector<Session>& sessions)
{
    std::vector<std::size_t> scan_counts;
    scan_counts.reserve(sessions.size());
    for (const auto& session : sessions)
        scan_counts.push_back(session.point_clouds_container.point_clouds.size());
    ValidateProjectScanIndices(settings, scan_counts);
}

void OrderProjectGroundTruthFirst(ProjectSettings& settings, std::vector<Session>& sessions)
{
    std::vector<std::size_t> order(sessions.size());
    std::iota(order.begin(), order.end(), std::size_t{ 0 });
    std::ranges::stable_partition(
        order,
        [&sessions](std::size_t index)
        {
            return sessions[index].is_ground_truth;
        });
    std::vector<int> new_indices(sessions.size());
    std::vector<std::string> paths;
    std::vector<Session> sorted;
    paths.reserve(order.size());
    sorted.reserve(order.size());
    for (const auto index : order)
    {
        new_indices[index] = static_cast<int>(sorted.size());
        paths.push_back(std::move(settings.session_file_names[index]));
        sorted.push_back(std::move(sessions[index]));
    }
    for (auto& edge : settings.edges)
    {
        edge.index_session_from = new_indices.at(static_cast<std::size_t>(edge.index_session_from));
        edge.index_session_to = new_indices.at(static_cast<std::size_t>(edge.index_session_to));
    }
    settings.session_file_names = std::move(paths);
    sessions = std::move(sorted);
}
