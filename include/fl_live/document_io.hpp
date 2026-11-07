#pragma once
#include "fl_live/session_document.hpp"
#include <QByteArray>
#include <QString>

class QSaveFile;

namespace fl_live {
// Optional Qt Core adapter, outside the engine and document-model libraries.
// Versioned identity metadata only; not the desktop musical project format.
inline constexpr qsizetype document_metadata_byte_limit = 4 * 1024 * 1024;
QByteArray encodeDocumentMetadata(const DocumentSnapshot& snapshot);
DocumentSnapshot decodeDocumentMetadata(const QByteArray& bytes);
DocumentSnapshot readDocumentMetadata(const QString& path);
void writeDocumentMetadata(const QString& path, const DocumentSnapshot& snapshot);
void loadDocumentMetadata(const QString& path, SessionDocument& document);
// Writes bytes to an opened QSaveFile, then flushes, syncs the temporary
// file's data, renames it over the destination and syncs the directory.
// QSaveFile::commit alone does not sync. Returns false on any failure; the
// temporary is discarded and the previous destination is left in place.
bool commitDurably(QSaveFile& file, const QByteArray& bytes);
} // namespace fl_live
