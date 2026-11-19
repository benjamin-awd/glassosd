// SPDX-FileCopyrightText: 2026 glassosd contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Test stand-in for the C++ Theme singleton: a plain key/value map. A test
// sets values and bumps revision, exactly as a theme reload would.
pragma Singleton
import QtQuick
QtObject {
    signal changed()
    property int revision: 0
    property var values: ({})
    function pick(k, f) { return values[k] !== undefined ? values[k] : f }
    function color(k, f) { return pick(k, f) }
    function num(k, f) { return pick(k, f) }
    function str(k, f) { return pick(k, f) }
    function flag(k, f) { return pick(k, f) }
}
