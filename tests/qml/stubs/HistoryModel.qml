// SPDX-FileCopyrightText: 2026 glassosd contributors
// SPDX-License-Identifier: GPL-2.0-or-later
pragma Singleton
import QtQuick
QtObject {
    property string lastGroup: ""
    function showGroup(k, settings) { lastGroup = k }
}
