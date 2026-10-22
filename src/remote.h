// Remote locations through GVFS (sftp, ftp, ftps, smb, nfs): "gio mount"
// with interactive prompts, then browsed via the FUSE bridge in
// /run/user/UID/gvfs like any local folder.
#pragma once
#include <QDialog>
#include <QObject>
#include <functional>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QSpinBox;

class Remote : public QObject {
    Q_OBJECT
public:
    struct Mount {
        QString label, uri, localPath, scheme;
    };
    static Remote &get();
    static QString gvfsRoot();
    static bool isRemotePath(const QString &path);
    static QString describe(const QString &mountDir, QString *scheme = nullptr, QString *uri = nullptr);
    QList<Mount> mounts() const;
    // Mounts (if needed) and calls back with the local FUSE path, or empty on failure.
    void connectTo(const QString &uri, QWidget *parent, std::function<void(const QString &localPath)> done);
    void unmount(const QString &uri, QWidget *parent);
    QStringList recent() const;

signals:
    void mountsChanged();

private:
    void addRecent(const QString &uri);
    QString localPathFor(const QString &uri) const;
};

class ConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectDialog(QWidget *parent = nullptr);
    QString uri() const;

private:
    void fromUri(const QString &uri);
    void updatePort();
    QComboBox *m_proto;
    QLineEdit *m_host, *m_user, *m_folder, *m_share;
    QSpinBox *m_port;
    QCheckBox *m_anon;
    QListWidget *m_recent;
};
