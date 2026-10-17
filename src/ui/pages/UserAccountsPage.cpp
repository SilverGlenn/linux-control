#include "UserAccountsPage.h"
#include "settings/AccountSettings.h"
#include "LinkLabel.h"
#include "IconHelper.h"
#include "Win7Ui.h"

#include <QScrollArea>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QFont>
#include <QFile>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QLineEdit>
#include <QPushButton>
#include <QFormLayout>
#include <QSignalBlocker>

#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <vector>

// Data gathering
UserAccountsPage::Account UserAccountsPage::gatherAccount()
{
    Account a;

    const uid_t uid = getuid();
    const passwd *pw = getpwuid(uid);
    if (pw) {
        a.userName = QString::fromLocal8Bit(pw->pw_name);
        // GECOS is a comma-separated list; the first field is the full name.
        a.fullName = QString::fromLocal8Bit(pw->pw_gecos).section(QLatin1Char(','), 0, 0);

        // The account is an "Administrator" if it belongs to the wheel or sudo
        // group, the standard admin groups a polkit/sudo policy grants rights to.
        int ngroups = 0;
        getgrouplist(pw->pw_name, pw->pw_gid, nullptr, &ngroups);
        if (ngroups > 0) {
            std::vector<gid_t> gids(ngroups);
            if (getgrouplist(pw->pw_name, pw->pw_gid, gids.data(), &ngroups) != -1) {
                for (gid_t g : gids) {
                    if (const group *gr = getgrgid(g)) {
                        const QString name = QString::fromLocal8Bit(gr->gr_name);
                        if (name == QLatin1String("wheel")
                            || name == QLatin1String("sudo")) {
                            a.accountType = QStringLiteral("Member of sudo or wheel group");
                            break;
                        }
                    }
                }
            }
        }
    }
    if (a.userName.isEmpty())
        a.userName = QString::fromLocal8Bit(qgetenv("USER"));
    if (a.fullName.isEmpty())
        a.fullName = a.userName;
    if (uid == 0)
        a.accountType = QStringLiteral("Administrator (root)");
    else if (a.accountType.isEmpty())
        a.accountType = QStringLiteral("No sudo or wheel group membership detected");

    // The account picture: user dotfiles first, then the AccountsService icon
    // that KDE/GDM login screens use.
    const QString home = QString::fromLocal8Bit(qgetenv("HOME"));
    const QStringList candidates = {
        home + QStringLiteral("/.face.icon"),
        home + QStringLiteral("/.face"),
        QStringLiteral("/var/lib/AccountsService/icons/") + a.userName,
    };
    for (const QString &path : candidates) {
        if (QFile::exists(path)) {
            a.picturePath = path;
            break;
        }
    }

    return a;
}

// Sidebar
QList<SidebarLink> UserAccountsPage::sidebarLinks()
{
    return {
        Nav::plain("Manage another account"),
        Nav::plain("Change User Account Control settings"),
    };
}

QList<SidebarLink> UserAccountsPage::sidebarSeeAlso()
{
    return {
        Nav::plain("Parental Controls"),
        Nav::plain("Credential Manager"),
    };
}

// Render the avatar as a rounded, framed thumbnail; fall back to a theme icon
// when the user has no picture of their own.
static QPixmap avatarPixmap(const QString &path, int size)
{
    QPixmap src;
    if (!path.isEmpty())
        src.load(path);
    if (src.isNull())
        src = themeIcon({"user-identity", "avatar-default",
                         "system-users"}).pixmap(size, size);

    QPixmap out(size, size);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, size, size), 6, 6);
    p.setClipPath(clip);
    const QPixmap scaled = src.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                                      Qt::SmoothTransformation);
    p.drawPixmap((size - scaled.width()) / 2, (size - scaled.height()) / 2, scaled);

    p.setClipping(false);
    p.setPen(QPen(QColor("#9DA7B5"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(0.5, 0.5, size - 1, size - 1), 6, 6);
    return out;
}

// Page
UserAccountsPage::UserAccountsPage(QScrollArea *sidebar, QWidget *parent)
    : UserAccountsPage(sidebar, AccountSettings::instance(), parent) {}

UserAccountsPage::UserAccountsPage(QScrollArea *sidebar, AccountSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    const Account acct = gatherAccount();

    // Windows 7 lays the content out at a fixed width and leaves the rest of
    // the window blank on the right rather than stretching to fill it.
    auto *contentV = Win7::pageScaffold(this, sidebar, /*bottomMargin=*/20,
                                        /*fixedWidth=*/700);
    Win7::styleSettingsControls(this);
    setStyleSheet(styleSheet() + QStringLiteral(
        " QLineEdit { color: #000000; background: #FFFFFF; border: 1px solid #ABADB3; padding: 4px 6px; }"
        " QLineEdit:focus { border-color: #3C7FB1; }"
        " QLineEdit:disabled { color: #777777; background: #F0F0F0; }"));

    // Page title.
    contentV->addWidget(Win7::pageTitle("Make changes to your user account"));
    contentV->addSpacing(18);

    // Body: task links on the left, the account summary card on the right.
    auto *body = new QHBoxLayout;
    body->setContentsMargins(6, 0, 0, 0);
    body->setSpacing(24);

    auto *tasks = new QVBoxLayout;
    tasks->setContentsMargins(0, 0, 0, 0);
    tasks->setSpacing(12);

    auto *changeName = new LinkLabel("Change your display name");
    tasks->addWidget(changeName, 0, Qt::AlignLeft);
    for (const QString &text : {QStringLiteral("Change your password"), QStringLiteral("Change your picture"),
                                QStringLiteral("Change your account type"), QStringLiteral("Manage another account")}) {
        auto *link = new LinkLabel(text);
        link->setEnabled(false);
        link->setToolTip("This page currently supports your own display name. Other account changes are unavailable here.");
        tasks->addWidget(link, 0, Qt::AlignLeft);
    }
    tasks->addStretch(1);
    body->addLayout(tasks, 0);

    // Account summary card: avatar, then name / type / password state.
    auto *card = new QHBoxLayout;
    card->setContentsMargins(0, 0, 0, 0);
    card->setSpacing(14);

    auto *avatar = new QLabel;
    avatar->setFixedSize(96, 96);
    avatar->setPixmap(avatarPixmap(acct.picturePath, 96));
    avatar->setStyleSheet("background: transparent;");
    card->addWidget(avatar, 0, Qt::AlignTop);

    auto *summary = new QVBoxLayout;
    summary->setContentsMargins(0, 2, 0, 0);
    summary->setSpacing(2);
    m_summaryName = Win7::label(acct.fullName, 11, "#1A3C7A");
    summary->addWidget(m_summaryName);
    summary->addWidget(Win7::label(acct.accountType));
    summary->addWidget(Win7::label("Password status unavailable", 9, "#777777"));
    summary->addStretch(1);
    card->addLayout(summary, 0);

    body->addLayout(card, 0);
    body->addStretch(1);

    contentV->addLayout(body);
    contentV->addSpacing(24);
    contentV->addWidget(Win7::label("Change your display name", 11, "#1A3C7A"));
    contentV->addSpacing(10);
    auto *explanation = Win7::label("This name identifies your account in the desktop. Your login name and user ID stay the same. Apply may ask you to authorize the change.", 9, "#555555");
    explanation->setWordWrap(true);
    contentV->addWidget(explanation);
    contentV->addSpacing(10);
    auto *form = new QFormLayout;
    m_name = new QLineEdit;
    m_name->setObjectName("accountDisplayName");
    m_name->setAccessibleName("Display name");
    m_name->setMaxLength(256);
    auto *nameLabel = Win7::label("&Display name:"); nameLabel->setBuddy(m_name);
    form->addRow(nameLabel, m_name);
    m_identity = Win7::label(acct.userName + QStringLiteral("  ·  UID %1").arg(::getuid()));
    m_identity->setObjectName("accountIdentity");
    m_identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(Win7::label("Current account:"), m_identity);
    contentV->addLayout(form);
    contentV->addSpacing(12);
    m_status = Win7::label("Reading your current account…", 9, "#555555");
    m_status->setObjectName("accountStatus");
    m_status->setWordWrap(true);
    contentV->addWidget(m_status);
    contentV->addSpacing(12);
    auto *buttons = new QHBoxLayout;
    m_apply = new QPushButton("&Apply");
    m_apply->setObjectName("accountApply");
    m_apply->setIcon(themeIcon({"security-high", "dialog-password"}));
    m_revert = new QPushButton("&Revert");
    m_revert->setObjectName("accountRevert");
    m_revert->setToolTip("Discard your staged name and read the current account again.");
    m_cancel = new QPushButton("Cancel &pending change");
    m_cancel->setObjectName("accountCancel");
    buttons->addWidget(m_apply); buttons->addWidget(m_revert); buttons->addWidget(m_cancel); buttons->addStretch();
    contentV->addLayout(buttons);
    contentV->addStretch(1);
    connect(changeName, &LinkLabel::clicked, m_name, [this] { m_name->setFocus(); m_name->selectAll(); });
    connect(m_name, &QLineEdit::textEdited, session, &AccountSettings::selectName);
    connect(m_apply, &QPushButton::clicked, session, &AccountSettings::apply);
    connect(m_revert, &QPushButton::clicked, session, &AccountSettings::revert);
    connect(m_cancel, &QPushButton::clicked, session, &AccountSettings::cancel);
    connect(session, &AccountSettings::stateChanged, this, &UserAccountsPage::updateState);
    updateState();
}
SettingsSession *UserAccountsPage::settingsSession() const { return m_session; }
void UserAccountsPage::updateState()
{
    const QSignalBlocker blocker(m_name);
    if (m_name->text() != m_session->stagedName()) m_name->setText(m_session->stagedName());
    const auto account = m_session->account();
    if (m_session->isReady()) {
        m_summaryName->setText(account.realName.isEmpty() ? account.userName : account.realName);
        m_identity->setText(account.userName + QStringLiteral("  ·  UID %1").arg(account.uid));
    }
    m_name->setEnabled(!m_session->isBusy() && m_session->canApply());
    m_apply->setEnabled(!m_session->isBusy() && m_session->canApply() && m_session->isDirty()
                        && !m_session->hasConflict() && AccountSettings::validName(m_session->stagedName()));
    m_revert->setEnabled(!m_session->isBusy());
    m_cancel->setEnabled(m_session->canCancel());
    m_cancel->setToolTip(m_session->canCancel() ? "Cancel before an account change is sent."
                        : "A request already sent to AccountsService cannot be canceled here.");
    QString status = m_session->errorText();
    if (status.isEmpty()) status = m_session->resultText();
    if (status.isEmpty()) status = m_session->isBusy() ? "Working with AccountsService…"
        : !m_session->canApply() ? m_session->unavailableReason()
        : m_session->isDirty() ? "Your display name is staged. Select Apply to save it."
        : "Only your current account's display name can be changed here.";
    m_status->setText(status);
}
