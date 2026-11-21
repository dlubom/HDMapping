#include <HDMapping/Processing/GuiStateBridge.h>
#include <HDMapping/Processing/ResultPublisher.h>
#include <HDMapping/SessionPaths.h>

#include "RuntimeJson.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace HDMapping::Processing
{
    namespace fs = std::filesystem;
    using nlohmann::json;
    using namespace RuntimeJson;

    namespace
    {
        OperationError error(std::string code, std::string message)
        {
            return { std::move(code), std::move(message), json::object() };
        }

        json summarize(const StateSummary& summary, const TargetRef& target)
        {
            return { { "target", target_to_json(target) }, { "parameters", summary.parameters }, { "metadata", summary.metadata } };
        }

        fs::path required_path(const json& value)
        {
            const auto& text = value.get_ref<const std::string&>();
            const auto path = sessionPathFromText(text);
            if (!path.is_absolute() || text.find('\0') != std::string::npos)
            {
                throw std::invalid_argument("Expected an absolute filesystem path");
            }
            return path;
        }

        RunResult read_result(const json& params, const TargetRef& expected, bool accepts_odometry, bool accepts_candidates)
        {
            const auto path = required_path(params.at("manifest_path"));
            if (path.filename() != "result.json" || fs::file_size(path) > 1024 * 1024)
            {
                return error("invalid_published_result", "Invalid worker receipt");
            }
            std::ifstream input(path, std::ios::binary);
            const auto receipt = json::parse(input);
            if (receipt.at("job_id") != params.at("job_id") || receipt.at("wire") != 1 || receipt.at("version") != HDMAPPING_GUI_VERSION)
            {
                return error("invalid_published_result", "Worker receipt identity mismatch");
            }
            auto manifest = receipt.at("manifest");
            const auto root = fs::canonical(path.parent_path());
            const auto owned = [&root](json& value)
            {
                const auto& text = value.get_ref<const std::string&>();
                if (text.empty() || text.find('\0') != std::string::npos)
                {
                    throw std::invalid_argument("Invalid path in result inventory");
                }
                const auto absolute = fs::canonical(root / sessionPathFromText(text));
                const auto relative = absolute.lexically_relative(root);
                if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
                {
                    throw std::invalid_argument("Published artifact is outside the job directory");
                }
                value = path_to_utf8(absolute);
                return absolute;
            };
            const auto normalize_inventory = [&owned](json& inventory)
            {
                const auto snapshot = owned(inventory.at("snapshot").at("manifest_path"));
                auto& files = inventory.at("files");
                if (!files.is_array())
                {
                    throw std::invalid_argument("Expected an array of files in the result inventory");
                }
                for (auto& file : files)
                {
                    owned(file);
                }
                return snapshot;
            };
            const auto snapshot = normalize_inventory(manifest);
            // The Step 1 adapter validates and restores its private prepared snapshot before changing the opening.
            const bool prepared = accepts_odometry && snapshot.parent_path() == root / "prepared";
            const bool candidates = accepts_candidates && snapshot.parent_path() == root / "loop-candidates";
            if ((prepared || candidates) && snapshot.filename() == "snapshot.json")
            {
                const auto target = parse_target(manifest.at("target"));
                const auto snapshot_target = parse_target(manifest.at("snapshot").at("target"));
                const auto id = manifest.at("artifact_id").get<std::string>();
                if (target != expected || snapshot_target != expected || id.empty() || !manifest.at("dependencies").empty())
                    return error("invalid_published_result", "Invalid private result receipt");
                std::vector<fs::path> files;
                for (const auto& file : manifest.at("files"))
                    files.push_back(sessionPathFromText(file.get_ref<const std::string&>()));
                return ArtifactManifest{ id, target, { snapshot, snapshot_target }, std::move(files), {} };
            }
            auto result = ReadPublishedGeneration(snapshot.parent_path().parent_path(), manifest.at("artifact_id").get<std::string>());
            if (const auto* artifact = std::get_if<ArtifactManifest>(&result))
            {
                auto published = manifest_to_json(*artifact);
                normalize_inventory(published);
                if (artifact->target != expected || published != manifest)
                {
                    return error("invalid_published_result", "Published artifact differs from the worker receipt or target");
                }
            }
            return result;
        }

        GuiResponse write_capture(CaptureSource& source, TargetRef target, const fs::path& directory, std::stop_token stop)
        {
            try
            {
                RunContext context{ directory, stop, {} };
                auto result = CaptureSnapshot(source, target.revision, directory, context);
                if (const auto* failure = std::get_if<OperationError>(&result))
                {
                    return *failure;
                }
                const auto& snapshot = std::get<SnapshotRef>(result);
                return json{ { "manifest_path", path_to_utf8(snapshot.manifest_path) }, { "target", target_to_json(snapshot.target) } };
            } catch (const std::exception& exception)
            {
                return error("capture_failed", exception.what());
            }
        }
    } // namespace

    GuiStateBridge::GuiStateBridge(GuiStateCallbacks callbacks)
        : callbacks_(std::move(callbacks))
        , owner_(std::this_thread::get_id())
    {
        Open();
    }

    GuiStateBridge::~GuiStateBridge()
    {
        for (auto& [id, reservation] : reservations_)
        {
            if (reservation.worker.joinable())
            {
                reservation.worker.join();
            }
        }
    }

    TargetRef GuiStateBridge::Target() const
    {
        return target_;
    }

    void GuiStateBridge::Open()
    {
        if (Reserved())
        {
            throw std::logic_error("Cannot replace reserved GUI data");
        }
        std::random_device random;
        std::ostringstream id;
        id << std::hex << std::setfill('0');
        for (int i = 0; i < 4; ++i)
        {
            id << std::setw(8) << random();
        }
        target_ = { id.str(), 1 };
        opened_ = true;
    }

    void GuiStateBridge::Close()
    {
        opened_ = false;
    }

    void GuiStateBridge::Changed()
    {
        ++target_.revision;
    }

    bool GuiStateBridge::Reserved() const
    {
        return std::ranges::any_of(
            reservations_,
            [](const auto& entry)
            {
                return entry.second.mutate || !entry.second.transferred.load();
            });
    }

    bool GuiStateBridge::Preparing() const
    {
        return std::ranges::any_of(
            reservations_,
            [](const auto& entry)
            {
                return !entry.second.transferred.load();
            });
    }

    void GuiStateBridge::Disconnect()
    {
        for (auto& [id, reservation] : reservations_)
        {
            reservation.worker.request_stop();
        }
        reservations_.clear();
    }

    void GuiStateBridge::Dispatch(std::string_view method, const json& params, GuiReply reply)
    {
        if (std::this_thread::get_id() != owner_)
        {
            reply(error("wrong_thread", "GUI commands require the window thread"));
            return;
        }
        std::optional<GuiResponse> response;
        try
        {
            response = Handle(method, params, reply);
        } catch (const std::exception& exception)
        {
            response = error("invalid_request", exception.what());
        }
        if (response)
        {
            reply(std::move(*response));
        }
    }

    std::optional<GuiResponse> GuiStateBridge::Handle(std::string_view method, const json& params, const GuiReply& reply)
    {
        if (method == "states.list")
        {
            auto states = json::array();
            if (opened_)
            {
                states.push_back(target_to_json(target_));
            }
            return json{ { "states", std::move(states) } };
        }
        const auto target = parse_target(params.at("target"));
        if (method == "states.capture")
        {
            return Capture(params, target, reply);
        }
        if (!opened_ || target.state_id != target_.state_id)
        {
            return error("state_closed", "The named opening is unavailable");
        }
        if (target.revision != target_.revision)
        {
            return error("stale_revision", "The opening has changed");
        }
        if (method == "states.describe")
        {
            return summarize(callbacks_.DescribeState(), target_);
        }
        if (method == "states.execute")
        {
            if (Reserved())
            {
                return error("state_busy", "The opening is reserved");
            }
            OperationRequest request{ params.at("operation").get<std::string>(), target, params.at("scope"), params.at("parameters") };
            if (request.operation.empty() || !request.scope.is_object() || !request.parameters.is_object())
            {
                return error("invalid_request", "Invalid operation, parameters, or scope");
            }
            auto result = callbacks_.ApplyMutation(request);
            if (const auto* failure = std::get_if<OperationError>(&result))
            {
                return *failure;
            }
            Changed();
            return summarize(std::get<StateSummary>(result), target_);
        }
        if (method == "states.apply_result")
        {
            const auto id = params.at("job_id").get<std::string>();
            for (const auto& [job, reservation] : reservations_)
            {
                if (!reservation.transferred.load() || (reservation.mutate && job != id))
                {
                    return error("state_busy", "A conflicting reservation is active");
                }
            }
            const bool presents_candidates = static_cast<bool>(callbacks_.PresentLoopCandidates);
            auto result = read_result(params, target, static_cast<bool>(callbacks_.PinOperationCapture), presents_candidates);
            if (const auto* failure = std::get_if<OperationError>(&result))
            {
                return *failure;
            }
            const auto& artifact = std::get<ArtifactManifest>(result);
            if (presents_candidates && artifact.snapshot.manifest_path.parent_path().filename() == "loop-candidates")
            {
                if (auto failure = callbacks_.PresentLoopCandidates(artifact))
                    return *failure;
                return json::object();
            }
            if (auto failure = callbacks_.ApplyResult(artifact))
            {
                return *failure;
            }
            Changed();
            return json::object();
        }
        return error("unsupported_operation", "Unknown GUI command");
    }

    std::optional<GuiResponse> GuiStateBridge::Capture(const json& params, const TargetRef& target, const GuiReply& reply)
    {
        const auto job = params.at("job_id").get<std::string>();
        const auto phase = params.at("phase").get<std::string>();
        if (job.empty())
        {
            return error("invalid_request", "Missing capture job ID");
        }
        auto existing = reservations_.find(job);
        if (phase == "reserve")
        {
            if (!opened_ || target.state_id != target_.state_id)
            {
                return error("state_closed", "The named opening is unavailable");
            }
            if (target.revision != target_.revision)
            {
                return error("stale_revision", "The opening has changed");
            }
            const auto access = params.at("access").get<std::string>();
            if (access != "read" && access != "mutate")
            {
                return error("invalid_request", "Unknown reservation access");
            }
            if (existing != reservations_.end() || reservations_.size() >= 64 ||
                std::ranges::any_of(
                    reservations_,
                    [&](const auto& entry)
                    {
                        return entry.second.mutate || (access == "mutate" && !entry.second.transferred.load());
                    }))
            {
                return error("state_busy", "Conflicting capture reservation");
            }
            std::unique_ptr<CaptureSource> source;
            if (callbacks_.PinOperationCapture && params.contains("operation"))
            {
                const auto& operation = params.at("operation");
                if (parse_target(operation.at("target")) != target || !operation.at("scope").is_object() ||
                    !operation.at("parameters").is_object())
                    return error("invalid_request", "Invalid operation for capture");
                source = callbacks_.PinOperationCapture(
                    { operation.at("operation").get<std::string>(), target, operation.at("scope"), operation.at("parameters") });
            }
            else
                source = callbacks_.PinCapture(target);
            if (!source || source->Target() != target)
            {
                return error("capture_failed", "The opening could not pin its data");
            }
            auto& reservation = reservations_[job];
            reservation.target = target;
            reservation.mutate = access == "mutate";
            reservation.source = std::move(source);
            return json::object();
        }
        if (existing == reservations_.end() || existing->second.target != target)
        {
            return error("reservation_missing", "Capture requires a matching reservation");
        }
        auto& reservation = existing->second;
        if (phase == "release")
        {
            reservation.worker.request_stop();
            reservations_.erase(existing);
            return json::object();
        }
        if (phase != "write" || reservation.worker.joinable())
        {
            return error("invalid_request", "Invalid or repeated capture phase");
        }
        const auto directory = required_path(params.at("directory"));
        reservation.worker = std::jthread(
            [&reservation, directory, target, reply](std::stop_token stop)
            {
                auto response = write_capture(*reservation.source, target, directory, stop);
                reservation.source.reset();
                reservation.transferred.store(true);
                reply(std::move(response));
            });
        return std::nullopt;
    }
} // namespace HDMapping::Processing
