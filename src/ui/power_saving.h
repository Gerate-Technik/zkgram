/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// Adapted for zkgram: real tdesktop .cpp files get base::flags/rpl/uint32
// for free from lib_ui's shared PCH; this file is compiled directly into
// zkgram (not part of lib_ui), which has no such PCH, so the includes it
// implicitly relied on are made explicit here instead.
#include "base/flags.h"
#include "base/basic_types.h"
#include <rpl/rpl.h>
#include "ui/effects/animation_value.h" // anim::SetDisabled

namespace PowerSaving {

enum Flag : uint32 {
	kAnimations = (1U << 0),
	kStickersPanel = (1U << 1),
	kStickersChat = (1U << 2),
	kEmojiPanel = (1U << 3),
	kEmojiReactions = (1U << 4),
	kEmojiChat = (1U << 5),
	kChatBackground = (1U << 6),
	kChatSpoiler = (1U << 7),
	kCalls = (1U << 8),
	kEmojiStatus = (1U << 9),
	kChatEffects = (1U << 10),

	kAll = (1U << 11) - 1,
};
inline constexpr bool is_flag_type(Flag) { return true; }
using Flags = base::flags<Flag>;

void Set(Flags flags);
[[nodiscard]] Flags Current();

void SetForceAll(bool force);
[[nodiscard]] bool ForceAll();

[[nodiscard]] rpl::producer<> Changes();

[[nodiscard]] inline bool On(Flag flag) {
	return ForceAll() || (Current() & flag);
}
[[nodiscard]] inline rpl::producer<bool> OnValue(Flag flag) {
	return rpl::single(On(flag)) | rpl::then(Changes() | rpl::map([=] {
		return On(flag);
	})) | rpl::distinct_until_changed();
}

} // namespace PowerSaving
