#include "monero/monero.h"

#include <QAuthenticator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace zkgram::monero {

MoneroWalletRpc::MoneroWalletRpc(QObject* parent, QString rpcUrl, QString rpcUser, QString rpcPassword)
    : QObject(parent), network_(new QNetworkAccessManager(this)), rpcUrl_(std::move(rpcUrl)),
      rpcUser_(std::move(rpcUser)), rpcPassword_(std::move(rpcPassword)) {
    // monero-wallet-rpc challenges with HTTP Digest auth when started with
    // --rpc-login user:pass - QNetworkAccessManager handles the actual
    // digest handshake itself once given credentials here, this class
    // never has to speak the digest protocol directly.
    connect(network_, &QNetworkAccessManager::authenticationRequired, this,
            [this](QNetworkReply*, QAuthenticator* authenticator) {
                if (!rpcUser_.isEmpty()) {
                    authenticator->setUser(rpcUser_);
                    authenticator->setPassword(rpcPassword_);
                }
            });
}

void MoneroWalletRpc::call(const QString& method,
                            std::function<void(bool ok, const QByteArray& result, const QString& error)> onDone) {
    QJsonObject body;
    body["jsonrpc"] = "2.0";
    body["id"] = "0";
    body["method"] = method;
    body["params"] = QJsonObject();

    QNetworkRequest request((QUrl(rpcUrl_)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply* reply = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, onDone] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            onDone(false, QByteArray(), reply->errorString());
            return;
        }
        QByteArray raw = reply->readAll();
        QJsonParseError parseError{};
        QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            onDone(false, QByteArray(), "malformed response from monero-wallet-rpc");
            return;
        }
        QJsonObject root = doc.object();
        if (root.contains("error")) {
            QJsonObject errorObj = root["error"].toObject();
            onDone(false, QByteArray(), errorObj["message"].toString("monero-wallet-rpc returned an error"));
            return;
        }
        onDone(true, QJsonDocument(root["result"].toObject()).toJson(QJsonDocument::Compact), QString());
    });
}

void MoneroWalletRpc::fetchBalance(std::function<void(bool, std::uint64_t, std::uint64_t, const QString&)> callback) {
    call("get_balance", [callback](bool ok, const QByteArray& result, const QString& error) {
        if (!ok) {
            callback(false, 0, 0, error);
            return;
        }
        QJsonObject obj = QJsonDocument::fromJson(result).object();
        // JSON numbers only guarantee ~53 bits of precision (IEEE double) -
        // a balance near or past that (well within reach for the atomic-
        // unit scale monero-wallet-rpc reports in) would silently lose
        // precision through QJsonValue::toDouble(). toVariant().toULongLong()
        // instead reads the underlying value as an integer, not a double,
        // avoiding that.
        std::uint64_t balance = obj["balance"].toVariant().toULongLong();
        std::uint64_t unlocked = obj["unlocked_balance"].toVariant().toULongLong();
        callback(true, balance, unlocked, QString());
    });
}

void MoneroWalletRpc::fetchPrimaryAddress(std::function<void(bool, const QString&, const QString&)> callback) {
    call("get_address", [callback](bool ok, const QByteArray& result, const QString& error) {
        if (!ok) {
            callback(false, QString(), error);
            return;
        }
        QJsonObject obj = QJsonDocument::fromJson(result).object();
        callback(true, obj["address"].toString(), QString());
    });
}

QString MoneroWalletRpc::formatAtomicXmr(std::uint64_t atomic) {
    // 1 XMR = 10^12 atomic units ("piconero") - fixed by the protocol, not
    // configurable, same constant every Monero wallet uses.
    constexpr std::uint64_t kAtomicPerXmr = 1'000'000'000'000ULL;
    std::uint64_t whole = atomic / kAtomicPerXmr;
    std::uint64_t frac = atomic % kAtomicPerXmr;
    QString fracStr = QString::number(frac).rightJustified(12, '0');
    // Trim trailing zeros, but keep at least 2 decimal places - matches
    // how real Monero wallet UIs display a balance.
    int keep = fracStr.size();
    while (keep > 2 && fracStr.at(keep - 1) == '0') {
        --keep;
    }
    return QString("%1.%2 XMR").arg(whole).arg(fracStr.left(keep));
}

}  // namespace zkgram::monero
