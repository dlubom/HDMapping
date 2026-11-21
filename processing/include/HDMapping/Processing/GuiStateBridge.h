#pragma once

#include <HDMapping/Processing/SnapshotStore.h>

#include <atomic>
#include <map>
#include <memory>
#include <string_view>
#include <thread>

namespace HDMapping::Processing
{
    //! Window-thread adapters. Mutations and result application commit only after successful validation.
    struct GuiStateCallbacks
    {
        std::function<StateSummary()> DescribeState;
        std::function<MutationResult(const OperationRequest&)> ApplyMutation;
        //! Pin the current input on the window thread. Materialize and destruction may run on a worker.
        std::function<std::unique_ptr<CaptureSource>(TargetRef)> PinCapture;
        std::function<std::optional<OperationError>(const ArtifactManifest&)> ApplyResult;
        //! Pin operation-specific inputs on the window thread when supported by this application.
        std::function<std::unique_ptr<CaptureSource>(const OperationRequest&)> PinOperationCapture = {};
        //! Present a verified read-only result without advancing the workspace revision.
        std::function<std::optional<OperationError>(const ArtifactManifest&)> PresentLoopCandidates = {};
    };

    using GuiResponse = std::variant<nlohmann::json, OperationError>;
    using GuiReply = std::function<void(GuiResponse)>;

    //! One GUI opening. All public methods belong to the window thread; a capture write replies on its worker.
    class GuiStateBridge
    {
    public:
        explicit GuiStateBridge(GuiStateCallbacks callbacks);
        ~GuiStateBridge();
        GuiStateBridge(const GuiStateBridge&) = delete;
        GuiStateBridge& operator=(const GuiStateBridge&) = delete;

        TargetRef Target() const;
        //! A new opening receives a new identity. Save As and camera changes leave it untouched.
        void Open();
        void Close();
        void Changed();
        bool Reserved() const;
        bool Preparing() const;
        //! Cancel and join capture workers on transport loss. Keep the current opening available for reconnection.
        void Disconnect();
        void Dispatch(std::string_view method, const nlohmann::json& params, GuiReply reply);

    private:
        struct Reservation
        {
            TargetRef target;
            bool mutate{};
            std::unique_ptr<CaptureSource> source;
            std::atomic<bool> transferred{};
            std::jthread worker;
        };
        std::optional<GuiResponse> Handle(std::string_view method, const nlohmann::json& params, const GuiReply& reply);
        std::optional<GuiResponse> Capture(const nlohmann::json& params, const TargetRef& target, const GuiReply& reply);
        GuiStateCallbacks callbacks_;
        const std::thread::id owner_;
        TargetRef target_;
        bool opened_{};
        std::map<std::string, Reservation> reservations_;
    };
} // namespace HDMapping::Processing
