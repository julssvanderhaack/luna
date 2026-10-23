#include "remote.h"
#include "icons.h"
#include "settings.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <unistd.h>

namespace {
const QHash<QString, QString> &protoNames()
{
    static const QHash<QString, QString> n = {{"sftp", "SSH"}, {"ftp", "FTP"}, {"ftps", "FTPS"}, {"ftpis", "FTPS"},
                                               {"smb", "SMB"}, {"smb-share", "SMB"}, {"nfs", "NFS"}};
    return n;
}
}

Remote &Remote::get()
{
    static Remote r;
    return r;
}

QString Remote::gvfsRoot() { return QString("/run/user/%1/gvfs").arg(getuid()); }

// gvfsd starts the FUSE bridge only once, at login; if it ever exits, "gio mount"
// still works (Nautilus talks to GIO directly) but /run/user/UID/gvfs stays an
// empty folder and there is nothing for Luna to browse.
bool Remote::fuseRunning()
{
    QFile f("/proc/self/mounts");
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray needle = ' ' + gvfsRoot().toUtf8() + " fuse.gvfsd-fuse ";
    return f.readAll().contains(needle);
}

bool Remote::ensureFuse()
{
    if (fuseRunning()) return true;
    QString bin;
    for (const char *c : {"/usr/lib/gvfsd-fuse", "/usr/libexec/gvfsd-fuse", "/usr/lib/gvfs/gvfsd-fuse", "/usr/libexec/gvfs/gvfsd-fuse"})
        if (QFileInfo(QString::fromLatin1(c)).isExecutable()) {
            bin = QString::fromLatin1(c);
            break;
        }
    if (bin.isEmpty()) return false;
    QDir().mkpath(gvfsRoot());
    // As its own user unit so it outlives Luna's app scope
    if (QProcess::execute("systemd-run", {"--user", "--collect", "--quiet", "--unit=luna-gvfsd-fuse", bin, "-f", gvfsRoot()}) != 0)
        QProcess::startDetached(bin, {gvfsRoot()});
    QElapsedTimer t;
    t.start();
    while (!fuseRunning() && t.elapsed() < 3000) QThread::msleep(50);
    // Its root lists nothing until it has asked gvfsd for the mounts
    QTimer::singleShot(600, &get(), [] { emit get().mountsChanged(); });
    return fuseRunning();
}

bool Remote::isRemotePath(const QString &path)
{
    static const QString prefix = gvfsRoot() + '/';   // asked for every painted item
    return path.startsWith(prefix);
}

// "sftp:host=example.org,user=julio,port=2222" -> "julio@example.org (SSH)"
QString Remote::describe(const QString &dir, QString *scheme, QString *uri)
{
    const int colon = dir.indexOf(':');
    const QString type = colon < 0 ? dir : dir.left(colon);
    QHash<QString, QString> kv;
    for (const QString &part : dir.mid(colon + 1).split(',', Qt::SkipEmptyParts)) {
        int eq = part.indexOf('=');
        if (eq > 0) kv.insert(part.left(eq), QUrl::fromPercentEncoding(part.mid(eq + 1).toUtf8()));
    }
    QString host = kv.value("host", kv.value("server"));
    QString user = kv.value("user");
    QString who = (user.isEmpty() ? QString() : user + '@') + host;
    if (type == "smb-share") who += "/" + kv.value("share");
    if (scheme) *scheme = type;
    if (uri) {
        QString s = type == "smb-share" ? "smb" : type;
        *uri = s + "://" + (user.isEmpty() ? QString() : user + '@') + host + (kv.contains("port") ? ":" + kv.value("port") : QString()) +
               (type == "smb-share" ? "/" + kv.value("share") : QString()) + "/";
    }
    return QString("%1 (%2)").arg(who, protoNames().value(type, type.toUpper()));
}

QList<Remote::Mount> Remote::mounts() const
{
    QList<Mount> out;
    static bool fuseTried = false;   // also lists mounts made from Nautilus
    if (!fuseTried) {
        fuseTried = true;
        ensureFuse();
    }
    QDir d(gvfsRoot());
    for (const QString &name : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        Mount m;
        m.label = describe(name, &m.scheme, &m.uri);
        m.localPath = d.filePath(name);
        out << m;
    }
    return out;
}

QStringList Remote::recent() const { return QSettings("luna", "luna").value("remote/recent").toStringList(); }

void Remote::addRecent(const QString &uri)
{
    QSettings q("luna", "luna");
    QStringList r = q.value("remote/recent").toStringList();
    r.removeAll(uri);
    r.prepend(uri);
    q.setValue("remote/recent", r.mid(0, 12));
}

QString Remote::localPathFor(const QString &uri) const
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C");
    p.setProcessEnvironment(env);
    p.start("gio", {"info", uri});
    p.waitForFinished(8000);
    for (const QString &line : QString::fromUtf8(p.readAllStandardOutput()).split('\n')) {
        if (line.startsWith("local path:")) return line.mid(11).trimmed();
    }
    return {};
}

void Remote::connectTo(const QString &uri0, QWidget *parent, std::function<void(const QString &)> done)
{
    QString uri = uri0.trimmed();
    if (!uri.contains("://")) uri = "sftp://" + uri;
    QUrl url(uri);
    if (!url.isValid() || url.host().isEmpty()) {
        QMessageBox::warning(parent, "Conectar", QString::fromUtf8("La dirección «%1» no es válida.").arg(uri0));
        done({});
        return;
    }
    if (!ensureFuse()) {
        QMessageBox::warning(parent, "Conectar",
                             QString::fromUtf8("No se pudo poner en marcha gvfsd-fuse, el puente de GVFS que Luna usa para abrir "
                                               "carpetas remotas en %1.").arg(gvfsRoot()));
        done({});
        return;
    }
    // Already mounted?
    const QString existing = localPathFor(uri);
    if (!existing.isEmpty() && QFileInfo(existing).exists()) {
        addRecent(uri);
        done(existing);
        return;
    }
    auto *proc = new QProcess(parent);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C");   // English prompts we can recognise
    proc->setProcessEnvironment(env);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    auto buffer = std::make_shared<QString>();
    auto question = std::make_shared<QString>();
    QPointer<QWidget> owner(parent);
    connect(proc, &QProcess::readyRead, proc, [=] {
        *buffer += QString::fromUtf8(proc->readAll());
        auto answer = [&](const QString &s) {
            proc->write((s + "\n").toUtf8());
            buffer->clear();
        };
        const QString b = buffer->trimmed();
        if (b.endsWith("User:")) {
            bool ok;
            QString u = QInputDialog::getText(owner, "Conectar", QString::fromUtf8("Usuario para %1:").arg(url.host()), QLineEdit::Normal,
                                              qEnvironmentVariable("USER"), &ok);
            if (!ok) proc->kill();
            else answer(u);
        } else if (b.endsWith("Domain:")) {
            bool ok;
            QString dmn = QInputDialog::getText(owner, "Conectar", "Dominio (grupo de trabajo):", QLineEdit::Normal, "WORKGROUP", &ok);
            if (!ok) proc->kill();
            else answer(dmn);
        } else if (b.endsWith("Password:")) {
            bool ok;
            QString pw = QInputDialog::getText(owner, "Conectar",
                                               QString::fromUtf8("Contraseña de %1:").arg(url.userName().isEmpty() ? url.host() : url.userName() + "@" + url.host()),
                                               QLineEdit::Password, {}, &ok);
            if (!ok) proc->kill();
            else answer(pw);
        } else if (b.endsWith("Choice:")) {
            // A question with numbered answers (typically the SSH host key)
            QStringList lines = b.split('\n');
            QStringList choices;
            QString text;
            static const QRegularExpression choiceRe("^\\[(\\d+)\\]\\s*(.*)$");
            for (const QString &l : lines) {
                auto m = choiceRe.match(l.trimmed());
                if (m.hasMatch()) choices << m.captured(2);
                else if (!l.trimmed().endsWith("Choice:")) text += l + "\n";
            }
            QMessageBox box(owner);
            box.setWindowTitle("Conectar");
            box.setText(text.trimmed().isEmpty() ? QString::fromUtf8("El servidor hace una pregunta:") : text.trimmed());
            QList<QAbstractButton *> buttons;
            for (const QString &c : choices) {
                QString label = c;
                if (c == "Log In Anyway") label = QString::fromUtf8("Conectar de todos modos");
                else if (c == "Cancel Login" || c == "Cancel") label = "Cancelar";
                buttons << box.addButton(label, QMessageBox::AcceptRole);
            }
            box.exec();
            int idx = buttons.indexOf(box.clickedButton());
            answer(QString::number(idx < 0 ? choices.size() : idx + 1));
        }
    });
    connect(proc, &QProcess::finished, proc, [=](int code) {
        const QString out = buffer->trimmed();
        proc->deleteLater();
        // The daemon answers in the user's language whatever LC_ALL says
        static const QRegularExpression alreadyRe(QString::fromUtf8("already mounted|ya está montad"), QRegularExpression::CaseInsensitiveOption);
        if (code != 0 && !out.contains(alreadyRe)) {
            QString msg = out.section('\n', -1).trimmed().section(": ", -1);
            if (msg.isEmpty()) msg = QString::fromUtf8("No se pudo conectar.");
            QMessageBox::warning(owner, "Conectar", QString::fromUtf8("No se pudo conectar con %1:\n%2").arg(url.host(), msg));
            done({});
            return;
        }
        addRecent(uri);
        emit mountsChanged();
        const QString path = localPathFor(uri);
        if (path.isEmpty() || !QFileInfo(path).exists()) {
            QMessageBox::warning(owner, "Conectar",
                                 QString::fromUtf8("Se ha conectado con %1, pero su carpeta no aparece en %2.").arg(url.host(), gvfsRoot()));
            done({});
            return;
        }
        done(path);
    });
    proc->start("gio", {"mount", uri});
}

void Remote::unmount(const QString &uri, QWidget *parent)
{
    auto *p = new QProcess(parent);
    connect(p, &QProcess::finished, p, [this, p, parent](int code) {
        p->deleteLater();
        if (code != 0) QMessageBox::warning(parent, "Desconectar", QString::fromUtf8("No se pudo desconectar:\n") + QString::fromUtf8(p->readAllStandardError()).trimmed());
        emit mountsChanged();
    });
    p->start("gio", {"mount", "-u", uri});
}

// ----------------------------------------------------------- ConnectDialog
ConnectDialog::ConnectDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Conectar con un servidor");
    resize(480, 480);
    auto *v = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_proto = new QComboBox;
    m_proto->addItem("SSH / SFTP", "sftp");
    m_proto->addItem("FTP", "ftp");
    m_proto->addItem("FTPS (FTP con TLS)", "ftps");
    m_proto->addItem("Carpeta compartida de Windows (SMB)", "smb");
    m_proto->addItem("NFS", "nfs");
    form->addRow("Tipo:", m_proto);
    m_host = new QLineEdit;
    m_host->setPlaceholderText("servidor.ejemplo.com o 192.168.1.10");
    form->addRow("Servidor:", m_host);
    m_port = new QSpinBox;
    m_port->setRange(0, 65535);
    m_port->setSpecialValueText("predeterminado");
    form->addRow("Puerto:", m_port);
    m_share = new QLineEdit;
    m_share->setPlaceholderText("nombre del recurso compartido");
    form->addRow("Recurso:", m_share);
    m_user = new QLineEdit;
    m_user->setPlaceholderText(qEnvironmentVariable("USER"));
    form->addRow("Usuario:", m_user);
    m_anon = new QCheckBox(QString::fromUtf8("Acceso anónimo"));
    form->addRow("", m_anon);
    m_folder = new QLineEdit;
    m_folder->setPlaceholderText("/ (opcional)");
    form->addRow("Carpeta:", m_folder);
    v->addLayout(form);
    auto *note = new QLabel(QString::fromUtf8("La contraseña se pide al conectar. También puedes escribir la dirección "
                                              "directamente en la barra de ruta: sftp://usuario@servidor/carpeta"));
    note->setProperty("role", "dim");
    note->setWordWrap(true);
    v->addWidget(note);
    v->addWidget(new QLabel("<b>Recientes</b>"));
    m_recent = new QListWidget;
    for (const QString &r : Remote::get().recent()) {
        auto *it = new QListWidgetItem(Icons::get().place("network"), r);
        m_recent->addItem(it);
    }
    v->addWidget(m_recent, 1);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText("Conectar");
    bb->button(QDialogButtonBox::Cancel)->setText("Cancelar");
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        if (!m_host->text().trimmed().isEmpty()) accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_recent, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { fromUri(it->text()); });
    connect(m_recent, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        fromUri(it->text());
        accept();
    });
    connect(m_proto, &QComboBox::currentIndexChanged, this, &ConnectDialog::updatePort);
    updatePort();
}

void ConnectDialog::updatePort()
{
    const QString p = m_proto->currentData().toString();
    m_share->setEnabled(p == "smb" || p == "nfs");
    m_anon->setEnabled(p == "ftp" || p == "ftps" || p == "smb");
    m_user->setEnabled(p != "nfs");
}

void ConnectDialog::fromUri(const QString &uri)
{
    QUrl u(uri);
    int i = m_proto->findData(u.scheme());
    if (i >= 0) m_proto->setCurrentIndex(i);
    m_host->setText(u.host());
    m_port->setValue(u.port() > 0 ? u.port() : 0);
    m_user->setText(u.userName());
    QString path = u.path();
    if (u.scheme() == "smb" || u.scheme() == "nfs") {
        QStringList parts = path.split('/', Qt::SkipEmptyParts);
        m_share->setText(parts.value(0));
        path = parts.size() > 1 ? "/" + parts.mid(1).join('/') : QString();
    }
    m_folder->setText(path == "/" ? QString() : path);
}

QString ConnectDialog::uri() const
{
    QUrl u;
    const QString scheme = m_proto->currentData().toString();
    u.setScheme(scheme);
    u.setHost(m_host->text().trimmed());
    if (m_port->value() > 0) u.setPort(m_port->value());
    if (!m_anon->isChecked() && !m_user->text().trimmed().isEmpty() && scheme != "nfs") u.setUserName(m_user->text().trimmed());
    if (m_anon->isChecked() && (scheme == "ftp" || scheme == "ftps")) u.setUserName("anonymous");
    QString path = m_folder->text().trimmed();
    if (scheme == "smb" || scheme == "nfs") path = "/" + m_share->text().trimmed() + (path.startsWith('/') ? path : path.isEmpty() ? QString() : "/" + path);
    if (!path.startsWith('/')) path.prepend('/');
    u.setPath(path);
    return u.toString();
}
