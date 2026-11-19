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

    The sender was told it closed when the group merged (coalescing does
    that), so there are no actions, reply or options here. A click opens it as
    the notification centre would: its default action if it is somehow still
    live, otherwise the application.

    `entry` is a map from HistoryModel.groupMembers(): id, summary, body,
    iconSource, when, repeatCount.
*/
GlassPanel {
    id: root

    property var entry: ({})
    /* 0 while it still stands in for a sheet under the top card, 1 once it is
       a card in its own right; the fill crosses from the sheet colour to the
       card's so nothing changes colour in one frame as the stack opens. */
    property real open: 1.0
    readonly property alias hovered: pointer.hovered
    signal activated()

    radius: Style.notifyRadius
    glass: true
    shadow: true
    surfaceColor: {
        const from = Style.cardStackEdge
        const to = area.pressed ? Style.lift(Style.cardBackground, 1.28) : Style.cardBackground
        const t = Math.max(0, Math.min(1, open))
        return Qt.rgba(from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t,
                       from.b + (to.b - from.b) * t, from.a + (to.a - from.a) * t)
    }
    implicitHeight: content.implicitHeight + Style.notifyPadTop + Style.notifyPadBottom

    HoverHandler { id: pointer }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.activated()
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
                source: root.entry.iconSource || ""
                sourceSize: Qt.size(width * Screen.devicePixelRatio,
                                    width * Screen.devicePixelRatio)
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
                readonly property bool isSymbolic:
                    Icons.isCustom(String(source)) || String(source).indexOf("symbolic") !== -1
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
                    maximumLineCount: 2
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
