#include <doctest.h>

#include <HDMapping/Processing/RuntimeProtocol.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace HDMapping::Processing;
using nlohmann::json;

namespace
{
    const WorkerIdentity identity{ "0.106.0", 300 };
    const std::vector<OperationDescriptor> test_operations{
        { "test-operation", { { "type", "object" } }, { { "type", "object" } }, { { "type", "object" } }, OperationClass::Heavy }
    };

    const std::string run_command =
        R"({"type":"run","job_id":"test-job","operation":{"operation":"test-operation","target":{"state_id":"open","revision":42},"scope":{},"parameters":{}},"snapshot":{"manifest_path":"wejście danych","target":{"state_id":"open","revision":42}}})"
        "\n";

    std::vector<json> run_worker(
        const std::string& input, const std::vector<OperationDescriptor>& operations = {}, const OperationExecutor& executor = {})
    {
        std::vector<json> packets;
        std::istringstream stream(input);
        ServeWorker(
            stream,
            [&](std::string_view line)
            {
                packets.push_back(json::parse(line));
            },
            identity,
            {},
            operations,
            executor);
        return packets;
    }
} // namespace

TEST_CASE("Describe lists exactly the capabilities of this worker build")
{
    const auto packets = run_worker("{\"type\":\"describe\"}\n", test_operations);
    REQUIRE(packets.size() == 2);
    CHECK(packets[0] == json{ { "type", "hello" }, { "wire", 1 }, { "version", "0.106.0" }, { "pid", 300 } });
    CHECK(packets[1].at("type") == "operations");
    REQUIRE(packets[1].at("operations").size() == 1);
    CHECK(packets[1].at("operations").at(0).at("id") == "test-operation");
}

TEST_CASE("Unknown commands and malformed frames produce explicit protocol errors")
{
    const std::array cases{ std::pair{ std::string("{\"type\":\"wrong\"}\n"), "unsupported_command" },
                            std::pair{ std::string("{\"type\":"), "invalid_request" },
                            std::pair{ std::string("{\"type\":\"describe\"}"), "invalid_request" },
                            std::pair{ std::string("[]\n"), "invalid_request" },
                            std::pair{ run_command, "unsupported_operation" } };
    for (const auto& [frame, expected] : cases)
    {
        CAPTURE(expected);
        const auto packets = run_worker(frame);
        REQUIRE(packets.size() == 2);
        CHECK(packets[1].at("type") == "error");
        CHECK(packets[1].at("error").at("code") == expected);
    }
}

TEST_CASE("Worker enforces the byte limit on valid JSON packets")
{
    constexpr std::size_t byte_limit = 1024 * 1024;
    const std::string prefix = R"({"type":"describe","padding":")";
    const std::string suffix = "\"}\n";
    const auto within_limit = prefix + std::string(byte_limit - prefix.size() - suffix.size() + 1, 'x') + suffix;
    const auto accepted = run_worker(within_limit);
    REQUIRE(accepted.size() == 2);
    CHECK(accepted[1].at("type") == "operations");

    const auto over_limit = prefix + "x" + within_limit.substr(prefix.size());
    const auto rejected = run_worker(over_limit);
    REQUIRE(rejected.size() == 2);
    CHECK(rejected[1].at("error").at("code") == "invalid_request");
}

TEST_CASE("Cancel and supervisor disconnect stop an active snapshot operation")
{
    for (const std::string_view control : { std::string_view("{\"type\":\"cancel\"}\n"), std::string_view{} })
    {
        bool input_received = false;
        bool operation_stopped = false;
        const auto packets = run_worker(
            run_command + std::string(control),
            test_operations,
            [&](const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context) -> RunResult
            {
                input_received = request.target == TargetRef{ "open", 42 } && snapshot.manifest_path.u8string() == u8"wejście danych";
                context.progress({ "waiting", 2, 3, "Waiting for stop" });
                std::mutex mutex;
                std::unique_lock lock(mutex);
                std::condition_variable_any stopped;
                stopped.wait_for(
                    lock,
                    context.stop,
                    std::chrono::seconds(5),
                    []
                    {
                        return false;
                    });
                operation_stopped = context.stop.stop_requested();
                return OperationError{ "stopped_operation", "Operation returned after stop", json::object() };
            });
        CHECK(input_received);
        CHECK(operation_stopped);
        REQUIRE(packets.size() == 3);
        CHECK(packets[1].at("progress").at("stage") == "waiting");
        CHECK(packets[1].at("progress").at("current") == 2);
        CHECK(packets.back().at("error").at("code") == "cancelled");
    }
}

namespace
{
    //! The supervisor leaves input open until it sees the worker's terminal packet.
    class SupervisorInput : public std::streambuf
    {
    public:
        explicit SupervisorInput(std::string command)
            : command_(std::move(command))
        {
            setg(command_.data(), command_.data(), command_.data() + command_.size());
        }

        void close()
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            changed_.notify_all();
        }

    protected:
        int_type underflow() override
        {
            std::unique_lock lock(mutex_);
            changed_.wait_for(
                lock,
                std::chrono::seconds(5),
                [&]
                {
                    return closed_;
                });
            return traits_type::eof();
        }

    private:
        std::string command_;
        std::mutex mutex_;
        std::condition_variable changed_;
        bool closed_ = false;
    };

    //! Remove the test's private directory even if an assertion throws.
    class TemporaryDirectory
    {
    public:
        TemporaryDirectory()
        {
            const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            const std::u8string name = std::u8string(u8"HDMapping wynik źródłowy ") + std::u8string(suffix.begin(), suffix.end());
            path = std::filesystem::temp_directory_path() / name;
            if (!std::filesystem::create_directory(path))
            {
                throw std::runtime_error("Test output directory already exists");
            }
        }

        ~TemporaryDirectory()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        std::filesystem::path path;
    };
} // namespace

TEST_CASE("Completion follows an atomically published receipt")
{
    const TemporaryDirectory directory;
    SupervisorInput input(run_command);
    std::istream stream(&input);
    std::vector<json> packets;
    bool published_before_completion = false;
    {
        std::ofstream manifest(directory.path / "snapshot.json");
        manifest << "{}";
        manifest.close();
        REQUIRE(manifest);
    }
    ServeWorker(
        stream,
        [&](std::string_view line)
        {
            const auto packet = json::parse(line);
            packets.push_back(packet);
            if (packet.at("type") == "completed")
            {
                published_before_completion = std::filesystem::is_regular_file(directory.path / "result.json");
                input.close();
            }
            if (packet.at("type") == "error")
            {
                input.close();
            }
        },
        identity,
        directory.path,
        test_operations,
        [](const OperationRequest& request, const SnapshotRef&, RunContext&) -> RunResult
        {
            return ArtifactManifest{ "output-id", request.target, { "snapshot.json", { "new-state", 0 } }, { "snapshot.json" }, {} };
        });
    REQUIRE(packets.size() == 2);
    CHECK(packets[1].at("type") == "completed");
    REQUIRE(published_before_completion);
    CHECK_FALSE(std::filesystem::exists(directory.path / "result.json.tmp"));
    std::ifstream result(directory.path / "result.json");
    const auto receipt = json::parse(result);
    CHECK(receipt.at("job_id") == "test-job");
    CHECK(receipt.at("version") == "0.106.0");
    CHECK(receipt.at("wire") == 1);
    CHECK(receipt.at("manifest").at("artifact_id") == "output-id");
    CHECK(receipt.at("manifest").at("target") == json{ { "state_id", "open" }, { "revision", 42 } });
    CHECK(receipt.at("manifest").at("snapshot").at("manifest_path") == "snapshot.json");
    CHECK(receipt.at("manifest").at("files") == json::array({ "snapshot.json" }));
}

TEST_CASE("A late cancellation preserves an already committed result")
{
    const TemporaryDirectory directory;
    std::ofstream(directory.path / "snapshot.json") << "{}";
    SupervisorInput input(run_command);
    std::istream stream(&input);
    std::vector<json> packets;
    ServeWorker(
        stream,
        [&](std::string_view line)
        {
            const auto packet = json::parse(line);
            if (packet.at("type") == "progress")
                input.close();
            packets.push_back(packet);
        },
        identity,
        directory.path,
        test_operations,
        [](const OperationRequest& request, const SnapshotRef&, RunContext& context) -> RunResult
        {
            context.progress({ "committed", {}, {}, {} });
            std::mutex mutex;
            std::unique_lock lock(mutex);
            std::condition_variable_any stopped;
            stopped.wait_for(
                lock,
                context.stop,
                std::chrono::seconds(5),
                []
                {
                    return false;
                });
            if (!context.stop.stop_requested())
                return OperationError{ "timeout", "Test did not observe cancellation", json::object() };
            return ArtifactManifest{ "output-id", request.target, { "snapshot.json", request.target }, { "snapshot.json" }, {} };
        });
    REQUIRE(packets.size() == 3);
    CHECK(packets.back().at("type") == "completed");
    CHECK(std::filesystem::is_regular_file(directory.path / "result.json"));
}
