/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import org.glassosd.ui

/*
    One earlier notification of an open group, drawn as a card of its own the
    way macOS lays out an expanded stack: the same icon, title, time and body
    as the group's top card, minus what only a live notification can offer.

    It is a live notification, as a member of a macOS stack is: a click opens
    it (its sender's default action, or else its app) and the corner button
    or a middle click dismisses it alone, its sender told as for any card.

    `entry` is a map from the model's `members` role: id, summary, body,
    iconSource, badgeSource, when, actions, hasDefaultAction, masked as a
    card of its own would be.
*/
Item {
    id: root

    property var entry: ({})
    /* 0 while it still stands in for a sheet under the top card, 1 once it is
       a card in its own right; the fill crosses from the sheet colour to the
       card's so nothing changes colour in one frame as the stack opens. */
    property real open: 1.0
    readonly property bool hovered: pointer.hovered || closeButton.hovered
    signal activated()
    signal dismissed()

    implicitHeight: content.implicitHeight + Style.notifyPadTop + Style.notifyPadBottom

    /* The card is a child, not the root, for the corner button's sake: the
       card carries a layer for its shadow, and Qt delivers no pointer events
       to a layered item's children outside its bounds, so a button hanging
       off its corner could never be clicked from inside it. Same split as
       the top card's. */
    GlassPanel {
        id: panel
        anchors.fill: parent
        radius: Style.notifyRadius
        glass: true
        shadow: true
        surfaceColor: {
            const from = Style.cardStackEdge
            const to = area.pressed ? Style.lift(Style.cardBackground, 1.28) : Style.cardBackground
            const t = Math.max(0, Math.min(1, root.open))
            return Qt.rgba(from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t,
                           from.b + (to.b - from.b) * t, from.a + (to.a - from.a) * t)
        }

        HoverHandler { id: pointer }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            onClicked: (mouse) => mouse.button === Qt.MiddleButton ? root.dismissed() : root.activated()
        }

        RowLayout {
            id: content
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Style.notifyPadding
            anchors.topMargin: Style.notifyPadTop
            anchors.rightMargin: Style.notifyPadRight
            spacing: Style.notifyIconGap
            /* Text fades in as the card leaves the stack: under the top card it
               is a sheet, and a sheet has nothing written on it. */
            opacity: Math.max(0, (root.open - 0.35) / 0.65)

            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: Style.notifyIconBox
                Layout.preferredHeight: Style.notifyIconBox
                visible: (root.entry.iconSource || "") !== ""
                         && Appearance.notifyHeader.includes("icon")
                radius: width * Style.chipRadiusRatio
                color: icon.isSymbolic ? Style.chipIdle : "transparent"

                Image {
                    id: icon
                    anchors.centerIn: parent
                    width: isSymbolic ? Style.notifyIconSize : Style.notifyAppIcon
                    height: width
                    source: Icons.themed(root.entry.iconSource, Style.foreground)
                    sourceSize: Qt.size(width * Screen.devicePixelRatio,
                                        width * Screen.devicePixelRatio)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    mipmap: true
                    readonly property bool isSymbolic:
                        Icons.isCustom(String(root.entry.iconSource || "")) || String(root.entry.iconSource || "").indexOf("symbolic") !== -1
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Style.notifyLinePx > 0 ? 2 : 3

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 7

                    Text {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: (root.entry.summary || "")
                              + ((root.entry.repeatCount || 1) > 1 ? "  ×" + root.entry.repeatCount : "")
                        textFormat: Text.StyledText
                        color: Style.foreground
                        font.family: Style.fontFamily
                        font.pointSize: Style.notifyTextPx > 0 ? Style.notifyTextPx * 0.75
                                                               : Style.notifyFontSize
                        lineHeightMode: Style.notifyLinePx > 0 ? Text.FixedHeight : Text.ProportionalHeight
                        lineHeight: Style.notifyLinePx > 0 ? Style.notifyLinePx : 1.0
                        font.bold: true
                        wrapMode: Text.WordWrap
                        maximumLineCount: Style.notifyTitleLines
                        elide: Text.ElideRight
                    }

                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        visible: Appearance.notifyHeader.includes("time")
                        text: root.entry.when || ""
                        color: Style.notifyTime
                        font.family: Style.fontFamily
                        font.pointSize: Style.notifyTimePx > 0 ? Style.notifyTimePx * 0.75
                                                               : Style.notifyFontSize - 2
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: root.entry.body || ""
                    textFormat: Text.StyledText
                    color: Style.notifyBody
                    font.family: Style.fontFamily
                    font.pointSize: Style.notifyTextPx > 0 ? Style.notifyTextPx * 0.75
                                                           : Style.notifyFontSize - 1
                    lineHeightMode: Style.notifyLinePx > 0 ? Text.FixedHeight : Text.ProportionalHeight
                    lineHeight: Style.notifyLinePx > 0 ? Style.notifyLinePx : 1.0
                    wrapMode: Text.WordWrap
                    maximumLineCount: Appearance.bodyLines
                    elide: Text.ElideRight
                }
            }
        }
    }

    /* The same corner button the top card has, for this one alone. It hangs
       half off the corner, so it registers an input region of its own while
       shown, as the top card's does, and a moment's grace keeps it up while
       the pointer crosses onto its outer half. */
    Timer {
        id: closeGrace
        interval: 250
    }
    Connections {
        target: pointer
        function onHoveredChanged() { if (!pointer.hovered) closeGrace.restart() }
    }
    CardButton {
        id: closeButton
        icon: "close"
        visible: root.open > 0.95 && (pointer.hovered || hovered || closeGrace.running)
        anchors.horizontalCenter: panel.left
        anchors.verticalCenter: panel.top
        z: 3
        onActivated: root.dismissed()
        onVisibleChanged: {
            const w = Window.window
            if (!w)
                return
            if (visible) {
                const p = mapToItem(null, 0, 0)
                Surface.setInputRegion(w, closeButton, Qt.rect(p.x, p.y, width, height), width / 2)
            } else {
                Surface.clearPanelRegion(w, closeButton)
            }
        }
        Component.onDestruction: if (Window.window) Surface.clearPanelRegion(Window.window, closeButton)
    }
}
