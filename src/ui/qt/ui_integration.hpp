#pragma once

#include "base/integration.h"
#include "ui/integration.h"

namespace zkgram::ui::qt {

// base::Integration (third_party/desktop-app/lib_base/base/integration.h) -
// a second, separate required singleton from Ui::Integration above, same
// "never constructed" gap: base::Integration::Instance() Expects()s a live
// instance, and lib_ui code reaches into it too (confirmed via a
// symbolized crash inside Ui::InputField's internal QTextEdit signal
// handling - base::qt_signal_producer -> base::Integration::Instance() ->
// assertion failure - the first time the user actually typed into the
// composer, since that is what first exercised this code path). Minimal
// implementation: no real crash-reporting/assertion-log wiring, just
// enough for lib_base code to have somewhere safe to call into.
class BaseIntegration final : public base::Integration {
public:
    using base::Integration::Integration;

    void enterFromEventLoop(FnMut<void()>&& method) override;
    bool logSkipDebug() override;
    void logMessageDebug(const QString& message) override;
    void logMessage(const QString& message) override;
};

// Ui::Integration ("Methods that must be implemented outside lib_ui", see
// third_party/desktop-app/lib_ui/ui/integration.h) is a required hook point:
// every lib_ui widget (Ui::RpWidget and everything built on it, including
// Ui::RoundButton/Ui::IconButton) can reach into Ui::Integration::Instance()
// during construction or painting. zkgram never implemented or registered
// one - Ui::Integration::Set() was simply never called - so Instance()
// dereferenced whatever the static instance pointer happened to be (never
// set), which is what crashed zkgram.exe a few seconds after startup
// (STATUS_ACCESS_VIOLATION, inside Ui::RoundButton's construction via
// Ui::RoundRect -> Images::PrepareCorners -> style::colorizeImage, confirmed
// with a symbolized stack trace via cdb). This is the minimal implementation
// needed to make lib_ui widgets safe to construct - not a full port of
// real tdesktop's own Core::Application (which implements phrase
// translations, emoji picker paths, GL backend checks, etc.), see
// zkgram/.claude/UI.md section 13c for what is and is not covered by this
// migration yet.
class UiIntegration final : public Ui::Integration {
public:
    void postponeCall(FnMut<void()>&& callable) override;
    void registerLeaveSubscription(not_null<QWidget*> widget) override;
    void unregisterLeaveSubscription(not_null<QWidget*> widget) override;

    QString emojiCacheFolder() override;
    QString openglCheckFilePath() override;
    QString angleBackendFilePath() override;

    void touchCounterIncrement() override;
    int touchCounterNow() override;
};

// Constructs and registers the one process-lifetime UiIntegration instance
// via Ui::Integration::Set(), and calls style::StartManager() (font
// registration + running every registered .style module, so every st::
// value - including plain int/pixel fields like st::buttonRadius, not just
// colors - actually gets its real runtime value instead of a zero-
// initialized default). Must run before constructing any lib_ui-backed
// widget (see main_qt.cpp): without style::StartManager(), Ui::RoundButton's
// fallback "st.radius ? st.radius : st::buttonRadius" (buttons.cpp) reads
// st::buttonRadius as 0 too, producing a degenerate zero-size corner image
// that fails an internal Assert() inside style::colorizeImage() -
// confirmed via a symbolized crash (cdb) as the actual cause of zkgram.exe
// crashing a few seconds after launch. Call InstallUiRuntime() once, from
// main(), and StopUiRuntime() before the process exits. argc/argv are
// forwarded to base::Integration's own constructor (see BaseIntegration
// above), which uses them to resolve the executable's own path.
void InstallUiRuntime(int argc, char** argv);
void StopUiRuntime();

}  // namespace zkgram::ui::qt
