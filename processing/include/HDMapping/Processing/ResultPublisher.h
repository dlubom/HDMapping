#pragma once

#include <HDMapping/Processing/Operation.h>

#include <string_view>

namespace HDMapping::Processing
{
    inline constexpr std::string_view session_export_kind = "session_export";

    struct PublishOptions
    {
        bool replace = false;
        std::string expected_artifact_id;
    };

    //! Consume a private sibling .hdmapping-result-stage-<id> built with CaptureSnapshot
    //! (kind native_session or session_export). The id is unique for each attempt. Shared inputs must be pinned
    //! independently of job cleanup. Replacement compares the current artifact id.
    RunResult PublishResult(
        const std::filesystem::path& staging_directory, const std::filesystem::path& destination, const PublishOptions& options = {});

    //! Discard an attempt's staging only after its writer has exited. The caller owns the attempt id.
    std::optional<OperationError> DiscardResultStaging(
        const std::filesystem::path& staging_directory, const std::filesystem::path& destination);

    //! Resolve the current committed result. The returned generation remains readable after replacement.
    RunResult ReadPublishedResult(const std::filesystem::path& destination);

    //! Resolve the immutable generation identified by a worker receipt, including after a newer publication.
    RunResult ReadPublishedGeneration(const std::filesystem::path& destination, const std::string& artifact_id);

    //! Copy a published session and its inputs into a relocatable result at a new destination.
    RunResult ExportComplete(const ArtifactManifest& source, const std::filesystem::path& destination, std::stop_token stop = {});
} // namespace HDMapping::Processing
