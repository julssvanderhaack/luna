// Office documents (Word, Excel, PowerPoint, OpenDocument, RTF): the
// picture they carry of their first page, and a PDF of the whole document
// made by LibreOffice and kept in ~/.cache/luna/office for the preview.
#pragma once
#include <QImage>
#include <QString>

namespace Office {
bool isOffice(const QString &mime);
bool haveLibreOffice();
// docProps/thumbnail.* (OOXML) or Thumbnails/thumbnail.png (ODF); null if none.
QImage embeddedThumbnail(const QString &path);
// The cached PDF of `path` if it is up to date, else empty. Cheap.
QString cachedPdf(const QString &path);
// Converts with soffice (one at a time, own profile so an open LibreOffice
// is not disturbed) and returns the cached PDF; blocks for seconds.
// `stillWanted` is asked before starting: a preview that moved on skips it.
QString toPdf(const QString &path, QString *error, bool (*stillWanted)(int) = nullptr, int ticket = 0);
}
