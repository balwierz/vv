// The `vv` command line that reproduces a vvg tab: the file, the component tab,
// the session's region / pileup / tags / GT-stats / contigs options, and the
// tab's filter, sort and visible columns. Qt-only (no widgets) so the headless
// self-test can check it.
#pragma once
#include <QString>
#include <QStringList>

#include "vv/vvcore.hpp"

struct VvCommandSpec {
    QString     path;                 // the file (or dataset directory)
    QString     tab;                  // component tab label; empty = the first tab
    QString     region;               // as typed in the Region box
    bool        ncbi     = false;     // region is NCBI 1-based (--coords NCBI)
    int         slop     = 0;
    bool        pileup   = false;
    QString     tags;                 // BAM aux tags, comma-separated
    bool        gtStats  = false;
    bool        contigs  = false;
    QString     filter;               // --filter expression, as applied
    QString     sortColumn;           // empty = unsorted
    bool        sortDesc = false;
    QStringList select;               // --select terms; empty = all columns
};

// POSIX shell quoting: the word unchanged when it is made only of characters
// no shell treats specially, otherwise wrapped in single quotes ('\'' for a
// quote inside).
QString shellQuote(const QString& word);

// "vv [options] FILE", each word shell-quoted.
QString vvCommandLine(const VvCommandSpec& spec);

// The same options as a reader Config (for export_view), so an export writes
// exactly what the copied command would.
Config vvCommandConfig(const VvCommandSpec& spec);
