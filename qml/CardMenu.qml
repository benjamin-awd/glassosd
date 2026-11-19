/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
import QtQuick
import QtQuick.Layouts
import org.glassosd.ui

/*
    Options: what macOS puts behind the button that replaces a banner's
    timestamp on hover. The sender's actions first (a banner keeps them here
    rather than on the card, as macOS does; an alert keeps its button bar and
    shows none here), then muting the app for a while, a reminder, and its
    settings.

    Inline in the card, which grows to hold it, rather than floating over
    it. A real popup is a window of its own, placed by the compositor
    somewhere this layer-shell surface does not control; and a sheet drawn
    over the card had room for two rows at most, where this needs up to
    seven. The card's own input and blur regions follow its new height.
*/
ColumnLayout {
    id: cardMenu

    /* The sender's actions ([{key, label}]), shown only when the card itself
       does not show them. */
    property var actions: []
    property bool showActions: false
    /* A reminder is offered only for a card without a stack tag: a tagged
       notification is replaced by its successor, so a copy brought back
       later would be stale. */
    property string stackTag: ""
    property int snoozeMinutes: 10
    property bool leaving: false

    signal actionTriggered(string key)
    signal muteRequested(int minutes)   // -1: until the end of the day
    signal snoozeRequested()
    signal settingsRequested()

    property bool open: false
    function openAt(x, y) { open = true }   // position kept for callers; it opens in place
    function close() { open = false }

    visible: open
    spacing: 0
    Layout.fillWidth: true
    Layout.topMargin: open ? 8 : 0

    /* Put away if the card starts leaving under it. */
    onLeavingChanged: if (leaving) open = false

    readonly property var rows: {
        const out = []
        if (showActions) {
            for (let i = 0; i < actions.length; ++i)
                out.push({ label: actions[i].label, kind: "action", key: actions[i].key })
            if (actions.length > 0)
                out.push({ kind: "separator" })
        }
        out.push({ label: "Mute for 1 hour", kind: "mute", minutes: 60 })
        out.push({ label: "Mute for today", kind: "mute", minutes: -1 })
        if (stackTag === "")
            out.push({ label: snoozeMinutes === 1 ? "Remind me in 1 minute"
                                                   : "Remind me in " + snoozeMinutes + " minutes",
                       kind: "snooze" })
        out.push({ label: "Settings…", kind: "settings" })
        return out
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: Style.controlEdge
    }

    Repeater {
        model: cardMenu.rows
        delegate: Item {
            id: row
            required property var modelData
            Layout.fillWidth: true
            Layout.preferredHeight: modelData.kind === "separator" ? 9 : 28

            Rectangle {
                visible: row.modelData.kind === "separator"
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 1
                color: Style.controlEdge
            }

            Rectangle {
                visible: row.modelData.kind !== "separator"
                anchors.fill: parent
                anchors.leftMargin: -6
                anchors.rightMargin: -6
                radius: 6
                color: rowArea.pressed ? Style.controlFillActive
                     : rowArea.containsMouse ? Style.controlFillHover : "transparent"
            }
            Text {
                visible: row.modelData.kind !== "separator"
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                text: row.modelData.label || ""
                textFormat: Text.PlainText   // an action label is the sender's text
                elide: Text.ElideRight
                color: Style.foreground
                font.family: Style.fontFamily
                font.pointSize: Style.notifyFontSize - 1
                font.weight: row.modelData.kind === "action" ? Font.DemiBold : Font.Normal
            }
            MouseArea {
                id: rowArea
                enabled: row.modelData.kind !== "separator"
                anchors.fill: parent
                hoverEnabled: true
                onClicked: {
                    cardMenu.close()
                    switch (row.modelData.kind) {
                    case "action":   cardMenu.actionTriggered(row.modelData.key); break
                    case "mute":     cardMenu.muteRequested(row.modelData.minutes); break
                    case "snooze":   cardMenu.snoozeRequested(); break
                    case "settings": cardMenu.settingsRequested(); break
                    }
                }
            }
        }
    }
}
