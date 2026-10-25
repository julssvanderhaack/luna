#include "discovery.h"
#include <QCoreApplication>
#include <QSysInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <cctype>

namespace {
// avahi-browse escapes names as \DDD (decimal): "Luna\032en\032Pixel"
QString unescape(const QByteArray &raw)
{
    QByteArray out;
    for (int i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 3 < raw.size() && std::isdigit(uchar(raw[i + 1])) && std::isdigit(uchar(raw[i + 2])) && std::isdigit(uchar(raw[i + 3]))) {
            out += char(raw.mid(i + 1, 3).toInt());
            i += 3;
        } else if (raw[i] == '\\' && i + 1 < raw.size()) {
            out += raw[++i];
        } else {
            out += raw[i];
        }
    }
    return QString::fromUtf8(out);
}
}

QString Discovery::Service::uri() const
{
    QString u = "sftp://";
    if (!user.isEmpty()) u += user + '@';
    return u + address + (port == 22 ? QString() : ':' + QString::number(port)) + '/';
}

Discovery &Discovery::get()
{
    static Discovery *d = new Discovery;   // lives as long as the app
    return *d;
}

QList<Discovery::Service> Discovery::services() const { return m_services; }

Discovery::Discovery() : QObject(QCoreApplication::instance())
{
    if (QStandardPaths::findExecutable("avahi-browse").isEmpty()) return;
    m_proc = new QProcess(this);
    // -p parsable, -r resolve (address and TXT), -k no database lookups;
    // without -t it keeps running and reports arrivals and departures.
    m_proc->start("avahi-browse", {"-p", "-r", "-k", "_sftp-ssh._tcp"});
    connect(m_proc, &QProcess::readyReadStandardOutput, this, [this] {
        m_pending += m_proc->readAllStandardOutput();
        for (qsizetype nl; (nl = m_pending.indexOf('\n')) >= 0;) {
            const QByteArray line = m_pending.left(nl);
            m_pending.remove(0, nl + 1);
            parse(line);
        }
    });
}

void Discovery::parse(const QByteArray &line)
{
    // "=;wlan0;IPv4;Luna\032en\032Pixel;_sftp-ssh._tcp;local;pixel.local;192.168.1.5;2222;"luna=1" "user=julss""
    // "-;wlan0;IPv4;Luna\032en\032Pixel;_sftp-ssh._tcp;local"
    const QList<QByteArray> f = line.split(';');
    if (f.size() < 6 || f[2] != "IPv4") return;
    const QString name = unescape(f[3]);
    const int known = int(std::find_if(m_services.cbegin(), m_services.cend(), [&](const Service &s) { return s.name == name; }) - m_services.cbegin());
    if (f[0] == "-") {
        if (known < m_services.size()) {
            m_services.removeAt(known);
            emit changed();
        }
        return;
    }
    if (f[0] != "=" || f.size() < 9) return;
    // this machine's own sshd, if avahi announces it, is not a place to go
    const QString host = QString::fromUtf8(f[6]);
    if (host.compare(QSysInfo::machineHostName() + ".local", Qt::CaseInsensitive) == 0) return;
    Service s;
    s.name = name;
    s.address = QString::fromUtf8(f[7]);
    s.port = f[8].toInt();
    const QString txt = QString::fromUtf8(f.mid(9).join(';'));
    static const QRegularExpression user("\"user=([^\"]*)\"");
    if (auto m = user.match(txt); m.hasMatch()) s.user = m.captured(1);
    s.luna = txt.contains("\"luna=1\"");
    if (known < m_services.size()) {
        const Service &old = m_services[known];
        if (old.address == s.address && old.port == s.port && old.user == s.user) return;
        m_services[known] = s;
    } else {
        m_services << s;
    }
    emit changed();
}
