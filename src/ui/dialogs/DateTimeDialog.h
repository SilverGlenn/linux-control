#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>
#include <functional>
#include "time/TimeSync.h"

class QLabel;
class QTimer;
class QTabWidget;
class QCheckBox;
class AnalogClock;
class QPushButton;
class QDialogButtonBox;

// A faithful clone of the Windows 7 "Date and Time" dialog, backed entirely by
// this app talking to the system directly, no KDE dialogs are launched.
//
//   * The clock, date, time-zone label and the Daylight-Saving-Time note are
//     read live (QDateTime / QTimeZone, with the zone id from the system).
//   * "Change date and time…" and "Change time zone…" open sub-dialogs that
//     apply through `pkexec timedatectl …`.
//   * Internet Time detects Chrony/timesyncd and distinguishes automatic
//     synchronization from a clock that actually reports synchronized.
//     Server changes use a narrow installed helper, never an elevated shell.
//
// Sub-dialogs are built inline (local QDialogs) rather than as separate QObject
// subclasses, so this is the only class here that needs the meta-object.
class DateTimeDialog : public QDialog {
    Q_OBJECT

public:
    explicit DateTimeDialog(QWidget *parent = nullptr,
                            std::function<TimeSync::Status()> statusReader = TimeSync::readStatus);

    // Tabs, for deep-linking (e.g. "Add clocks…" opens Additional Clocks).
    enum Tab { TabDateTime = 0, TabAdditionalClocks = 1, TabInternetTime = 2 };
    void showTab(Tab tab);

protected:
    void done(int result) override;

private:
    QWidget *buildDateTimeTab();
    QWidget *buildAdditionalClocksTab();
    QWidget *buildInternetTimeTab();

    // Live refreshers.
    void refreshDateTime();       // clock, date, time, zone label, DST note
    void refreshInternetStatus(); // the Internet Time status paragraph
    void renderInternetStatus();

    // Privileged actions (each opens a single polkit prompt via pkexec).
    void openChangeDateTime();
    void openChangeTimeZone();
    void openInternetSettings();
    void runPrivileged(const QStringList &argv, std::function<void(bool)> onDone);

    QTabWidget  *m_tabs           = nullptr;
    AnalogClock *m_clock          = nullptr;
    QLabel      *m_dateValue      = nullptr;
    QLabel      *m_timeValue      = nullptr;
    QLabel      *m_tzValue        = nullptr;
    QLabel      *m_dstNote        = nullptr;
    QLabel      *m_internetStatus = nullptr;
    QLabel      *m_internetSched  = nullptr;
    QCheckBox   *m_notifyCheck    = nullptr;
    QTimer      *m_timer          = nullptr;
    QPushButton *m_internetChange = nullptr;
    QDialogButtonBox *m_buttons   = nullptr;
    TimeSync::Status m_timeStatus;
    bool m_statusLoading = false;
    bool m_privilegedBusy = false;
    int m_statusGeneration = 0;
    std::function<TimeSync::Status()> m_statusReader;

    QString m_timezone;   // current IANA zone id
};
