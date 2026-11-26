#include "recording_capture.h"

#include <HDMapping/SessionPaths.h>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace HDMapping::Processing
{
    namespace fs = std::filesystem;
    using nlohmann::json;

    namespace
    {
        using RecordingFiles = std::vector<std::pair<fs::path, fs::path>>;

        std::string extension(const fs::path& path)
        {
            std::string ext = sessionPathText(path.extension());
            std::ranges::transform(
                ext,
                ext.begin(),
                [](unsigned char ch)
                {
                    return static_cast<char>(std::tolower(ch));
                });
            return ext;
        }

        RecordingFiles recording_files(const fs::path& source)
        {
            RecordingFiles files;
            for (const auto& file : fs::directory_iterator(source))
            {
                const auto ext = extension(file.path());
                if (file.is_regular_file() &&
                    (ext == ".csv" || ext == ".las" || ext == ".laz" || ext == ".sn" || ext == ".json" || ext == ".mjc"))
                    files.emplace_back(file.path(), file.path().filename());
            }
            const auto murata = fs::path("MURATA_IMU");
            if (fs::is_directory(source / murata))
                for (const auto& file : fs::directory_iterator(source / murata))
                    if (file.is_regular_file() && extension(file.path()) == ".csv")
                        files.emplace_back(file.path(), murata / file.path().filename());
            std::ranges::sort(files);
            return files;
        }

    } // namespace

    bool CopyStep1Input(const fs::path& from, const fs::path& to, std::stop_token stop)
    {
        if (!fs::is_regular_file(from))
            throw std::invalid_argument("Expected an input file: " + sessionPathText(from));
        const auto time = fs::last_write_time(from);
        const auto size = fs::file_size(from);
        std::ifstream input(from, std::ios::binary);
        if (!input)
            throw std::runtime_error("Cannot open recording input: " + sessionPathText(from));
        std::ofstream output(to, std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        std::vector<char> buffer(256 * 1024);
        std::uintmax_t written = 0;
        while (!stop.stop_requested() && input)
        {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            output.write(buffer.data(), count);
            written += static_cast<std::uintmax_t>(count);
        }
        output.close();
        if (stop.stop_requested())
            return false;
        if (!input.eof() || input.bad() || written != size || fs::file_size(from) != size || fs::last_write_time(from) != time)
            throw std::runtime_error("Recording input changed or could not be read: " + sessionPathText(from));
        fs::last_write_time(to, time);
        return true;
    }

    RecordingCapture::RecordingCapture(
        TargetRef target, fs::path recording, fs::path toml, json settings, std::unique_ptr<CaptureSource> seed)
        : target_(std::move(target))
        , recording_(std::move(recording))
        , toml_(std::move(toml))
        , settings_(std::move(settings))
        , seed_(std::move(seed))
    {
    }

    TargetRef RecordingCapture::Target() const
    {
        return target_;
    }

    CaptureResult RecordingCapture::Materialize(const fs::path& directory, RunContext& context)
    {
        const auto cancelled = OperationError{ "cancelled", "Recording capture cancelled", json::object() };
        if (context.stop.stop_requested())
            return cancelled;
        if (!recording_.is_absolute() || !fs::is_directory(recording_) || (!toml_.empty() && !toml_.is_absolute()))
            return OperationError{ "invalid_input", "Expected an absolute recording directory and optional TOML path", json::object() };

        const auto files = recording_files(recording_);
        if (files.empty())
            return OperationError{ "invalid_input", "The recording does not contain Mandeye input files", json::object() };

        const auto data = directory / "recording";
        fs::create_directory(data);
        if (fs::is_directory(recording_ / "MURATA_IMU"))
            fs::create_directory(data / "MURATA_IMU");
        std::uint64_t copied = 0;
        for (const auto& [source, leaf] : files)
        {
            if (!CopyStep1Input(source, data / leaf, context.stop))
                return cancelled;
            ++copied;
            if (context.progress)
                context.progress({ "capture_recording", copied, files.size(), "Copying Mandeye recording" });
        }
        if (files != recording_files(recording_))
            return OperationError{ "invalid_input", "The recording file inventory changed during capture", json::object() };

        json entry{ { "directory", "recording" } };
        if (!toml_.empty())
        {
            if (!CopyStep1Input(toml_, directory / "parameters.toml", context.stop))
                return cancelled;
            entry["toml"] = "parameters.toml";
        }
        if (seed_)
        {
            const auto captured = CaptureSnapshot(*seed_, target_.revision, directory / "seed", context);
            if (const auto* failure = std::get_if<OperationError>(&captured))
                return *failure;
            entry["seed"] = "seed/snapshot.json";
        }
        std::ofstream output(directory / "recording.json", std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << entry;
        output.close();
        settings_["recording_directory"] = sessionPathText(recording_);
        return SnapshotContents{ "step1_recording", "recording.json", settings_, {} };
    }
} // namespace HDMapping::Processing
