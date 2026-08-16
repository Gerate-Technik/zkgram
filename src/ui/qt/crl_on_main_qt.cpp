// zkgram patch, not a vendored desktop-app file.
//
// crl::on_main_update_requests() is declared in
// third_party/desktop-app/lib_crl/crl/crl_on_main.h but has no definition
// anywhere in lib_crl (checked against a fresh clone of the upstream repo
// too, not just the vendored copy - genuinely missing upstream, not a
// version-skew issue between the vendored libraries). lib_ui's
// Ui::Animations::Manager and Ui::Toast both need it at link time
// (crl::on_main_update_requests() | rpl::on_next(...) drives their per-frame
// update tick), so building against this lib_crl without it fails with an
// unresolved external symbol.
//
// zkgram always has a Qt event loop running whenever lib_ui is linked in
// (ZKGRAM_VENDOR_LIB_UI implies ZKGRAM_UI=qt). One process-lifetime QTimer
// (zero interval, so Qt only fires it when there is no other pending event
// to process first - "main queue went idle, allow one more update pass")
// feeds a single shared rpl::event_stream that every caller's producer
// subscribes to.
//
// An earlier version created a brand-new QTimer per subscription, owned by
// a std::shared_ptr captured into both the QTimer::timeout connection and
// the rpl::lifetime cleanup callback - that connection's own lambda held a
// shared_ptr to the very QTimer it was connected to, so the timer was never
// actually destroyed by its lifetime callback (a real cycle), and stopping
// it from the lifetime callback while a timeout was already queued on the
// event loop was a genuine use-after-free race against Ui::Toast/Animations
// objects that had already been destroyed - this is what crashed zkgram.exe
// a few seconds after launch (STATUS_ACCESS_VIOLATION, fault inside
// zkgram.exe itself, confirmed via Windows Event Log). rpl::event_stream
// already handles per-subscriber lifetime correctly on its own; the earlier
// code was solving a problem rpl solves better, by hand, wrongly.
#include "base/basic_types.h"

#include <crl/crl_on_main.h>
#include <rpl/rpl.h>

#include <QTimer>

namespace crl {
namespace {

rpl::event_stream<>& MainUpdateStream() {
    static rpl::event_stream<> stream;
    return stream;
}

void EnsureTimerStarted() {
    static QTimer* timer = [] {
        auto* result = new QTimer();
        result->setTimerType(Qt::CoarseTimer);
        result->setInterval(0);
        QObject::connect(result, &QTimer::timeout, [] { MainUpdateStream().fire({}); });
        result->start();
        return result;
    }();
    (void)timer;
}

}  // namespace

rpl::producer<> on_main_update_requests() {
    EnsureTimerStarted();
    return MainUpdateStream().events();
}

}  // namespace crl
