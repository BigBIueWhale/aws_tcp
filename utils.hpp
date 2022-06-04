#pragma once

#include <boost/current_function.hpp>
#include <gsl/gsl>
#include <concepts>
#include <exception>
#include <utility>
#include <string_view>
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <functional>

template <typename callable_T>
concept function_like_object_with_side_effects = requires(callable_T callable) {
    { callable() } -> std::same_as<void>;
};

template <typename callable_T>
concept error_handling_object = requires(callable_T callable) {
    { callable(std::string{}) } -> std::same_as<void>;
};

enum class exit_e : int {
    scope = 1,
    failure,
    success
};

template <exit_e exit_behaviour,
          function_like_object_with_side_effects action_T,
          error_handling_object error_handling_T>
class scope_exit {
protected:
    action_T m_action;
    error_handling_T m_error_handling;
    int m_exceptions_count{};
    [[nodiscard]] constexpr int get_global_uncaught_exceptions() const {
        if constexpr (exit_behaviour == exit_e::scope) {
            return 0;
        }
        else {
            return std::uncaught_exceptions();
        }
    }
public:
    scope_exit() = delete;
    template <function_like_object_with_side_effects perfect_forwarding_action_T,
            error_handling_object perfect_forwarding_logger_T>
    constexpr explicit scope_exit(perfect_forwarding_action_T&& action,
                        perfect_forwarding_logger_T&& error_handling)
            : m_action{ std::forward<perfect_forwarding_action_T>(action) },
              m_error_handling{ std::forward<perfect_forwarding_logger_T>(error_handling) },
              m_exceptions_count{ this->get_global_uncaught_exceptions() }
    {}
    ~scope_exit() noexcept {
        try {
            const int prev = this->m_exceptions_count;
            const int curr = this->get_global_uncaught_exceptions();
            bool run_func = true;
            if constexpr (exit_behaviour == exit_e::failure) {
                run_func = curr > prev;
            }
            else if constexpr (exit_behaviour == exit_e::success) {
                run_func = curr <= prev;
            }
            if (run_func) {
                this->m_action();
            }
        }
        catch (std::exception& e) {
            std::ostringstream err_msg;
            err_msg << "Can\'t throw exception from: " << BOOST_CURRENT_FUNCTION
                    << " Error message: " << e.what() << "\n";
            try {
                m_error_handling(err_msg.str());
            }
            catch (std::exception& e) {
                static_cast<void>(e);
            }
        }
    }
};

template <exit_e exit_behaviour,
          function_like_object_with_side_effects perfect_forwarding_action_T,
          error_handling_object perfect_forwarding_logger_T>
[[nodiscard]] constexpr auto make_scope_exit(perfect_forwarding_action_T&& action,
                               perfect_forwarding_logger_T&& error_handling)
{
    return scope_exit<
            exit_behaviour,
            std::remove_cvref_t<perfect_forwarding_action_T>,
            std::remove_cvref_t<perfect_forwarding_logger_T>>{
        std::forward<perfect_forwarding_action_T>(action),
        std::forward<perfect_forwarding_logger_T>(error_handling)
    };
}

template <typename elem_T>
class thread_safe_vec {
protected:
    std::mutex m_mtx{};
    std::vector<elem_T> m_vec{};
public:
    using value_type = elem_T;
    template <typename T> requires std::convertible_to<T, elem_T>
    void push_back(T&& elem) & {
        const std::lock_guard<std::mutex> lckr{ this->m_mtx };
        this->m_vec.emplace_back(std::forward<T>(elem));
    }
    std::vector<elem_T> embezzle() & {
        std::vector<elem_T> ret{};
        {
            const std::lock_guard<std::mutex> lckr{this->m_mtx};
            swap(ret, this->m_vec);
        }
        return ret;
    }
};

class oneshot_wait {
    std::condition_variable m_cv{};
    std::mutex m_mtx{};
    bool m_already_done = false;
public:
    void done() {
        std::lock_guard<std::mutex> lckr(this->m_mtx);
        if (!this->m_already_done) {
            this->m_already_done = true;
            this->m_cv.notify_all();
        }
    }
    void wait_until_done() {
        std::unique_lock<std::mutex> lckr{ this->m_mtx };
        if (this->m_already_done) {
            return;
        }
        const auto was_closed = [this]() -> bool { return this->m_already_done; };
        this->m_cv.wait(lckr, was_closed);
    }
};

class logger_t {
    gsl::not_null<std::function<void(std::string_view)>> m_callback_errors;
public:
    explicit logger_t(std::function<void(std::string_view)> callback_errors)
        : m_callback_errors{ std::move(callback_errors) }
    {}
    void log_error(const std::string_view err) noexcept {
        try {
            this->m_callback_errors.get()(err);
        }
        catch (std::exception& e) {
            // Nothing to do if logging fails
            static_cast<void>(e);
        }
    }
};
