// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

// Unexpected() below is a macro from here, and this header did not include
// it - it only ever compiled because every translation unit that reached
// this file happened to have pulled assertion.h in earlier (lib_base's own
// sources get it from base_pch.h). zkgram's sources have no such
// precompiled header, so for them the macro is undefined and the call
// parses as an undeclared function in a template body. GCC accepted that
// silently until 14, warns about it under -Wtemplate-body, and rejects it
// outright from 15 on - which is where distributions with a current
// toolchain fail. Self-contained now, so it no longer depends on include
// order. No cycle: assertion.h pulls in only <cstdlib> and <gsl/assert>.
#include "base/assertion.h"

#include <compare>
#include <variant>
#include <gsl/pointers>

#include <QString>

#if !defined(__apple_build_version__) || (__apple_build_version__ > 12000032)

template <typename P>
[[nodiscard]] inline std::strong_ordering operator<=>(
		const gsl::not_null<P> &a,
		const gsl::not_null<P> &b) noexcept {
	return a.get() <=> b.get();
}

#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0) || !defined __cpp_lib_three_way_comparison
[[nodiscard]] inline std::strong_ordering operator<=>(
		const QString &a,
		const QString &b) noexcept {
	return a.compare(b) <=> 0;
}
#endif // Qt < 6.8.0 || !__cpp_lib_three_way_comparison

template <typename T>
[[nodiscard]] inline std::strong_ordering operator<=>(
		const QVector<T> &a,
		const QVector<T> &b) noexcept {
	const auto as = a.size();
	const auto bs = b.size();
	const auto s = int(std::min(as, bs));
	for (auto i = 0; i != s; ++i) {
		const auto result = (a[i] <=> b[i]);
		if (result != std::strong_ordering::equal
			&& result != std::strong_ordering::equivalent) {
			return result;
		}
	}
	return (as <=> bs);
}

#ifndef _MSC_VER
namespace base::details {

template <typename T>
using compare_three_way_result_t = decltype(
	(std::declval<const std::remove_reference_t<T>&>()
		<=> std::declval<const std::remove_reference_t<T>&>()));

template <typename ...Types>
using variant_compare_result = std::common_comparison_category_t<
	compare_three_way_result_t<Types>...>;

template <int Index, typename ...Types>
constexpr variant_compare_result<Types...> variant_comparator(
		const std::variant<Types...> &a,
		const std::variant<Types...> &b,
		int index) {
	if (index == Index) {
		return *std::get_if<Index>(&a) <=> *std::get_if<Index>(&b);
	} else if constexpr (Index + 1 < sizeof...(Types)) {
		return variant_comparator<Index + 1>(a, b, index);
	} else {
		Unexpected("Index value in variant_comparator.");
	}
}

} // namespace base::details

template <typename ...Types>
inline constexpr auto operator<=>(
	const std::variant<Types...> &a,
	const std::variant<Types...> &b)
-> base::details::variant_compare_result<Types...> {
	const auto index = a.index();
	if (const auto result = (index <=> b.index())
		; result != std::strong_ordering::equal) {
		return result;
	}
	return base::details::variant_comparator<0>(a, b, index);
}

template <typename Type, typename Result = decltype(std::declval<Type>() <=> std::declval<Type>())>
inline constexpr auto operator<=>(
		const std::vector<Type> &a,
		const std::vector<Type> &b) -> Result {
	const auto asize = a.size();
	const auto bsize = b.size();
	const auto min = std::min(asize, bsize);
	for (auto i = std::size_t(); i != min; ++i) {
		const auto result = (a[i] <=> b[i]);
		if (result != Result::equivalent) {
			return result;
		}
	}
	if (asize < bsize) {
		return Result::less;
	} else if (asize > bsize) {
		return Result::greater;
	}
	return Result::equivalent;
}

#endif // _MSC_VER
#endif // __apple_build_version__
