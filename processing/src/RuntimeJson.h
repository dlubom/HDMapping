#pragma once

#include <HDMapping/Processing/Operation.h>

namespace HDMapping::Processing::RuntimeJson
{
    using nlohmann::json;

    inline std::string path_to_utf8(const std::filesystem::path& path)
    {
        const auto utf8 = path.u8string();
        return { utf8.begin(), utf8.end() };
    }

    inline json target_to_json(const TargetRef& target)
    {
        return { { "state_id", target.state_id }, { "revision", target.revision } };
    }

    inline json paths_to_json(const std::vector<std::filesystem::path>& paths)
    {
        json result = json::array();
        for (const auto& path : paths)
        {
            result.push_back(path_to_utf8(path));
        }
        return result;
    }

    inline json manifest_to_json(const ArtifactManifest& manifest)
    {
        return { { "artifact_id", manifest.artifact_id },
                 { "target", target_to_json(manifest.target) },
                 { "snapshot",
                   { { "manifest_path", path_to_utf8(manifest.snapshot.manifest_path) },
                     { "target", target_to_json(manifest.snapshot.target) } } },
                 { "files", paths_to_json(manifest.files) },
                 { "dependencies", paths_to_json(manifest.dependencies) } };
    }

    inline TargetRef parse_target(const json& target)
    {
        const auto& state = target.at("state_id").get_ref<const std::string&>();
        const auto& revision = target.at("revision");
        if (state.empty() || !revision.is_number_unsigned())
        {
            throw std::invalid_argument("Invalid target state or revision");
        }
        return { state, revision.get<std::uint64_t>() };
    }

} // namespace HDMapping::Processing::RuntimeJson
