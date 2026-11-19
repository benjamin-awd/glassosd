/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
import QtQuick
import QtQuick.Layouts
import org.glassosd.ui

/* The sender's actions as one segmented control. Split out of
   NotificationCard.qml. */
RowLayout {
    id: actionsRow
    /* [{key, label}, ...] as the model gives them. */
    property var actions: []
    signal triggered(string key)

    spacing: 1

    Repeater {
        model: actionsRow.actions
        delegate: Rectangle {
            required property var modelData
            required property int index
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            color: actHover.pressed ? Style.controlFillActive
                 : actHover.containsMouse ? Style.controlFillHover
                                          : Style.controlFill
            Behavior on color { ColorAnimation { duration: 100 } }

            // Square off the inner edges so the row reads as one
            // segmented control rather than separate buttons.
            topLeftRadius: index === 0 ? 9 : 0
            bottomLeftRadius: index === 0 ? 9 : 0
            topRightRadius: index === actionsRow.actions.length - 1 ? 9 : 0
            bottomRightRadius: index === actionsRow.actions.length - 1 ? 9 : 0

            Text {
                anchors.centerIn: parent
                text: modelData.label
                color: Style.foreground
                font.family: Style.fontFamily
                font.pointSize: Style.notifyFontSize - 1
            }

            MouseArea {
                id: actHover
                anchors.fill: parent
                hoverEnabled: true
                onClicked: actionsRow.triggered(modelData.key)
            }
        }
    }
}
