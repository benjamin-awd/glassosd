/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
import QtQuick
import QtQuick.Layouts
import org.glassosd.ui

/*
    An open group's header in Notification Centre, as macOS titles an open
    stack and as the popup titles its own open group: the app's name, then
    Show less and a clear button. The app's settings sit beside them,
    reachable from the centre itself and not only from a live popup.

    Only an open group has one. A folded group is a stack (CentreCard) and a
    single entry is a card on its own; the old header over every group, small
    uppercase with a chevron and a count, is gone.
*/
Item {
    id: root

    property string appName: ""
    property string icon: ""
    /* The keyboard is on this row. */
    property bool current: false

    signal foldRequested()
    signal clearRequested()
    signal settingsRequested()

    implicitHeight: 34

    /* The keyboard highlight: the hovered card's fill and accent hairline,
       one look for "the keyboard is here" whichever kind of row it is. */
    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: Style.entryCardHover
        border.width: 1
        border.color: Qt.rgba(Style.accent.r, Style.accent.g, Style.accent.b, 0.55)
        opacity: root.current ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 110 } }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.rightMargin: 2
        spacing: 6

        Text {
            Layout.fillWidth: true
            text: root.appName
            textFormat: Text.PlainText   // app_name is not markup and is not sanitised
            color: Style.foreground
            font.family: Style.fontFamily
            font.pointSize: Style.notifyFontSize + 2
            font.bold: true
            elide: Text.ElideRight

            /* The name folds the group too, as the chevron row did. */
            MouseArea {
                anchors.fill: parent
                onClicked: root.foldRequested()
            }
        }

        CardButton {
            icon: "settings"
            onActivated: root.settingsRequested()
        }

        Rectangle {
            implicitWidth: lessLabel.implicitWidth + 20
            implicitHeight: 24
            radius: height / 2
            color: lessArea.pressed ? Style.controlFillActive
                 : lessArea.containsMouse ? Style.controlFillHover : Style.controlFill
            border.color: Style.controlEdge
            border.width: 1
            Behavior on color { ColorAnimation { duration: 100 } }

            Text {
                id: lessLabel
                anchors.centerIn: parent
                text: "Show less"   // Apple's own capitalisation
                color: Style.foreground
                font.family: Style.fontFamily
                font.pointSize: Style.notifyFontSize - 2
            }
            MouseArea {
                id: lessArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.foldRequested()
            }
        }

        CardButton {
            icon: "close"
            onActivated: root.clearRequested()
        }
    }
}
