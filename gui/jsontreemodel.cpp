#include "jsontreemodel.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QPalette>

using vvjson::JKind;
using vvjson::JNode;

namespace {

constexpr int kFetchBatch = 1000;   // children added per fetchMore()
constexpr int kShownBytes = 400;    // value text shown in a cell

// A JSON string's body (between the quotes) decoded: escapes and \uXXXX,
// surrogate pairs combined; a malformed escape is kept as written.
QString unescape(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    std::string pending;
    auto flush = [&] {
        if (pending.empty()) return;
        const QString q = QString::fromUtf8(pending.data(), (qsizetype)pending.size());
        out.append(reinterpret_cast<const char16_t*>(q.utf16()), (size_t)q.size());
        pending.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) { pending += c; continue; }
        const char e = s[++i];
        switch (e) {
            case 'n': pending += '\n'; break;
            case 't': pending += '\t'; break;
            case 'r': pending += '\r'; break;
            case 'b': pending += '\b'; break;
            case 'f': pending += '\f'; break;
            case '"': case '\\': case '/': pending += e; break;
            case 'u':
                if (i + 4 < s.size()) {
                    bool ok = false;
                    const ushort u = QString::fromLatin1(s.data() + i + 1, 4).toUShort(&ok, 16);
                    if (ok) {
                        flush();
                        out.push_back((char16_t)u);
                        i += 4;
                        break;
                    }
                }
                pending += "\\u";
                break;
            default: pending += '\\'; pending += e; break;
        }
    }
    flush();
    return QString(reinterpret_cast<const QChar*>(out.data()), (qsizetype)out.size());
}

bool ident(std::string_view k) {
    if (k.empty() || !(std::isalpha((unsigned char)k[0]) || k[0] == '_')) return false;
    for (char c : k) if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

QString oneLine(QString t) {
    t.replace(QLatin1Char('\n'), QStringLiteral("⏎"));
    t.replace(QLatin1Char('\t'), QLatin1Char(' '));
    return t;
}

}  // namespace

JsonTreeModel::JsonTreeModel(std::unique_ptr<vvjson::JsonDoc> doc, QObject* parent)
    : QAbstractItemModel(parent), doc_(std::move(doc)) {
    Item root;
    root.node = doc_->root();
    items_.push_back(root);
}

QModelIndex JsonTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (row < 0 || column < 0 || column >= 3) return {};
    if (!parent.isValid()) return row == 0 ? createIndex(0, column, quintptr(0)) : QModelIndex();
    const Item& p = items_[(size_t)itemOf(parent)];
    if (row >= (int)p.kids.size()) return {};
    return createIndex(row, column, quintptr(p.kids[(size_t)row]));
}

QModelIndex JsonTreeModel::parent(const QModelIndex& child) const {
    const int it = itemOf(child);
    if (it <= 0) return {};
    const int p = items_[(size_t)it].parent;
    return createIndex(items_[(size_t)p].row, 0, quintptr(p));
}

int JsonTreeModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid()) return 1;
    if (parent.column() != 0) return 0;
    return (int)items_[(size_t)itemOf(parent)].kids.size();
}

bool JsonTreeModel::hasChildren(const QModelIndex& parent) const {
    if (!parent.isValid()) return true;
    const JNode& n = items_[(size_t)itemOf(parent)].node;
    return parent.column() == 0 && n.container() && n.count > 0;
}

bool JsonTreeModel::canFetchMore(const QModelIndex& parent) const {
    if (!parent.isValid() || parent.column() != 0) return false;
    const Item& it = items_[(size_t)itemOf(parent)];
    return it.node.container() && (int64_t)it.kids.size() < it.node.count;
}

void JsonTreeModel::fetchMore(const QModelIndex& parent) {
    if (!canFetchMore(parent)) return;
    fetchTo(parent, (int64_t)items_[(size_t)itemOf(parent)].kids.size() + kFetchBatch);
}

void JsonTreeModel::fetchTo(const QModelIndex& parent, int64_t upto) {
    if (!canFetchMore(parent)) return;
    const int pi = itemOf(parent);
    const int64_t have = (int64_t)items_[(size_t)pi].kids.size();
    std::vector<Item> got;
    for (int64_t i = have; i < items_[(size_t)pi].node.count && i < upto; ++i) {
        JNode c;
        if (!doc_->child(items_[(size_t)pi].node, i, &c)) break;
        Item k;
        k.node = c;
        k.parent = pi;
        k.row = (int)i;
        k.index = i;
        got.push_back(k);
    }
    if (got.empty()) {
        // A broken tail: the count promised more than the scan found.
        items_[(size_t)pi].node.count = have;
        return;
    }
    beginInsertRows(parent, (int)have, (int)have + (int)got.size() - 1);
    for (Item& k : got) {
        items_.push_back(k);
        items_[(size_t)pi].kids.push_back((int)items_.size() - 1);
    }
    endInsertRows();
}

JNode JsonTreeModel::node(const QModelIndex& idx) const {
    const int it = itemOf(idx);
    return it < 0 ? JNode{} : items_[(size_t)it].node;
}

QString JsonTreeModel::keyText(int item) const {
    if (item == 0) return tr("(document)");
    const Item& it = items_[(size_t)item];
    const JNode& par = items_[(size_t)it.parent].node;
    if (par.kind == JKind::Object && !par.virt) return unescape(doc_->key(it.node));
    return QStringLiteral("[%1]").arg(it.index);
}

QString JsonTreeModel::countText(const JNode& n) const {
    const char* what = n.virt ? (doc_->root_mode() == vvjson::JsonDoc::Root::Lines ? "line" : "value")
                     : n.kind == JKind::Object ? "key" : "item";
    return QStringLiteral("%1 %2%3").arg(n.count).arg(QLatin1String(what))
        .arg(n.count == 1 ? QString() : QStringLiteral("s"));
}

QString JsonTreeModel::typeText(const JNode& n) const {
    switch (n.kind) {
        case JKind::Object: return QStringLiteral("object");
        case JKind::Array:  return n.virt ? QStringLiteral("document") : QStringLiteral("array");
        case JKind::String: return QStringLiteral("string");
        case JKind::Number: return QStringLiteral("number");
        case JKind::True: case JKind::False: return QStringLiteral("boolean");
        case JKind::Null:   return QStringLiteral("null");
        default:            return QStringLiteral("error");
    }
}

QString JsonTreeModel::valueText(const QModelIndex& idx, int cap) const {
    const JNode n = node(idx);
    if (n.kind == JKind::Error || n.virt) return {};
    std::string_view b = doc_->bytes(n);
    if ((int64_t)b.size() > cap) b = b.substr(0, (size_t)cap);
    if (n.kind == JKind::String && b.size() >= 2) return unescape(b.substr(1, b.size() - 2));
    return QString::fromUtf8(b.data(), (qsizetype)b.size());
}

QVariant JsonTreeModel::data(const QModelIndex& idx, int role) const {
    const int it = itemOf(idx);
    if (it < 0) return {};
    const JNode& n = items_[(size_t)it].node;
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
        switch (idx.column()) {
            case Key: return keyText(it);
            case Type: return typeText(n);
            case Value: {
                if (n.kind == JKind::Error)
                    return tr("⚠ %1 at byte %2")
                        .arg(QString::fromStdString(doc_->has_struct_err() ? doc_->struct_err()
                                                                           : std::string("invalid JSON")))
                        .arg(n.off);
                if (n.container()) {
                    QString t = (n.kind == JKind::Object && !n.virt ? QStringLiteral("{…}  ")
                                                                    : QStringLiteral("[…]  ")) + countText(n);
                    if (!n.virt && n.count > 0)
                        t += QStringLiteral("   ") +
                             QString::fromStdString(doc_->compact(n, (size_t)kShownBytes));
                    if (n.broken) t += tr("   ⚠ cut short");
                    return oneLine(t);
                }
                const int cap = role == Qt::ToolTipRole ? 4096 : kShownBytes;
                QString t = valueText(idx, cap);
                const uint64_t len = n.end - n.off;
                if (n.kind == JKind::String) t = QStringLiteral("\"") + t + QStringLiteral("\"");
                if ((int64_t)len > cap) t += QStringLiteral("…");
                return role == Qt::ToolTipRole ? t : oneLine(t);
            }
        }
    }
    if (role == Qt::ForegroundRole && idx.column() == Value) {
        const bool dark = QGuiApplication::palette().color(QPalette::Base).lightness() < 128;
        switch (n.kind) {
            case JKind::String: return QBrush(dark ? QColor(0x98, 0xc3, 0x79) : QColor(0x1a, 0x7f, 0x37));
            case JKind::Number: return QBrush(dark ? QColor(0x61, 0xaf, 0xef) : QColor(0x05, 0x50, 0xae));
            case JKind::True: case JKind::False:
                return QBrush(dark ? QColor(0xc6, 0x78, 0xdd) : QColor(0x82, 0x50, 0xdf));
            case JKind::Null: return QBrush(dark ? QColor(0x7f, 0x84, 0x8e) : QColor(0x6e, 0x77, 0x81));
            case JKind::Error: return QBrush(QColor(0xcf, 0x22, 0x2e));
            default: return QBrush(QGuiApplication::palette().color(QPalette::PlaceholderText));
        }
    }
    if (role == Qt::BackgroundRole && rowMatches(it)) {
        const bool dark = QGuiApplication::palette().color(QPalette::Base).lightness() < 128;
        return QBrush(dark ? QColor(0x5c, 0x4b, 0x12) : QColor(0xff, 0xf1, 0x9e));
    }
    if (role == Qt::FontRole && idx.column() == Key && it > 0) {
        QFont f;
        f.setBold(true);
        return f;
    }
    return {};
}

QVariant JsonTreeModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case Key: return tr("Key");
        case Value: return tr("Value");
        case Type: return tr("Type");
    }
    return {};
}

QString JsonTreeModel::path(const QModelIndex& idx) const {
    int it = itemOf(idx);
    QString s;
    std::vector<int> chain;
    for (; it > 0; it = items_[(size_t)it].parent) chain.push_back(it);
    for (auto c = chain.rbegin(); c != chain.rend(); ++c) {
        const Item& item = items_[(size_t)*c];
        const JNode& par = items_[(size_t)item.parent].node;
        if (par.kind == JKind::Object && !par.virt) {
            const std::string_view k = doc_->key(item.node);
            if (ident(k)) s += QStringLiteral(".") + QString::fromUtf8(k.data(), (qsizetype)k.size());
            else {
                if (s.isEmpty()) s += QLatin1Char('.');
                s += QStringLiteral("[\"") + QString::fromUtf8(k.data(), (qsizetype)k.size()) + QStringLiteral("\"]");
            }
        } else {
            if (s.isEmpty()) s += QLatin1Char('.');
            s += QStringLiteral("[%1]").arg(item.index);
        }
    }
    return s.isEmpty() ? QStringLiteral(".") : s;
}

QModelIndex JsonTreeModel::recordsAt(const QModelIndex& idx) const {
    for (QModelIndex i = idx.sibling(idx.row(), 0); i.isValid(); i = i.parent()) {
        const JNode n = node(i);
        if (n.kind != JKind::Array || n.virt || n.count == 0) continue;
        JNode first;
        if (doc_->child(n, 0, &first) && first.kind == JKind::Object) return i;
    }
    return {};
}

bool JsonTreeModel::rowMatches(int item) const {
    if (query_.isEmpty() || item <= 0) return false;
    const Item& it = items_[(size_t)item];
    const JNode& par = items_[(size_t)it.parent].node;
    if (par.kind == JKind::Object && !par.virt) {
        const std::string_view k = doc_->key(it.node);
        if (!k.empty() && pat_.match(k)) return true;
    }
    const JNode& n = it.node;
    if (n.container() || n.kind == JKind::Error) return false;
    std::string_view b = doc_->bytes(n);
    if (n.kind == JKind::String && b.size() >= 2) b = b.substr(1, b.size() - 2);
    return pat_.match(b.substr(0, std::min<size_t>(b.size(), 1 << 16)));
}

void JsonTreeModel::setSearch(const QString& query) {
    if (query == query_) return;
    query_ = query;
    pat_.compile(query.toStdString());   // the view repaints its viewport
}

QModelIndex JsonTreeModel::indexForChain(const std::vector<std::pair<JNode, int64_t>>& chain) {
    if (chain.empty()) return {};
    QModelIndex at = index(0, 0);
    for (size_t k = 1; k < chain.size(); ++k) {
        const int64_t want = chain[k].second;
        fetchTo(at, want + 1);
        if (want >= rowCount(at)) return {};
        at = index((int)want, 0, at);
    }
    return at;
}

QString JsonTreeModel::summary() const {
    const double mib = (double)doc_->size() / (1024.0 * 1024.0);
    QString s = tr("Format: JSON document  |  %1").arg(
        mib >= 1 ? QStringLiteral("%1 MiB").arg(mib, 0, 'f', 1)
                 : QStringLiteral("%1 B").arg((qulonglong)doc_->size()));
    const JNode& r = doc_->root();
    if (r.virt)
        s += doc_->root_mode() == vvjson::JsonDoc::Root::Lines ? tr("  |  JSON Lines") : tr("  |  JSON sequence");
    bool zeroed = false;
    if (doc_->file_changed(&zeroed) || zeroed) return s + tr("  |  ⚠ file changed on disk");
    switch (doc_->validation()) {
        case 0: return s + tr("  |  validating %1%").arg(
                    doc_->size() ? (int)(100.0 * (double)doc_->validated_bytes() / (double)doc_->size()) : 100);
        case 1: return s + tr("  |  ✓ valid");
        default: {
            const vvjson::JsonError e = doc_->validation_error();
            return s + tr("  |  ⚠ invalid at %1:%2: %3").arg(e.line).arg(e.col)
                           .arg(QString::fromStdString(e.msg));
        }
    }
}
