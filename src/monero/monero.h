#pragma once

#include <cstdint>
#include <functional>

#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QNetworkReply;
QT_END_NAMESPACE

// Talks to a locally running monero-wallet-rpc daemon over its HTTP
// JSON-RPC API - the actual Monero wallet logic (keys, syncing, the
// blockchain) lives entirely in that separate process (a real prebuilt
// binary from the Monero Project), not in this app. zkgram never links
// against monero-cpp/monero-core: that would mean vendoring the whole
// Monero project (Boost, OpenSSL, LMDB, cryptonote, wallet2 - the same
// scale problem this project already ran into and stepped back from once
// for tdesktop, see docs/README.md). Talking to an already-running
// monero-wallet-rpc over plain HTTP has none of that cost.
//
// Qt-coupled by design (QNetworkAccessManager) - unlike telegram::
// TelegramClient/crypto::CryptoLayer, this is not meant to be toolkit-
// agnostic: it only exists for the Qt UI's wallet panel (see MainWindow's
// hamburger menu), so it is compiled only under ZKGRAM_UI=qt (see
// CMakeLists.txt).
namespace zkgram::monero {

class MoneroWalletRpc : public QObject {
    Q_OBJECT

public:
    // rpcUrl is monero-wallet-rpc's own JSON-RPC endpoint, e.g.
    // "http://127.0.0.1:38083/json_rpc" (its default --rpc-bind-port is
    // 18082/38083 depending on mainnet/stagenet - the caller is
    // responsible for already having the daemon running with a wallet
    // open; this class does not start or configure it). rpcUser/rpcPassword
    // are for monero-wallet-rpc's own --rpc-login digest auth, empty if it
    // was started without one.
    explicit MoneroWalletRpc(QObject* parent = nullptr, QString rpcUrl = QStringLiteral("http://127.0.0.1:38083/json_rpc"),
                              QString rpcUser = QString(), QString rpcPassword = QString());

    // Every call reports ok=false with a human-readable error on any
    // failure (daemon unreachable, wallet not open, RPC error object) -
    // callers are expected to show that directly rather than needing to
    // distinguish failure kinds, matching how MainWindow already surfaces
    // Telegram/CryptoLayer errors as plain status text.

    // Balances are in atomic units (1 XMR = 1e12 atomic units, "piconero")
    // - see formatAtomicXmr() below for display formatting.
    void fetchBalance(std::function<void(bool ok, std::uint64_t balanceAtomic, std::uint64_t unlockedAtomic,
                                          const QString& error)>
                           callback);
    void fetchPrimaryAddress(std::function<void(bool ok, const QString& address, const QString& error)> callback);

    // "12.345678901234 XMR" - trims trailing zeros but always keeps at
    // least two decimal places, same convention real Monero wallets use.
    static QString formatAtomicXmr(std::uint64_t atomic);

private:
    void call(const QString& method, std::function<void(bool ok, const QByteArray& result, const QString& error)> onDone);

    QNetworkAccessManager* network_;
    QString rpcUrl_;
    QString rpcUser_;
    QString rpcPassword_;
};

}  // namespace zkgram::monero
