// Encrypting and decrypting with GnuPG the way Kleopatra does: a folder or
// several items become name.tar.gpg (tar piped into gpg, nothing plain is
// written), one file becomes file.gpg; with a password (AES-256) or for the
// public keys of chosen people, optionally signed. The jobs run gpg in the
// background; the dialogs only gather what they need.
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

class QObject;
class QWidget;

namespace Crypto {
using Done = std::function<void(bool ok, const QString &message, const QString &made)>;

struct Key {
    QString fingerprint, uid, shortId;
    bool secret = false;      // one of mine
    bool valid = true;        // not expired or revoked, can encrypt
    bool verified = false;    // certified (full/ultimate validity)
};

bool haveGpg();
bool isEncrypted(const QString &path);   // .gpg .pgp .asc (PGP message)
QList<Key> keys();                       // public keys, mine flagged; blocks briefly

struct EncryptJob {
    QStringList paths;      // all in one folder
    QString out;            // full path of the .gpg to write
    bool symmetric = true;
    QString passphrase;     // symmetric
    QStringList recipients; // fingerprints
    QString signKey;        // fingerprint, empty = unsigned
};
void encrypt(const EncryptJob &job, QObject *ctx, Done done);

// True when the file was encrypted with a password (Luna asks for it); with
// a key gpg-agent's pinentry asks for the key's passphrase if needed.
bool needsPassphrase(const QString &file);
// Decrypts into destDir: a tar comes out as a folder, anything else as the
// file without .gpg. The message tells about the signature, if any.
void decrypt(const QString &file, const QString &destDir, const QString &passphrase, QObject *ctx, Done done);

void createKey(const QString &name, const QString &email, const QString &passphrase, QObject *ctx, Done done);
void importKey(const QString &file, QObject *ctx, Done done);

// The dialogs, wired to the jobs above.
void encryptDialog(QWidget *parent, const QStringList &paths, const QString &destDir, Done done);
void decryptDialog(QWidget *parent, const QString &file, const QString &destDir, Done done);
}
