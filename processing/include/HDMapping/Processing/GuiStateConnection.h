#pragma once

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
        void Close();

    private:
        struct Connection;
        std::unique_ptr<Connection> connection_;
    };
} // namespace HDMapping::Processing
