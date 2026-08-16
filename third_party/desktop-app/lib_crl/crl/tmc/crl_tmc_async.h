// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <crl/common/crl_common_config.h>

#ifdef CRL_USE_TMC

#include <crl/common/crl_common_utils.h>
#include <crl/common/crl_common_sync.h>

#include <tmc/ex_cpu.hpp>
#include <tmc/sync.hpp>

namespace crl::details {

inline void ensure_tmc_cpu_executor_init() {
    // init() is idempotent, but doesn't block other callers while executing.
    // This static initializer protects against multiple execution, and
    // concurrent callers block here until it finishes, ensuring that the 
    // executor is fully initialized before post(). Subsequent calls are
    // just a cheap acquire-load of the guard variable.
    static int once = (tmc::cpu_executor().init(), 0);
    (void)once;
}

template <typename Callable>
inline void async_any(Callable &&callable) {
    ensure_tmc_cpu_executor_init();
    tmc::post(tmc::cpu_executor(), std::forward<Callable>(callable));
}

inline void async_plain(void (*callable)(void*), void *argument) {
    async_any([=] {
        callable(argument);
    });
}

} // namespace crl::details

namespace crl {

template <
    typename Callable,
    typename Return = decltype(std::declval<Callable>()())>
inline void async(Callable &&callable) {
    details::async_any(std::forward<Callable>(callable));
}

} // namespace crl

#endif // CRL_USE_TMC
