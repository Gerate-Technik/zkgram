// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <crl/common/crl_common_config.h>

// crl::semaphore is a lightweight wake/notify primitive.
//
// Everywhere except Apple it is a single std::atomic-based saturating event
// (see crl_common_semaphore.h), which lowers to the lightest OS wait
// primitive available: WaitOnAddress on Windows 8+, futex on Linux. On older
// Windows (7) the MSVC runtime transparently falls back to a wait table - it
// still compiles and runs.
//
// On Apple we keep a dispatch_semaphore backend: std::atomic<>::wait and
// std::counting_semaphore are availability-gated to macOS 11.0 in libc++
// (the wait/notify machinery lives in the dylib), while we still target
// macOS 10.13+. dispatch_semaphore is available since macOS 10.6.
#if defined CRL_USE_DISPATCH
#include <crl/dispatch/crl_dispatch_semaphore.h>
#else // CRL_USE_DISPATCH
#include <crl/common/crl_common_semaphore.h>
#endif // !CRL_USE_DISPATCH
