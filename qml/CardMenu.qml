/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
import QtQuick
import org.glassosd.ui

/* Drawn inside the card rather than as a Qt popup. A popup is its
   own window, and this is a layer-shell surface with room for the
   card and its shadow and nothing else, so a real menu would be
   placed by the compositor somewhere we do not control. Two rows fit
   within the card comfortably. */
Item {
    id: cardMenu
    /* Snooze is offered only for a card without a stack tag. */
    property string stackTag: ""
    property bool leaving: false
    signal snoozeRequested()
    signal settingsRequested()

    visible: open
    property bool open: false
    property real atX: 0
    property real atY: 0

    function openAt(x, y) {
        atX = x
        atY = y
        open = true
    }
    function close() {
        open = false
    }

    /* Put away if the card starts leaving under it. */
    onLeavingChanged: if (leaving) open = false

    Rectangle {
        id: menuSheet
        width: 184
        height: menuCol.implicitHeight + 10
        radius: 12
        /* Opaque: a translucent menu over a translucent card reads as
           neither, and the card beneath is busy with text. */
        color: Style.opaque(Style.cardBackground)
        border.color: Style.controlEdge
        border.width: 1
        /* Clamped, so a right-click near an edge does not hang the
           menu outside the card and into the shadow padding. */
        x: Math.max(6, Math.min(cardMenu.atX, cardMenu.width - width - 6))
        y: Math.max(6, Math.min(cardMenu.atY, cardMenu.height - height - 6))

        Column {
            id: menuCol
            width: parent.width
            y: 5

            Repeater {
                model: [
                    { label: "Snooze", key: "snooze", on: cardMenu.stackTag === "" },
                    { label: "Notification settings", key: "settings", on: true }
                ]
                delegate: Rectangle {
                    required property var modelData
                    visible: modelData.on
                    width: menuCol.width
                    height: 30
                    color: rowHover.containsMouse ? Style.controlFillHover
                                                  : "transparent"

                    Text {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.label
                        color: Style.foreground
                        font.family: Style.fontFamily
                        font.pointSize: Style.notifyFontSize - 1
                    }

                    MouseArea {
                        id: rowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            cardMenu.close()
                            if (modelData.key === "snooze")
                                cardMenu.snoozeRequested()
                            else
                                cardMenu.settingsRequested()
                        }
                    }
                }
            }
        }
    }
}
