#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/error.hpp>

namespace tcp {
    namespace asio = boost::asio;
    using strand_t = asio::strand<asio::io_context::executor_type>;
}
