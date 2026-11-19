# SPDX-FileCopyrightText: 2026 glassosd contributors
# SPDX-License-Identifier: GPL-2.0-or-later

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)

# Qt6::DBus for the headers alone: notification.h carries the freedesktop wire
# types, so anything that knows what a Notification is needs QDBusArgument to
# be declared. Nothing here opens a bus connection.

# Each test compiles the sources it exercises rather than linking a shared
# library of them. The application target is a QML module, and moving its
# sources out from under qt_add_qml_module to share them here would risk the
# type registration for no gain — four files compiled twice costs nothing.
function(glassosd_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})

    add_executable(${name} ${name}.cpp)
    foreach(src IN LISTS T_SOURCES)
        target_sources(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src/${src})
    endforeach()

    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE Qt6::Test Qt6::Gui Qt6::DBus ${T_LIBS})

    add_test(NAME ${name} COMMAND ${name})
    # A test that reaches for a display it cannot have fails for the wrong
    # reason. Nothing here opens a window; offscreen keeps it that way even
    # when the suite is run from inside a session.
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60)
endfunction()

glassosd_test(tst_rules
    SOURCES rules.cpp
    LIBS KF6::ConfigCore)

glassosd_test(tst_soundplayer
    SOURCES soundplayer.cpp)

glassosd_test(tst_historymodel
    SOURCES historymodel.cpp notification.cpp
    LIBS Qt6::Qml KF6::ConfigCore)

glassosd_test(tst_notificationmodel
    SOURCES notificationmodel.cpp soundplayer.cpp
    LIBS Qt6::Qml Qt6::Quick KF6::WindowSystem KF6::IdleTime)

glassosd_test(tst_portalserver
    SOURCES portalserver.cpp portalbackend.cpp notificationserver.cpp notificationmodel.cpp
            historymodel.cpp rules.cpp soundplayer.cpp notification.cpp
    LIBS Qt6::Qml Qt6::Quick KF6::WindowSystem KF6::IdleTime KF6::ConfigCore)

glassosd_test(tst_portalrelay
    SOURCES portalrelay.cpp portalbackend.cpp notificationserver.cpp notificationmodel.cpp
            historymodel.cpp rules.cpp soundplayer.cpp notification.cpp
    LIBS Qt6::Qml Qt6::Quick KF6::WindowSystem KF6::IdleTime KF6::ConfigCore)

glassosd_test(tst_notificationserver
    SOURCES notificationserver.cpp notificationmodel.cpp historymodel.cpp rules.cpp
            soundplayer.cpp notification.cpp
    LIBS Qt6::Qml Qt6::Quick KF6::WindowSystem KF6::IdleTime KF6::ConfigCore)

glassosd_test(tst_snoozestore
    SOURCES snoozestore.cpp)

glassosd_test(tst_theme
    SOURCES theme.cpp
    LIBS Qt6::Qml KF6::ConfigCore)

glassosd_test(tst_lockwatcher
    SOURCES lockwatcher.cpp)

glassosd_test(tst_mpriscontroller
    SOURCES mpriscontroller.cpp
    LIBS Qt6::Qml)

# The CLI's list of known keys, checked against the man page.
add_test(NAME glassosdctl_set_keys
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/glassosdctl-set-keys.sh
                 ${CMAKE_SOURCE_DIR}/tools/glassosdctl ${CMAKE_SOURCE_DIR}/man/glassosdrc.5)

# The relay's round-trip tests need a session bus of their own: on a desktop
# session's bus they would be trying to take org.freedesktop.Notifications
# from whatever daemon is showing the user's notifications. dbus-run-session
# provides a throwaway one, and the variable tells the test it is inside it.
# Without the tool, tst_portalrelay still runs the translation tests and
# skips the rest rather than failing a build machine that lacks dbus.
find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    add_test(NAME tst_portalrelay_bus
             COMMAND ${DBUS_RUN_SESSION} --config-file=${CMAKE_CURRENT_SOURCE_DIR}/dbus-test-session.conf
                     -- ${CMAKE_COMMAND} -E env GLASSOSD_PRIVATE_BUS=1
                     $<TARGET_FILE:tst_portalrelay>)
    set_tests_properties(tst_portalrelay_bus PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60)

    add_test(NAME tst_lockwatcher_bus
             COMMAND ${DBUS_RUN_SESSION} --config-file=${CMAKE_CURRENT_SOURCE_DIR}/dbus-test-session.conf
                     -- ${CMAKE_COMMAND} -E env GLASSOSD_PRIVATE_BUS=1
                     $<TARGET_FILE:tst_lockwatcher>)
    set_tests_properties(tst_lockwatcher_bus PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60)

    # Fake media players take names on the bus, so the same throwaway one.
    add_test(NAME tst_mpriscontroller_bus
             COMMAND ${DBUS_RUN_SESSION} --config-file=${CMAKE_CURRENT_SOURCE_DIR}/dbus-test-session.conf
                     -- ${CMAKE_COMMAND} -E env GLASSOSD_PRIVATE_BUS=1
                     $<TARGET_FILE:tst_mpriscontroller>)
    set_tests_properties(tst_mpriscontroller_bus PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60)
endif()

# ---- QML ------------------------------------------------------------------
# The notification stack and card are tested offscreen against the real
# NotificationModel, with the C++ singletons that need a compositor (Surface,
# Appearance, Theme, ...) replaced by QML stand-ins from tests/qml/stubs. The
# real QML files are copied beside the stubs into a throwaway module directory
# in the build tree, so the test loads exactly what ships, from source.
set(GLASSOSD_QML_TEST_MODULE ${CMAKE_CURRENT_BINARY_DIR}/qmlimports/org/glassosd/ui)
set(GLASSOSD_QML_UNDER_TEST
    NotificationStack.qml NotificationCard.qml GlassPanel.qml CardButton.qml
    CardStackSurface.qml CardMenu.qml CardReply.qml CardActions.qml
    Style.qml Icons.qml)
set(_qml_copy_cmds)
foreach(f IN LISTS GLASSOSD_QML_UNDER_TEST)
    list(APPEND _qml_copy_cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different
         ${CMAKE_SOURCE_DIR}/qml/${f} ${GLASSOSD_QML_TEST_MODULE}/${f})
endforeach()
file(GLOB _qml_stubs ${CMAKE_CURRENT_SOURCE_DIR}/qml/stubs/*)
foreach(f IN LISTS _qml_stubs)
    get_filename_component(n ${f} NAME)
    list(APPEND _qml_copy_cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different
         ${f} ${GLASSOSD_QML_TEST_MODULE}/${n})
endforeach()
add_custom_target(glassosd_qml_test_module ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GLASSOSD_QML_TEST_MODULE}
    ${_qml_copy_cmds}
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_SOURCE_DIR}/qml/icons ${GLASSOSD_QML_TEST_MODULE}/icons
    COMMENT "Assembling the QML test module"
    VERBATIM)

glassosd_test(tst_notificationstack
    SOURCES notificationmodel.cpp soundplayer.cpp
    LIBS Qt6::Qml Qt6::Quick KF6::WindowSystem KF6::IdleTime)
add_dependencies(tst_notificationstack glassosd_qml_test_module)
target_compile_definitions(tst_notificationstack PRIVATE
    GLASSOSD_QML_TEST_IMPORTS="${CMAKE_CURRENT_BINARY_DIR}/qmlimports")
