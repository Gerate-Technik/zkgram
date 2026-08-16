// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <crl/common/crl_common_config.h>

#include <atomic>

namespace crl {

// A lightweight wake/notify primitive: a saturating, auto-reset event with
// a single waiter. release() raises the signal and is coalescing - repeated
// release() calls before the waiter consumes the signal collapse into one,
// they never accumulate and never overflow. acquire() waits for the signal
// and consumes it.
//
// This intentionally is not a counting semaphore. Every use in the codebase
// is either a one-shot "block until the async task finished" handshake or a
// "wake the worker thread so it re-checks its state" notification, and the
// latter must tolerate redundant wakes coming from several sources at once.
//
// Backed by std::atomic<>::wait/notify, which lowers to the lightest wait
// primitive the OS provides (WaitOnAddress on Windows 8+, futex on Linux):
// no kernel handle, no allocation, userspace fast path. On Windows 7 the
// MSVC runtime falls back to a wait table - still correct, just heavier.
//
// The flag is std::atomic<int>, not std::atomic<bool>, on purpose. The Linux
// futex works on a 4-byte word, and std::atomic<>::wait can only futex
// directly on a type the size of __platform_wait_t (int). A 1-byte
// atomic<bool> can't, so libstdc++ routes wait/notify through a hashed
// waiter-pool proxy (_M_ver) - measured ~40% slower on the sync benchmark.
// An int futexes straight on its own word, matching std::binary_semaphore.
// Values are only ever 0 or 1.
//
// Not used on Apple: std::atomic<>::wait is availability-gated to macOS 11.0
// in libc++, while we target 10.13+, so crl_semaphore.h selects a
// dispatch_semaphore backend there instead.
class semaphore {
public:
	semaphore() = default;
	semaphore(const semaphore &other) = delete;
	semaphore &operator=(const semaphore &other) = delete;
	semaphore(semaphore &&other) = delete;
	semaphore &operator=(semaphore &&other) = delete;

	void acquire() {
		while (!_signaled.exchange(0, std::memory_order_acquire)) {
			_signaled.wait(0, std::memory_order_relaxed);
		}
	}
	void release() {
		if (!_signaled.exchange(1, std::memory_order_release)) {
			_signaled.notify_one();
		}
	}

private:
	std::atomic<int> _signaled = 0;

};

} // namespace crl
