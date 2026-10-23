#include "actionpalette.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QScreen>
#include <QPainter>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// The name on the left, the shortcut on the right in a quieter colour.
class RowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& i) const override {
        QStyleOptionViewItem o = opt;
        initStyleOption(&o, i);
        o.text.clear();
        o.widget->style()->drawControl(QStyle::CE_ItemViewItem, &o, p, o.widget);
        const bool selected = o.state & QStyle::State_Selected;
        const QRect r = o.rect.adjusted(o.icon.isNull() ? 6 : 26, 0, -6, 0);
        p->save();
        p->setFont(o.font);
        p->setPen(o.palette.color(selected ? QPalette::HighlightedText : (o.state & QStyle::State_Enabled ? QPalette::Text : QPalette::PlaceholderText)));
        p->drawText(r, Qt::AlignLeft | Qt::AlignVCenter, i.data(Qt::UserRole + 2).toString());
        QColor muted = o.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText);
        p->setPen(muted);
        p->drawText(r, Qt::AlignRight | Qt::AlignVCenter, i.data(Qt::UserRole + 3).toString());
        p->restore();
    }
};

}  // namespace

ActionPalette::ActionPalette(const QList<QPair<QString, QAction*>>& commands, QWidget* parent) : QFrame(parent, Qt::Popup) {
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(4);
    query_ = new QLineEdit(this);
    query_->setPlaceholderText(tr("Find a command: its name, shortcut or menu"));
    query_->setClearButtonEnabled(true);
    query_->installEventFilter(this);
    v->addWidget(query_);
    model_ = new QStandardItemModel(this);
    for (const auto& [where, a] : commands) {
        QString name = a->text();
        name.remove(QLatin1Char('&'));
        const QString keys = a->shortcut().toString(QKeySequence::NativeText);   // the first key
        auto* item = new QStandardItem(name);
        item->setData(keys, Qt::UserRole + 3);
        item->setData(name + QLatin1Char(' ') + keys + QLatin1Char(' ') + where, Qt::UserRole);   // the haystack
        item->setData(QVariant::fromValue(static_cast<QObject*>(a)), Qt::UserRole + 1);
        item->setData(name, Qt::UserRole + 2);
        item->setToolTip(a->toolTip().isEmpty() ? where : a->toolTip() + QStringLiteral("\n") + where);
        item->setEnabled(a->isEnabled());
        if (!a->icon().isNull()) item->setIcon(a->icon());
        model_->appendRow(item);
    }
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterRole(Qt::UserRole);
    list_ = new QListView(this);
    list_->setModel(proxy_);
    list_->setUniformItemSizes(true);
    list_->setItemDelegate(new RowDelegate(list_));
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->installEventFilter(this);
    // Typing happens in the query box, so the list never has focus: draw its
    // selection in the active colours, as the row Enter will run.
    QPalette lp = list_->palette();
    for (auto role : {QPalette::Highlight, QPalette::HighlightedText})
        lp.setColor(QPalette::Inactive, role, lp.color(QPalette::Active, role));
    list_->setPalette(lp);
    v->addWidget(list_, 1);
    connect(query_, &QLineEdit::textChanged, this, [this](const QString& t) {
        // Every word, anywhere.
        QString pattern;
        for (const QString& w : t.split(QLatin1Char(' '), Qt::SkipEmptyParts)) pattern += QStringLiteral("(?=.*%1)").arg(QRegularExpression::escape(w));
        proxy_->setFilterRegularExpression(QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption));
        if (proxy_->rowCount() > 0) list_->setCurrentIndex(proxy_->index(0, 0));
    });
    connect(list_, &QListView::clicked, this, [this] { runSelected(); });
    if (proxy_->rowCount() > 0) list_->setCurrentIndex(proxy_->index(0, 0));
    resize(520, std::min(16, std::max(6, model_->rowCount())) * (fontMetrics().height() + 6) + query_->sizeHint().height() + 24);
}

void ActionPalette::popupAt(const QPoint& global_top_centre) {
    QPoint p(global_top_centre.x() - width() / 2, global_top_centre.y());
    if (const QScreen* s = QGuiApplication::screenAt(global_top_centre)) {
        const QRect a = s->availableGeometry();
        p.setX(std::clamp(p.x(), a.left(), std::max(a.left(), a.right() - width())));
        p.setY(std::clamp(p.y(), a.top(), std::max(a.top(), a.bottom() - height())));
    }
    move(p);
    show();
    query_->setFocus();
}

void ActionPalette::setQuery(const QString& text) { query_->setText(text); }

QStringList ActionPalette::shown() const {
    QStringList out;
    for (int r = 0; r < proxy_->rowCount(); ++r) out << proxy_->index(r, 0).data(Qt::UserRole + 2).toString();
    return out;
}

void ActionPalette::runSelected() {
    const QModelIndex i = list_->currentIndex().isValid() ? list_->currentIndex() : proxy_->index(0, 0);
    if (!i.isValid()) return;
    auto* a = qobject_cast<QAction*>(i.data(Qt::UserRole + 1).value<QObject*>());
    close();
    if (a && a->isEnabled()) a->trigger();
}

bool ActionPalette::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::KeyPress) return QFrame::eventFilter(watched, event);
    auto* k = static_cast<QKeyEvent*>(event);
    if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) { runSelected(); return true; }
    if (k->key() == Qt::Key_Escape) { close(); return true; }
    if (watched == query_ && (k->key() == Qt::Key_Up || k->key() == Qt::Key_Down || k->key() == Qt::Key_PageUp || k->key() == Qt::Key_PageDown)) {
        QApplication::sendEvent(list_, event);
        return true;
    }
    if (watched == list_ && !k->text().isEmpty() && k->text().at(0).isPrint()) {
        query_->setFocus();
        QApplication::sendEvent(query_, event);
        return true;
    }
    return QFrame::eventFilter(watched, event);
}
