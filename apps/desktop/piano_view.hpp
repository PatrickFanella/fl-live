#pragma once
#include <QQuickPaintedItem>
#include <QVariantList>

// Draws the piano-roll rows, grid and notes into one texture (#41). It
// repaints only when an input actually changes, so frames in between reuse
// the texture and edits cost one paint instead of a delegate per note.
// Hit-testing stays in QML and works on note data, not on drawn items.
class PianoNotesView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QVariantList notes READ notes WRITE setNotes NOTIFY inputsChanged)
    Q_PROPERTY(QVariantList selected READ selected WRITE setSelected NOTIFY inputsChanged)
    Q_PROPERTY(int base MEMBER base_ NOTIFY inputsChanged)
    Q_PROPERTY(int rows MEMBER rows_ NOTIFY inputsChanged)
    Q_PROPERTY(int length MEMBER length_ NOTIFY inputsChanged)
    // Drag preview for selected notes: a move offsets start and pitch, a
    // resize changes the length.
    Q_PROPERTY(bool dragging MEMBER dragging_ NOTIFY inputsChanged)
    Q_PROPERTY(bool resizing MEMBER resizing_ NOTIFY inputsChanged)
    Q_PROPERTY(int previewTicks MEMBER preview_ticks_ NOTIFY inputsChanged)
    Q_PROPERTY(int previewPitch MEMBER preview_pitch_ NOTIFY inputsChanged)
    Q_PROPERTY(int paints READ paints NOTIFY inputsChanged) // repaint count, for tests and measurement
public:
    explicit PianoNotesView(QQuickItem* parent = nullptr);
    void paint(QPainter* painter) override;
    QVariantList notes() const { return notes_; }
    void setNotes(const QVariantList& notes);
    QVariantList selected() const { return selected_; }
    void setSelected(const QVariantList& selected);
    int paints() const { return paints_; }
signals:
    void inputsChanged();
private:
    QVariantList notes_, selected_;
    int base_ = 48, rows_ = 24, length_ = 384, preview_ticks_ = 0, preview_pitch_ = 0, paints_ = 0;
    bool dragging_ = false, resizing_ = false;
};
void registerDesktopTypes();
