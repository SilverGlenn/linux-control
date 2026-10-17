#pragma once

#include <QWidget>
#include <QStringList>
#include "PageId.h"
#include "perf/WeiBenchmark.h"

class QScrollArea;
class QLabel;
class QPushButton;
class QProgressBar;
class QFrame;

// "Performance Information and Tools", the Linux Experience Index screen, a
// faithful take on the Windows 7 WEI page. Shows the five component subscores,
// the base score (the minimum subscore) in a glossy badge, and a "Rate this
// computer" action that runs the app-owned benchmark engine. Inline progress
// stays attached to the page while users navigate between sections.
class PerformancePage : public QWidget {
    Q_OBJECT
public:
    explicit PerformancePage(QScrollArea *sidebar, QWidget *parent = nullptr);

    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();

private:
    void populate(const WeiResult &r);
    void applyState(bool rated);
    void refreshBenchmarkState();
    void onRateClicked();

    WeiBenchmark    *m_bench = nullptr;
    WeiResult        m_result;

    QFrame          *m_banner      = nullptr;   // yellow "not yet established" bar
    QPushButton     *m_rateBtn     = nullptr;   // lives in the banner (unrated)
    QFrame          *m_progressPanel = nullptr;
    QLabel          *m_progressTitle = nullptr;
    QProgressBar    *m_progressBar = nullptr;

    QLabel          *m_subHeader   = nullptr;   // "Subscore"  column header
    QLabel          *m_baseHeader  = nullptr;   // "Base score" column header
    QFrame          *m_shade       = nullptr;   // shaded panel behind score cols
    QFrame          *m_basePanel   = nullptr;   // holds the glossy badge + caption
    QFrame          *m_badge       = nullptr;   // glossy blue base-score badge
    QLabel          *m_baseCaption = nullptr;
    QLabel          *m_subscore[5] = {};        // Processor, Memory, Graphics, Gaming, Disk
    QLabel          *m_baseBig     = nullptr;

    QLabel          *m_detailToggle= nullptr;   // "View and print detailed…"
    QWidget         *m_detailWrap  = nullptr;
    QLabel          *m_detailLabel = nullptr;

    QLabel          *m_scoresCurrent = nullptr; // "Your scores are current / Last update…"
    QLabel          *m_rerunLink   = nullptr;   // "Re-run the assessment" (rated)

};
