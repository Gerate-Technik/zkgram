// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <rpl/producer.h>

namespace rpl {
namespace details {

struct type_erased_t {
	template <typename Value, typename Error, typename Generator>
	producer<Value, Error> operator()(
			producer<Value, Error, Generator> &&initial) const {
		return std::move(initial);
	}
};

} // namespace details

inline constexpr details::type_erased_t type_erased{};

} // namespace rpl
