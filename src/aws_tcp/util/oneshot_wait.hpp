#pragma once

#include <mutex>
#include <condition_variable>

namespace tcp::util {
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
            std::unique_lock<std::mutex> lckr{this->m_mtx};
            if (this->m_already_done) {
                return;
            }
            const auto was_closed = [this]() -> bool { return this->m_already_done; };
            this->m_cv.wait(lckr, was_closed);
        }
    };
}
