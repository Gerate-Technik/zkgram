// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include <crl/crl_queue.h>

#ifdef CRL_USE_COMMON_QUEUE

#include <crl/crl_async.h>
#include <crl/crl_fp_exceptions.h>

namespace crl {

queue::queue() = default;

queue::queue(main_queue_processor processor) : _main_processor(processor) {
}

void queue::wake_async() {
	if (!_queued.test_and_set()) {
		(_main_processor ? _main_processor : details::async_plain)(
			ProcessCallback,
			static_cast<void*>(this));
	}
}

void queue::process() {
	if (!_list.process()) {
		return;
	}
	_queued.clear();

	if (!_list.empty()) {
		wake_async();
	}
}

void queue::ProcessCallback(void *that) {
#ifdef CRL_THROW_FP_EXCEPTIONS
	static thread_local const bool kInited = [] {
		toggle_fp_exceptions(true);
		return true;
	}();
#endif // CRL_THROW_FP_EXCEPTIONS

	static_cast<queue*>(that)->process();
}

} // namespace crl

#endif // CRL_USE_COMMON_QUEUE
