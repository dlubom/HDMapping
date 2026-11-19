#include <HDMapping/Processing/GuiStateConnection.h>

#include "GuiRelayProcess.h"
#include <HDMapping/Processing/GuiStateBridge.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

namespace HDMapping::Processing
{
    namespace
    {
        using json = nlohmann::json;
        using namespace std::chrono_literals;
    } // namespace

    struct GuiStateConnection::Connection
    {
        GuiStateBridge& bridge;
        std::string name;
        std::mutex mutex;
        std::condition_variable ready;
        json states = json::array();
        std::deque<json> commands, replies;
        std::uint64_t generation = 0, acknowledged = 0;
        bool stopped = false, disconnected = false, closing = false;
        std::size_t pending = 0;
        std::thread worker;

        Connection(GuiStateBridge& owner, std::string_view executable)
            : bridge(owner)
            , name(executable)
            , worker(
                  [this]
                  {
                      run();
                  })
        {
        }

        void run()
        {
            std::unique_lock lock(mutex);
            while (!stopped)
            {
                lock.unlock();
                try
                {
                    GuiRelayProcess relay(name);
                    lock.lock();
                    while (!stopped)
                    {
                        json request{ { "wire", 1 }, { "version", HDMAPPING_GUI_VERSION }, { "states", states } };
                        const auto sending = generation;
                        const bool has_reply = !replies.empty();
                        if (has_reply)
                            request["reply"] = replies.front();
                        lock.unlock();
                        auto response = json::parse(relay.Exchange(request.dump()));
                        if (response.at("wire") != 1 || response.at("version") != HDMAPPING_GUI_VERSION || response.contains("error"))
                            throw std::runtime_error(response.value("error", "Incompatible GUI relay"));
                        lock.lock();
                        acknowledged = sending;
                        if (has_reply)
                        {
                            replies.pop_front();
                            --pending;
                        }
                        if (response.contains("command") && !response.at("command").is_null())
                        {
                            const auto& command = response.at("command");
                            if (!command.is_object() || command.value("id", std::string{}).empty() ||
                                command.value("method", std::string{}).empty() || !command.at("body").is_object())
                                throw std::runtime_error("Malformed GUI command");
                            if (commands.size() >= 65)
                                throw std::runtime_error("GUI command queue is full");
                            commands.push_back(command);
                            ++pending;
                        }
                        ready.notify_all();
                        ready.wait_for(
                            lock,
                            100ms,
                            [&]
                            {
                                return stopped || !replies.empty() || generation != sending;
                            });
                    }
                    lock.unlock();
                } catch (const std::exception& error)
                {
                    std::cerr << "HDMapping GUI bridge: " << error.what() << '\n';
                    if (!lock.owns_lock())
                        lock.lock();
                    disconnected = true;
                    ready.notify_all();
                    ready.wait(
                        lock,
                        [&]
                        {
                            return stopped || !disconnected;
                        });
                    if (!stopped)
                        ready.wait_for(
                            lock,
                            2s,
                            [&]
                            {
                                return stopped;
                            });
                    lock.unlock();
                }
                lock.lock();
            }
        }

        void poll()
        {
            std::unique_lock lock(mutex);
            if (disconnected)
            {
                lock.unlock();
                bridge.Disconnect();
                lock.lock();
                commands.clear();
                replies.clear();
                pending = 0;
                if (closing)
                    stopped = true;
                disconnected = false;
                ready.notify_all();
            }

            const auto target = bridge.Target();
            json visible = json::array();
            if (!closing)
                visible.push_back({ { "state_id", target.state_id }, { "revision", target.revision } });
            if (visible != states)
            {
                states = std::move(visible);
                ++generation;
                ready.notify_all();
            }

            auto batch = std::move(commands);
            commands.clear();
            lock.unlock();
            for (const auto& command : batch)
            {
                const auto id = command.at("id").get<std::string>();
                bridge.Dispatch(
                    command.at("method").get<std::string>(),
                    command.at("body"),
                    [this, id](std::variant<json, OperationError> result)
                    {
                        json reply{ { "id", id } };
                        if (auto* e = std::get_if<OperationError>(&result))
                            reply["error"] = e->code + ": " + e->message;
                        else
                            reply["body"] = std::get<json>(std::move(result));
                        std::lock_guard guard(mutex);
                        replies.push_back(std::move(reply));
                        ready.notify_all();
                    });
            }
        }

        void close()
        {
            if (!worker.joinable())
                return;
            bridge.Close();
            closing = true;
            for (;;)
            {
                poll();
                std::unique_lock lock(mutex);
                if (stopped || (!bridge.Preparing() && pending == 0 && acknowledged == generation))
                {
                    stopped = true;
                    ready.notify_all();
                    break;
                }
                ready.wait_for(lock, 25ms);
            }
            worker.join();
            bridge.Disconnect();
        }
    };

    GuiStateConnection::GuiStateConnection(GuiStateBridge& bridge, std::string_view executable_name)
        : connection_(std::make_unique<Connection>(bridge, executable_name))
    {
    }
    GuiStateConnection::~GuiStateConnection()
    {
        Close();
    }
    void GuiStateConnection::Poll()
    {
        connection_->poll();
    }
    void GuiStateConnection::Close()
    {
        connection_->close();
    }
} // namespace HDMapping::Processing
