/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import org.glassosd.ui

/*
    One entry in Notification Centre, drawn as the popup draws it: the same
    icon box, title, time and body, at the same sizes, with the card's
    corner close button on hover. A folded group is the same card with the
    group's sheets beneath it and "Show N more", as a popup stack is.

    The centre used to draw its own compact rows, under small uppercase
    headers with a chevron, which read as a log rather than as the cards
    that had just been on screen.

    Not GlassPanel and not MemberCard. Both register blur and input regions
    with the window per card, which is right for the popup surface, whose
    cards are the window's only content; in the centre they sit inside one
    glass panel already, and per-card regions in a scrolling list would go
    stale the moment it scrolled (GlassPanel re-registers on resize, not on
    moving).

    `row` is the ListView delegate's model object: summary, body,
    iconSource, when, repeatCount, stackDepth, groupCount.
*/
Item {
    id: root

    property var row: ({})
    /* The keyboard is on this row. */
    property bool current: false

    signal activated()        // a click: open it
    signal removed()          // its close button or a middle click
    signal expandRequested()  // a stack clicked, or its Show N more

    readonly property int depth: row.stackDepth || 0
    readonly property bool stacked: depth > 0
    readonly property bool hovered: pointer.hovered || closeButton.hovered
    readonly property color fill: area.containsMouse || current ? Style.entryCardHover : Style.entryCard

    implicitHeight: face.height + (stacked ? Style.stackDrop(depth - 1) : 0)

    /* The sheets and an opaque stand-in for the card's fill, flattened into
       one layer and faded to the card's alpha as a whole, as the popup's
       CardStackSurface does and for the same reason: translucent sheets
       overlapping a translucent card double up into a dark band. The card
       above draws its text and edge at full strength with no fill. */
    Item {
        anchors.fill: parent
        visible: root.stacked
        opacity: root.fill.a
        layer.enabled: root.stacked

        Repeater {
            model: root.depth
            delegate: Rectangle {
                required property int index
                z: -1 - index
                anchors.horizontalCenter: parent.horizontalCenter
                width: face.width - (index + 1) * Style.stackInset * 2
                y: Style.stackDrop(index)
                height: face.height
                radius: Style.stackRadius
                color: Qt.darker(Style.opaque(Style.entryCard), 1.18 + 0.1 * index)
                antialiasing: true
                Rectangle {
                    anchors.fill: parent
                    radius: parent.radius
                    color: "transparent"
                    border.color: Style.entryCardEdge
                    border.width: 1
                    antialiasing: true
                }
            }
        }
        Rectangle {
            width: face.width
            height: face.height
            radius: face.radius
            antialiasing: true
            color: Style.opaque(root.fill)
        }
    }

    Rectangle {
        id: face
        width: parent.width
        height: content.implicitHeight + Style.notifyPadTop + Style.notifyPadBottom
        radius: Style.notifyRadius
        antialiasing: true
        color: root.stacked ? "transparent" : root.fill
        /* The keyboard highlight is the hover fill plus an accent hairline:
           the fill alone is too faint to find again after looking away. */
        border.color: root.current ? Qt.rgba(Style.accent.r, Style.accent.g, Style.accent.b, 0.55)
                                   : Style.entryCardEdge
        border.width: 1
        Behavior on color { ColorAnimation { duration: 110 } }

        HoverHandler { id: pointer }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            onClicked: (mouse) => {
                if (mouse.button === Qt.MiddleButton)
                    root.removed()
                /* A stack opens, as one does in Notification Centre: its
                   top card is one of several, and which one a click meant is
                   only clear once they are spread out. */
                else if (root.stacked)
                    root.expandRequested()
                else
                    root.activated()
            }
        }

        RowLayout {
            id: content
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: Style.notifyPadding
            anchors.topMargin: Style.notifyPadTop
            anchors.rightMargin: Style.notifyPadRight
            spacing: Style.notifyIconGap

            Rectangle {
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: Style.notifyIconBox
                Layout.preferredHeight: Style.notifyIconBox
                visible: (root.row.iconSource || "") !== ""
                radius: width * Style.chipRadiusRatio
                color: icon.isSymbolic ? Style.chipIdle : "transparent"

                Image {
                    id: icon
                    anchors.centerIn: parent
                    width: isSymbolic ? Style.notifyIconSize : Style.notifyAppIcon
                    height: width
                    source: Icons.themed(root.row.iconSource, Style.foreground)
                    sourceSize: Qt.size(width * Screen.devicePixelRatio,
                                        width * Screen.devicePixelRatio)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    mipmap: true
                    readonly property bool isSymbolic:
                        Icons.isCustom(String(root.row.iconSource || "")) || String(root.row.iconSource || "").indexOf("symbolic") !== -1
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
                        /* How many times this exact message arrived: history
                           keeps one entry and counts, and "it happened 129
                           times" is the interesting part of a repeat. */
                        text: (root.row.summary || "")
                              + ((root.row.repeatCount || 1) > 1 ? "  ×" + root.row.repeatCount : "")
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
                        Layout.alignment: Qt.AlignTop
                        text: root.row.when || ""
                        color: Style.notifyTime
                        font.family: Style.fontFamily
                        font.pointSize: Style.notifyTimePx > 0 ? Style.notifyTimePx * 0.75
                                                               : Style.notifyFontSize - 2
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: root.row.body || ""
                    textFormat: Text.StyledText   // the daemon sanitises it
                    color: Style.notifyBody
                    font.family: Style.fontFamily
                    font.pointSize: Style.notifyTextPx > 0 ? Style.notifyTextPx * 0.75
                                                           : Style.notifyFontSize - 1
                    lineHeightMode: Style.notifyLinePx > 0 ? Text.FixedHeight : Text.ProportionalHeight
                    lineHeight: Style.notifyLinePx > 0 ? Style.notifyLinePx : 1.0
                    wrapMode: Text.WordWrap
                    maximumLineCount: Appearance.centreBodyLines
                    elide: Text.ElideRight
                }

                /* The popup stack's own pill, words and all. */
                Rectangle {
                    Layout.topMargin: 6
                    visible: root.stacked
                    implicitWidth: moreLine.implicitWidth + 22
                    implicitHeight: moreLine.implicitHeight + 10
                    radius: height / 2
                    color: moreArea.containsMouse ? Style.controlFillHover : Style.controlFill
                    border.color: Style.controlEdge
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 110 } }

                    Text {
                        id: moreLine
                        anchors.centerIn: parent
                        text: "Show " + ((root.row.groupCount || 1) - 1) + " more"
                        color: Style.foreground
                        font.family: Style.fontFamily
                        font.pointSize: Style.notifyFontSize - 2
                    }

                    MouseArea {
                        id: moreArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.expandRequested()
                    }
                }
            }
        }
    }

    /* The card's corner button, as on a popup. On a stack it clears the
       group, as the X on a stack does in Notification Centre. Shown on the
       keyboard's row too, as the hint that Delete does what it does. */
    Timer {
        id: closeGrace
        interval: 250
        onRunningChanged: Qt.callLater(root.syncClose)
    }
    Connections {
        target: pointer
        function onHoveredChanged() {
            if (!pointer.hovered)
                closeGrace.restart()
            Qt.callLater(root.syncClose)
        }
    }
    onCurrentChanged: Qt.callLater(root.syncClose)

    /* Worked out once per turn of the event loop, not bound. Each input
       depends on the button itself: shown under a resting pointer it takes
       the hover from the card, and hidden it loses its own, so a binding
       re-ran inside its own evaluation (a loop whenever the keyboard moved
       off a row with the pointer on its X). Settling a turn later breaks
       the cycle; callLater runs it once however many inputs changed. */
    property bool showClose: false
    function syncClose() {
        showClose = pointer.hovered || closeButton.hovered || closeGrace.running || root.current
    }
    CardButton {
        id: closeButton
        icon: "close"
        onHoveredChanged: Qt.callLater(root.syncClose)
        visible: root.showClose
        anchors.horizontalCenter: face.left
        anchors.verticalCenter: face.top
        z: 3
        onActivated: root.removed()
    }
}
