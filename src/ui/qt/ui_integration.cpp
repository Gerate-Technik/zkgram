#include "ui/qt/ui_integration.hpp"

#include "ui/style/style_core.h"
#include "ui/style/style_core_scale.h"
#include "ui/effects/animations.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QStandardPaths>

namespace zkgram::ui::qt {

void UiIntegration::postponeCall(FnMut<void()>&& callable) {
    // Marshalled onto qApp (always the GUI thread) regardless of which
    // thread calls postponeCall() - lib_ui code (and crl::on_main, see
    // crl_on_main_qt.cpp) calls this expecting the callback to run on the
    // main/GUI thread's event loop, not synchronously and not on whatever
    // thread happened to call postponeCall().
    QMetaObject::invokeMethod(
        qApp, [callable = std::move(callable)]() mutable { callable(); }, Qt::QueuedConnection);
}

void UiIntegration::registerLeaveSubscription(not_null<QWidget*> widget) {
    // Used by lib_ui to batch QEvent::Leave handling across widgets (tooltip
    // dismissal and similar). Not wired up yet - src/ui/qt/ does not use any
    // lib_ui tooltip/hover-popup widgets as of this phase (UI.md section
    // 13c), so there is nothing real to register against yet. A no-op here
    // is safe: it means "no batched leave handling", not "crash".
    Q_UNUSED(widget);
}

void UiIntegration::unregisterLeaveSubscription(not_null<QWidget*> widget) {
    Q_UNUSED(widget);
}

QString UiIntegration::emojiCacheFolder() {
    // zkgram does not use lib_ui's emoji picker/rendering yet - a real,
    // writable directory is still returned (some lib_ui code paths assume
    // one exists even if custom emoji are never actually requested) rather
    // than an empty string.
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/emoji";
}

QString UiIntegration::openglCheckFilePath() {
    // Empty: zkgram does not use lib_ui's ui/gl/ OpenGL rendering path (see
    // zkgram/docs/README.md - the go/no-go build check deliberately did not
    // need it), so there is no GL-capability-check marker file to track.
    return QString();
}

QString UiIntegration::angleBackendFilePath() {
    return QString();
}

void BaseIntegration::enterFromEventLoop(FnMut<void()>&& method) {
    // Real tdesktop's Core::Application marshals this onto its own
    // event-loop-reentry mechanism; zkgram has none of that machinery, so
    // just run it directly - callers only need "run this soon, not
    // necessarily synchronously right here."
    method();
}

bool BaseIntegration::logSkipDebug() {
    return true;
}

void BaseIntegration::logMessageDebug(const QString& message) {
    Q_UNUSED(message);
}

void BaseIntegration::logMessage(const QString& message) {
    Q_UNUSED(message);
}

void UiIntegration::touchCounterIncrement() {
    // Touch-input analytics only (real tdesktop uses this for a settings
    // toggle suggestion) - not implemented, not needed for zkgram to work.
}

int UiIntegration::touchCounterNow() {
    return 0;
}

void InstallUiRuntime(int argc, char** argv) {
    static BaseIntegration baseInstance(argc, argv);
    base::Integration::Set(&baseInstance);
    static UiIntegration instance;
    Ui::Integration::Set(&instance);
    style::StartManager(style::kScaleDefault);
    // Ui::Animations::Basic::start() (used by every animated real widget -
    // MaskedInputField's focus border, RoundButton's ripple, and now the
    // real chat bubble/theme code) hard-Expects() a live
    // Ui::Animations::Manager instance; nothing constructed one, so the
    // first widget whose animation actually started (the password field's
    // focus-in animation, triggered by the very first runSlide() call)
    // crashed with an assertion failure. Constructed here, once, same
    // process lifetime as UiIntegration above.
    static Ui::Animations::Manager animationsManager;
}

void StopUiRuntime() {
    style::StopManager();
}

}  // namespace zkgram::ui::qt
