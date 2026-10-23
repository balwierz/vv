// ContextBar — the strip under the tab bar with the current tab's format
// options: only the ones its file and tab offer (FileFeatures / TabTraits),
// set to the file's FileOptions. Hidden on a tab that has none.
#pragma once

#include "fileoptions.h"

#include <QWidget>

class QButtonGroup;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QSpinBox;
class QToolButton;

class ContextBar : public QWidget {
    Q_OBJECT
public:
    explicit ContextBar(QWidget* parent = nullptr);

    // Show what `file` and `tab` offer, set to `opts`; hidden when nothing applies.
    void setState(const FileFeatures& file, const TabTraits& tab, const FileOptions& opts);
    void clearState();                      // a tab with no file options
    // The file the tab belongs to, at the strip's left, with its tab-bar
    // colour; an empty `name` hides it (one file open). Shown on every tab
    // while set, with or without options.
    void setFile(const QString& name, const QString& tip, const QColor& color);
    // A re-open with `attempted` failed: say why, until the next setState,
    // and keep the typed region and tags for editing.
    void showError(const QString& why, const FileOptions& attempted);
    void focusRegion();                     // when the region box shows
    // The visible controls, e.g. "region pileup tags contigs", or "none".
    QString describeForTest() const;
    QString fileForTest() const;            // the file name shown, or ""

signals:
    void optionsChanged(const FileOptions& opts);
    void sliceStep(int delta);

private:
    FileOptions current() const;            // the options the controls show
    void        apply() { emit optionsChanged(current()); }
    QToolButton* toggle(const QString& text, const QString& tip);
    void         updateVisibility();
    QWidget*     group(QHBoxLayout** lay);

    FileOptions   opts_;                    // what the file was opened with
    QWidget*      fileGroup_;
    QLabel*       fileSwatch_;
    QLabel*       fileName_;
    bool          regionReady_ = false;

    QWidget*      regionGroup_;
    QLineEdit*    regionEdit_;
    QToolButton*  goButton_;
    QComboBox*    coords_;
    QSpinBox*     slop_;
    QWidget*      alignGroup_;
    QToolButton*  pileup_;
    QWidget*      tagsGroup_;
    QLineEdit*    tagsEdit_;
    QWidget*      samplesGroup_;
    QButtonGroup* samples_;
    QToolButton*  gtStats_;
    QToolButton*  expand_;
    QToolButton*  contigs_;
    QWidget*      matrixGroup_;
    QButtonGroup* matrix_;
    QWidget*      sliceGroup_;
    QLabel*       hint_;
    QLabel*       error_;
};
