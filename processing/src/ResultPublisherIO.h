#pragma once

#include <HDMapping/Processing/ResultPublisher.h>

namespace HDMapping::Processing
{
    //! I/O boundaries used to verify failures at the write and commit steps.
    struct PublicationIO
    {
        void (*write_manifest)(const std::filesystem::path&, const std::string&);
        void (*move)(const std::filesystem::path&, const std::filesystem::path&, bool replace);
    };

    //! Commit a same-filesystem staging file or directory with native no-replace semantics.
    void MovePublishedPath(const std::filesystem::path& source, const std::filesystem::path& destination, bool replace);

    RunResult PublishResult(
        const std::filesystem::path& staging_directory,
        const std::filesystem::path& destination,
        const PublishOptions& options,
        const PublicationIO& io,
        std::stop_token stop = {});
} // namespace HDMapping::Processing
