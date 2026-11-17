#pragma once

#include <filesystem>
#include <string_view>

namespace HDMapping::Processing
{
    //! Publish a complete worker receipt atomically in an existing private job directory.
    void PublishRuntimeReceipt(const std::filesystem::path& directory, std::string_view contents);
} // namespace HDMapping::Processing
