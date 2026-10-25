// Properties window: general info, permissions editor, media metadata
// (libmediainfo + exiv2) and the default application for the file type.
#pragma once
#include <QDialog>
#include <QPair>
#include <QStringList>

// A few lines of audio/video facts (duration, resolution, artist…) for the
// quick preview. Slow-ish (libmediainfo reads the file): call it off the GUI thread.
QList<QPair<QString, QString>> mediaSummary(const QString &path);

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QVBoxLayout;

class PropertiesDialog : public QDialog {
    Q_OBJECT
public:
    PropertiesDialog(const QStringList &paths, QWidget *parent = nullptr);

private:
    QWidget *generalTab();
    QWidget *permissionsTab();
    QWidget *metadataTab();
    QWidget *openWithTab();
    void loadPermissions();
    void applyMode(quint32 set, quint32 clear);

    QStringList m_paths;
    bool m_single, m_isDir = false;
    QLabel *m_size = nullptr;
    QCheckBox *m_bits[3][3] = {};
    QCheckBox *m_suid = nullptr, *m_sgid = nullptr, *m_sticky = nullptr, *m_exec = nullptr, *m_recursive = nullptr;
    QLineEdit *m_octal = nullptr;
    QComboBox *m_group = nullptr;
    bool m_updating = false;
};
