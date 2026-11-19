// SPDX-FileCopyrightText: 2026 glassosd contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Test stand-in for the C++ SystemControls singleton: no wpctl, no
// brightnessctl. The centre only asks it to start and stop polling.
pragma Singleton
import QtQuick
QtObject {
    property bool volumeAvailable: false
    property bool brightnessAvailable: false
    property bool polling: false
    function setPolling(on) { polling = on }
}
