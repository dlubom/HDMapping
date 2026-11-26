#pragma once

#include "multi_session_factor_graph.h"

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>

//! Native MJP session list and inter-session constraints, in file order.
struct ProjectSettings
{
    std::vector<std::string> session_file_names;
    std::vector<Edge> edges;
};

struct ProjectLoadOptions
{
    bool decimate = false;
    std::array<double, 3> buckets{};
    bool calculate_offset = false;
    bool strict = false;
};

//! Read the native project without opening sessions or updating the window.
ProjectSettings ReadProjectSettings(const std::filesystem::path& path, bool strict = false);
nlohmann::json ProjectSettingsJson(const ProjectSettings& settings);
void WriteProjectSettings(const std::filesystem::path& path, const ProjectSettings& settings);
Session ReadProjectSession(const std::string& path, const ProjectLoadOptions& options = {});
bool AddProjectSessionPath(ProjectSettings& settings, const std::string& path);
void RemoveProjectSession(ProjectSettings& settings, std::vector<Session>& sessions, std::size_t index);
void SetProjectGroundTruth(std::vector<Session>& sessions, int index);
void ValidateProjectScanIndices(const ProjectSettings& settings, const std::vector<std::size_t>& scan_counts);
void ValidateProjectScans(const ProjectSettings& settings, const std::vector<Session>& sessions);
void OrderProjectGroundTruthFirst(ProjectSettings& settings, std::vector<Session>& sessions);
