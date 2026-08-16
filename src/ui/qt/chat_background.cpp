#include "ui/qt/chat_background.hpp"

#include <QRandomGenerator>
#include <algorithm>
#include <cmath>

namespace zkgram::ui::qt {

namespace {

// Eight positions the 4 control points travel between. Phase N's active
// points are orbit positions N, N+2, N+4, N+6 (mod 8) - advancing phase by
// 1 keeps half the points where they were and rotates the other half one
// step around the circle, producing the characteristic "breathing" shift
// instead of every point jumping to an unrelated spot each time.
//
// A working approximation, not verified against the exact constants the
// current Telegram Desktop release ships (see ui/chat/chat_theme.cpp in a
// real tdesktop checkout, not vendored here) - visually correct, not
// guaranteed bit-identical.
constexpr std::array<QPointF, 8> kOrbit = {
    QPointF(0.80, 0.10), QPointF(0.60, 0.20), QPointF(0.35, 0.25), QPointF(0.25, 0.60),
    QPointF(0.20, 0.90), QPointF(0.40, 0.80), QPointF(0.65, 0.75), QPointF(0.75, 0.40),
};

// Side of the square buffer the gradient is actually computed in before
// upscaling - 64x64 is 4096 pixels versus ~2 million for a 1920x1080
// window. The gradient is low-frequency by construction (inverse-distance
// weighting with no sharp edges), so upscaling with bilinear/smooth
// interpolation is visually indistinguishable from computing it at full
// resolution, at roughly 1/500th the cost - doing this per-pixel at
// window resolution on every paint would be the real-time-graphics
// equivalent of the O(n^2) sidebar/history bugs already fixed elsewhere
// in this codebase.
constexpr int kSmallSize = 64;

// How fast a control point's influence falls off with distance. 4 is what
// gives Telegram's characteristic soft "blobs" - 2 blurs everything into
// mush, 8 produces hard-edged regions instead of a gradient.
constexpr double kFalloff = 4.0;

}  // namespace

GradientPoints GradientPointsForPhase(int phase) {
    GradientPoints result;
    int wrapped = ((phase % 8) + 8) % 8;
    for (int i = 0; i < 4; ++i) {
        result[i] = kOrbit[(wrapped + i * 2) % 8];
    }
    return result;
}

QImage GenerateFreeformGradient(const GradientColors& colors, const GradientPoints& points, QSize size) {
    QImage small(kSmallSize, kSmallSize, QImage::Format_RGB32);

    // Colors pulled apart into doubles once, not per-pixel - QColor::redF()
    // etc. inside the double loop below showed up in profiling.
    std::array<double, 4> cr{}, cg{}, cb{};
    for (int i = 0; i < 4; ++i) {
        cr[i] = colors[i].redF();
        cg[i] = colors[i].greenF();
        cb[i] = colors[i].blueF();
    }

    for (int y = 0; y < kSmallSize; ++y) {
        auto* line = reinterpret_cast<QRgb*>(small.scanLine(y));
        const double ny = double(y) / (kSmallSize - 1);
        for (int x = 0; x < kSmallSize; ++x) {
            const double nx = double(x) / (kSmallSize - 1);

            double r = 0.0, g = 0.0, b = 0.0, weightSum = 0.0;
            for (int i = 0; i < 4; ++i) {
                const double dx = nx - points[i].x();
                const double dy = ny - points[i].y();
                double distance = std::sqrt(dx * dx + dy * dy);
                // Guards against dividing by zero exactly at a control point.
                distance = std::max(distance, 1e-4);
                const double weight = 1.0 / std::pow(distance, kFalloff);

                weightSum += weight;
                r += cr[i] * weight;
                g += cg[i] * weight;
                b += cb[i] * weight;
            }

            line[x] = qRgb(int(255.0 * r / weightSum + 0.5), int(255.0 * g / weightSum + 0.5),
                            int(255.0 * b / weightSum + 0.5));
        }
    }

    QImage result = small.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // Dithering: a 64x64 gradient stretched over a large area shows
    // visible banding (adjacent pixels differing by exactly one integer
    // value reads as stripes to the eye, not a smooth ramp). Random noise
    // of amplitude +-2 breaks the regularity that makes banding visible,
    // without being large enough to read as noise itself - the same
    // technique real Telegram uses for the same reason.
    result = result.convertToFormat(QImage::Format_RGB32);
    auto* rng = QRandomGenerator::global();
    for (int y = 0; y < result.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            const int noise = int(rng->bounded(5)) - 2;  // -2..+2
            line[x] = qRgb(qBound(0, qRed(line[x]) + noise, 255), qBound(0, qGreen(line[x]) + noise, 255),
                            qBound(0, qBlue(line[x]) + noise, 255));
        }
    }
    return result;
}

}  // namespace zkgram::ui::qt
