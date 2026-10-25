// SFTP servers announced on the local network (mDNS, _sftp-ssh._tcp) — the
// phone's Luna Android server among them. One long-running avahi-browse for
// the whole app: it only wakes when something appears or goes away.
#pragma once
#include <QList>
#include <QObject>
#include <QString>

class QProcess;

class Discovery : public QObject {
    Q_OBJECT
public:
    struct Service {
        QString name;      // "Luna en Pixel 8"
        QString address;   // IPv4
        int port = 22;
        QString user;      // from the TXT record, may be empty
        bool luna = false; // announced by Luna Android
        QString uri() const;   // sftp://user@address:port/
    };
    static Discovery &get();
    QList<Service> services() const;

signals:
    void changed();

private:
    Discovery();
    void parse(const QByteArray &line);
    QProcess *m_proc = nullptr;
    QByteArray m_pending;
    QList<Service> m_services;
};
