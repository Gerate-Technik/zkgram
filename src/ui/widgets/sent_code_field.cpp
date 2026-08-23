/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/widgets/sent_code_field.h"

#include <QRegularExpression>

namespace Ui {

SentCodeField::SentCodeField(
	QWidget *parent,
	const style::InputField &st,
	rpl::producer<QString> placeholder,
	const QString &val)
: Ui::InputField(parent, st, std::move(placeholder), val) {
	changes() | rpl::on_next([=] { fix(); }, lifetime());
}

void SentCodeField::setAutoSubmit(int length, Fn<void()> submitCallback) {
	_autoSubmitLength = length;
	_submitCallback = std::move(submitCallback);
}

void SentCodeField::setChangedCallback(Fn<void()> changedCallback) {
	_changedCallback = std::move(changedCallback);
}

QString SentCodeField::getDigitsOnly() const {
	return QString(
		getLastText()
	).remove(TextUtilities::RegExpDigitsExclude());
}

void SentCodeField::fix() {
	if (_fixing) return;

	_fixing = true;
	auto newText = QString();
	const auto now = getLastText();
	auto oldPos = textCursor().position();
	auto newPos = -1;
	auto oldLen = now.size();
	auto digitCount = 0;
	for (const auto &ch : now) {
		if (ch.isDigit()) {
			++digitCount;
		}
	}

	if (_autoSubmitLength > 0 && digitCount > _autoSubmitLength) {
		digitCount = _autoSubmitLength;
	}
	const auto strict = (_autoSubmitLength > 0)
		&& (digitCount == _autoSubmitLength);

	newText.reserve(oldLen);
	int i = 0;
	for (const auto &ch : now) {
		if (i++ == oldPos) {
			newPos = newText.length();
		}
		if (ch.isDigit()) {
			if (!digitCount--) {
				break;
			}
			newText += ch;
			if (strict && !digitCount) {
				break;
			}
		} else if (ch == '-') {
			newText += ch;
		}
	}
	if (newPos < 0) {
		newPos = newText.length();
	}
	if (newText != now) {
		setText(newText);
		setCursorPosition(newPos);
	}
	_fixing = false;

	if (_changedCallback) {
		_changedCallback();
	}
	if (strict && _submitCallback) {
		_submitCallback();
	}
}

} // namespace Ui
