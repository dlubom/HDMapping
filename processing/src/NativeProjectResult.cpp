#include "project_operations.h"

#include "NativeSessionResult.h"
#include <HDMapping/SessionPaths.h>

#include <algorithm>
#include <stdexcept>

namespace HDMapping::Processing
{
    void ValidateNativeProject(const SnapshotContents& contents, const ArtifactManifest& manifest, bool read_sessions, std::stop_token stop)
    {
        if (!contents.dependencies.empty() || contents.entry.extension() != ".mjp")
            throw std::invalid_argument("Expected a self-contained native project");
        const auto settings = ReadProjectSettings(contents.entry, true);
        std::vector<std::size_t> scan_counts;
        for (std::size_t index = 0; index < settings.session_file_names.size(); ++index)
        {
            const auto& name = settings.session_file_names[index];
            if (stop.stop_requested())
                throw std::runtime_error("Project publication cancelled");
            const auto entry = sessionPathFromText(name);
            if (entry.extension() != ".mjs" || std::ranges::find(manifest.files, entry) == manifest.files.end())
                throw std::invalid_argument("Project session is not owned by the result");
            const auto subdirectory = entry.parent_path();
            if (subdirectory != contents.entry.parent_path() / std::to_string(index))
                throw std::invalid_argument("Project session must have its own directory");
            ArtifactManifest child;
            for (const auto& file : manifest.files)
                if (file.parent_path() == subdirectory)
                    child.files.push_back(file);
            ValidateNativeSessionReferences({ std::string(native_session_kind), entry, {}, {} }, child);
            if (read_sessions)
                scan_counts.push_back(VerifyNativeSessionRead(entry));
        }
        if (read_sessions)
            ValidateProjectScanIndices(settings, scan_counts);
    }
} // namespace HDMapping::Processing
