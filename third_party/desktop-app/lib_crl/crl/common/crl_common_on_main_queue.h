// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <crl/common/crl_common_config.h>

#include <functional>
#include <deque>

namespace crl {
namespace details {

template <typename Guard>
using GetGuardTraits = guard_traits<std::decay_t<Guard>>;

template <typename Guard>
using GetGuardType = decltype(
	GetGuardTraits<Guard>::create(std::declval<Guard>()));

template <typename Guard>
inline void perform_next_step(
		GetGuardType<Guard> guard,
		std::deque<std::function<void()>> steps) {
	if (steps.empty()) {
		return;
	}
	on_main([guard = std::move(guard), steps = std::move(steps)]() mutable {
		if (!GetGuardTraits<Guard>::check(guard)) {
			return;
		}
		auto step = std::move(steps.front());
		steps.pop_front();
		step();
		perform_next_step<Guard>(std::move(guard), std::move(steps));
	});
}

template <typename Guard>
class guarded_on_main_queue {
public:
	guarded_on_main_queue(
		Guard &&object,
		std::deque<std::function<void()>> steps)
	: _guard(GetGuardTraits<Guard>::create(std::forward<Guard>(object)))
	, _steps(std::move(steps)) {
	}

	guarded_on_main_queue &next(std::function<void()> step) {
		_steps.push_back(std::move(step));
		return *this;
	}

	void dispatch() {
		perform_next_step<Guard>(std::move(_guard), std::move(_steps));
	}

private:
	GetGuardType<Guard> _guard;
	std::deque<std::function<void()>> _steps;

};

} // namespace details

template <
	typename Guard,
	typename GuardTraits = guard_traits<std::decay_t<Guard>>,
	typename = std::enable_if_t<
		sizeof(GuardTraits) != details::dependent_zero<GuardTraits>>>
[[nodiscard]] inline details::guarded_on_main_queue<Guard> on_main_queue(
		Guard &&object,
		std::deque<std::function<void()>> steps) {
	return details::guarded_on_main_queue<Guard>(
		std::forward<Guard>(object),
		std::move(steps));
}

} // namespace crl
