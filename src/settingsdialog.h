// Preferences window: every change applies live and is saved immediately.
#pragma once
#include <QDialog>

class QFormLayout;
class QListWidget;
class QStackedWidget;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    void showPage(const QString &title);

private:
    QFormLayout *page(const QString &title, const QString &glyph);
    QListWidget *m_nav;
    QStackedWidget *m_pages;
};
