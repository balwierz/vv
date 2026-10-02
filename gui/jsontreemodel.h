// JsonTreeModel — a QAbstractItemModel over vvjson::JsonDoc, the lazy index
// the terminal tree viewer uses: a container's children are fetched a page at
// a time (canFetchMore / fetchMore), so a multi-GB document opens as fast as
// its first scan and expands without reading it all into Qt.
#pragma once

#include <QAbstractItemModel>
#include <memory>
#include <vector>

#include "vv/vvjson.hpp"

class JsonTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { Key = 0, Value = 1, Type = 2 };

    JsonTreeModel(std::unique_ptr<vvjson::JsonDoc> doc, QObject* parent = nullptr);

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override { (void)parent; return 3; }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation o, int role = Qt::DisplayRole) const override;
    bool hasChildren(const QModelIndex& parent = {}) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    vvjson::JsonDoc& doc() { return *doc_; }
    const vvjson::JsonDoc& doc() const { return *doc_; }
    // jq-style path of an item (".a.b[3]", ".[\"odd key\"]"; "." for the root).
    QString path(const QModelIndex& idx) const;
    // The value as text: a string decoded, anything else as written (at most
    // `cap` bytes of a container).
    QString valueText(const QModelIndex& idx, int cap = 1 << 20) const;
    // The nearest array of objects at or above `idx` (an invalid index when
    // there is none): the records a table can show.
    QModelIndex recordsAt(const QModelIndex& idx) const;
    vvjson::JNode node(const QModelIndex& idx) const;
    // A one-line summary for the status bar: size, root kind, validation.
    QString summary() const;
    // The item for a path from JsonDoc::path_to() (root first), fetching
    // the children up to each step; invalid when a step is not found.
    QModelIndex indexForChain(const std::vector<std::pair<vvjson::JNode, int64_t>>& chain);
    // Highlight rows whose key or value matches (the Find bar); "" clears.
    void setSearch(const QString& query);
    bool hasSearch() const { return !query_.isEmpty(); }
    const vvjson::JsonSearch& search() const { return pat_; }
    QString searchQuery() const { return query_; }

private:
    struct Item {
        vvjson::JNode node;
        int parent = -1;            // item index; -1 for the root
        int row = 0;                // row under the parent
        int64_t index = 0;          // index in the parent container
        std::vector<int> kids;      // item indices of the children fetched so far
    };
    std::unique_ptr<vvjson::JsonDoc> doc_;
    mutable std::vector<Item> items_;   // [0] is the root
    QString            query_;
    vvjson::JsonSearch pat_;

    // Add children of `pi` until there are at least `upto` (capped by the count).
    void fetchTo(const QModelIndex& parent, int64_t upto);
    bool rowMatches(int item) const;
    int itemOf(const QModelIndex& idx) const { return idx.isValid() ? (int)idx.internalId() : -1; }
    QString keyText(int item) const;
    QString typeText(const vvjson::JNode& n) const;
    QString countText(const vvjson::JNode& n) const;
};
