#pragma once
#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

// Session files (Session::save() in core, session.mjs from lidar_odometry_step_1)
// store the paths of the files they reference relative to the session file's
// folder, so a session keeps working after its folder is moved or copied to
// another computer. In memory the paths stay absolute: writers convert with
// toSessionPath(), readers with resolveSessionPath().

// Path to write into session_file for `path`: relative to the session file's
// folder with '/' separators, or absolute when there is no relative path (e.g.
// another drive on Windows). An empty path stays empty.
inline std::filesystem::path sessionPathRelativeTo(const std::filesystem::path& path, const std::filesystem::path& session_file)
{
    namespace fs = std::filesystem;
    if (path.empty())
        return path;
    std::error_code ec_path;
    std::error_code ec_session;
    const fs::path absolute_path = fs::absolute(path, ec_path).lexically_normal();
    const fs::path session_dir = fs::absolute(session_file, ec_session).parent_path().lexically_normal();
    if (ec_path || ec_session)
        return path;
    const fs::path relative_path = absolute_path.lexically_relative(session_dir);
    return relative_path.empty() ? absolute_path : relative_path;
}

inline std::string toSessionPath(const std::string& path, const std::string& session_file)
{
    return sessionPathRelativeTo(path, session_file).generic_string();
}

// Absolute path for a path read from session_file. Relative paths resolve
// against the session file's folder, absolute ones (older sessions) are used as
// they are. If the result doesn't exist, the file of the same name next to the
// session file is used instead, so sessions moved together with their data keep
// loading.
inline std::string resolveSessionPath(const std::string& stored_path, const std::string& session_file)
{
    namespace fs = std::filesystem;
    // Sessions written on Windows use '\' separators
    std::string normalized = stored_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    const fs::path path(normalized);

    std::error_code ec;
    const fs::path session_dir = fs::absolute(session_file, ec).parent_path();
    const fs::path resolved = path.is_absolute() ? path : (session_dir / path).lexically_normal();
    if (fs::exists(resolved, ec))
    {
        return resolved.string();
    }
    return (session_dir / path.filename()).string();
}

// Native JSON references are UTF-8, including on Windows where path::string uses the system code page.
inline std::string sessionPathText(const std::filesystem::path& path)
{
    const auto bytes = path.generic_u8string();
    return { bytes.begin(), bytes.end() };
}

inline std::filesystem::path sessionPathFromText(const std::string& text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::filesystem::path sessionTrajectoryPath(const std::filesystem::path& cloud)
{
    const auto stem = sessionPathText(cloud.stem());
    const auto end = stem.find_last_not_of('_');
    if (end == std::string::npos)
    {
        return {};
    }
    const auto start = stem.rfind('_', end) + 1;
    return cloud.parent_path() / sessionPathFromText("trajectory_lio_" + stem.substr(start, end - start + 1) + ".csv");
}
