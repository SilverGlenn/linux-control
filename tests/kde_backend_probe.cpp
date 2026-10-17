// Manual, read-only installed-KDE probe. Never select a value or call apply().
// --simulate-packages optionally runs the hardened read-only APT preview.
#include "settings/AppearanceSettings.h"
#include "settings/PowerPolicySettings.h"
#include "settings/DisplaySettings.h"
#include "platform/kde/AppearanceBackend.h"
#include "platform/kde/PowerPolicyBackend.h"
#include "display/KdeDisplayBackend.h"
#include "settings/WallpaperSettings.h"
#include "settings/ThemeComponentSettings.h"
#include "settings/FontSettings.h"
#include "platform/kde/WallpaperBackend.h"
#include "platform/kde/ThemeComponentBackend.h"
#include "platform/kde/FontBackend.h"
#include "settings/AeroEffectSettings.h"
#include "platform/kde/AeroEffectBackend.h"
#include "settings/AeroThemeSettings.h"
#include "settings/AeroThemeContract.h"
#include "platform/kde/AeroThemeBackend.h"
#include "settings/AccountSettings.h"
#include "platform/accounts/AccountsBackend.h"
#include "packages/PackageSimulationService.h"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    AppearanceSettings appearance(new KdeAppearanceBackend);
    PowerPolicySettings power(new KdePowerPolicyBackend);
    DisplaySettings display(new KdeDisplayBackend);
    WallpaperSettings wallpaper(new KdeWallpaperBackend);
    ThemeComponentSettings components(new KdeThemeComponentBackend);
    FontSettings fonts(new KdeFontBackend);
    AeroEffectSettings aero(new KdeAeroEffectBackend);
    AeroThemeSettings aeroTheme(new KdeAeroThemeBackend);
    AccountSettings account(new AccountsBackend);
    auto *simulation = PackageSimulationService::instance();
    const bool simulatePackages = app.arguments().contains(QStringLiteral("--simulate-packages"));
    if (simulatePackages) QTimer::singleShot(0, simulation, &PackageSimulationService::startPreview);
    bool timedOut = false;
    QTimer poll;
    poll.setInterval(50);
    auto complete = [&] {
        return !appearance.isBusy() && !power.isBusy() && !display.isBusy()
            && !wallpaper.isBusy() && !components.isBusy() && !fonts.isBusy()
            && !aero.isBusy() && !aeroTheme.isBusy() && !account.isBusy()
            && (!simulatePackages || (!simulation->isBusy() && simulation->state() != PackageSimulationService::State::Idle))
            && !appearance.unavailableReason().startsWith(QStringLiteral("Checking"));
    };
    QObject::connect(&poll, &QTimer::timeout, &app, [&] { if (complete()) app.quit(); });
    QTimer::singleShot(simulatePackages ? 70000 : 10000, &app, [&] {
        timedOut = true;
        if (simulation->isBusy()) { simulation->cancel(); QTimer::singleShot(1000, &app, &QCoreApplication::quit); }
        else app.quit();
    });
    poll.start();
    app.exec();
    QJsonArray policies;
    for (const auto &profile : power.applied().profiles) {
        policies.append(QJsonObject{{"profile", profile.id}, {"displaySeconds", profile.displaySeconds},
            {"displayEnabled", profile.displayEnabled}, {"displayEditable", profile.displayEditable},
            {"sleepSeconds", profile.suspendSeconds}, {"sleepAction", int(profile.suspendAction)},
            {"sleepEditable", profile.sleepEditable}, {"displayReason", profile.displayReason},
            {"sleepReason", profile.sleepReason}});
    }
    QJsonObject report{
        {"timedOut", timedOut},
        {"appearance", QJsonObject{{"ready", appearance.isReady()}, {"canApply", appearance.canApply()},
            {"currentAction", appearance.appliedAction()}, {"reason", appearance.unavailableReason()},
            {"error", appearance.errorText()}}},
        {"power", QJsonObject{{"ready", power.isReady()}, {"serviceAvailable", power.applied().serviceAvailable},
            {"reason", power.applied().unavailableReason}, {"error", power.errorText()}, {"profiles", policies}}},
        {"display", QJsonObject{{"ready", display.isReady()}, {"outputCount", display.snapshot().outputs.size()},
            {"rollbackAvailable", display.rollbackAvailable()}, {"reason", display.supportText()},
            {"error", display.errorText()}}}};
    QJsonArray desktops;
    for (const auto &desktop : wallpaper.state().desktops)
        desktops.append(QJsonObject{{"id", double(desktop.id)}, {"screen", desktop.screen}, {"plugin", desktop.plugin},
            {"fillMode", desktop.fillMode}, {"writable", desktop.writable}, {"reason", desktop.reason}});
    report.insert("wallpaper", QJsonObject{{"ready", wallpaper.isReady()}, {"desktops", desktops}, {"error", wallpaper.errorText()}});
    QJsonArray fields;
    for (int i = 0; i < 3; ++i) {
        const auto &field = components.field(static_cast<ThemeComponent>(i));
        fields.append(QJsonObject{{"component", i}, {"saved", field.value}, {"choices", field.choices.size()},
            {"canApply", field.canApply()}, {"reason", field.unavailableReason}});
    }
    report.insert("components", QJsonObject{{"ready", components.isReady()}, {"fields", fields}, {"error", components.errorText()}});
    report.insert("fonts", QJsonObject{{"ready", fonts.isReady()}, {"canApply", fonts.canApply()},
        {"generalFont", fonts.appliedPreferences().fonts[0].toString()}, {"antialias", fonts.appliedPreferences().antialias},
        {"hinting", fonts.appliedPreferences().hinting}, {"subpixel", fonts.appliedPreferences().subpixel},
        {"reason", fonts.unavailableReason()}, {"error", fonts.errorText()}});
    report.insert("aero", QJsonObject{{"ready", aero.isReady()}, {"canApply", aero.canApply()},
        {"blurStrength", aero.appliedStrength()}, {"pluginSupported", aero.state().pluginSupported},
        {"runtimeSupported", aero.state().runtimeSupported}, {"effectLoaded", aero.state().effectLoaded},
        {"reason", aero.state().unavailableReason}, {"error", aero.errorText()}});
    report.insert("account", QJsonObject{{"ready", account.isReady()}, {"canApply", account.canApply()},
        {"uid", double(account.account().uid)}, {"userName", account.account().userName},
        {"reason", account.account().unavailableReason}, {"error", account.errorText()}});
    QJsonObject identities, tuning;
    const auto pinned = AeroThemeContract::identities();
    const auto saved = aeroTheme.appliedValues();
    bool identitiesMatch = true;
    for (auto it = saved.cbegin(); it != saved.cend(); ++it) {
        if (pinned.contains(it.key())) {
            const bool matches = pinned.value(it.key()) == it.value();
            identitiesMatch = matches && identitiesMatch;
            identities.insert(it.key(), QJsonObject{{"matches", matches},
                {"saved", QJsonValue::fromVariant(it.value())}});
        } else tuning.insert(it.key(), QJsonValue::fromVariant(it.value()));
    }
    report.insert("aeroTheme", QJsonObject{{"contractVersion", AeroThemeContract::version()},
        {"ready", aeroTheme.isReady()}, {"canApply", aeroTheme.canApply()},
        {"assetsSupported", aeroTheme.state().assetsSupported},
        {"runtimeSupported", aeroTheme.state().runtimeSupported},
        {"identitiesMatch", identitiesMatch}, {"identities", identities}, {"tuning", tuning},
        {"issues", QJsonArray::fromStringList(aeroTheme.state().issues)}, {"error", aeroTheme.errorText()}});
    report.insert("packageSimulation", QJsonObject{{"providerAvailable", simulation->providerAvailable()},
        {"reason", simulation->unavailableReason()}, {"started", simulatePackages},
        {"success", simulation->result().success}, {"status", simulation->statusText()},
        {"error", simulation->result().error}});
    QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented);
    return timedOut || !appearance.isReady() || !power.isReady() || !display.isReady()
        || !wallpaper.isReady() || !components.isReady() || !fonts.isReady()
        || !aero.isReady() || !aeroTheme.isReady() || !account.isReady() || (simulatePackages && !simulation->result().success) ? 1 : 0;
}
