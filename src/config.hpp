#pragma once

// Публичные API ID/Hash официального клиента Telegram Desktop.
// Используются для быстрого старта/теста. Не принадлежат zkgram,
// для продакшена стоит завести свои на my.telegram.org.
namespace zkgram::config {

inline constexpr int kTelegramApiId = 2040;
inline constexpr const char* kTelegramApiHash = "b18441a1ff607e10a989891a5462e627";

}  // namespace zkgram::config

