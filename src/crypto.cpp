#include "crypto.h"
#include "fileops.h"
#include "i18n.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <fcntl.h>
#include <unistd.h>

namespace {
QProcessEnvironment cEnv()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C");   // gpg's messages are parsed
    return env;
}

// gpg reads the secret from fd 3 (--passphrase-fd 3): a pipe whose read end
// the child gets as fd 3, so it never shows in the command line or a file.
// Call before start(); then sendSecret() right after it.
struct SecretPipe {
    int r = -1, w = -1;
};

SecretPipe attachSecret(QProcess *p)
{
    SecretPipe s;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return s;
    s.r = fds[0];
    s.w = fds[1];
    const int r = s.r;
    p->setChildProcessModifier([r] {
        if (r == 3) fcntl(3, F_SETFD, 0);   // keep it across exec
        else dup2(r, 3);                    // the copy is not close-on-exec
    });
    return s;
}

void sendSecret(SecretPipe &s, const QString &secret)
{
    if (s.w < 0) return;
    const QByteArray b = secret.toUtf8() + '\n';
    ssize_t off = 0;
    while (off < b.size()) {
        const ssize_t n = ::write(s.w, b.constData() + off, size_t(b.size() - off));
        if (n <= 0) break;
        off += n;
    }
    ::close(s.w);
    ::close(s.r);   // the child already has its fd 3
    s.w = s.r = -1;
}

QString lastLines(const QByteArray &err)
{
    QStringList lines = QString::fromUtf8(err).split('\n', Qt::SkipEmptyParts);
    for (QString &l : lines) l.remove(QRegularExpression("^gpg: "));
    return lines.mid(std::max<qsizetype>(0, lines.size() - 3)).join('\n');
}

// gpg's stderr (LC_ALL=C) in words the user knows.
QString explainFailure(const QByteArray &err)
{
    const QString e = QString::fromUtf8(err);
    if (e.contains("Bad session key") || e.contains("Bad passphrase") || e.contains("bad passphrase")) return i18n("La contraseña no es correcta.");
    if (e.contains("No secret key")) return i18n("No tienes la clave privada con la que se cifró.");
    if (e.contains("Operation cancelled") || e.contains("cancelled by user")) return i18n("Cancelado.");
    if (e.contains("no valid OpenPGP data")) return i18n("No es un archivo cifrado con GPG.");
    return lastLines(err);
}

QString signatureNote(const QByteArray &err)
{
    const QString e = QString::fromUtf8(err);
    static const QRegularExpression good("Good signature from \"([^\"]*)\"");
    if (auto m = good.match(e); m.hasMatch()) return i18n("Firma válida de %1").arg(m.captured(1));
    if (e.contains("BAD signature")) return i18n("⚠ ¡La firma NO es válida: el archivo se ha modificado!");
    if (e.contains("No public key")) return i18n("Firmado con una clave que no tienes");
    return {};
}

QString stripCryptoExt(const QString &name)
{
    for (const char *ext : {".gpg", ".pgp", ".asc"})
        if (name.endsWith(QLatin1String(ext), Qt::CaseInsensitive)) return name.chopped(4);
    return name;
}

const QRegularExpression &tarExt()
{
    static const QRegularExpression re("\\.(tar|tar\\.gz|tgz|tar\\.xz|txz|tar\\.zst|tar\\.bz2|tbz2)$", QRegularExpression::CaseInsensitiveOption);
    return re;
}
}

namespace Crypto {
bool haveGpg()
{
    static const bool have = !QStandardPaths::findExecutable("gpg").isEmpty();
    return have;
}

bool isEncrypted(const QString &path)
{
    return stripCryptoExt(QFileInfo(path).fileName()) != QFileInfo(path).fileName() && QFileInfo(path).isFile();
}

QList<Key> keys()
{
    auto run = [](const QString &what) {
        QProcess p;
        p.setProcessEnvironment(cEnv());
        p.start("gpg", {"--batch", "--with-colons", what});
        p.waitForFinished(10000);
        return QString::fromUtf8(p.readAllStandardOutput()).split('\n');
    };
    QSet<QString> mine;
    QString cur;
    for (const QString &l : run("--list-secret-keys")) {
        const QStringList f = l.split(':');
        if (f.value(0) == "sec") cur = "want";
        else if (f.value(0) == "fpr" && cur == "want") {
            mine.insert(f.value(9));
            cur.clear();
        }
    }
    QList<Key> out;
    Key k;
    bool inKey = false, haveFpr = false, haveUid = false;
    auto flush = [&] {
        if (inKey && haveFpr) out << k;
        k = Key();
        inKey = haveFpr = haveUid = false;
    };
    for (const QString &l : run("--list-keys")) {
        const QStringList f = l.split(':');
        const QString type = f.value(0);
        if (type == "pub") {
            flush();
            inKey = true;
            const QString validity = f.value(1);
            k.valid = !QString("rideo").contains(validity.left(1)) && f.value(11).contains('E');
            k.verified = validity == "f" || validity == "u";
            k.shortId = f.value(4).right(8);
        } else if (type == "fpr" && inKey && !haveFpr) {
            k.fingerprint = f.value(9);
            k.secret = mine.contains(k.fingerprint);
            haveFpr = true;
        } else if (type == "uid" && inKey && !haveUid) {
            k.uid = f.value(9).replace("\\x3a", ":");
            haveUid = true;
        }
    }
    flush();
    return out;
}

void encrypt(const EncryptJob &job, QObject *ctx, Done done)
{
    if (job.paths.isEmpty()) return;
    const QFileInfo first(job.paths[0]);
    const bool single = job.paths.size() == 1 && first.isFile();
    QStringList args{"--batch", "--yes", "--output", job.out};
    if (job.symmetric) {
        args << "--pinentry-mode" << "loopback" << "--passphrase-fd" << "3" << "--symmetric" << "--cipher-algo" << "AES256";
    } else {
        args << "--trust-model" << "always" << "--encrypt";   // the dialog already shows which keys are unverified
        for (const QString &r : job.recipients) args << "--recipient" << r;
        if (!job.signKey.isEmpty()) args << "--sign" << "--local-user" << job.signKey;
    }
    auto *gpg = new QProcess(ctx);
    gpg->setProcessEnvironment(cEnv());
    QProcess *tar = nullptr;
    if (single) {
        args << "--" << first.absoluteFilePath();
    } else {
        // tar streams straight into gpg: no plain copy ever reaches the disk
        tar = new QProcess(ctx);
        tar->setWorkingDirectory(first.absolutePath());
        tar->setStandardOutputProcess(gpg);
        QStringList names{"-cf", "-", "--"};
        for (const QString &p : job.paths) names << QFileInfo(p).fileName();
        tar->start("tar", names);
    }
    SecretPipe secret;
    if (job.symmetric) secret = attachSecret(gpg);
    const QString out = job.out;
    QObject::connect(gpg, &QProcess::finished, ctx, [gpg, tar, out, done](int code, QProcess::ExitStatus st) {
        bool ok = st == QProcess::NormalExit && code == 0;
        if (tar) {
            tar->waitForFinished(5000);
            ok = ok && tar->exitStatus() == QProcess::NormalExit && tar->exitCode() == 0;
        }
        const QByteArray err = gpg->readAllStandardError() + (tar ? tar->readAllStandardError() : QByteArray());
        gpg->deleteLater();
        if (tar) tar->deleteLater();
        if (!ok) QFile::remove(out);
        done(ok, ok ? i18n("Cifrado en «%1»").arg(QFileInfo(out).fileName()) : i18n("No se pudo cifrar: %1").arg(explainFailure(err)), ok ? out : QString());
    });
    gpg->start("gpg", args);
    if (job.symmetric) sendSecret(secret, job.passphrase);
}

bool needsPassphrase(const QString &file)
{
    QProcess p;
    p.setProcessEnvironment(cEnv());
    p.start("gpg", {"--batch", "--pinentry-mode", "cancel", "--list-packets", "--", file});
    p.waitForFinished(10000);
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    return out.contains("symkey enc packet") && !out.contains("pubkey enc packet");
}

void decrypt(const QString &file, const QString &destDir, const QString &passphrase, QObject *ctx, Done done)
{
    const QString base = stripCryptoExt(QFileInfo(file).fileName());
    const bool isTar = tarExt().match(base).hasMatch();
    QStringList args{"--batch", "--yes"};
    if (!passphrase.isEmpty()) args << "--pinentry-mode" << "loopback" << "--passphrase-fd" << "3";
    auto *gpg = new QProcess(ctx);
    gpg->setProcessEnvironment(cEnv());
    QProcess *untar = nullptr;
    QString made;
    if (isTar) {
        // the folder is named after the archive, never spilling into destDir
        QString folder = base;
        folder.remove(tarExt());
        made = destDir + '/' + FileOps::uniqueName(destDir, folder);
        QDir().mkpath(made);
        untar = new QProcess(ctx);
        gpg->setStandardOutputProcess(untar);
        untar->start("bsdtar", {"-xf", "-", "-C", made});
        args << "--decrypt" << "--" << file;
    } else {
        made = destDir + '/' + FileOps::uniqueName(destDir, base == QFileInfo(file).fileName() ? base + ".descifrado" : base);
        args << "--output" << made << "--decrypt" << "--" << file;
    }
    SecretPipe secret;
    if (!passphrase.isEmpty()) secret = attachSecret(gpg);
    QObject::connect(gpg, &QProcess::finished, ctx, [gpg, untar, made, isTar, done](int code, QProcess::ExitStatus st) {
        bool ok = st == QProcess::NormalExit && code == 0;
        if (untar) {
            untar->waitForFinished(30000);
            ok = ok && untar->exitStatus() == QProcess::NormalExit && untar->exitCode() == 0;
        }
        const QByteArray err = gpg->readAllStandardError();
        const QByteArray untarErr = untar ? untar->readAllStandardError() : QByteArray();
        gpg->deleteLater();
        if (untar) untar->deleteLater();
        if (!ok) {
            if (isTar) QDir(made).removeRecursively();
            else QFile::remove(made);
            done(false, i18n("No se pudo descifrar: %1").arg(code == 0 ? lastLines(untarErr) : explainFailure(err)), QString());
            return;
        }
        QString result = made;
        if (isTar) {
            // the archive of folder "fotos" holds "fotos/…": no fotos/fotos (Kleopatra does the same)
            const QFileInfoList in = QDir(made).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            if (in.size() == 1 && in[0].isDir() && !in[0].isSymLink() && in[0].fileName() == QFileInfo(made).fileName()) {
                const QString tmp = made + ".luna-" + QString::number(QCoreApplication::applicationPid());
                if (QDir().rename(made, tmp)) {
                    if (QDir().rename(tmp + '/' + in[0].fileName(), made)) QDir().rmdir(tmp);
                    else QDir().rename(tmp, made);
                }
            }
        }
        QString msg = i18n("Descifrado en «%1»").arg(QFileInfo(result).fileName());
        if (const QString sig = signatureNote(err); !sig.isEmpty()) msg += QString::fromUtf8("  ·  ") + sig;
        done(true, msg, result);
    });
    gpg->start("gpg", args);
    if (!passphrase.isEmpty()) sendSecret(secret, passphrase);
}

void createKey(const QString &name, const QString &email, const QString &passphrase, QObject *ctx, Done done)
{
    auto *gpg = new QProcess(ctx);
    gpg->setProcessEnvironment(cEnv());
    SecretPipe secret = attachSecret(gpg);
    const QString uid = email.isEmpty() ? name : QString("%1 <%2>").arg(name, email);
    QObject::connect(gpg, &QProcess::finished, ctx, [gpg, uid, done](int code, QProcess::ExitStatus st) {
        const bool ok = st == QProcess::NormalExit && code == 0;
        const QByteArray err = gpg->readAllStandardError();
        gpg->deleteLater();
        done(ok, ok ? i18n("Creado el par de claves de %1").arg(uid) : i18n("No se pudo crear la clave: %1").arg(lastLines(err)), QString());
    });
    gpg->start("gpg", {"--batch", "--pinentry-mode", "loopback", "--passphrase-fd", "3", "--quick-generate-key", uid, "default", "default", "3y"});
    sendSecret(secret, passphrase);
}

void importKey(const QString &file, QObject *ctx, Done done)
{
    auto *gpg = new QProcess(ctx);
    gpg->setProcessEnvironment(cEnv());
    QObject::connect(gpg, &QProcess::finished, ctx, [gpg, done](int code, QProcess::ExitStatus st) {
        const bool ok = st == QProcess::NormalExit && code == 0;
        const QByteArray err = gpg->readAllStandardError();
        gpg->deleteLater();
        static const QRegularExpression count("imported: (\\d+)");
        const auto m = count.match(QString::fromUtf8(err));
        done(ok, ok ? i18n("Claves importadas: %1").arg(m.hasMatch() ? m.captured(1) : QString("0")) : i18n("No se pudo importar: %1").arg(lastLines(err)), QString());
    });
    gpg->start("gpg", {"--batch", "--import", "--", file});
}

// ------------------------------------------------------------------ dialogs
namespace {
// Name, e-mail and password for a new key pair; runs createKey.
void newKeyDialog(QWidget *parent, std::function<void()> created)
{
    QDialog d(parent);
    d.setWindowTitle(i18n("Crear mi par de claves"));
    auto *form = new QFormLayout(&d);
    form->addRow(new QLabel(i18n("Con tu par de claves otros pueden cifrarte archivos que solo tú abres,\ny tú puedes firmar los tuyos. "
                                 "La contraseña protege la clave privada.")));
    auto *name = new QLineEdit(qEnvironmentVariable("USER"));
    auto *email = new QLineEdit;
    auto *pw = new QLineEdit;
    auto *pw2 = new QLineEdit;
    pw->setEchoMode(QLineEdit::Password);
    pw2->setEchoMode(QLineEdit::Password);
    form->addRow(i18n("Nombre:"), name);
    form->addRow(i18n("Correo:"), email);
    form->addRow(i18n("Contraseña:"), pw);
    form->addRow(i18n("Repítela:"), pw2);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(i18n("Crear"));
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, [&] {
        if (name->text().trimmed().isEmpty()) return;
        if (pw->text().isEmpty() || pw->text() != pw2->text()) {
            QMessageBox::warning(&d, d.windowTitle(), i18n("Las contraseñas no coinciden o están vacías."));
            return;
        }
        d.accept();
    });
    if (d.exec() != QDialog::Accepted) return;
    QWidget *w = parent;
    createKey(name->text().trimmed(), email->text().trimmed(), pw->text(), parent, [w, created](bool ok, const QString &msg, const QString &) {
        if (!ok) QMessageBox::warning(w, i18n("Crear mi par de claves"), msg);
        created();
    });
}
}

void encryptDialog(QWidget *parent, const QStringList &paths, const QString &destDir, Done done)
{
    if (paths.isEmpty()) return;
    if (!haveGpg()) {
        QMessageBox::warning(parent, i18n("Cifrar"), i18n("Hace falta GnuPG (gpg) para cifrar."));
        return;
    }
    const QFileInfo first(paths[0]);
    const bool single = paths.size() == 1 && first.isFile();
    // Kleopatra's names: file.ext.gpg, folder.tar.gpg, archive.tar.gpg
    const QString base = paths.size() == 1 ? first.fileName() : i18n("archivo");

    QDialog d(parent);
    d.setWindowTitle(i18n("Comprimir y cifrar"));
    d.resize(560, 520);
    auto *v = new QVBoxLayout(&d);
    v->addWidget(new QLabel(paths.size() == 1 ? i18n("Cifrar «%1»").arg(first.fileName()) : i18n("Cifrar %1 elementos").arg(paths.size())));
    auto *outRow = new QFormLayout;
    auto *outName = new QLineEdit(base + (single ? ".gpg" : ".tar.gpg"));
    outRow->addRow(i18n("Guardar como:"), outName);
    v->addLayout(outRow);

    auto *byPw = new QRadioButton(i18n("Con una contraseña"));
    auto *byKey = new QRadioButton(i18n("Para estas personas (con sus claves públicas)"));
    byPw->setChecked(true);
    v->addWidget(byPw);
    auto *pwBox = new QWidget;
    auto *pf = new QFormLayout(pwBox);
    pf->setContentsMargins(24, 0, 0, 0);
    auto *pw = new QLineEdit;
    auto *pw2 = new QLineEdit;
    pw->setEchoMode(QLineEdit::Password);
    pw2->setEchoMode(QLineEdit::Password);
    auto *show = new QCheckBox(i18n("Mostrar"));
    QObject::connect(show, &QCheckBox::toggled, &d, [pw, pw2](bool on) {
        pw->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        pw2->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    pf->addRow(i18n("Contraseña:"), pw);
    pf->addRow(i18n("Repítela:"), pw2);
    pf->addRow("", show);
    pf->addRow("", new QLabel(i18n("Cifrado AES-256. Quien tenga la contraseña podrá abrirlo (también con Kleopatra o gpg).")));
    v->addWidget(pwBox);

    v->addWidget(byKey);
    auto *keyBox = new QWidget;
    auto *kv = new QVBoxLayout(keyBox);
    kv->setContentsMargins(24, 0, 0, 0);
    auto *list = new QListWidget;
    kv->addWidget(list, 1);
    auto *signRow = new QHBoxLayout;
    auto *sign = new QCheckBox(i18n("Firmar con mi clave:"));
    auto *signWith = new QComboBox;
    signRow->addWidget(sign);
    signRow->addWidget(signWith, 1);
    kv->addLayout(signRow);
    auto *keyButtons = new QHBoxLayout;
    auto *importBtn = new QPushButton(i18n("Importar clave…"));
    auto *createBtn = new QPushButton(i18n("Crear mi par de claves…"));
    keyButtons->addWidget(importBtn);
    keyButtons->addWidget(createBtn);
    keyButtons->addStretch();
    kv->addLayout(keyButtons);
    v->addWidget(keyBox, 1);

    auto fillKeys = [list, sign, signWith, byKey] {
        const QStringList checked = [&] {
            QStringList c;
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->checkState() == Qt::Checked) c << list->item(i)->data(Qt::UserRole).toString();
            return c;
        }();
        list->clear();
        signWith->clear();
        for (const Key &k : keys()) {
            QString text = k.uid + "  (" + k.shortId + ")";
            if (k.secret) text += i18n("  · tú");
            else if (!k.verified) text += i18n("  · sin verificar");
            auto *it = new QListWidgetItem(text, list);
            it->setData(Qt::UserRole, k.fingerprint);
            it->setFlags(k.valid ? it->flags() | Qt::ItemIsUserCheckable : it->flags() & ~Qt::ItemIsEnabled);
            it->setCheckState(k.valid && (k.secret || checked.contains(k.fingerprint)) ? Qt::Checked : Qt::Unchecked);
            if (k.secret) signWith->addItem(k.uid, k.fingerprint);
        }
        if (list->count() == 0) {
            auto *it = new QListWidgetItem(i18n("Aún no hay claves: crea la tuya o importa la de quien vaya a abrirlo."), list);
            it->setFlags(Qt::NoItemFlags);
        }
        sign->setEnabled(signWith->count() > 0);
        signWith->setEnabled(signWith->count() > 0);
        if (signWith->count() == 0) sign->setChecked(false);
        byKey->setEnabled(true);
    };
    auto syncMode = [&] {
        pwBox->setEnabled(byPw->isChecked());
        keyBox->setEnabled(byKey->isChecked());
    };
    QObject::connect(byPw, &QRadioButton::toggled, &d, syncMode);
    QObject::connect(byKey, &QRadioButton::toggled, &d, [&, fillKeys](bool on) {
        if (on && list->count() == 0) fillKeys();
        syncMode();
    });
    QObject::connect(importBtn, &QPushButton::clicked, &d, [&d, fillKeys] {
        const QString f = QFileDialog::getOpenFileName(&d, i18n("Importar clave pública"), QDir::homePath(), i18n("Claves (*.asc *.gpg *.pgp *.key);;Todo (*)"));
        if (f.isEmpty()) return;
        importKey(f, &d, [&d, fillKeys](bool ok, const QString &msg, const QString &) {
            if (!ok) QMessageBox::warning(&d, i18n("Importar clave pública"), msg);
            fillKeys();
        });
    });
    QObject::connect(createBtn, &QPushButton::clicked, &d, [&d, fillKeys] { newKeyDialog(&d, fillKeys); });
    syncMode();

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(i18n("Cifrar"));
    v->addWidget(bb);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, [&] {
        if (outName->text().trimmed().isEmpty() || outName->text().contains('/')) return;
        if (byPw->isChecked() && (pw->text().isEmpty() || pw->text() != pw2->text())) {
            QMessageBox::warning(&d, d.windowTitle(), i18n("Las contraseñas no coinciden o están vacías."));
            return;
        }
        if (byKey->isChecked()) {
            bool any = false;
            for (int i = 0; i < list->count(); ++i) any |= list->item(i)->checkState() == Qt::Checked;
            if (!any) {
                QMessageBox::warning(&d, d.windowTitle(), i18n("Marca al menos una persona."));
                return;
            }
        }
        d.accept();
    });
    if (d.exec() != QDialog::Accepted) return;

    EncryptJob job;
    job.paths = paths;
    job.out = destDir + '/' + FileOps::uniqueName(destDir, outName->text().trimmed());
    job.symmetric = byPw->isChecked();
    job.passphrase = pw->text();
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked) job.recipients << list->item(i)->data(Qt::UserRole).toString();
    if (!job.symmetric && sign->isChecked()) job.signKey = signWith->currentData().toString();
    encrypt(job, parent, done);
}

void decryptDialog(QWidget *parent, const QString &file, const QString &destDir, Done done)
{
    if (!haveGpg()) {
        QMessageBox::warning(parent, i18n("Descifrar"), i18n("Hace falta GnuPG (gpg) para descifrar."));
        return;
    }
    QString pass;
    if (needsPassphrase(file)) {
        bool ok = false;
        pass = QInputDialog::getText(parent, i18n("Descifrar"), i18n("Contraseña de «%1»:").arg(QFileInfo(file).fileName()), QLineEdit::Password, {}, &ok);
        if (!ok) return;
        if (pass.isEmpty()) return;
    }
    decrypt(file, destDir, pass, parent, done);
}
}
