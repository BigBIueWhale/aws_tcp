#pragma once

#include <concepts>
#include <vector>
#include <mutex>

namespace tcp::util {
    template<typename elem_T>
    class thread_safe_vec {
    protected:
        std::mutex m_mtx{};
        std::vector <elem_T> m_vec{};
    public:
        using value_type = elem_T;

        template<typename T>
        requires std::convertible_to<T, elem_T>
        void push_back(T &&elem) &{
            const std::lock_guard <std::mutex> lckr{this->m_mtx};
            this->m_vec.emplace_back(std::forward<T>(elem));
        }

        std::vector <elem_T> embezzle() &{
            std::vector <elem_T> ret{};
            {
                const std::lock_guard <std::mutex> lckr{this->m_mtx};
                swap(ret, this->m_vec);
            }
            return ret;
        }
    };
}
