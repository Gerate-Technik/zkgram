#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <thread>

#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QMetaObject>

#include "core/session.hpp"
#include "platform/paths.hpp"
#include "ui/qt/main_window.hpp"
#include "ui/qt/qt_ui_provider.hpp"
#include "ui/qt/ui_integration.hpp"

namespace {

// Next to the exe, same as every other logDebug() in this codebase (see
// session.cpp/telegram_client.cpp/main_window.cpp). Через
// platform::executableDir(), а не через QCoreApplication::
// applicationDirPath(), как в main_window.cpp: этот путь вычисляется в том
// числе из обработчика std::terminate, где Qt уже может быть разрушен.
std::string logFilePath() {
    std::string dir = zkgram::platform::executableDir();
    if (dir.empty()) {
        dir = ".";
    }
    return dir + "/zkgram_debug.log";
}

// Diagnostic only: this codebase has repeatedly hit an uncaught-exception
// crash (Windows Event Log shows ucrtbase.dll fast-fail, exception code
// 0xc0000409 - the CRT's abort() path) with no breadcrumb anywhere as to
// what actually threw, since exceptions crossing a Qt slot/event-loop
// boundary are not covered by any of the try/catch blocks already in this
// file (those only wrap session construction/session->start(), not
// anything running during app.exec() itself). This at least logs the
// exception's what() before the process goes down, instead of leaving
// nothing to go on.
[[noreturn]] void logUncaughtExceptionAndAbort() {
    std::string message = "unknown exception (not derived from std::exception)";
    if (auto currentException = std::current_exception()) {
        try {
            std::rethrow_exception(currentException);
        } catch (const std::exception& e) {
            message = e.what();
        } catch (...) {
        }
    }
    std::ofstream file(logFilePath(), std::ios::app);
    if (file) {
        file << "FATAL: std::terminate called, uncaught exception: " << message << "\n";
    }
    std::abort();
}

}  // namespace

int main(int argc, char** argv) {
    std::set_terminate(logUncaughtExceptionAndAbort);

    QApplication app(argc, argv);

    // Must run before constructing MainWindow (or anything else that builds
    // a lib_ui widget) - see ui_integration.hpp's own comment for what this
    // fixes.
    zkgram::ui::qt::InstallUiRuntime(argc, argv);

    // Next to the exe, not a bare "data" resolved against the current
    // working directory: launching zkgram.exe by double-click, a desktop
    // shortcut, or Start-Process can each leave the process with a
    // different working directory, so a relative "data" silently pointed
    // at a different folder every time - CryptoLayer's encrypted identity
    // (dataDir/sign_private) never got found on the next launch, which
    // looked like "the password isn't saved, it asks for a new one every
    // time" even though nothing was actually lost - it was creating a new
    // identity in a new place each run instead of reusing the old one.
    QString dataDir = QCoreApplication::applicationDirPath() + "/data";

    zkgram::ui::qt::MainWindow window;
    window.showMaximized();

    // One full-window slide (local CryptoLayer password) instead of a
    // startup dialog - see MainWindow::runStartupWizard() and AuthSlide in
    // main_window.hpp/.cpp. No companion id slide anymore: chats are
    // picked from the sidebar once logged in (see TODO.md "мультиюзерность").
    zkgram::ui::qt::MainWindow::StartupCredentials credentials = window.runStartupWizard(dataDir);
    if (!credentials.accepted) {
        return 0;
    }

    window.setConnectionStatus("Connecting...", "#f7b928");

    // core::Session's constructor runs CryptoLayer's constructor directly
    // (Python interpreter/module init, see crypto_layer.cpp), on this
    // thread, before the event loop or the try/catch below even exist -
    // std::unique_ptr + try/catch here instead of a stack Session so a
    // Python-side init failure (missing module, bad working directory,
    // etc.) shows an error instead of taking the whole process down via
    // std::terminate (see TODO.md, "ввел номер потом пароль и потом
    // приложение вылетело" - this exact gap was the actual cause).
    auto ui = std::make_shared<zkgram::ui::qt::QtUiProvider>(&window);
    std::unique_ptr<zkgram::core::Session> session;
    try {
        session = std::make_unique<zkgram::core::Session>(ui, dataDir.toStdString(), credentials.password.toStdString());
    } catch (const std::exception& e) {
        QMessageBox::critical(&window, "zkgram", QString("Failed to start: %1").arg(e.what()));
        return 1;
    }
    window.setSession(session.get());

    // Session::start() blocks until TDLib authorization finishes, and that
    // authorization flow shows modal dialogs on the GUI thread (see
    // QtUiProvider::requestCredential). Running start() on the GUI thread
    // would deadlock: the modal dialog needs the event loop that start()
    // itself would be blocking. Run it on a worker thread instead.
    std::thread starter([&session, &window] {
        try {
            session->start();
            QMetaObject::invokeMethod(&window, "setConnectionStatus", Qt::QueuedConnection,
                                       Q_ARG(QString, QString("Connected")), Q_ARG(QString, QString("#31a24c")));
        } catch (const std::exception& e) {
            QMetaObject::invokeMethod(&window, "setConnectionStatus", Qt::QueuedConnection,
                                       Q_ARG(QString, QString("Connection failed")), Q_ARG(QString, QString("#e41e3f")));
            QMetaObject::invokeMethod(&window, "showConnectingProgress", Qt::QueuedConnection,
                                       Q_ARG(QString, QString("Connection failed")),
                                       Q_ARG(QString, QString::fromStdString(e.what())));
        }
    });

    int exitCode = app.exec();

    // If the window was closed while session.start() was still waiting on
    // TDLib authorization, unblock it (Session::stop() -> TelegramClient::
    // disconnect() fails the pending wait, see telegram_client.cpp) before
    // joining, otherwise starter.join() would wait forever for a wait that
    // nothing would ever satisfy.
    session->stop();
    starter.join();
    zkgram::ui::qt::StopUiRuntime();
    return exitCode;
}
