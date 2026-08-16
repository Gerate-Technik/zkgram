#include "ui/qt/ui_integration.hpp"

#include "ui/style/style_core.h"
#include "ui/style/style_core_scale.h"
#include "ui/effects/animations.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QScreen>
#include <QStandardPaths>

namespace zkgram::ui::qt {

namespace {
// A fixed, explicitly requested value, not derived from the screen the
// way DevicePixelRatio is - see InstallUiRuntime()'s own comment for why
// style::SetScale() (not just StartManager()) had to be called for this to
// have any visible effect on the hand-painted canvas code at all. 100
// (real tdesktop's own default) read as too small once the pipeline
// actually worked end to end; 150 read as too large - 120 is the
// requested middle ground.
constexpr int kZkgramScale = 120;
}  // namespace

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

    // style::DevicePixelRatio()/Scale() default to 1 / kScaleDefault (100)
    // and nothing here ever changed them - every style-system-rendered
    // asset (bubble corner masks, the tail icon, glyph icons) was prepared
    // assuming a 1:1 pixel screen, then Qt's own automatic HiDPI upscaling
    // stretched the already-rendered result to match the real Windows
    // display scale (125%/150% are common) - the visible seams/blur at
    // bubble corners the user reported are exactly what that mismatch
    // looks like. Read the real ratio from the primary screen and set
    // both: DevicePixelRatio (integer, for @2x/@3x asset selection) and
    // Scale (percent, for precise fractional layout) - matching real
    // tdesktop's own DPI-detection intent, not guessed values.
    qreal devicePixelRatio =
        QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0;
    style::SetDevicePixelRatio(qMax(1, qRound(devicePixelRatio)));
    // Scale (logical interface size) is deliberately NOT set to match the
    // real device pixel ratio, unlike DevicePixelRatio above - real
    // tdesktop treats these as two separate knobs (its own Settings has
    // an independent "interface size" the user can pick independently of
    // what the OS scale would suggest). Matching them 1:1 made the whole
    // UI larger than wanted; kScaleDefault (100) afterwards read as too
    // small on this display - kZkgramScale is a fixed middle ground the
    // user picked explicitly, not derived from the screen.
    //
    // style::SetScale() is NOT redundant with StartManager() below, even
    // though both take the same scale argument - they feed two completely
    // separate mechanisms. StartManager(scale) forwards `scale` straight
    // into each generated style module's own start(int scale) (see
    // gen/styles/style_basic.cpp: `ConvertScale(13, scale)` etc.), which
    // bakes it into every real .style-declared st:: value ONCE at
    // startup - that part was already correctly scaling actual lib_ui
    // widgets (the composer, buttons, input fields). SetScale(scale) is
    // what sets the *global* accessor style::Scale() reads - the value
    // every no-argument style::ConvertScale(x) call in this codebase's own
    // hand-painted canvas code (HistoryCanvas/SidebarCanvas bubble/sidebar
    // sizing, see main_window.cpp's bubbleMargin()/sidebarRowHeight()/etc.)
    // resolves against. Without this call the global stayed at
    // kScaleDefault (100) forever regardless of what StartManager was
    // given, silently no-op-ing every one of those calls - the exact "the
    // scale doesn't do anything" symptom reported, and why only genuine
    // .style widgets (not the message list or sidebar) ever visibly
    // responded to changing kZkgramScale.
    style::SetScale(kZkgramScale);
    style::StartManager(kZkgramScale);
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
