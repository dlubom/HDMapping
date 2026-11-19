#pragma once

#include <HDMapping/Processing/Operation.h>

#include <memory>
#include <string_view>

namespace HDMapping::Processing
{
    class GuiStateBridge;

    //! Poll on the GUI thread; close after hiding the window and before releasing its data.
    class GuiStateConnection
    {
    public:
        GuiStateConnection(GuiStateBridge& bridge, std::string_view executable_name);
        ~GuiStateConnection();
        void Poll();
        //! One request at a time. Poll the state bridge independently while waiting for its reply.
        bool RequestJob(std::string_view method, nlohmann::json body);
        std::optional<std::variant<nlohmann::json, OperationError>> TakeJobReply();
        void Close();

    private:
        struct Connection;
        std::unique_ptr<Connection> connection_;
        struct JobConnection;
        std::unique_ptr<JobConnection> jobs_;
    };
} // namespace HDMapping::Processing
