#pragma once

#include <gsl/gsl>
#include <functional>
#include <string_view>
#include <exception>

namespace tcp::util {
    class logger_t {
        gsl::not_null<std::function<void(std::string_view)>> m_callback_errors;
    public:
        explicit logger_t(std::function<void(std::string_view)> callback_errors)
                : m_callback_errors{std::move(callback_errors)} {}

        void log_error(const std::string_view err) noexcept {
            try {
                this->m_callback_errors.get()(err);
            }
            catch (std::exception &e) {
                // Nothing to do if logging fails
                static_cast<void>(e);
            }
        }
    };
}
