// SPDX-FileCopyrightText: 2026 glassosd contributors
// SPDX-License-Identifier: GPL-2.0-or-later
pragma Singleton
import QtQuick
QtObject {
    property string lastGroup: ""
    signal changed()
    property int total: 0
    property var members: []
    property int lastActivated: -1
    function showGroup(k, settings) { lastGroup = k }
    function groupMembers(key, excludeId, max) { return members.slice(0, max) }
    function activateId(id) { lastActivated = id }
}
