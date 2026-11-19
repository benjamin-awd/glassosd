/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
import QtQuick
import QtQuick.Window
import QtQuick.Effects
import org.glassosd.ui

/*
    The sheets peeking out beneath a grouped card, and the shadow the stack
    casts as one. Split out of NotificationCard.qml.
*/
/* The stack is flattened and faded once, rather than composited card by
   card. That is the whole trick, and everything else here follows from it.

   Two constraints fight each other on a translucent card. A sheet must not
   overlap the card, because 0.78 over 0.78 composes to an effective 0.95
   and the doubled strip goes nearly opaque — that was the black band. But
   not overlapping means the sheet has to stop at the card's bottom edge,
   and the card's rounded corner curves away from it, leaving a wedge of
   backdrop between the two. Squaring the corners closed the wedge and gave
   three different shapes down one edge; rounding them back restored the
   shapes and the wedge with them; widening the offset shrank the wedge by
   making the whole stack twice as tall.

   None of that is fixable one card at a time, because the problem is that
   they are being composited one card at a time. Rendering the lot into a
   layer at full opacity and applying the glass alpha to the *result* means
   overlap costs nothing: sheets sit fully behind the card, no strip is
   doubled, no corner has a wedge to hide, and the offset can go back to
   being small. Inside the layer they are ordinary opaque shapes stacked in
   z order, which is what they always wanted to be. */
/* The flattened stack is the *surfaces only*: the sheets plus an opaque
   stand-in for the card's own fill, rendered into one layer and faded to
   the card alpha as a whole (see above for why). The card itself sits on
   top of this with a transparent fill, so its text, icon and edge are
   drawn at full strength. When the card's content lived inside the faded
   layer it was faded with it, which went unnoticed at alpha 0.83 and
   read as greyed-out text and a washed-out icon at the macOS theme's
   0.60. */
Item {
    id: stackSurface

    /* The card this stack sits under, its depth, and whether it is pressed. */
    required property Item card
    property int depth: 0
    property bool pressed: false
    readonly property bool stacked: depth > 0
    /* The sheet items, for the card to register and clear their regions. */
    readonly property alias sheets: sheets

    visible: stacked
    opacity: Style.cardBackground.a
    layer.enabled: stackSurface.stacked
    layer.effect: MultiEffect {
        shadowEnabled: true
        shadowColor: Style.shadowColor
        shadowBlur: Style.shadowBlur
        blurMax: Style.shadowBlurMax
        shadowOpacity: Style.shadowOpacity
        shadowVerticalOffset: Style.shadowYOffset
        autoPaddingEnabled: true
    }

    Rectangle {
        x: stackSurface.card.x
        y: stackSurface.card.y
        width: stackSurface.card.width
        height: stackSurface.card.height
        radius: stackSurface.card.radius
        antialiasing: true
        color: {
            const base = Style.opaque(Style.cardBackground)
            return stackSurface.pressed ? Style.lift(base, 1.28) : base
        }
        Behavior on color { ColorAnimation { duration: 90 } }
    }

    Repeater {
        id: sheets
        model: stackSurface.depth
        delegate: Rectangle {
            id: sheetRect
            required property int index

            z: -1 - index
            anchors.horizontalCenter: parent.horizontalCenter
            width: stackSurface.card.width - (index + 1) * Style.stackInset * 2
            y: Style.stackDrop(index)
            height: stackSurface.card.height
            radius: Style.stackRadius
            color: stackSurface.stacked ? Style.opaque(Style.cardStackEdge) : Style.cardStackEdge
            /* The same lit edge the card carries. Without it the sheets had a
               fill and nothing else, so where one ended and the next began
               there was no boundary at all — same colour, no border, and no
               seam since a single sheet does not need one. The strip below the
               card rendered as one undifferentiated ledge rather than as two
               cards, and the silhouette went soft exactly where the card's own
               crisp edge stopped. */
            antialiasing: true

            /* Over the fill rather than as the border, for the reason given
               in GlassPanel: a border replaces the fill in its band. */
            Rectangle {
                anchors.fill: parent
                radius: parent.radius
                color: "transparent"
                border.color: Style.glassEdge
                border.width: Style.edgeWidth
                antialiasing: true
            }

            /* Registered so the compositor blurs behind the sliver too. The
               region is this rectangle exactly; the part of it behind the card
               simply lands inside the card's own region, and regions union. */
            function refreshSheet() {
                if (!Window.window)
                    return
                const p = mapToItem(null, 0, 0)
                const r = Qt.rect(p.x, p.y, width, height)
                Surface.setPanelRegion(Window.window, sheetRect, r, radius)
            }
            Component.onCompleted: refreshSheet()
            onYChanged: refreshSheet()
            onWidthChanged: refreshSheet()
            onHeightChanged: refreshSheet()
            Component.onDestruction: if (Window.window) Surface.clearPanelRegion(Window.window, sheetRect)

            /* No seam. With a single sheet the only boundary that matters is
               between it and the card, and the card's own bottom edge already
               draws that. A seam along the sheet's bottom would be a dark line
               under the outermost edge of the whole stack, which is not a
               boundary between anything. The references separate the layers by
               inset and tone alone, and so do we. */
        }
    }
}
