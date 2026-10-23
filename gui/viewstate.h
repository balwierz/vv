// TabViewState — what a table tab shows beyond its file: the filter, the sort
// keys, the hidden columns and their order, the frozen columns and the cursor.
// Taken before a file is re-read (Reload) or vvg closes (the session) and
// applied to the new tab of the same label once its columns are known.
// Columns are named, so a reload that adds or drops columns keeps the rest.
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

struct TabViewState {
    QString                     label;      // the tab's own label (TabPane::label)
    QString                     filter;     // as applied; empty = none
    QList<QPair<QString, bool>> sort;       // (column, descending), first key first
    QStringList                 hidden;     // hidden columns
    QStringList                 order;      // columns in on-screen order; empty = file order
    int                         frozen = 0; // leading columns frozen
    QString                     curColumn;  // the cursor's column; empty = none
    qint64                      curRow = -1;

    bool isDefault() const {
        return filter.isEmpty() && sort.isEmpty() && hidden.isEmpty() && order.isEmpty() &&
               frozen == 0 && curColumn.isEmpty() && curRow < 0;
    }

    QJsonObject toJson() const {
        QJsonObject o;
        o[QStringLiteral("label")] = label;
        if (!filter.isEmpty()) o[QStringLiteral("filter")] = filter;
        if (!sort.isEmpty()) {
            QJsonArray a;
            for (const auto& [col, desc] : sort)
                a.append(QJsonObject{{QStringLiteral("column"), col}, {QStringLiteral("desc"), desc}});
            o[QStringLiteral("sort")] = a;
        }
        if (!hidden.isEmpty()) o[QStringLiteral("hidden")] = QJsonArray::fromStringList(hidden);
        if (!order.isEmpty()) o[QStringLiteral("order")] = QJsonArray::fromStringList(order);
        if (frozen) o[QStringLiteral("frozen")] = frozen;
        if (!curColumn.isEmpty()) {
            o[QStringLiteral("column")] = curColumn;
            o[QStringLiteral("row")] = curRow;
        }
        return o;
    }

    static TabViewState fromJson(const QJsonObject& o) {
        TabViewState s;
        s.label = o.value(QStringLiteral("label")).toString();
        s.filter = o.value(QStringLiteral("filter")).toString();
        for (const auto& v : o.value(QStringLiteral("sort")).toArray()) {
            const QJsonObject k = v.toObject();
            s.sort.append({k.value(QStringLiteral("column")).toString(),
                           k.value(QStringLiteral("desc")).toBool()});
        }
        for (const auto& v : o.value(QStringLiteral("hidden")).toArray()) s.hidden << v.toString();
        for (const auto& v : o.value(QStringLiteral("order")).toArray()) s.order << v.toString();
        s.frozen = o.value(QStringLiteral("frozen")).toInt();
        s.curColumn = o.value(QStringLiteral("column")).toString();
        s.curRow = (qint64)o.value(QStringLiteral("row")).toDouble(-1);
        return s;
    }
};
