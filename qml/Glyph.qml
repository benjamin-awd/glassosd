/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
import QtQuick
import QtQuick.Effects

/*
    An icon that follows the surface's text colour when it is a glyph.

    Our own icons are drawn white and every surface is expected to tint them.
    The OSD and the card buttons did; the centre's Do Not Disturb row, the
    slider rows, the button grid and the notification chips did not, and in
    the light theme they were white on white. One component, so a new call
    site cannot forget.

    Monochrome is decided by Icons.isMonochrome on the source; anything else
    (application artwork) is shown as drawn.
*/
Item {
    id: root

    property url source
    property color tint: Style.foreground
    property size sourceSize
    property int fillMode: Image.PreserveAspectFit
    /* Whether to tint. Defaults to "the source is one of our glyphs or a
       -symbolic theme icon"; a caller that knows better can override. */
    property bool monochrome: {
        const s = String(source)
        return s !== "" && (Icons.isMonochrome(s.replace(/^image:\/\/icon\//, ""))
                            || s.indexOf("/icons/") !== -1 && s.endsWith(".svg"))
    }
    readonly property alias status: img.status

    implicitWidth: img.implicitWidth
    implicitHeight: img.implicitHeight

    Image {
        id: img
        anchors.fill: parent
        source: root.source
        sourceSize: root.sourceSize
        fillMode: root.fillMode
        smooth: true
        mipmap: true
        visible: !root.monochrome
    }

    MultiEffect {
        anchors.fill: img
        source: img
        visible: root.monochrome
        colorization: 1.0
        colorizationColor: root.tint
    }
}
