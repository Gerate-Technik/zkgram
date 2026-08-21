/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/widgets/fields/input_field.h"

// Adapted for zkgram: real tdesktop's sent_code_field.h also declares
// SentCodeCall (the "resend code via automated phone call" countdown/
// status text) - dropped here since it exists only to render
// tr::lng_code_call/lng_code_calling/lng_code_called, which would pull in
// the whole lang/ codegen pipeline for a call-based resend flow zkgram
// does not offer (TelegramClient's own auth flow only ever asks for an SMS
// code, see telegram_client.cpp's authorizationStateWaitCode handling -
// there is no "call me instead" option wired up anywhere to call this
// from). SentCodeField itself (the digit-only, auto-submitting input) has
// no such dependency and is kept as-is.
namespace Ui {

class SentCodeField final : public Ui::InputField {
public:
	SentCodeField(
		QWidget *parent,
		const style::InputField &st,
		rpl::producer<QString> placeholder = nullptr,
		const QString &val = QString());

	void setAutoSubmit(int length, Fn<void()> submitCallback);
	void setChangedCallback(Fn<void()> changedCallback);
	[[nodiscard]] QString getDigitsOnly() const;

private:
	void fix();

	// Flag for not calling onTextChanged() recursively.
	bool _fixing = false;

	int _autoSubmitLength = 0;
	Fn<void()> _submitCallback;
	Fn<void()> _changedCallback;

};

} // namespace Ui
