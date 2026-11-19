// SPDX-FileCopyrightText: 2026 glassosd contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Test stand-in for the C++ Surface singleton. No layer shell and no
// compositor: it records which panels hold a blur/input region, so a test can
// assert that nothing is left holding one, and what setPosition() was told.
pragma Singleton
import QtQuick
QtObject {
    property bool blurAvailable: true
    property int regionCalls: 0
    property var holders: []
    property var lastPosition: []
    function initLayerShell() {}
    function setPosition(w, a, top, right, bottom, left) { lastPosition = [a, top, right, bottom, left] }
    function setInputFollowsPanels() {}
    function setKeyboardFocus() {}
    function setOutput() {}
    function allowOnLockScreen() {}
    function applyContrast() {}
    function clearContrast() {}
    function clearBlur() {}
    function holdBlur() {}
    function setPanelRegion(w, p, r, rad) {
        regionCalls++
        if (holders.indexOf(p) < 0) holders.push(p)
    }
    property var inputHolders: []
    function setInputRegion(w, p, r, rad) {
        if (inputHolders.indexOf(p) < 0) inputHolders.push(p)
    }
    function clearPanelRegion(w, p) {
        const k = inputHolders.indexOf(p)
        if (k >= 0) inputHolders.splice(k, 1)
        const i = holders.indexOf(p)
        if (i >= 0) holders.splice(i, 1)
    }
}
