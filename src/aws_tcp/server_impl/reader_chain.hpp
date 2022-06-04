#pragma once

#include <aws_tcp/server_impl/asio_declarations.hpp>
#include <gsl/gsl>
#include <aws_tcp/util/logger_t.hpp>
#include <aws_tcp/util/oneshot_wait.hpp>
#include <memory>
#include <span>
#include <array>
#include <string_view>
#include <functional>
#include <cstddef>

namespace tcp {
    class reader_chain : public std::enable_shared_from_this<reader_chain> {
    public:
        using handle_id_t = long long;

        enum class drop_e : bool {
            keep_client = false,
            drop_client = true,
        };

        static std::shared_ptr <reader_chain> New(std::shared_ptr<strand_t> strand,
                                                  std::shared_ptr<asio::ip::tcp::socket> socket,
                                                  std::function<drop_e(std::span<const std::byte>)> callback_receive,
                                                  std::function<void()> callback_closed,
                                                  handle_id_t id,
                                                  util::logger_t logger);

        void stop();

    private:
        gsl::not_null <std::shared_ptr<strand_t>> m_strand;
        gsl::not_null <std::shared_ptr<asio::ip::tcp::socket>> m_socket;
        enum class socket_state_e : int {
            active = 1,
            shutdown,
            closed,
        };
        socket_state_e m_socket_state{socket_state_e::active};
        util::oneshot_wait m_sync_close_socket{};
        asio::ip::tcp::endpoint m_remote_endpoint{};
        gsl::not_null<std::unique_ptr<std::array<std::byte, 1024>>> m_buffer{
            std::make_unique<std::array<std::byte, 1024>>()
        };
        gsl::not_null<std::function<drop_e(std::span<const std::byte>)>> m_callback_receive;
        gsl::not_null<std::function<void()>> m_callback_closed;
        handle_id_t m_id{};
        util::logger_t m_logger;

        void report_error(std::string_view msg);

        void close_socket();
        void on_close();

        explicit reader_chain(std::shared_ptr <strand_t> strand,
                              std::shared_ptr <asio::ip::tcp::socket> socket,
                              std::function<drop_e(std::span<const std::byte>)> callback_receive,
                              std::function<void()> callback_closed,
                              handle_id_t id,
                              util::logger_t logger);

        void reading_chain();
    };
}
