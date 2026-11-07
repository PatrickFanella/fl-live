#include "piano_view.hpp"
#include <QPainter>
#include <QQmlEngine>
#include <QSet>
#include <algorithm>

PianoNotesView::PianoNotesView(QQuickItem* parent) : QQuickPaintedItem(parent) {
    connect(this, &PianoNotesView::inputsChanged, this, [this] { update(); });
    connect(this, &QQuickItem::widthChanged, this, [this] { update(); });
    connect(this, &QQuickItem::heightChanged, this, [this] { update(); });
}
// Bindings re-evaluate on every session tick; equal inputs must not repaint.
void PianoNotesView::setNotes(const QVariantList& notes) {
    if (notes == notes_) return;
    notes_ = notes;
    emit inputsChanged();
}
void PianoNotesView::setSelected(const QVariantList& selected) {
    if (selected == selected_) return;
    selected_ = selected;
    emit inputsChanged();
}
void PianoNotesView::paint(QPainter* painter) {
    ++paints_;
    const double w = width(), h = height();
    const int rows = std::max(1, rows_);
    const double row = h / rows, tick = w / std::max(1, length_);
    static const int black[] = {1, 3, 6, 8, 10};
    for (int r = 0; r < rows; ++r) {
        const int pitch = base_ + rows - 1 - r;
        if (std::find(std::begin(black), std::end(black), ((pitch % 12) + 12) % 12) != std::end(black))
            painter->fillRect(QRectF(0, r * row, w, row), QColor("#151d28"));
    }
    for (int k = 0; k * 24 < length_; ++k)
        painter->fillRect(QRectF(k * 24 * tick, 0, 1, h), QColor(k % 4 == 0 ? "#3d4d62" : "#202a38"));
    QSet<QString> chosen;
    for (const auto& id : selected_) chosen.insert(id.toString());
    painter->setPen(QColor("#0f141c"));
    for (const auto& value : notes_) {
        const auto note = value.toMap();
        const bool sel = chosen.contains(note.value("id").toString());
        const bool moving = sel && dragging_ && !resizing_, sizing = sel && dragging_ && resizing_;
        const int pitch = note.value("pitch").toInt() + (moving ? preview_pitch_ : 0);
        if (pitch < base_ || pitch >= base_ + rows) continue;
        const QRectF rect((note.value("start").toInt() + (moving ? preview_ticks_ : 0)) * tick, (base_ + rows - 1 - pitch) * row,
            std::max(2.0, (note.value("length").toInt() + (sizing ? preview_ticks_ : 0)) * tick), row);
        painter->setOpacity(moving || sizing ? 0.7 : 1.0);
        painter->setBrush(QColor(sel ? "#e4bc75" : "#5fa8d3"));
        painter->drawRect(rect);
    }
    painter->setOpacity(1.0);
}
void registerDesktopTypes() { qmlRegisterType<PianoNotesView>("FlLive", 1, 0, "PianoNotesView"); }
