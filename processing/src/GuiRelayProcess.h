#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace HDMapping::Processing
{
    //! Owns a private stdio connection to the installed sibling MCP relay.
    class GuiRelayProcess
    {
    public:
        enum class Mode
        {
            State,
            Jobs
        };
        explicit GuiRelayProcess(std::string_view executable_name, Mode mode = Mode::State);
        ~GuiRelayProcess();
        std::string Exchange(std::string frame);

    private:
        struct Process;
        std::unique_ptr<Process> process_;
        std::mutex mutex_;
        std::condition_variable_any changed_;
        std::optional<std::chrono::steady_clock::time_point> deadline_;
        std::jthread watchdog_;
    };
} // namespace HDMapping::Processing
