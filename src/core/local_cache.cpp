#include "core/local_cache.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

namespace zkgram::core {

namespace {

constexpr int kKeySize = 32;   // AES-256.
constexpr int kIvSize = 16;    // AES block size, used as the CTR nonce/counter.
constexpr int kSaltSize = 16;
// Real-world password, so a real KDF cost is worth paying once at
// startup - unlike tdesktop's own "1 iteration when the local passcode
// is empty" shortcut (storage_file_utilities.cpp), zkgram's local
// password is mandatory (see CLAUDE.md: it is the only thing that
// decrypts the local identity file), so there is no empty-password case
// to special-case here.
constexpr int kPbkdf2Iterations = 200000;

std::string saltFilePath(const std::string& cacheDir) { return cacheDir + "/salt"; }

// Loads the existing per-installation salt, or generates and persists a
// new random one on first run. The salt is not secret (see local_cache.hpp's
// key_ comment) - only the password fed alongside it into the KDF is.
std::vector<std::uint8_t> loadOrCreateSalt(const std::string& cacheDir) {
    std::string path = saltFilePath(cacheDir);
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::vector<std::uint8_t> salt(kSaltSize);
        in.read(reinterpret_cast<char*>(salt.data()), kSaltSize);
        if (in.gcount() == kSaltSize) {
            return salt;
        }
    }
    std::vector<std::uint8_t> salt(kSaltSize);
    RAND_bytes(salt.data(), kSaltSize);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out) {
        out.write(reinterpret_cast<const char*>(salt.data()), kSaltSize);
    }
    return salt;
}

std::vector<std::uint8_t> deriveKey(const std::string& password, const std::vector<std::uint8_t>& salt) {
    std::vector<std::uint8_t> key(kKeySize);
    PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(),
                       static_cast<int>(salt.size()), kPbkdf2Iterations, EVP_sha256(), kKeySize, key.data());
    return key;
}

// AES-256-CTR in place - the same construction (and reasoning: a stream
// cipher needs no padding, and CTR mode's keystream is only ever used
// once per key+IV pair, guaranteed here by a fresh random IV per file)
// documented in local_cache.hpp's class comment as modeled on tdesktop's
// own storage_encryption.cpp.
std::vector<std::uint8_t> aesCtrTransform(const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& iv,
                                           const std::uint8_t* data, std::size_t size) {
    std::vector<std::uint8_t> out(size);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int outLen = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, key.data(), iv.data());
    EVP_EncryptUpdate(ctx, out.data(), &outLen, data, static_cast<int>(size));
    int finalLen = 0;
    EVP_EncryptFinal_ex(ctx, out.data() + outLen, &finalLen);
    EVP_CIPHER_CTX_free(ctx);
    return out;
}

// Length-prefixed binary serialization - fields never contain the
// delimiter-ambiguity problem a text format would (message text itself
// can contain any byte sequence, so a plain newline/comma format would
// need escaping; length prefixes need none).
void appendU32(std::vector<std::uint8_t>& buffer, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        buffer.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void appendI64(std::vector<std::uint8_t>& buffer, std::int64_t value) {
    for (int i = 0; i < 8; ++i) {
        buffer.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) >> (i * 8)) & 0xFF));
    }
}

void appendString(std::vector<std::uint8_t>& buffer, const std::string& value) {
    appendU32(buffer, static_cast<std::uint32_t>(value.size()));
    buffer.insert(buffer.end(), value.begin(), value.end());
}

bool readU32(const std::uint8_t* data, std::size_t size, std::size_t& pos, std::uint32_t& value) {
    if (pos + 4 > size) {
        return false;
    }
    value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data[pos + i]) << (i * 8);
    }
    pos += 4;
    return true;
}

bool readI64(const std::uint8_t* data, std::size_t size, std::size_t& pos, std::int64_t& value) {
    if (pos + 8 > size) {
        return false;
    }
    std::uint64_t raw = 0;
    for (int i = 0; i < 8; ++i) {
        raw |= static_cast<std::uint64_t>(data[pos + i]) << (i * 8);
    }
    pos += 8;
    value = static_cast<std::int64_t>(raw);
    return true;
}

bool readString(const std::uint8_t* data, std::size_t size, std::size_t& pos, std::string& value) {
    std::uint32_t length = 0;
    if (!readU32(data, size, pos, length) || pos + length > size) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(data + pos), length);
    pos += length;
    return true;
}

}  // namespace

LocalCache::LocalCache(std::string cacheDir, const std::string& password) : cacheDir_(std::move(cacheDir)) {
    if (cacheDir_.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(cacheDir_, ec);
    if (ec) {
        cacheDir_.clear();  // Cannot create the directory - treat as "no cache available".
        return;
    }
    key_ = deriveKey(password, loadOrCreateSalt(cacheDir_));
}

std::string LocalCache::historyFilePath(std::int64_t chatId) const {
    return cacheDir_ + "/history_" + std::to_string(chatId) + ".cache";
}

void LocalCache::saveHistory(std::int64_t chatId, const std::vector<PlainMessage>& messages) const {
    if (!available()) {
        return;
    }
    std::vector<std::uint8_t> plain;
    appendU32(plain, static_cast<std::uint32_t>(messages.size()));
    for (const PlainMessage& message : messages) {
        appendI64(plain, message.id);
        plain.push_back(message.isOutgoing ? 1 : 0);
        appendI64(plain, message.date);
        appendString(plain, message.text);
        appendString(plain, message.photoPath);
        appendString(plain, message.senderName);
    }

    std::vector<std::uint8_t> iv(kIvSize);
    RAND_bytes(iv.data(), kIvSize);
    std::vector<std::uint8_t> cipher = aesCtrTransform(key_, iv, plain.data(), plain.size());

    std::ofstream out(historyFilePath(chatId), std::ios::binary | std::ios::trunc);
    if (!out) {
        return;  // Best-effort - see the class comment on saveHistory().
    }
    out.write(reinterpret_cast<const char*>(iv.data()), kIvSize);
    out.write(reinterpret_cast<const char*>(cipher.data()), static_cast<std::streamsize>(cipher.size()));
}

std::vector<PlainMessage> LocalCache::loadHistory(std::int64_t chatId) const {
    if (!available()) {
        return {};
    }
    std::ifstream in(historyFilePath(chatId), std::ios::binary);
    if (!in) {
        return {};
    }
    std::vector<std::uint8_t> iv(kIvSize);
    in.read(reinterpret_cast<char*>(iv.data()), kIvSize);
    if (in.gcount() != kIvSize) {
        return {};
    }
    std::vector<std::uint8_t> cipher((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::uint8_t> plain = aesCtrTransform(key_, iv, cipher.data(), cipher.size());

    std::vector<PlainMessage> messages;
    std::size_t pos = 0;
    std::uint32_t count = 0;
    if (!readU32(plain.data(), plain.size(), pos, count)) {
        return {};  // Corrupt or wrong key - treated as a cache miss, see the header comment.
    }
    messages.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        PlainMessage message;
        std::uint8_t isOutgoing = 0;
        bool ok = readI64(plain.data(), plain.size(), pos, message.id);
        if (pos < plain.size()) {
            isOutgoing = plain[pos];
            pos += 1;
        } else {
            ok = false;
        }
        ok = ok && readI64(plain.data(), plain.size(), pos, message.date);
        ok = ok && readString(plain.data(), plain.size(), pos, message.text);
        ok = ok && readString(plain.data(), plain.size(), pos, message.photoPath);
        ok = ok && readString(plain.data(), plain.size(), pos, message.senderName);
        if (!ok) {
            return {};  // Same "treat as a miss" reasoning as the count check above.
        }
        message.isOutgoing = isOutgoing != 0;
        messages.push_back(std::move(message));
    }
    return messages;
}

}  // namespace zkgram::core
