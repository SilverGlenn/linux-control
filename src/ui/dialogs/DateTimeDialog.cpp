#include "DateTimeDialog.h"
#include "AnalogClock.h"
#include "IconHelper.h"
#include "WindowsTimeZones.h"
#include "Win7Ui.h"
#include "time/TimeSyncConfig.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QTabWidget>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QGroupBox>
#include <QCalendarWidget>
#include <QTimeEdit>
#include <QDialogButtonBox>
#include <QFrame>
#include <QStyle>
#include <QTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QMessageBox>
#include <QDateTime>
#include <QTimeZone>
#include <QLocale>
#include <QStandardPaths>
#include <QPointer>
#include <QFileInfo>
#include <QFont>
#include <QRegularExpression>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>

// System backend helpers
namespace {

QString timeHelperPath() { return QString::fromLocal8Bit(CONTROL_TIME_HELPER_PATH); }

bool timeHelperAvailable()
{
    return TimeSyncConfig::trustedPath(timeHelperPath())
        && QFileInfo(timeHelperPath()).isExecutable();
}

QString providerArgument(TimeSync::Provider provider)
{
    return provider == TimeSync::Provider::Chrony ? QStringLiteral("chrony")
                                                : QStringLiteral("timesyncd");
}

// The locale's long date, but without the leading weekday name, Windows' date
// fields show "11 juli 2026", not "lördag 11 juli 2026". Strips the day-name
// token ('ddd'/'dddd') from the locale's own long-date pattern so the month
// order stays correct for every locale.
QString longDateNoWeekday(const QDate &d)
{
    QLocale loc;
    QString fmt = loc.dateFormat(QLocale::LongFormat);
    fmt.remove(QRegularExpression(QStringLiteral("d{3,4}[,]?\\s*")));
    fmt = fmt.trimmed();
    return loc.toString(d, fmt);
}

QString systemZoneId()
{
    QString id = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    if (id.isEmpty())
        id = QStringLiteral("UTC");
    return id;
}

// Windows-style label for a zone id: the grouped name if known, otherwise a
// synthesized "(UTC±HH:MM) Zone/Id" from the live offset.
QString zoneLabel(const QString &id)
{
    const QString known = windowsDisplayForIana(id);
    if (!known.isEmpty())
        return known;

    const QTimeZone tz(id.toUtf8());
    QString prefix = QStringLiteral("(UTC) ");
    if (tz.isValid()) {
        const int secs = tz.standardTimeOffset(QDateTime::currentDateTimeUtc());
        const QChar sign = secs >= 0 ? QLatin1Char('+') : QLatin1Char('-');
        const int mins = qAbs(secs) / 60;
        prefix = QStringLiteral("(UTC%1%2:%3) ")
                     .arg(sign)
                     .arg(mins / 60, 2, 10, QLatin1Char('0'))
                     .arg(mins % 60, 2, 10, QLatin1Char('0'));
    }
    return prefix + id;
}

// The Daylight-Saving-Time sentence Windows shows under the zone, computed from
// the zone's next transition. Empty when the zone doesn't observe DST.
QString dstNote(const QString &id)
{
    const QTimeZone tz(id.toUtf8());
    if (!tz.isValid() || !tz.hasDaylightTime())
        return QStringLiteral("This time zone does not observe Daylight Saving "
                              "Time.");

    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    const QTimeZone::OffsetData next = tz.nextTransition(nowUtc);
    if (!next.atUtc.isValid())
        return QString();

    // The wall-clock reading at the change is the UTC instant plus the offset in
    // effect just before it (so a fall-back reads 03:00, as Windows shows).
    const int offBefore = tz.offsetFromUtc(next.atUtc.addSecs(-1));
    QDateTime wall = next.atUtc.toUTC().addSecs(offBefore);

    const bool dstEnds = (next.daylightTimeOffset == 0);
    return QStringLiteral(
               "Daylight Saving Time %1 on %2 at %3. The clock is set to go %4 "
               "1 hour at that time.")
        .arg(dstEnds ? QStringLiteral("ends") : QStringLiteral("begins"))
        .arg(longDateNoWeekday(wall.date()))
        .arg(wall.toString(QStringLiteral("HH:mm")))
        .arg(dstEnds ? QStringLiteral("back") : QStringLiteral("forward"));
}

// The Windows UAC "administrator" shield, drawn to mark privileged buttons.
// Qt's QStyle::SP_VistaShield is empty off Windows, so we paint the familiar
// quartered shield ourselves (blue top, gold and red below, thin light cross).
QIcon uacShieldIcon()
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    QPainterPath shield;
    shield.moveTo(8, 0.5);
    shield.lineTo(15, 2.5);
    shield.lineTo(15, 8);
    shield.cubicTo(15, 12.2, 11.2, 15, 8, 15.6);
    shield.cubicTo(4.8, 15, 1, 12.2, 1, 8);
    shield.lineTo(1, 2.5);
    shield.closeSubpath();

    p.save();
    p.setClipPath(shield);
    p.fillRect(QRectF(0, 0, 16, 8), QColor("#2E77D0"));       // top: blue
    p.fillRect(QRectF(0, 8, 8, 8), QColor("#F2B01E"));        // lower-left: gold
    p.fillRect(QRectF(8, 8, 8, 8), QColor("#D0021B"));        // lower-right: red
    // The subtle light cross dividing the quarters.
    p.setPen(QPen(QColor(255, 255, 255, 200), 1));
    p.drawLine(QPointF(8, 1), QPointF(8, 15));
    p.drawLine(QPointF(1.5, 8), QPointF(14.5, 8));
    p.restore();

    p.setPen(QPen(QColor("#E8EEF6"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(shield);
    return QIcon(pm);
}

// A blue underline-on-hover help link, matching the app's other pages.
QLabel *makeHelpLink(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setPointSize(9);
    l->setFont(f);
    l->setEnabled(false);
    l->setStyleSheet("color: #777777; background: transparent;");
    l->setToolTip(QStringLiteral("This help link is not implemented yet."));
    return l;
}

} // namespace

// Dialog
DateTimeDialog::DateTimeDialog(QWidget *parent, std::function<TimeSync::Status()> statusReader)
    : QDialog(parent), m_statusReader(std::move(statusReader))
{
    m_timezone = systemZoneId();

    setWindowTitle(QStringLiteral("Date and Time"));
    setWindowIcon(themeIcon({"preferences-system-time", "clock",
                             "preferences-system"}));
    setModal(true);
    // Windows dialogs float white tab pages on a grey body; scope the grey to
    // the dialog itself so it doesn't bleed into the white tab pages.
    setStyleSheet("DateTimeDialog { background: #F0F0F0; }");

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(9, 9, 9, 9);
    root->setSpacing(8);

    m_tabs = new QTabWidget;
    m_tabs->addTab(buildDateTimeTab(), QStringLiteral("Date and Time"));
    m_tabs->addTab(buildAdditionalClocksTab(), QStringLiteral("Additional Clocks"));
    m_tabs->addTab(buildInternetTimeTab(), QStringLiteral("Internet Time"));
    root->addWidget(m_tabs, 1);

    // Real changes happen in the privileged subdialogs. There are no staged
    // settings here for a cosmetic Apply button to pretend to save.
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    root->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    refreshDateTime();
    refreshInternetStatus();

    // Tick the clock and the time value every second.
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &DateTimeDialog::refreshDateTime);
    m_timer->start();

    auto *statusTimer = new QTimer(this);
    statusTimer->setInterval(10000);
    connect(statusTimer, &QTimer::timeout, this, &DateTimeDialog::refreshInternetStatus);
    statusTimer->start();

    setFixedSize(432, 452);
}

void DateTimeDialog::done(int result)
{
    // Keep the owner alive until authorization and the bounded helper finish.
    if (!m_privilegedBusy)
        QDialog::done(result);
}

void DateTimeDialog::showTab(Tab tab)
{
    if (m_tabs)
        m_tabs->setCurrentIndex(tab);
}

// Tab 1: Date and Time
// A plain 9pt black label used throughout the tab.
static QLabel *dtLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setPointSize(9);
    l->setFont(f);
    l->setStyleSheet("color: #000000; background: transparent;");
    return l;
}

QWidget *DateTimeDialog::buildDateTimeTab()
{
    auto *page = new QWidget;
    page->setObjectName("dtPage");
    page->setStyleSheet("#dtPage { background: #FFFFFF; }");
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(14, 12, 14, 10);
    v->setSpacing(0);

    // Clock + Date/Time block.
    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(16);

    m_clock = new AnalogClock(110);
    top->addWidget(m_clock, 0, Qt::AlignTop);

    auto *info = new QVBoxLayout;
    info->setContentsMargins(0, 6, 0, 0);
    info->setSpacing(0);
    info->addWidget(dtLabel(QStringLiteral("Date:")));
    m_dateValue = dtLabel(QString());
    info->addWidget(m_dateValue);
    info->addSpacing(12);
    info->addWidget(dtLabel(QStringLiteral("Time:")));
    m_timeValue = dtLabel(QString());
    info->addWidget(m_timeValue);
    info->addStretch(1);
    top->addLayout(info, 1);
    v->addLayout(top);
    v->addSpacing(4);

    // "Change date and time…", a privileged action, so it carries the UAC
    // shield, right-aligned like Windows.
    auto *changeDtRow = new QHBoxLayout;
    changeDtRow->addStretch(1);
    auto *changeDt = new QPushButton(QStringLiteral("Change date and time…"));
    changeDt->setIcon(uacShieldIcon());
    changeDt->setCursor(Qt::PointingHandCursor);
    connect(changeDt, &QPushButton::clicked, this,
            &DateTimeDialog::openChangeDateTime);
    changeDtRow->addWidget(changeDt);
    v->addLayout(changeDtRow);
    v->addSpacing(12);

    // Section heading with a trailing hairline rule, the same divider the
    // System page uses.
    auto addHeading = [&](const QString &text) {
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        row->addWidget(dtLabel(text), 0, Qt::AlignVCenter);
        auto *line = new QFrame;
        line->setFrameShape(QFrame::HLine);
        line->setFixedHeight(1);
        line->setStyleSheet("QFrame { background: #DDDDDD; border: none; }");
        row->addWidget(line, 1, Qt::AlignVCenter);
        v->addLayout(row);
    };

    addHeading(QStringLiteral("Time zone"));
    v->addSpacing(6);

    m_tzValue = dtLabel(QString());
    m_tzValue->setContentsMargins(2, 0, 0, 0);
    v->addWidget(m_tzValue);
    v->addSpacing(6);

    auto *tzRow = new QHBoxLayout;
    tzRow->addStretch(1);
    auto *changeTz = new QPushButton(QStringLiteral("Change time zone…"));
    changeTz->setIcon(uacShieldIcon());
    changeTz->setCursor(Qt::PointingHandCursor);
    connect(changeTz, &QPushButton::clicked, this,
            &DateTimeDialog::openChangeTimeZone);
    tzRow->addWidget(changeTz);
    v->addLayout(tzRow);
    v->addSpacing(10);

    m_dstNote = dtLabel(QString());
    m_dstNote->setWordWrap(true);
    v->addWidget(m_dstNote);
    v->addSpacing(8);

    m_notifyCheck = new QCheckBox(QStringLiteral("Notify me when the clock changes"));
    {
        QFont f = m_notifyCheck->font();
        f.setPointSize(9);
        m_notifyCheck->setFont(f);
    }
    m_notifyCheck->setChecked(false);
    m_notifyCheck->setEnabled(false);
    m_notifyCheck->setToolTip(QStringLiteral("Clock-change notifications are not implemented yet."));
    v->addWidget(m_notifyCheck);

    v->addStretch(1);

    v->addWidget(makeHelpLink(
        QStringLiteral("Get more time zone information online")));
    v->addSpacing(2);
    v->addWidget(makeHelpLink(
        QStringLiteral("How do I set the clock and time zone?")));

    return page;
}

// Tab 2: Additional Clocks
QWidget *DateTimeDialog::buildAdditionalClocksTab()
{
    auto *page = new QWidget;
    page->setObjectName("acPage");
    page->setStyleSheet("#acPage { background: #FFFFFF; }");
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(16, 16, 16, 12);
    v->setSpacing(10);

    auto *intro = new QLabel(
        QStringLiteral("Additional taskbar clocks are not connected to this "
                       "Control Panel yet. These controls are unavailable."));
    {
        QFont f = intro->font();
        f.setPointSize(9);
        intro->setFont(f);
    }
    intro->setWordWrap(true);
    v->addWidget(intro);

    v->addSpacing(6);

    for (int i = 1; i <= 2; ++i) {
        // A checkable group box: the "Show this clock" checkbox is the box's
        // title, embedded in the top of a light framed rectangle, the same
        // hairline divider as the Time zone section, closed into a box like
        // Windows. Unchecking it disables the fields inside automatically.
        auto *box = new QGroupBox(QStringLiteral("Show this clock"));
        box->setCheckable(true);
        box->setChecked(false);
        box->setEnabled(false);
        box->setToolTip(QStringLiteral("Changing the taskbar clocks is not implemented yet."));
        {
            QFont f = box->font();
            f.setPointSize(9);
            box->setFont(f);
        }
        // The title's white background masks the border behind it, leaving the
        // Windows-style gap in the top edge where the checkbox sits.
        box->setStyleSheet(
            "QGroupBox { border: 1px solid #DDDDDD; border-radius: 3px;"
            " margin-top: 7px; background: transparent; }"
            "QGroupBox::title { subcontrol-origin: margin;"
            " subcontrol-position: top left; left: 8px; padding: 0 4px;"
            " background: #FFFFFF; color: #777777; }"
            "QGroupBox:disabled, QGroupBox:disabled::title { color: #777777; }");

        auto *bv = new QVBoxLayout(box);
        bv->setContentsMargins(18, 12, 14, 12);
        bv->setSpacing(4);

        auto *tzLabel = new QLabel(QStringLiteral("Select time zone:"));
        { QFont f = tzLabel->font(); f.setPointSize(9); tzLabel->setFont(f); }
        bv->addWidget(tzLabel);

        auto *tzCombo = new QComboBox;
        for (const WinTimeZone &z : windowsTimeZones())
            tzCombo->addItem(z.display, z.iana);
        // Default the selection to the current zone.
        int cur = tzCombo->findData(m_timezone);
        if (cur < 0)
            cur = tzCombo->findText(zoneLabel(m_timezone));
        tzCombo->setCurrentIndex(qMax(0, cur));
        bv->addWidget(tzCombo);
        bv->addSpacing(2);

        auto *nameLabel = new QLabel(QStringLiteral("Enter display name:"));
        { QFont f = nameLabel->font(); f.setPointSize(9); nameLabel->setFont(f); }
        bv->addWidget(nameLabel);

        auto *nameEdit = new QLineEdit(QStringLiteral("Clock %1").arg(i));
        bv->addWidget(nameEdit);

        // Stable names also let the regression tests check these placeholders.
        box->setObjectName(QStringLiteral("clock%1_show").arg(i));
        tzCombo->setObjectName(QStringLiteral("clock%1_tz").arg(i));
        nameEdit->setObjectName(QStringLiteral("clock%1_name").arg(i));

        v->addWidget(box);
        v->addSpacing(10);
    }

    v->addStretch(1);
    return page;
}

// Tab 3: Internet Time
QWidget *DateTimeDialog::buildInternetTimeTab()
{
    auto *page = new QWidget;
    page->setObjectName("itPage");
    page->setStyleSheet("#itPage { background: #FFFFFF; }");
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(16, 18, 16, 12);
    v->setSpacing(0);

    m_internetStatus = new QLabel;
    m_internetStatus->setObjectName(QStringLiteral("internetTimeStatus"));
    m_internetStatus->setTextFormat(Qt::PlainText);
    m_internetStatus->setText(QStringLiteral("Checking Internet time settings…"));
    {
        QFont f = m_internetStatus->font();
        f.setPointSize(9);
        m_internetStatus->setFont(f);
    }
    m_internetStatus->setWordWrap(true);
    v->addWidget(m_internetStatus);
    v->addSpacing(22);

    m_internetSched = new QLabel;
    m_internetSched->setObjectName(QStringLiteral("internetTimeSynchronization"));
    m_internetSched->setTextFormat(Qt::PlainText);
    {
        QFont f = m_internetSched->font();
        f.setPointSize(9);
        m_internetSched->setFont(f);
    }
    m_internetSched->setWordWrap(true);
    v->addWidget(m_internetSched);

    v->addStretch(1);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    m_internetChange = new QPushButton(QStringLiteral("Change settings…"));
    m_internetChange->setObjectName(QStringLiteral("internetTimeChange"));
    m_internetChange->setIcon(uacShieldIcon());
    m_internetChange->setEnabled(false);
    connect(m_internetChange, &QPushButton::clicked, this,
            &DateTimeDialog::openInternetSettings);
    btnRow->addWidget(m_internetChange);
    v->addLayout(btnRow);
    v->addSpacing(10);

    v->addWidget(makeHelpLink(
        QStringLiteral("What is Internet time synchronization?")));

    return page;
}

// Live refreshers
void DateTimeDialog::refreshDateTime()
{
    const QDateTime now = QDateTime::currentDateTime();
    if (m_dateValue)
        m_dateValue->setText(longDateNoWeekday(now.date()));
    if (m_timeValue)
        m_timeValue->setText(now.toString(QStringLiteral("HH:mm:ss")));
    if (m_clock)
        m_clock->update();
    if (m_tzValue)
        m_tzValue->setText(zoneLabel(m_timezone));
    if (m_dstNote)
        m_dstNote->setText(dstNote(m_timezone));
}

void DateTimeDialog::refreshInternetStatus()
{
    if (m_statusLoading || m_privilegedBusy)
        return;
    m_statusLoading = true;
    m_internetChange->setEnabled(false);
    const int generation = m_statusGeneration;
    Win7::runAsync(this, m_statusReader,
        [this, generation](TimeSync::Status status) {
            m_statusLoading = false;
            if (generation != m_statusGeneration) {
                refreshInternetStatus();
                return;
            }
            m_timeStatus = std::move(status);
            renderInternetStatus();
        });
}

void DateTimeDialog::renderInternetStatus()
{
    QString text = !m_timeStatus.enabled.has_value()
        ? QStringLiteral("Automatic time synchronization: Unknown.")
        : *m_timeStatus.enabled ? QStringLiteral("Automatic time synchronization: On.")
                               : QStringLiteral("Automatic time synchronization: Off.");
    if (m_timeStatus.provider != TimeSync::Provider::Unknown)
        text += QStringLiteral("\nTime service: %1.").arg(TimeSync::providerName(m_timeStatus.provider));
    if (!m_timeStatus.activeSource.isEmpty())
        text += QStringLiteral("\nCurrent source: %1.").arg(m_timeStatus.activeSource);
    if (!m_timeStatus.configuredServer.isEmpty())
        text += QStringLiteral("\n%1: %2.")
            .arg(m_timeStatus.provider == TimeSync::Provider::Chrony
                     ? QStringLiteral("Preferred server") : QStringLiteral("Configured server"),
                 m_timeStatus.configuredServer);
    m_internetStatus->setText(text);
    QString sync = !m_timeStatus.synchronized.has_value()
        ? QStringLiteral("Clock synchronization could not be verified.")
        : *m_timeStatus.synchronized ? QStringLiteral("The system clock reports synchronized.")
                                    : QStringLiteral("The system clock does not currently report synchronization.");
    if (!m_timeStatus.detail.isEmpty())
        sync += QLatin1Char('\n') + m_timeStatus.detail;
    m_internetSched->setText(sync);
    const bool available = m_timeStatus.provider != TimeSync::Provider::Unknown
        && m_timeStatus.enabled.has_value() && QFileInfo::exists(QStringLiteral("/usr/bin/pkexec"))
        && QFileInfo::exists(QStringLiteral("/usr/bin/timedatectl"));
    m_internetChange->setEnabled(available && !m_privilegedBusy);
    m_internetChange->setToolTip(available ? QString()
        : QStringLiteral("A supported time service and its state must be available before changing these settings."));
}

// Privileged actions
void DateTimeDialog::runPrivileged(const QStringList &argv,
                                   std::function<void(bool)> onDone)
{
    if (m_privilegedBusy)
        return;
    m_privilegedBusy = true;
    ++m_statusGeneration;
    m_tabs->setEnabled(false);
    m_buttons->setEnabled(false);
    auto *proc = new QProcess(this);
    auto complete = [this, proc, onDone](bool success, const QString &message) {
        m_privilegedBusy = false;
        m_tabs->setEnabled(true);
        m_buttons->setEnabled(true);
        if (!message.isEmpty())
            QMessageBox::warning(this, QStringLiteral("Date and Time"), message);
        if (onDone)
            onDone(success);
        refreshInternetStatus();
        proc->deleteLater();
    };
    connect(proc, &QProcess::errorOccurred, this, [complete, proc](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            complete(false, QStringLiteral("Could not start administrative authorization: %1").arg(proc->errorString()));
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [complete, proc](int code, QProcess::ExitStatus status) {
                const bool success = status == QProcess::NormalExit && code == 0;
                QString message;
                if (!success) {
                    if (status == QProcess::NormalExit && code == 126)
                        message = QStringLiteral("Authentication was cancelled. The change was not applied.");
                    else {
                        message = QString::fromUtf8(proc->readAllStandardError()).trimmed().left(2000);
                        if (message.isEmpty())
                            message = QStringLiteral("The time-setting operation failed (exit %1).").arg(code);
                    }
                }
                complete(success, message);
            });
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    proc->setProcessEnvironment(env);
    proc->start(QStringLiteral("/usr/bin/pkexec"), argv);
}

void DateTimeDialog::openChangeDateTime()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Date and Time Settings"));
    dlg.setModal(true);
    auto *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(16, 16, 16, 12);
    v->setSpacing(12);

    auto *row = new QHBoxLayout;
    row->setSpacing(18);

    auto *cal = new QCalendarWidget;
    cal->setGridVisible(false);
    cal->setSelectedDate(QDate::currentDate());
    row->addWidget(cal, 1);

    auto *right = new QVBoxLayout;
    right->setSpacing(10);
    auto *clock = new AnalogClock(96);
    right->addWidget(clock, 0, Qt::AlignHCenter);
    auto *timeEdit = new QTimeEdit(QTime::currentTime());
    timeEdit->setDisplayFormat(QStringLiteral("HH:mm:ss"));
    timeEdit->setAlignment(Qt::AlignCenter);
    right->addWidget(timeEdit);
    right->addStretch(1);
    row->addLayout(right, 0);

    v->addLayout(row, 1);

    auto *hint = new QLabel;
    {
        QFont f = hint->font();
        f.setPointSize(9);
        hint->setFont(f);
    }
    hint->setStyleSheet("color: #555555;");
    if (m_timeStatus.enabled.value_or(false))
        hint->setText(QStringLiteral(
            "Internet time synchronization is on; turn it off in the Internet "
            "Time tab before setting the clock manually."));
    else if (!m_timeStatus.enabled.has_value())
        hint->setText(QStringLiteral("Automatic synchronization could not be verified. The system may reject manual clock changes."));
    hint->setWordWrap(true);
    v->addWidget(hint);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok
                                    | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setEnabled(!m_timeStatus.enabled.value_or(false));
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const QDateTime chosen(cal->selectedDate(), timeEdit->time());
    const QString stamp = chosen.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    runPrivileged({ QStringLiteral("/usr/bin/timedatectl"), QStringLiteral("set-time"),
                    stamp },
                  [this](bool success) { if (success) refreshDateTime(); });
}

void DateTimeDialog::openChangeTimeZone()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Time Zone Settings"));
    dlg.setModal(true);
    auto *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(16, 16, 16, 12);
    v->setSpacing(10);

    auto *label = new QLabel(QStringLiteral("Time zone:"));
    v->addWidget(label);

    auto *combo = new QComboBox;
    for (const WinTimeZone &z : windowsTimeZones())
        combo->addItem(z.display, z.iana);
    int cur = combo->findData(m_timezone);
    if (cur < 0) {
        // The system zone isn't one of the grouped Windows zones: add it so the
        // current setting is always represented and selectable.
        combo->addItem(zoneLabel(m_timezone), m_timezone);
        cur = combo->count() - 1;
    }
    combo->setCurrentIndex(cur);
    v->addWidget(combo);
    v->addSpacing(6);

    auto *dst = new QCheckBox(
        QStringLiteral("Automatically adjust clock for Daylight Saving Time"));
    dst->setChecked(true);
    dst->setEnabled(false);   // IANA zones handle DST automatically
    v->addWidget(dst);

    v->addStretch(1);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok
                                    | QDialogButtonBox::Cancel);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString id = combo->currentData().toString();
    if (id.isEmpty() || id == m_timezone)
        return;
    runPrivileged({ QStringLiteral("/usr/bin/timedatectl"),
                    QStringLiteral("set-timezone"), id },
                  [this](bool success) {
                      if (success) {
                          m_timezone = systemZoneId();
                          refreshDateTime();
                      }
                  });
}

void DateTimeDialog::openInternetSettings()
{
    const TimeSync::Status status = m_timeStatus;
    if (status.provider == TimeSync::Provider::Unknown || !status.enabled.has_value())
        return;
    const bool helperAvailable = timeHelperAvailable();
    const bool canEditServer = helperAvailable && status.canEditServer;
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Internet Time Settings"));
    dlg.setModal(true);
    auto *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(16, 16, 16, 12);
    v->setSpacing(10);

    v->addWidget(new QLabel(QStringLiteral("Time service: %1").arg(TimeSync::providerName(status.provider))));

    auto *sync = new QCheckBox(
        QStringLiteral("Synchronize with an Internet time server"));
    sync->setChecked(*status.enabled);
    v->addWidget(sync);

    auto *serverRow = new QHBoxLayout;
    serverRow->setSpacing(8);
    auto *serverLabel = new QLabel(status.provider == TimeSync::Provider::Chrony
        ? QStringLiteral("Preferred server:") : QStringLiteral("Server:"));
    serverRow->addWidget(serverLabel);
    auto *serverCombo = new QComboBox;
    serverCombo->setEditable(true);
    serverCombo->addItem(QStringLiteral("System defaults"), QString());
    // Preserve the configured source instead of silently selecting a public
    // third-party service when the user only toggles synchronization.
    if (!status.configuredServer.isEmpty()) {
        serverCombo->addItem(status.configuredServer, status.configuredServer);
        serverCombo->setCurrentIndex(1);
    }
    serverRow->addWidget(serverCombo, 1);
    auto *update = new QPushButton(QStringLiteral("Update now"));
    serverRow->addWidget(update);
    v->addLayout(serverRow);

    auto selectedServer = [serverCombo]() {
        const int index = serverCombo->currentIndex();
        if (index >= 0 && serverCombo->currentText() == serverCombo->itemText(index))
            return serverCombo->itemData(index).toString();
        return serverCombo->currentText().trimmed();
    };
    auto updateControls = [=]() {
        serverLabel->setEnabled(sync->isChecked() && canEditServer);
        serverCombo->setEnabled(sync->isChecked() && canEditServer);
        const bool pending = sync->isChecked() != *status.enabled
            || selectedServer() != status.configuredServer;
        update->setEnabled(*status.enabled && sync->isChecked() && !pending);
        update->setToolTip(pending ? QStringLiteral("Apply the changed settings before requesting synchronization.")
                                  : QStringLiteral("Request fresh time measurements; synchronization may take time."));
    };
    updateControls();
    connect(sync, &QCheckBox::toggled, &dlg, updateControls);
    connect(serverCombo, &QComboBox::currentTextChanged, &dlg, updateControls);
    if (!canEditServer)
        serverCombo->setToolTip(helperAvailable
            ? QStringLiteral("The current Chrony configuration does not support this server editor.")
            : QStringLiteral("Server editing requires a trusted root-owned helper and system authorization policy. It is unavailable in a user-local installation."));

    v->addSpacing(4);
    auto *note = new QLabel;
    {
        QFont f = note->font();
        f.setPointSize(9);
        note->setFont(f);
    }
    note->setWordWrap(true);
    note->setStyleSheet("color: #555555;");
    note->setText(status.provider == TimeSync::Provider::Chrony
        ? QStringLiteral("A preferred server is added alongside the existing sources. System defaults removes only this preference. Update now requests new measurements; it does not force a clock jump.")
        : QStringLiteral("System defaults restores the other configured time sources. Update now requests synchronization; it does not confirm success."));
    if (!helperAvailable)
        note->setText(note->text() + QStringLiteral("\nServer editing is unavailable in a user-local installation: it requires a trusted root-owned helper and system authorization policy. Supported synchronization toggles and refresh requests use existing system tools."));
    v->addWidget(note);

    connect(update, &QPushButton::clicked, &dlg, [this, status, helperAvailable, note, &dlg]() {
        const QPointer<QDialog> dialogGuard(&dlg);
        const QPointer<QLabel> noteGuard(note);
        dlg.setEnabled(false);
        const auto launch = [this, dialogGuard, noteGuard](const QStringList &command) {
            runPrivileged(command, [dialogGuard, noteGuard](bool success) {
                if (dialogGuard)
                    dialogGuard->setEnabled(true);
                if (success && noteGuard)
                    noteGuard->setText(QStringLiteral("A synchronization request was accepted. Close this dialog to view the current synchronization status."));
            });
        };
        if (helperAvailable) {
            // The helper verifies the current provider again as root.
            launch({timeHelperPath(), QStringLiteral("--refresh"), providerArgument(status.provider)});
            return;
        }
        Win7::runAsync(this, m_statusReader,
            [this, status, dialogGuard, noteGuard, launch](TimeSync::Status fresh) {
                if (!dialogGuard)
                    return;
                if (fresh.provider != status.provider || !fresh.enabled.value_or(false)) {
                    dialogGuard->setEnabled(true);
                    if (noteGuard)
                        noteGuard->setText(QStringLiteral("The time service changed or its state could not be verified. Reopen Internet Time settings before requesting synchronization."));
                    m_timeStatus = std::move(fresh);
                    renderInternetStatus();
                    return;
                }
                if (fresh.provider == TimeSync::Provider::Chrony)
                    launch({QStringLiteral("/usr/bin/chronyc"), QStringLiteral("-h"),
                            QStringLiteral("/run/chrony/chronyd.sock"), QStringLiteral("burst"), QStringLiteral("4/4")});
                else
                    // Do not start an old provider if it stops after the probe.
                    launch({QStringLiteral("/usr/bin/systemctl"), QStringLiteral("try-restart"),
                            QStringLiteral("systemd-timesyncd.service")});
            });
    });

    v->addStretch(1);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok
                                    | QDialogButtonBox::Cancel);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, [=, &dlg]() {
        const QString server = selectedServer();
        if (sync->isChecked() && !server.isEmpty() && !TimeSync::validServer(server)) {
            QMessageBox::warning(&dlg, QStringLiteral("Internet Time Settings"),
                QStringLiteral("Enter a single DNS host name or IPv4/IPv6 address. Spaces, options, URLs and command characters are not allowed."));
            return;
        }
        dlg.accept();
    });
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const bool wantSync = sync->isChecked();
    const QString server = selectedServer();
    const bool serverChanged = wantSync && server != status.configuredServer;
    if (wantSync == *status.enabled && !serverChanged)
        return;
    if (serverChanged && !canEditServer) {
        QMessageBox::warning(this, QStringLiteral("Internet Time Settings"),
                             QStringLiteral("Server editing is unavailable. No changes were applied."));
        return;
    }
    // One ordered privileged operation saves the validated source, changes
    // service state and reloads it. The helper validates again as root.
    const QStringList command = helperAvailable && (status.canEditServer || !wantSync)
        ? QStringList{timeHelperPath(), QStringLiteral("--configure"), providerArgument(status.provider),
                      wantSync ? QStringLiteral("on") : QStringLiteral("off"), server}
        : QStringList{QStringLiteral("/usr/bin/timedatectl"), QStringLiteral("set-ntp"),
                      wantSync ? QStringLiteral("true") : QStringLiteral("false")};
    runPrivileged(command, [](bool) {});
}
