cmake_minimum_required(VERSION 3.16)
project(ControlPanel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia Concurrent DBus OpenGL UiPlugin Network)
find_package(KF6WidgetsAddons REQUIRED)
find_package(KF6WindowSystem REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(KF6FrameworkIntegration REQUIRED)
find_package(KF6Config REQUIRED)
find_package(KF6Service REQUIRED)
find_package(KF6GlobalAccel REQUIRED)
find_package(KF6XmlGui REQUIRED)
find_package(KF6IconThemes REQUIRED)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6KIO REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Xml Positioning)
find_package(KNightTime 6.6.5 REQUIRED)
find_package(KF6Holidays 6.24 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Fontconfig REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(XkbCommon REQUIRED IMPORTED_TARGET xkbcommon)
find_package(AeroQt REQUIRED)
find_package(Cups REQUIRED)

include(GNUInstallDirs)
set(CONTROL_AERO_LOGIN_HELPER_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/controlpanel-aero-login-helper")
set(CONTROL_AERO_LOGIN_POLICY_PATH "${CMAKE_INSTALL_FULL_DATADIR}/polkit-1/actions/org.linuxcontrol.aerologin.policy")
set(CONTROL_AERO_LOGIN_MANIFEST_PATH "${CMAKE_INSTALL_FULL_DATADIR}/linux-control/aero-login-helper.json")
configure_file(src/login/org.linuxcontrol.aerologin.policy.in org.linuxcontrol.aerologin.policy @ONLY)
configure_file(cmake/InstallAeroLoginHelperIdentity.cmake.in InstallAeroLoginHelperIdentity.cmake @ONLY)

add_library(control_aero_login STATIC src/settings/AeroLoginTypes.cpp
    src/login/AeroLoginFiles.cpp src/login/AeroLoginProtocol.cpp)
target_include_directories(control_aero_login PUBLIC src)
target_link_libraries(control_aero_login PUBLIC Qt6::Core Qt6::Gui)
add_executable(controlpanel-aero-login-helper src/login/AeroLoginHelper.cpp)
target_link_libraries(controlpanel-aero-login-helper PRIVATE control_aero_login)
set_property(TARGET controlpanel-aero-login-helper APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/org.linuxcontrol.aerologin.policy"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateAeroLoginHelperIdentity.cmake")
add_custom_command(TARGET controlpanel-aero-login-helper POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DHELPER=$<TARGET_FILE:controlpanel-aero-login-helper>"
        "-DPOLICY=${CMAKE_CURRENT_BINARY_DIR}/org.linuxcontrol.aerologin.policy"
        "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/aero-login-helper.json"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateAeroLoginHelperIdentity.cmake"
    VERBATIM)
set(CONTROL_TIME_HELPER_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/controlpanel-time-helper")
configure_file(src/time/org.linuxcontrol.time.policy.in
    org.linuxcontrol.time.policy @ONLY)

add_library(control_time STATIC src/time/TimeSync.cpp src/time/TimeSyncConfig.cpp)
target_include_directories(control_time PUBLIC src)
target_link_libraries(control_time PUBLIC Qt6::Core Qt6::Network)

add_library(control_operations STATIC
    src/operations/SettingsFileReservations.h src/operations/SettingsFileReservations.cpp
    src/operations/OperationManager.cpp src/operations/UpdateService.cpp
    src/packages/ProgramsService.cpp
    src/packages/PackageSimulationService.h src/packages/PackageSimulationService.cpp)
target_include_directories(control_operations PUBLIC src)
target_link_libraries(control_operations PUBLIC Qt6::Core Qt6::Concurrent)

# KNightTime's public header requires C++20. Keep that requirement inside
# the native forecast adapter; the app and its public POD interface use C++17.
add_library(control_nighttime_forecast STATIC src/settings/NightTimeForecast.cpp)
set_target_properties(control_nighttime_forecast PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(control_nighttime_forecast PUBLIC src)
target_link_libraries(control_nighttime_forecast PUBLIC Qt6::Core PRIVATE KNightTime KF6::Holidays)

add_library(control_settings STATIC
    src/settings/AeroLoginSettings.h src/settings/AeroLoginSettings.cpp
    src/platform/kde/AeroLoginBackend.h src/platform/kde/AeroLoginBackend.cpp
    src/settings/SettingsSession.h src/settings/ColorSchemeSettings.h src/settings/ColorSchemeSettings.cpp
    src/settings/AeroThemeContract.h src/settings/AeroThemeContract.cpp
    src/settings/AeroThemeSettings.h src/settings/AeroThemeSettings.cpp
    src/platform/kde/AeroThemeBackend.h src/platform/kde/AeroThemeBackend.cpp
    src/settings/NetworkTypes.h src/settings/NetworkTypes.cpp
    src/settings/NetworkSettings.h src/settings/NetworkSettings.cpp
    src/platform/network/NetworkManagerBackend.h src/platform/network/NetworkManagerBackend.cpp
    src/settings/AudioSettings.h src/settings/AudioSettings.cpp
    src/platform/audio/AudioBackend.h src/platform/audio/AudioBackend.cpp
    src/settings/ShortcutsTypes.h src/settings/ShortcutsTypes.cpp
    src/settings/ShortcutsSettings.h src/settings/ShortcutsSettings.cpp
    src/settings/ShortcutScheme.h src/settings/ShortcutScheme.cpp
    src/settings/ShortcutImportCoordinator.h src/settings/ShortcutImportCoordinator.cpp
    src/settings/CustomShortcutsSettings.h src/settings/CustomShortcutsSettings.cpp
    src/platform/shortcuts/ShortcutsBackend.h src/platform/shortcuts/ShortcutsBackend.cpp
    src/platform/shortcuts/StandardShortcutsBackend.h src/platform/shortcuts/StandardShortcutsBackend.cpp
    src/platform/shortcuts/CustomShortcutsBackend.h src/platform/shortcuts/CustomShortcutsBackend.cpp
    src/settings/KeyboardSettings.h src/settings/KeyboardSettings.cpp
    src/platform/keyboard/KeyboardBackend.h src/platform/keyboard/KeyboardBackend.cpp
    src/settings/DefaultAppsSettings.h src/settings/DefaultAppsSettings.cpp
    src/platform/apps/DefaultAppsBackend.h src/platform/apps/DefaultAppsBackend.cpp
    src/settings/InputSettings.h src/settings/InputSettings.cpp
    src/platform/input/InputBackend.h src/platform/input/InputBackend.cpp
    src/settings/PowerProfileService.cpp src/ui/SettingsNavigationGuard.cpp
    src/settings/DisplaySettings.h src/settings/DisplaySettings.cpp
    src/display/DisplayProtocol.cpp src/display/KdeDisplayBackend.cpp
    src/settings/ScreenLockSettings.h src/settings/ScreenLockSettings.cpp
    src/platform/kde/ScreenLockBackend.h src/platform/kde/ScreenLockBackend.cpp
    src/platform/kde/ScreenLockAppearance.h src/platform/kde/ScreenLockAppearance.cpp
    src/settings/NightTimeSettings.h src/settings/NightTimeSettings.cpp
    src/platform/kde/NightTimeBackend.h src/platform/kde/NightTimeBackend.cpp
    src/platform/location/NightTimeLocation.h src/platform/location/NightTimeLocation.cpp
    src/settings/NightLightSettings.h src/settings/NightLightSettings.cpp
    src/platform/kde/NightLightBackend.h src/platform/kde/NightLightBackend.cpp
    src/settings/AppearanceSettings.h src/settings/AppearanceSettings.cpp
    src/platform/kde/AppearanceBackend.h src/platform/kde/AppearanceBackend.cpp
    src/settings/PowerPolicySettings.h src/settings/PowerPolicySettings.cpp
    src/settings/PowerPolicyFields.h src/settings/PowerPolicyFields.cpp
    src/platform/kde/PowerPolicyBackend.h src/platform/kde/PowerPolicyBackend.cpp
    src/settings/WallpaperSettings.h src/settings/WallpaperSettings.cpp
    src/platform/kde/WallpaperBackend.h src/platform/kde/WallpaperBackend.cpp
    src/settings/ThemeComponentSettings.h src/settings/ThemeComponentSettings.cpp
    src/platform/kde/ThemeComponentBackend.h src/platform/kde/ThemeComponentBackend.cpp
    src/settings/FontSettings.h src/settings/FontSettings.cpp
    src/platform/kde/FontBackend.h src/platform/kde/FontBackend.cpp
    src/settings/UserFontService.h src/settings/UserFontService.cpp
    src/settings/AccountSettings.h src/settings/AccountSettings.cpp
    src/platform/accounts/AccountsBackend.h src/platform/accounts/AccountsBackend.cpp
    src/settings/AeroEffectSettings.h src/settings/AeroEffectSettings.cpp
    src/platform/kde/AeroEffectBackend.h src/platform/kde/AeroEffectBackend.cpp)
target_include_directories(control_settings PUBLIC src)
target_link_libraries(control_settings PUBLIC Qt6::Core Qt6::DBus Qt6::Widgets Qt6::Xml Qt6::Concurrent Qt6::Network
    KF6::ConfigCore KF6::ConfigGui KF6::Service KF6::IconThemes KF6::CoreAddons
    PkgConfig::Fontconfig control_operations)
target_link_libraries(control_settings PRIVATE KF6::GlobalAccel Qt6::Positioning)
target_link_libraries(control_settings PUBLIC control_nighttime_forecast control_aero_login)
target_compile_definitions(control_settings PRIVATE
    CONTROL_AERO_LOGIN_HELPER_PATH="${CONTROL_AERO_LOGIN_HELPER_PATH}"
    CONTROL_AERO_LOGIN_POLICY_PATH="${CONTROL_AERO_LOGIN_POLICY_PATH}"
    CONTROL_AERO_LOGIN_MANIFEST_PATH="${CONTROL_AERO_LOGIN_MANIFEST_PATH}"
    CONTROL_DISPLAY_GUARD_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/controlpanel-display-guard")

add_executable(controlpanel-display-guard src/display/DisplayGuard.cpp src/display/DisplayProtocol.cpp)
target_include_directories(controlpanel-display-guard PRIVATE src)
target_link_libraries(controlpanel-display-guard PRIVATE Qt6::Core Qt6::DBus)

add_executable(controlpanel-time-helper src/time/TimeSyncHelper.cpp)
target_link_libraries(controlpanel-time-helper PRIVATE control_time)

add_library(control_catalog STATIC
    src/ui/ActionCatalog.cpp
    src/data/Categories.cpp
    src/data/SystemAndSecurity.cpp
    src/data/NetworkAndInternet.cpp
    src/data/HardwareAndSound.cpp
    src/data/Programs.cpp
    src/data/UserAccounts.cpp
    src/data/AppearanceAndPersonalization.cpp
    src/data/ClockLanguageRegion.cpp
    src/data/EaseOfAccess.cpp
)
target_include_directories(control_catalog PUBLIC src/ui src/data)
target_link_libraries(control_catalog PUBLIC Qt6::Core)

add_library(control_ui STATIC
    src/ui/LinkLabel.h
    src/ui/MainWindow.cpp
    src/ui/PageRegistry.cpp
    src/ui/CatalogView.cpp
    src/ui/CategoryWidget.cpp
    src/ui/pages/LinuxUpdatePage.cpp
    src/ui/pages/SystemPage.cpp
    src/ui/pages/InstalledUpdatesPage.cpp
    src/ui/pages/ProgramsFeaturesPage.cpp
    src/ui/pages/NetworkSharingPage.cpp
    src/ui/pages/NetworkConnectionsPage.cpp
    src/ui/pages/InputDevicesPage.cpp
    src/ui/pages/KeyboardPage.cpp
    src/ui/pages/ShortcutsPage.cpp
    src/ui/pages/CustomShortcutsPage.cpp
    src/ui/dialogs/ShortcutSchemeDialog.h src/ui/dialogs/ShortcutSchemeDialog.cpp
    src/keyboard/KeyboardPreview.h src/keyboard/KeyboardPreview.cpp
    src/ui/pages/DefaultAppsPage.cpp
    src/ui/pages/FirewallPage.cpp
    src/ui/pages/ActionCenterPage.cpp
    src/ui/pages/PowerOptionsPage.cpp
    src/ui/pages/DisplayPage.cpp
    src/ui/pages/NightLightPage.cpp
    src/ui/pages/ScreenLockPage.cpp
    src/ui/pages/AeroLoginPage.cpp
    src/ui/pages/NightTimePage.cpp
    src/ui/pages/NightTimeMapWidget.cpp
    src/ui/pages/NightTimePreviewWidget.cpp
    src/ui/pages/WindowBehaviorPage.cpp
    src/ui/pages/DesktopBackgroundPage.cpp
    src/ui/pages/ThemeComponentsPage.cpp
    src/ui/pages/FontSettingsPage.cpp
    src/ui/pages/AeroEffectsPage.cpp
    src/ui/pages/PackagePreviewPage.cpp
    src/ui/pages/PerformancePage.cpp
    src/ui/pages/PersonalizationPage.cpp
    src/ui/pages/FontsPage.cpp
    src/ui/pages/UserAccountsPage.cpp
    src/ui/pages/EaseOfAccessPage.cpp
    src/ui/pages/DevicesAndPrintersPage.cpp
    src/ui/dialogs/DateTimeDialog.cpp
    src/ui/dialogs/SoundDialog.cpp
    src/ui/dialogs/DevicePropertiesDialog.cpp
    src/ui/dialogs/LinverConfigDialog.cpp
    src/devices/SysfsScanner.cpp
    src/devices/PrinterScanner.cpp
    src/data/WindowsTimeZones.cpp
    src/perf/WeiBenchmark.cpp
    src/data/Branding.cpp
)

target_include_directories(control_ui PUBLIC
    src/ui
    src/ui/pages
    src/ui/dialogs
    src/devices
    src/data
    src
)

target_link_libraries(control_ui PUBLIC
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Concurrent
    Qt6::DBus
    Qt6::OpenGL
    KF6::WidgetsAddons
    KF6::WindowSystem
    ZLIB::ZLIB
    OpenSSL::Crypto
    AeroQt::AeroQt
    PkgConfig::XkbCommon
    Cups::Cups
    control_time
    control_operations
    control_settings
    control_catalog
)
add_library(control_form_style STATIC src/ui/Win7FormStyle.cpp)
target_link_libraries(control_form_style PRIVATE Qt6::Core)
qt_add_resources(control_form_style win7_fields
    PREFIX "/ControlPanel/fields"
    BASE "src/ui/assets"
    FILES src/ui/assets/check.svg src/ui/assets/spin-up.svg src/ui/assets/spin-down.svg)
target_link_libraries(control_ui PRIVATE KF6::XmlGui control_form_style)
target_compile_definitions(control_ui PRIVATE
    CONTROL_TIME_HELPER_PATH="${CONTROL_TIME_HELPER_PATH}")

qt_add_resources(control_ui nighttime_map
    PREFIX "/ControlPanel/nighttime"
    BASE "src/ui/assets"
    FILES src/ui/assets/worldmap.png src/ui/assets/worldmap-attribution.txt)

add_executable(controlpanel src/main.cpp)
target_link_libraries(controlpanel PRIVATE control_ui)
add_dependencies(controlpanel controlpanel-display-guard)

set_target_properties(controlpanel PROPERTIES
    OUTPUT_NAME control
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib"
)

install(TARGETS controlpanel RUNTIME DESTINATION bin)
install(FILES linux-controlpanel.desktop DESTINATION share/applications)
install(TARGETS controlpanel-time-helper RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")
install(TARGETS controlpanel-display-guard RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")
install(TARGETS controlpanel-aero-login-helper RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.linuxcontrol.aerologin.policy"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/polkit-1/actions")
# Hash final installed bytes after install-time RPATH rewriting and optional
# stripping. The build-tree manifest is useful evidence but is not installed.
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallAeroLoginHelperIdentity.cmake")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.linuxcontrol.time.policy"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/polkit-1/actions")

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test Qml)

    foreach(batch22_suite IN ITEMS aero_login_settings aero_login_protocol aero_login_files aero_login_backend aero_login_auth aero_login_page aero_login_helper)
        add_executable(${batch22_suite}_test tests/${batch22_suite}_test.cpp)
        target_link_libraries(${batch22_suite}_test PRIVATE control_ui Qt6::Test)
        add_test(NAME ${batch22_suite} COMMAND ${batch22_suite}_test)
        set_tests_properties(${batch22_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    endforeach()
    target_compile_definitions(aero_login_helper_test PRIVATE
        CONTROL_AERO_LOGIN_HELPER_TEST_PATH="$<TARGET_FILE:controlpanel-aero-login-helper>")
    add_dependencies(aero_login_helper_test controlpanel-aero-login-helper)
    add_executable(aero_login_probe tests/aero_login_probe.cpp)
    target_link_libraries(aero_login_probe PRIVATE control_settings)

    foreach(batch21_suite IN ITEMS screen_lock_settings screen_lock_backend screen_lock_dbus screen_lock_async screen_lock_page screen_lock_appearance)
        add_executable(${batch21_suite}_test tests/${batch21_suite}_test.cpp)
        target_link_libraries(${batch21_suite}_test PRIVATE control_ui Qt6::Test)
        add_test(NAME ${batch21_suite} COMMAND ${batch21_suite}_test)
        set_tests_properties(${batch21_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    endforeach()
    add_executable(screen_lock_probe tests/screen_lock_probe.cpp)
    target_link_libraries(screen_lock_probe PRIVATE control_settings)

    foreach(batch20_suite IN ITEMS night_time_settings night_time_backend night_time_async night_time_dbus night_time_page night_time_forecast night_time_location)
        add_executable(${batch20_suite}_test tests/${batch20_suite}_test.cpp)
        target_link_libraries(${batch20_suite}_test PRIVATE control_ui Qt6::Test)
        add_test(NAME ${batch20_suite} COMMAND ${batch20_suite}_test)
        set_tests_properties(${batch20_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    endforeach()
    add_executable(night_time_probe tests/night_time_probe.cpp)
    target_link_libraries(night_time_probe PRIVATE control_settings)

    foreach(batch19_suite IN ITEMS night_light_settings night_light_backend night_light_async night_light_dbus night_light_page)
        add_executable(${batch19_suite}_test tests/${batch19_suite}_test.cpp)
        target_link_libraries(${batch19_suite}_test PRIVATE control_ui Qt6::Test)
        add_test(NAME ${batch19_suite} COMMAND ${batch19_suite}_test)
        set_tests_properties(${batch19_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    endforeach()
    add_executable(night_light_probe tests/night_light_probe.cpp)
    target_link_libraries(night_light_probe PRIVATE control_settings)

    add_executable(settings_file_reservations_test tests/settings_file_reservations_test.cpp)
    target_link_libraries(settings_file_reservations_test PRIVATE control_operations Qt6::Test)
    add_test(NAME settings_file_reservations COMMAND settings_file_reservations_test)
    set_tests_properties(settings_file_reservations PROPERTIES TIMEOUT 90)

    add_executable(settings_file_jobs_integration_test tests/settings_file_jobs_integration_test.cpp)
    target_link_libraries(settings_file_jobs_integration_test PRIVATE control_settings Qt6::Test)
    add_test(NAME settings_file_jobs_integration COMMAND settings_file_jobs_integration_test)
    set_tests_properties(settings_file_jobs_integration PROPERTIES TIMEOUT 90)

    foreach(batch9_suite IN ITEMS audio_settings audio_backend audio_dialog)
        add_executable(${batch9_suite}_test tests/${batch9_suite}_test.cpp)
        target_link_libraries(${batch9_suite}_test PRIVATE control_ui Qt6::Test)
        add_test(NAME ${batch9_suite} COMMAND ${batch9_suite}_test)
        set_tests_properties(${batch9_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    endforeach()
    add_executable(audio_backend_probe tests/audio_backend_probe.cpp)
    target_link_libraries(audio_backend_probe PRIVATE control_settings)

    foreach(batch13_suite IN ITEMS shortcuts_settings shortcuts_backend shortcuts_page keyboard_preview)
        add_executable(${batch13_suite}_test tests/${batch13_suite}_test.cpp)
        target_link_libraries(${batch13_suite}_test PRIVATE control_ui Qt6::Test KF6::GlobalAccel KF6::XmlGui)
        add_test(NAME ${batch13_suite} COMMAND ${batch13_suite}_test)
        set_tests_properties(${batch13_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    endforeach()
    add_executable(shortcuts_backend_probe tests/shortcuts_backend_probe.cpp)
    target_link_libraries(shortcuts_backend_probe PRIVATE control_settings)

    add_executable(standard_shortcuts_backend_test tests/standard_shortcuts_backend_test.cpp)
    target_link_libraries(standard_shortcuts_backend_test PRIVATE control_ui Qt6::Test)
    add_test(NAME standard_shortcuts_backend COMMAND standard_shortcuts_backend_test)
    set_tests_properties(standard_shortcuts_backend PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    add_executable(standard_shortcuts_backend_probe tests/standard_shortcuts_backend_probe.cpp)
    target_link_libraries(standard_shortcuts_backend_probe PRIVATE control_settings)

    foreach(batch15_suite IN ITEMS shortcut_scheme shortcut_scheme_dialog custom_shortcuts_backend custom_shortcuts_settings custom_shortcuts_page)
        add_executable(${batch15_suite}_test tests/${batch15_suite}_test.cpp)
        target_link_libraries(${batch15_suite}_test PRIVATE control_ui Qt6::Test KF6::GlobalAccel)
        add_test(NAME ${batch15_suite} COMMAND ${batch15_suite}_test)
        set_tests_properties(${batch15_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    endforeach()
    add_executable(custom_shortcuts_backend_probe tests/custom_shortcuts_backend_probe.cpp)
    target_link_libraries(custom_shortcuts_backend_probe PRIVATE control_settings)
    add_executable(shortcut_import_coordinator_test tests/shortcut_import_coordinator_test.cpp)
    target_link_libraries(shortcut_import_coordinator_test PRIVATE control_settings Qt6::Test)
    add_test(NAME shortcut_import_coordinator COMMAND shortcut_import_coordinator_test)
    set_tests_properties(shortcut_import_coordinator PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)

    foreach(native_suite IN ITEMS keyboard_settings keyboard_backend keyboard_page
                                  default_apps_settings default_apps_backend default_apps_page)
        add_executable(${native_suite}_test tests/${native_suite}_test.cpp)
        target_link_libraries(${native_suite}_test PRIVATE control_ui Qt6::Test)
        add_test(NAME ${native_suite} COMMAND ${native_suite}_test)
        set_tests_properties(${native_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    endforeach()
    foreach(native_probe IN ITEMS keyboard_backend default_apps)
        add_executable(${native_probe}_probe tests/${native_probe}_probe.cpp)
        target_link_libraries(${native_probe}_probe PRIVATE control_settings)
    endforeach()

    foreach(batch10_suite IN ITEMS input_settings input_backend input_page)
        add_executable(${batch10_suite}_test tests/${batch10_suite}_test.cpp)
        target_link_libraries(${batch10_suite}_test PRIVATE control_ui Qt6::Test)
        add_test(NAME ${batch10_suite} COMMAND ${batch10_suite}_test)
        set_tests_properties(${batch10_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    endforeach()
    add_executable(input_backend_probe tests/input_backend_probe.cpp)
    target_link_libraries(input_backend_probe PRIVATE control_settings)

    foreach(batch8_suite IN ITEMS network_types network_settings network_backend network_pages)
        add_executable(${batch8_suite}_test tests/${batch8_suite}_test.cpp)
        target_link_libraries(${batch8_suite}_test PRIVATE control_ui Qt6::Test)
        add_test(NAME ${batch8_suite} COMMAND ${batch8_suite}_test)
        set_tests_properties(${batch8_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    endforeach()
    add_executable(network_backend_probe tests/network_backend_probe.cpp)
    target_link_libraries(network_backend_probe PRIVATE control_settings)

    foreach(batch7_suite IN ITEMS aero_theme_settings aero_theme_backend aero_theme_contract)
        add_executable(${batch7_suite}_test tests/${batch7_suite}_test.cpp)
        target_link_libraries(${batch7_suite}_test PRIVATE control_ui Qt6::Test)
        add_test(NAME ${batch7_suite} COMMAND ${batch7_suite}_test)
        set_tests_properties(${batch7_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    endforeach()

    foreach(batch6_suite IN ITEMS wallpaper theme_component_settings theme_component_backend
                                 font_settings font_backend font_browser user_font_service)
        add_executable(${batch6_suite}_test tests/${batch6_suite}_test.cpp)
        target_link_libraries(${batch6_suite}_test PRIVATE control_ui Qt6::Test)
        add_test(NAME ${batch6_suite} COMMAND ${batch6_suite}_test)
        set_tests_properties(${batch6_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    endforeach()
    target_link_libraries(wallpaper_test PRIVATE Qt6::Qml)

    foreach(batch5_suite IN ITEMS account_settings account_backend aero_effect_settings
                                 aero_effect_backend package_simulation style_guard)
        add_executable(${batch5_suite}_test tests/${batch5_suite}_test.cpp)
        target_link_libraries(${batch5_suite}_test PRIVATE control_ui Qt6::Test)
        add_test(NAME ${batch5_suite} COMMAND ${batch5_suite}_test)
        set_tests_properties(${batch5_suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    endforeach()

    add_executable(timesync_test tests/timesync_test.cpp)
    target_link_libraries(timesync_test PRIVATE control_time Qt6::Test)
    add_test(NAME timesync COMMAND timesync_test)

    add_executable(time_config_test tests/time_config_test.cpp)
    target_link_libraries(time_config_test PRIVATE control_time Qt6::Test)
    target_compile_definitions(time_config_test PRIVATE
        CONTROL_TIME_HELPER_TEST_PATH="$<TARGET_FILE:controlpanel-time-helper>")
    add_dependencies(time_config_test controlpanel-time-helper)
    add_test(NAME time_config COMMAND time_config_test)

    add_executable(status_test tests/status_test.cpp)
    target_include_directories(status_test PRIVATE src/data)
    target_link_libraries(status_test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME status COMMAND status_test)

    add_executable(ui_controls_test tests/ui_controls_test.cpp
        src/ui/CategoryWidget.cpp src/ui/LinkLabel.h src/data/Branding.cpp)
    target_include_directories(ui_controls_test PRIVATE src/ui src/data)
    target_link_libraries(ui_controls_test PRIVATE control_catalog Qt6::Widgets Qt6::Concurrent Qt6::Test)
    add_test(NAME ui_controls COMMAND ui_controls_test)
    set_tests_properties(ui_controls PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(date_time_dialog_test tests/date_time_dialog_test.cpp
        src/ui/dialogs/DateTimeDialog.cpp src/data/WindowsTimeZones.cpp)
    target_include_directories(date_time_dialog_test PRIVATE src/ui src/ui/dialogs src/data)
    target_compile_definitions(date_time_dialog_test PRIVATE
        CONTROL_TIME_HELPER_PATH="${CONTROL_TIME_HELPER_PATH}")
    target_link_libraries(date_time_dialog_test PRIVATE control_time
        Qt6::Widgets Qt6::Concurrent Qt6::Test)
    add_test(NAME date_time_dialog COMMAND date_time_dialog_test)
    set_tests_properties(date_time_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(package_status_test tests/package_status_test.cpp
        src/ui/pages/LinuxUpdatePage.cpp src/data/Branding.cpp)
    target_include_directories(package_status_test PRIVATE src/ui src/ui/pages src/data src)
    target_link_libraries(package_status_test PRIVATE control_operations Qt6::Widgets Qt6::Concurrent Qt6::Test)
    add_test(NAME package_status COMMAND package_status_test)
    set_tests_properties(package_status PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(navigation_guard_test tests/navigation_guard_test.cpp)
    target_link_libraries(navigation_guard_test PRIVATE control_settings Qt6::Test)
    add_test(NAME navigation_guard COMMAND navigation_guard_test)

    add_executable(programs_lifecycle_test tests/programs_lifecycle_test.cpp
        src/ui/pages/ProgramsFeaturesPage.cpp)
    target_include_directories(programs_lifecycle_test PRIVATE src/ui src/ui/pages src/data)
    target_link_libraries(programs_lifecycle_test PRIVATE control_operations
        Qt6::Widgets Qt6::Multimedia Qt6::Concurrent Qt6::Test)
    add_test(NAME programs_lifecycle COMMAND programs_lifecycle_test)
    add_executable(action_catalog_test tests/action_catalog_test.cpp)
    target_link_libraries(action_catalog_test PRIVATE control_catalog Qt6::Test)
    add_test(NAME action_catalog COMMAND action_catalog_test)

    add_executable(catalog_view_test tests/catalog_view_test.cpp)
    target_link_libraries(catalog_view_test PRIVATE control_ui Qt6::Test)
    add_test(NAME catalog_view COMMAND catalog_view_test)
    set_tests_properties(catalog_view PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(shell_navigation_test tests/shell_navigation_test.cpp)
    target_link_libraries(shell_navigation_test PRIVATE control_ui Qt6::Test)
    add_test(NAME shell_navigation COMMAND shell_navigation_test)
    set_tests_properties(shell_navigation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    set_tests_properties(programs_lifecycle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(operation_test tests/operation_test.cpp)
    target_link_libraries(operation_test PRIVATE control_operations Qt6::Test)
    add_test(NAME operations COMMAND operation_test)

    add_executable(update_lifecycle_test tests/update_lifecycle_test.cpp
        src/ui/pages/LinuxUpdatePage.cpp src/data/Branding.cpp)
    target_include_directories(update_lifecycle_test PRIVATE src/ui src/ui/pages src/data)
    target_link_libraries(update_lifecycle_test PRIVATE control_operations Qt6::Widgets Qt6::Test)
    add_test(NAME update_lifecycle COMMAND update_lifecycle_test)
    set_tests_properties(update_lifecycle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(power_lifecycle_test tests/power_lifecycle_test.cpp
        src/ui/pages/PowerOptionsPage.cpp)
    target_include_directories(power_lifecycle_test PRIVATE src/ui src/ui/pages src/data)
    target_link_libraries(power_lifecycle_test PRIVATE control_settings control_form_style Qt6::Widgets Qt6::Concurrent Qt6::Test)
    add_test(NAME power_lifecycle COMMAND power_lifecycle_test)
    set_tests_properties(power_lifecycle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(benchmark_lifecycle_test tests/benchmark_lifecycle_test.cpp
        src/perf/WeiBenchmark.cpp src/ui/pages/PerformancePage.cpp src/ui/LinkLabel.h src/data/Branding.cpp)
    target_include_directories(benchmark_lifecycle_test PRIVATE src src/ui src/ui/pages src/data)
    target_compile_definitions(benchmark_lifecycle_test PRIVATE WEI_BENCHMARK_TESTING)
    target_link_libraries(benchmark_lifecycle_test PRIVATE Qt6::Widgets Qt6::Concurrent
        Qt6::OpenGL Qt6::Test ZLIB::ZLIB OpenSSL::Crypto)
    add_test(NAME benchmark_lifecycle COMMAND benchmark_lifecycle_test)
    set_tests_properties(benchmark_lifecycle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(settings_session_test tests/settings_session_test.cpp)
    target_link_libraries(settings_session_test PRIVATE control_settings Qt6::Test)
    add_test(NAME settings_session COMMAND settings_session_test)

    add_executable(personalization_test tests/personalization_test.cpp)
    target_link_libraries(personalization_test PRIVATE control_ui Qt6::Test)
    add_test(NAME personalization COMMAND personalization_test)
    set_tests_properties(personalization PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(shared_load_test tests/shared_load_test.cpp)
    target_link_libraries(shared_load_test PRIVATE control_operations Qt6::Test)
    add_test(NAME shared_load COMMAND shared_load_test)

    add_executable(main_window_lifecycle_test tests/main_window_lifecycle_test.cpp)
    target_link_libraries(main_window_lifecycle_test PRIVATE control_ui Qt6::Test)
    add_test(NAME main_window_lifecycle COMMAND main_window_lifecycle_test)
    set_tests_properties(main_window_lifecycle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(kde_integration_test tests/kde_integration_test.cpp)
    target_link_libraries(kde_integration_test PRIVATE control_ui Qt6::Test)
    add_test(NAME kde_integration COMMAND kde_integration_test)
    set_tests_properties(kde_integration PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

    add_executable(kde_backend_probe tests/kde_backend_probe.cpp)
    target_link_libraries(kde_backend_probe PRIVATE control_settings)
    add_dependencies(kde_backend_probe controlpanel-display-guard)

    add_executable(display_power_probe tests/display_power_probe.cpp)
    target_link_libraries(display_power_probe PRIVATE control_settings)
    add_dependencies(display_power_probe controlpanel-display-guard)

    add_executable(display_protocol_test tests/display_protocol_test.cpp)
    target_link_libraries(display_protocol_test PRIVATE control_settings Qt6::Test)
    add_test(NAME display_protocol COMMAND display_protocol_test)

    add_executable(display_settings_test tests/display_settings_test.cpp)
    target_link_libraries(display_settings_test PRIVATE control_settings Qt6::Test)
    add_test(NAME display_settings COMMAND display_settings_test)

    add_executable(display_page_test tests/display_page_test.cpp)
    target_link_libraries(display_page_test PRIVATE control_ui Qt6::Test)
    add_test(NAME display_page COMMAND display_page_test)
    set_tests_properties(display_page PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(display_fake_doctor tests/display_fake_doctor.cpp)
    target_link_libraries(display_fake_doctor PRIVATE Qt6::Core)
    add_executable(controlpanel-display-guard-test src/display/DisplayGuard.cpp src/display/DisplayProtocol.cpp)
    target_include_directories(controlpanel-display-guard-test PRIVATE src)
    target_link_libraries(controlpanel-display-guard-test PRIVATE Qt6::Core Qt6::DBus)
    target_compile_definitions(controlpanel-display-guard-test PRIVATE
        CONTROL_DISPLAY_GUARD_TESTING
        CONTROL_DISPLAY_TEST_DOCTOR_PATH="$<TARGET_FILE:display_fake_doctor>")
    add_dependencies(controlpanel-display-guard-test display_fake_doctor)
    add_executable(display_guard_test tests/display_guard_test.cpp)
    target_link_libraries(display_guard_test PRIVATE control_settings Qt6::Test)
    target_compile_definitions(display_guard_test PRIVATE
        CONTROL_DISPLAY_GUARD_TEST_PATH="$<TARGET_FILE:controlpanel-display-guard-test>")
    add_dependencies(display_guard_test controlpanel-display-guard-test)
    add_test(NAME display_guard COMMAND display_guard_test)

    add_executable(appearance_settings_test tests/appearance_settings_test.cpp)
    target_link_libraries(appearance_settings_test PRIVATE control_ui Qt6::Test)
    add_test(NAME appearance_settings COMMAND appearance_settings_test)
    set_tests_properties(appearance_settings PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(appearance_backend_test tests/appearance_backend_test.cpp)
    target_link_libraries(appearance_backend_test PRIVATE control_settings Qt6::Test)
    add_test(NAME appearance_backend COMMAND appearance_backend_test)

    add_executable(power_policy_test tests/power_policy_test.cpp)
    target_link_libraries(power_policy_test PRIVATE control_ui Qt6::Test)
    add_test(NAME power_policy COMMAND power_policy_test)
    set_tests_properties(power_policy PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(power_policy_page_test tests/power_policy_page_test.cpp)
    target_link_libraries(power_policy_page_test PRIVATE control_ui Qt6::Test)
    add_test(NAME power_policy_page COMMAND power_policy_page_test)
    set_tests_properties(power_policy_page PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    add_executable(power_policy_dbus_test tests/power_policy_dbus_test.cpp)
    target_link_libraries(power_policy_dbus_test PRIVATE control_settings Qt6::Test)
    add_test(NAME power_policy_dbus COMMAND power_policy_dbus_test)

    add_executable(power_policy_backend_test tests/power_policy_backend_test.cpp)
    target_link_libraries(power_policy_backend_test PRIVATE control_settings Qt6::Test)
    add_test(NAME power_policy_backend COMMAND power_policy_backend_test)

    set_tests_properties(programs_lifecycle update_lifecycle operations
        power_lifecycle benchmark_lifecycle settings_session personalization navigation_guard
        shared_load main_window_lifecycle action_catalog catalog_view shell_navigation
        display_protocol display_settings display_page display_guard
        appearance_settings appearance_backend power_policy power_policy_backend power_policy_dbus
        PROPERTIES TIMEOUT 60)
endif()
