#pragma once
#include "fl_live/session_document.hpp"
#include <QByteArray>
#include <QString>

namespace fl_live {
// Optional Qt Core adapter, outside the engine and document-model libraries.
// Versioned identity metadata only; not the desktop musical project format.
inline constexpr qsizetype document_metadata_byte_limit = 4 * 1024 * 1024;
QByteArray encodeDocumentMetadata(const DocumentSnapshot& snapshot);
DocumentSnapshot decodeDocumentMetadata(const QByteArray& bytes);
DocumentSnapshot readDocumentMetadata(const QString& path);
void writeDocumentMetadata(const QString& path, const DocumentSnapshot& snapshot);
void loadDocumentMetadata(const QString& path, SessionDocument& document);
} // namespace fl_live
