#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/ui_provider.hpp"

// Persists decrypted conversation history to local disk, encrypted at
// rest, so it survives an app restart instead of only living in
// MainWindow::conversationMessages_ (in-memory, gone the moment the
// process exits).
//
// This is not just a speed optimization: crypto::CryptoLayer's AES
// session key comes from a fresh ECDH exchange every time a conversation
// starts (see cryptolayer/src/crypto_layer.py, self.AES_KEY =
// self.MY_PRIVATE_KEY.exchange(ec.ECDH(), ...)) - forward secrecy by
// design, so the *ciphertext* TDLib itself already has cached locally
// becomes permanently undecryptable the moment the app restarts and a
// new session/key is negotiated. Without this cache, every encrypted
// conversation's history would be unrecoverable after any restart, not
// merely slow to reload.
//
// Modeled on (not copied from) real Telegram Desktop's own local cache -
// see telegram_fork/tdesktop's Telegram/lib_storage/storage/cache/ and
// storage/storage_encryption.cpp: AES at rest, key derived from a local
// secret via a real KDF (PBKDF2/SHA-256 there, same primitive used here),
// not a fixed/hardcoded key. tdesktop's actual on-disk format (a
// content-addressed binlog plus one file per cached blob, built for many
// small media entries) is not reproduced as-is here - one whole
// conversation easily fits in a single small file, so this uses one
// encrypted file per chat instead, chosen deliberately for zkgram's much
// smaller scale rather than guessed.
namespace zkgram::core {

class LocalCache {
public:
    // cacheDir: platform::appDataDir() + "/tdata" - passed in rather than
    // resolved internally so this class stays testable/platform-agnostic
    // itself. password: the same local CryptoLayer password the caller
    // already holds (Session::password_) - reused as the cache's own key
    // material rather than inventing separate credential storage, since
    // it is already the one secret this device's owner is trusted to
    // remember. Never persisted itself, only ever fed through the KDF
    // below (see deriveKey()).
    LocalCache(std::string cacheDir, const std::string& password);

    // No-op (not an error) if cacheDir could not be resolved or created -
    // callers should treat a cache miss and "caching is unavailable" the
    // same way, since the fallback (fetch from Telegram/CryptoLayer live)
    // already exists and is correct on its own, just slower/impossible
    // offline.
    bool available() const { return !cacheDir_.empty(); }

    // Overwrites any previously cached history for this chat - callers
    // pass the full current message list, not a delta, matching how
    // MainWindow::conversationMessages_ already holds it. Silently
    // no-ops if !available(); logs (does not throw) on a write failure,
    // since a cache write failing must never interrupt sending/receiving
    // an actual message.
    void saveHistory(std::int64_t chatId, const std::vector<PlainMessage>& messages) const;

    // Returns an empty vector on a cache miss, a corrupt file, or a wrong
    // key (e.g. the password changed) - all treated identically as "no
    // usable cache", never surfaced as an error to the caller, which
    // already has a live fallback.
    std::vector<PlainMessage> loadHistory(std::int64_t chatId) const;

private:
    std::string cacheDir_;
    // 32-byte AES-256 key, derived once in the constructor via
    // PBKDF2-HMAC-SHA256 from the password plus a random per-installation
    // salt (see local_cache.cpp's saltFilePath()/loadOrCreateSalt()) -
    // the salt itself is not secret (same reasoning tdesktop's own
    // per-file salts rely on, storage_encryption.cpp) and is stored
    // alongside the cache in plain text, only the password is.
    std::vector<std::uint8_t> key_;

    std::string historyFilePath(std::int64_t chatId) const;
};

}  // namespace zkgram::core
