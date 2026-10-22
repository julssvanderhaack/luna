#include "smoothscroll.h"
#include "settings.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QScrollBar>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kDurationMs = 180;

// One animation per scroll bar, created on its first wheel and reused: it
// only ticks while it runs, so an idle window costs nothing.
QVariantAnimation *animationFor(QScrollBar *bar)
{
    if (auto *a = bar->findChild<QVariantAnimation *>(QString(), Qt::FindDirectChildrenOnly)) return a;
    auto *a = new QVariantAnimation(bar);
    a->setDuration(kDurationMs);
    a->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(a, &QVariantAnimation::valueChanged, bar, [bar](const QVariant &v) { bar->setValue(qRound(v.toDouble())); });
    return a;
}
}

bool SmoothScroll::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() != QEvent::Wheel) return false;
    auto *we = static_cast<QWheelEvent *>(e);
    const Qt::KeyboardModifiers mods = we->modifiers() & ~Qt::KeypadModifier;
    if (mods != Qt::NoModifier && mods != Qt::ShiftModifier) return false;   // Ctrl = zoom, etc.
    // Touchpads send pixels: Qt would turn them into "lines" of a whole
    // row (139 px in the icon view) and fly; here they follow the finger.
    const bool touch = !we->pixelDelta().isNull();
    if (!touch && (we->phase() != Qt::NoScrollPhase || !Settings::get().smoothScroll)) return false;

    auto *w = qobject_cast<QWidget *>(o);
    auto *area = w ? qobject_cast<QAbstractScrollArea *>(w->parentWidget()) : nullptr;
    if (!area || area->viewport() != w) return false;

    // Item views scroll whole rows by default; the bars must count pixels
    // for the animation to have anything to interpolate (column view
    // columns are created later, so this is done here and not up front).
    if (auto *iv = qobject_cast<QAbstractItemView *>(area); iv && iv->verticalScrollMode() != QAbstractItemView::ScrollPerPixel) {
        iv->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        iv->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    }

    QPoint d = touch ? we->pixelDelta() : we->angleDelta();
    if (mods == Qt::ShiftModifier) d = d.transposed();
    QScrollBar *bar = nullptr;
    int delta = 0;
    auto scrollable = [](QScrollBar *b) { return b->maximum() > b->minimum(); };
    if (d.y() && scrollable(area->verticalScrollBar())) bar = area->verticalScrollBar(), delta = d.y();
    else if (d.y() && scrollable(area->horizontalScrollBar())) bar = area->horizontalScrollBar(), delta = d.y();   // compact view wraps sideways
    else if (d.x() && scrollable(area->horizontalScrollBar())) bar = area->horizontalScrollBar(), delta = d.x();
    if (!bar) return touch;   // nothing to scroll: don't let Qt bubble pixels up either
    const Settings &st = Settings::get();

    if (touch) {
        if (auto *a = bar->findChild<QVariantAnimation *>(QString(), Qt::FindDirectChildrenOnly)) a->stop();
        // Keep the fraction the slower speeds leave behind, or slow
        // gestures would round to nothing.
        const double move = delta * st.touchSpeed / 100.0 + bar->property("lunaTouchRest").toDouble();
        const int whole = int(move);
        bar->setProperty("lunaTouchRest", move - whole);
        bar->setValue(bar->value() - whole);
        return true;
    }

    // wheelScrollLines "lines" per notch, as Qt does, but a line is kept
    // between one and two text lines: icon rows and compact columns have a
    // step of a whole row/column and would jump 400 px per notch.
    const int text = area->fontMetrics().height();
    const int line = std::clamp(bar->singleStep(), text, 2 * text);
    const double pixels = delta / 120.0 * QApplication::wheelScrollLines() * line * st.wheelSpeed / 100.0;

    QVariantAnimation *a = animationFor(bar);
    const bool running = a->state() == QAbstractAnimation::Running;
    const double from = running ? a->currentValue().toDouble() : bar->value();
    double target = (running ? a->endValue().toDouble() : bar->value()) - pixels;
    target = std::clamp(target, double(bar->minimum()), double(bar->maximum()));
    a->stop();
    if (std::abs(target - from) < 0.5) return true;
    // A stopped animation sits at its end: changing the values would emit
    // the new end at once and the bar would flash there before starting.
    a->blockSignals(true);
    a->setStartValue(from);
    a->setEndValue(target);
    a->blockSignals(false);
    a->start();
    return true;
}
