// Animated mouse-wheel scrolling for every scroll area of the app. Each
// wheel notch moves the target and a short eased animation carries the
// scroll bar there; touchpads follow the finger pixel by pixel.
#pragma once
#include <QObject>

class SmoothScroll : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
};
