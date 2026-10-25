// Animated mouse-wheel scrolling for every scroll area of the app. Each
// wheel notch moves the target and a short eased animation carries the
// scroll bar there; touchpads follow the finger pixel by pixel, and a
// two-finger swipe sideways on a file view goes back/forward.
#pragma once
#include <QObject>

class QAbstractScrollArea;
class QWheelEvent;

class SmoothScroll : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    bool swipe(QWidget *viewport, QAbstractScrollArea *area, QWheelEvent *we, bool scrollsSideways);
    double m_swipeX = 0, m_swipeY = 0;
    qint64 m_swipeLast = 0;   // ms, to tell gestures apart when there are no phases
    bool m_swipeDone = false;
};
