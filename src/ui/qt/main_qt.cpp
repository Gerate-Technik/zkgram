#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <thread>

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
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

    // Must run before QApplication/QGuiApplication is constructed (Qt's own
    // requirement - setting it later is a no-op). Without this, on a
    // fractional OS display scale (125%/150%, common on Windows) Qt reports
    // QScreen::devicePixelRatio() as that exact fraction (e.g. 1.5) and
    // paints every widget's backing store at that ratio, while
    // ui_integration.cpp's InstallUiRuntime() rounds the SAME value up to
    // an integer (qRound(1.5) == 2) for style::SetDevicePixelRatio(), since
    // the style system only ever prepares assets at a whole @1x/@2x/@3x
    // multiplier - real tdesktop does the same thing (see cRetinaFactor()).
    // The two ratios used to disagree (2 vs 1.5): bubble corner pixmaps
    // were pre-rendered assuming a 2x surface, then painted onto a 1.5x
    // one, landing on fractional pixel coordinates - the visible seams
    // between the corner pixmaps and the flat-fill middle of a bubble were
    // exactly that 0.75 coverage mismatch. Rounding Qt's own scale factor
    // the same way here makes QScreen::devicePixelRatio() already return
    // the rounded integer everywhere (including the actual paint surface),
    // so both sides agree.
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::Round);

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

    // Everything below runs inside a lambda so the window, and every widget
    // in it, is destroyed when the lambda returns - before StopUiRuntime()
    // afterwards, and while QApplication is still alive. Both halves matter,
    // and neither used to hold: StopUiRuntime() calls style::StopManager(),
    // which tears down the palette, fonts and cached corner/ripple images
    // that lib_ui widgets reach into from their own destructors, and it ran
    // while the window was still standing. Getting that order wrong is not a
    // clean failure - it frees memory whose owner is already gone, and it
    // aborted the process on exit.
    //
    // A lambda rather than a bare scope because the early returns below
    // (wizard cancelled, session construction failed) would otherwise leave
    // the block without ever reaching StopUiRuntime(); this way every exit
    // path tears the two down in the same order.
    int exitCode = [&]() -> int {
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
            // One-shot, for the hamburger menu's profile section (see
            // MainWindow::updateMyProfile()) - runs on this same worker
            // thread's callback, same marshalling as setConnectionStatus
            // above.
            session->fetchMyProfile([&window](const std::string& name, const std::string& username) {
                QMetaObject::invokeMethod(&window, "updateMyProfile", Qt::QueuedConnection,
                                           Q_ARG(QString, QString::fromStdString(name)),
                                           Q_ARG(QString, QString::fromStdString(username)));
            });
        } catch (const std::exception& e) {
            QMetaObject::invokeMethod(&window, "setConnectionStatus", Qt::QueuedConnection,
                                       Q_ARG(QString, QString("Connection failed")), Q_ARG(QString, QString("#e41e3f")));
            QMetaObject::invokeMethod(&window, "showConnectingProgress", Qt::QueuedConnection,
                                       Q_ARG(QString, QString("Connection failed")),
                                       Q_ARG(QString, QString::fromStdString(e.what())));
        } catch (...) {
            // A thread function must not let anything escape: an exception
            // leaving here goes straight to std::terminate() with no handler
            // of any kind, which on this path would look exactly like the
            // window-close abort this file's shutdown ordering fixes.
            // Python-side failures do arrive as std::exception (see
            // CryptoLayer's rethrowAsRuntimeError), so this is the
            // belt-and-braces case, not the expected one.
            QMetaObject::invokeMethod(&window, "setConnectionStatus", Qt::QueuedConnection,
                                       Q_ARG(QString, QString("Connection failed")), Q_ARG(QString, QString("#e41e3f")));
            QMetaObject::invokeMethod(&window, "showConnectingProgress", Qt::QueuedConnection,
                                       Q_ARG(QString, QString("Connection failed")),
                                       Q_ARG(QString, QString("unknown error")));
        }
    });

    int code = app.exec();

    // If the window was closed while session.start() was still waiting on
    // TDLib authorization, unblock it (Session::stop() -> TelegramClient::
    // disconnect() fails the pending wait, see telegram_client.cpp) before
    // joining, otherwise starter.join() would wait forever for a wait that
    // nothing would ever satisfy.
    //
    // Before stop(), not after: stop() joins TDLib's receive thread, and
    // that thread may be parked in a blocking call back to the GUI thread
    // that can no longer be served now that exec() has returned. See
    // QtUiProvider::beginShutdown().
    ui->beginShutdown();

    // Session::stop() is noexcept, so the join below is always reached.
    // That ordering is not cosmetic: destroying a joinable std::thread calls
    // std::terminate(), so anything that could skip this join turns a clean
    // exit into an abort - which is what closing the window used to do,
    // back when stop() could still propagate a Python-side exception out of
    // CryptoLayer::stop().
    session->stop();
    starter.join();

    // MainWindow outlives `session` (it is declared earlier, so it is
    // destroyed later) and has been holding a raw pointer to it since
    // setSession() above. Cleared here, while the Session is still alive, so
    // the window cannot reach freed memory through a stale pointer during
    // its own teardown.
    window.setSession(nullptr);

    return code;
    }();

    // The window is gone by now (see the lambda's own comment), so the style
    // system has no live widget still depending on it.
    zkgram::ui::qt::StopUiRuntime();
    return exitCode;
}
