/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
import QtQuick
import QtQuick.Layouts
import org.glassosd.ui

/* Inline reply: a KDE extension, offered only when the sender asks for it
   (an "inline-reply" action). Split out of NotificationCard.qml. */
RowLayout {
    id: reply
    property string placeholder: ""
    property string submitText: ""
    /* While typing, horizontal drags belong to the field, not the swipe. */
    readonly property alias inputFocused: replyInput.activeFocus
    signal sent(string text)

    spacing: 6

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 32
        radius: 9
        color: Qt.rgba(1, 1, 1, 0.08)
        border.color: replyInput.activeFocus ? Style.accent : Qt.rgba(1, 1, 1, 0.12)
        border.width: 1

        Text {
            anchors.fill: parent
            anchors.leftMargin: 10
            verticalAlignment: Text.AlignVCenter
            visible: replyInput.text === "" && !replyInput.activeFocus
            text: reply.placeholder
            color: Style.foregroundDim
            font.family: Style.fontFamily
            font.pointSize: Style.notifyFontSize - 1
        }

        TextInput {
            id: replyInput
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            verticalAlignment: Text.AlignVCenter
            color: Style.foreground
            font.family: Style.fontFamily
            font.pointSize: Style.notifyFontSize - 1
            selectByMouse: true
            clip: true
            onAccepted: reply.sent(text)
        }
    }

    Rectangle {
        Layout.preferredWidth: sendLabel.implicitWidth + 22
        Layout.preferredHeight: 32
        radius: 9
        color: replyInput.text !== ""
               ? (sendHover.containsMouse ? Qt.lighter(Style.accent, 1.15) : Style.accent)
               : Qt.rgba(1, 1, 1, 0.08)
        Behavior on color { ColorAnimation { duration: 100 } }

        Text {
            id: sendLabel
            anchors.centerIn: parent
            text: reply.submitText
            color: Style.foreground
            font.family: Style.fontFamily
            font.pointSize: Style.notifyFontSize - 1
        }

        MouseArea {
            id: sendHover
            anchors.fill: parent
            hoverEnabled: true
            onClicked: if (replyInput.text !== "") reply.sent(replyInput.text)
        }
    }
}
