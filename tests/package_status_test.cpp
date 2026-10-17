#include "../src/ui/pages/LinuxUpdatePage.h"

#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QStandardPaths>

namespace {

class ScopedPath final {
public:
    explicit ScopedPath(const QString &path)
        : m_wasSet(qEnvironmentVariableIsSet("PATH")),
          m_previous(qgetenv("PATH"))
    {
        qputenv("PATH", path.toLocal8Bit());
    }

    ~ScopedPath()
    {
        if (m_wasSet) qputenv("PATH", m_previous);
        else qunsetenv("PATH");
    }

private:
    bool m_wasSet;
    QByteArray m_previous;
};

static bool writeExecutable(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (file.write(contents) != contents.size())
        return false;
    file.close();
    return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner
                               | QFile::ExeOwner | QFile::ReadGroup
                               | QFile::ExeGroup | QFile::ReadOther
                               | QFile::ExeOther);
}

static QString makeBinDir(QTemporaryDir &dir)
{
    const QString bin = dir.filePath(QStringLiteral("bin"));
    return QDir().mkpath(bin) ? bin : QString();
}

static bool hasVisibleLabel(QWidget &root, const QString &fragment)
{
    for (QLabel *label : root.findChildren<QLabel *>())
        if (label->isVisible() && label->text().contains(fragment,
                                                          Qt::CaseInsensitive))
            return true;
    return false;
}

static QPushButton *installButton(QWidget &root)
{
    for (QPushButton *button : root.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("Install updates"))
            return button;
    return nullptr;
}

} // namespace

class PackageStatusTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void packagePageGatesWhenPacmanIsMissing();
    void successfulCheckWithMalformedOutputIsUnavailable();
    void repositoryUpdatesRemainInstallableWhenYayIsMissing();
    void successfulButMalformedYayResultsAreUnknown();
    void emptyRepositoryResultWithUnknownAurIsIncomplete();

private:
    QTemporaryDir m_settingsDir;
    QByteArray m_oldConfigHome;
    QByteArray m_oldDataHome;
    bool m_configHomeWasSet = false;
    bool m_dataHomeWasSet = false;
};

void PackageStatusTest::initTestCase()
{
    QVERIFY(m_settingsDir.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("linux-control-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("package-status-tests"));
    QStandardPaths::setTestModeEnabled(true);

    m_configHomeWasSet = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    m_dataHomeWasSet = qEnvironmentVariableIsSet("XDG_DATA_HOME");
    m_oldConfigHome = qgetenv("XDG_CONFIG_HOME");
    m_oldDataHome = qgetenv("XDG_DATA_HOME");
    QVERIFY(QDir().mkpath(m_settingsDir.filePath(QStringLiteral("config"))));
    QVERIFY(QDir().mkpath(m_settingsDir.filePath(QStringLiteral("data"))));
    qputenv("XDG_CONFIG_HOME", m_settingsDir.filePath(QStringLiteral("config")).toLocal8Bit());
    qputenv("XDG_DATA_HOME", m_settingsDir.filePath(QStringLiteral("data")).toLocal8Bit());
}

void PackageStatusTest::cleanupTestCase()
{
    if (m_configHomeWasSet) qputenv("XDG_CONFIG_HOME", m_oldConfigHome);
    else qunsetenv("XDG_CONFIG_HOME");
    if (m_dataHomeWasSet) qputenv("XDG_DATA_HOME", m_oldDataHome);
    else qunsetenv("XDG_DATA_HOME");
}

void PackageStatusTest::packagePageGatesWhenPacmanIsMissing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = makeBinDir(dir);
    QVERIFY(!bin.isEmpty());
    ScopedPath path(bin);

    LinuxUpdatePage page(new QScrollArea);
    page.resize(900, 650);
    page.show();

    QTRY_VERIFY(hasVisibleLabel(page, QStringLiteral("Package updates are unavailable")));
    QVERIFY(!hasVisibleLabel(page, QStringLiteral("up to date")));
}

void PackageStatusTest::successfulCheckWithMalformedOutputIsUnavailable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = makeBinDir(dir);
    QVERIFY(!bin.isEmpty());
    QVERIFY(writeExecutable(dir.filePath(QStringLiteral("bin/pacman")),
                           "#!/bin/sh\nexit 0\n"));
    QVERIFY(writeExecutable(dir.filePath(QStringLiteral("bin/checkupdates")),
                           "#!/bin/sh\nprintf 'not a package update\\n'\nexit 0\n"));
    ScopedPath path(bin);

    LinuxUpdatePage page(new QScrollArea);
    page.resize(900, 650);
    page.show();
    page.checkForUpdates();

    QTRY_VERIFY(hasVisibleLabel(page,
        QStringLiteral("The repository update check returned no valid package results")));
    QVERIFY(!hasVisibleLabel(page, QStringLiteral("up to date")));
    QPushButton *button = installButton(page);
    QVERIFY(button);
    QVERIFY(!button->isEnabled());
}

void PackageStatusTest::repositoryUpdatesRemainInstallableWhenYayIsMissing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = makeBinDir(dir);
    QVERIFY(!bin.isEmpty());
    QVERIFY(writeExecutable(dir.filePath(QStringLiteral("bin/pacman")),
                           "#!/bin/sh\nexit 0\n"));
    QVERIFY(writeExecutable(dir.filePath(QStringLiteral("bin/checkupdates")),
                           "#!/bin/sh\nprintf 'sample 1.0 -> 1.1\\n'\nexit 0\n"));
    ScopedPath path(bin);

    LinuxUpdatePage page(new QScrollArea);
    page.resize(900, 650);
    page.show();
    page.checkForUpdates();

    QTRY_VERIFY(hasVisibleLabel(page, QStringLiteral("Install updates for your computer")));
    QTRY_VERIFY(hasVisibleLabel(page, QStringLiteral("AUR update status is unavailable")));
    QPushButton *button = installButton(page);
    QVERIFY(button);
    QVERIFY(button->isVisible());
    QVERIFY(button->isEnabled());
}

void PackageStatusTest::successfulButMalformedYayResultsAreUnknown()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = makeBinDir(dir);
    QVERIFY(!bin.isEmpty());
    QVERIFY(writeExecutable(dir.filePath(QStringLiteral("bin/pacman")),
                           "#!/bin/sh\nexit 0\n"));
    QVERIFY(writeExecutable(dir.filePath(QStringLiteral("bin/checkupdates")),
                           "#!/bin/sh\nprintf 'sample 1.0 -> 1.1\\n'\nexit 0\n"));
    QVERIFY(writeExecutable(dir.filePath(QStringLiteral("bin/yay")),
                           "#!/bin/sh\nprintf 'unexpected yay output\\n'\nexit 0\n"));
    ScopedPath path(bin);

    LinuxUpdatePage page(new QScrollArea);
    page.resize(900, 650);
    page.show();
    page.checkForUpdates();

    QTRY_VERIFY(hasVisibleLabel(page,
        QStringLiteral("AUR update status is unavailable because yay returned results")));
    QPushButton *button = installButton(page);
    QVERIFY(button);
    QVERIFY(button->isVisible());
    QVERIFY(button->isEnabled());
}

void PackageStatusTest::emptyRepositoryResultWithUnknownAurIsIncomplete()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = makeBinDir(dir);
    QVERIFY(!bin.isEmpty());
    QVERIFY(writeExecutable(dir.filePath(QStringLiteral("bin/pacman")),
                           "#!/bin/sh\nexit 0\n"));
    QVERIFY(writeExecutable(dir.filePath(QStringLiteral("bin/checkupdates")),
                           "#!/bin/sh\nexit 2\n"));
    ScopedPath path(bin);

    LinuxUpdatePage page(new QScrollArea);
    page.resize(900, 650);
    page.show();
    page.checkForUpdates();

    QTRY_VERIFY(hasVisibleLabel(page, QStringLiteral("Update status incomplete")));
    QTRY_VERIFY(hasVisibleLabel(page, QStringLiteral("AUR update status is unavailable")));
    QVERIFY(!hasVisibleLabel(page, QStringLiteral("up to date")));
    QPushButton *button = installButton(page);
    QVERIFY(button);
    QVERIFY(!button->isEnabled());
}

QTEST_MAIN(PackageStatusTest)
#include "package_status_test.moc"
