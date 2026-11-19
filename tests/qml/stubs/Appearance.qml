// SPDX-FileCopyrightText: 2026 glassosd contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Test stand-in for the C++ Appearance singleton: the properties the
// notification QML reads, at their defaults. Tests assign to them.
pragma Singleton
import QtQuick
QtObject {
    signal changed()
    property bool dark: true
    property color accent: "#3daee9"
    property string notifyOutput: ""
    property real scale: 1
    property int notifyWidth: 360
    property real notifyScale: 1
    property real solidity: 0.6
    property int notifyAnchors: 9          // Top | Right
    property int notifyMarginX: 0
    property int notifyMarginY: 0
    property int notifyLayer: 2
    property int bodyLines: 3
    property int fontSize: 0
    property var notifyHeader: ["icon", "time"]
    property string iconStyle: "builtin"
    // The centre's. Only the widgets tst_historypanel exercises: the others
    // would each need a stand-in for a singleton the test has no use for.
    property string centreOutput: ""
    property int centreSide: 8             // Right
    property int centreBodyLines: 3
    property bool centreCards: true
    property var widgets: ["title", "search", "notifications"]
}
