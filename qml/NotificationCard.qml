/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
import QtQuick
import QtQuick.Window
import QtQuick.Effects
import QtQuick.Layouts
import org.glassosd.ui

/*
    Notification card, following Apple's notification language:
    a small-caps app identity line with the time on the right, then title,
    body, a "Show N more" line for the rest of a group, and full-width
    segmented actions.

    Grouped notifications render as a physical stack with card edges peeking
    out beneath — the single clearest signal that more are hiding behind this
    one, and the reason a burst reads as "several" rather than "a card with a
    number on it".
*/
Item {
    id: root

    property var entry            // the model row
    property int stackDepth: 0    // how many edges to peek out below

    readonly property bool stacked: stackDepth > 0

    /* One layer for the whole stack: it flattens the cards so their alphas
       cannot compound, and it gives MultiEffect a silhouette to cast a single
       shadow around — the card and its sheets together, rather than the card
       shadowing its own sheets. Off for a lone card, which needs neither and
       should not pay for a layer.

       The glass alpha is applied here, to the flattened result, which is why
       everything inside is drawn opaque while stacked. */
    /* Fades in on arrival. `reveal` is a separate 0..1 factor rather than
       opacity itself, because opacity is already carrying the glass alpha for
       the flattened stack — assigning to it directly would drop the stack to
       fully opaque the moment the animation finished.

       Nothing moves. A card that slides or grows has to re-send its blur
       region every frame, since compositor blur is binary — a surface is
       blurred or it is not, there is no alpha to animate — and the region is
       rebuilt from every panel in the window on each change. Fading leaves the
       geometry alone, so the region is registered once and stays correct. */
    property real reveal: 0.0
    /* Set by the view from the model's `closing` role: the row is kept for the
       length of the animation after it has been closed. */
    property bool leaving: false

    /* Swipe-to-dismiss. swipeRaw is how far the finger or pointer has moved;
       swipeX is how far the card is allowed to follow it.

       They are not the same number because the card cannot leave the screen.
       The layer-shell surface is the width of the card plus its shadow
       padding, so a translation past that is clipped by the surface edge
       rather than sliding off — a swipe that tracked the finger one-to-one
       would simply be cut in half. tanh saturates at Style.swipeTravel while
       staying near one-to-one for the first few pixels, so the gesture starts
       out feeling direct and then resists, which is also the honest signal
       that this card is not going to fly anywhere. The fade does the rest. */
    property real swipeRaw: 0.0
    property bool swipingOut: false
    /* Set by whichever gesture is ending, immediately before endSwipe(). */
    property real releaseVelocity: 0.0
    /* With slideTravel set the surface reaches the screen edge, so a
       rightward swipe follows the finger exactly and is clipped by the edge
       the way the macOS banner is. Leftwards still saturates: the gesture
       means nothing in that direction, and the resistance says so. */
    readonly property real swipeX:
        (Style.slideTravel > 0 && swipeRaw > 0)
            ? swipeRaw
            : Style.swipeTravel * Math.tanh(swipeRaw / Math.max(1, Style.swipeTravel))
    /* How close the gesture is to committing, 0..1. Drives the fade, so the
       card is visibly half gone by the time letting go would dismiss it. */
    readonly property real swipeProgress:
        Math.min(1.0, Math.abs(swipeRaw) / Math.max(1, root.width * Style.swipeCommit))

    /* The swipe fade existed because the card could not leave: it saturated
       40px out and had to disappear somehow. Once it can travel off the edge
       there is nothing to cover up, and macOS does not fade either. */
    /* A sliding card is opaque for nearly all of its travel — macOS does not
       dissolve a banner, it moves it — and only fades in the last stretch
       nearest the edge, which softens the clip against the screen edge. A
       card that stays where it is has nothing but the fade, so it keeps the
       full ramp. */
    opacity: (Style.slideTravel > 0 ? Math.min(1.0, reveal * 4.0) : reveal)
             * (Style.slideTravel > 0 ? 1.0 : (1.0 - 0.8 * swipeProgress))

    /* Quint rather than Cubic: Material's "emphasized" curves are far steeper
       than a plain ease, and the gentle version reads as a dissolve rather
       than as something arriving. Decelerate in, accelerate out, paired. */
    Behavior on reveal {
        NumberAnimation {
            id: revealAnim
            duration: Style.animIn
            easing.type: Style.curveIn.length ? Easing.BezierSpline : Easing.OutQuint
            easing.bezierCurve: Style.curveIn
        }
    }
    /* Revealed once the window has put a frame on screen (see
       NotificationStack's `exposed`), so the blur is there before the card
       is. A card arriving in a window that is already up starts at once. */
    function startReveal() {
        if (reveal === 0.0 && !leaving)
            reveal = 1.0
    }
    /* A binding rather than a one-off signal: the card can be created before
       it has a window, or after the window's first frame, and listening for
       the change alone missed both — the card then never appeared at all. */
    readonly property bool windowReady: Window.window !== null && Window.window.exposed !== false
    onWindowReadyChanged: if (windowReady) startReveal()
    Component.onCompleted: if (windowReady) startReveal()
    /* The exit curve is set here, before reveal moves, rather than bound to
       `leaving`. Bound, nothing guaranteed the binding re-evaluated before
       this handler ran, and it did not: frame-by-frame capture showed the
       exit running the *entry* curve (OutQuint, 225 ms), so the card dropped
       to 16% in 67 ms and then hung there faintly for 150 ms more. The blur
       is given back at 30% (below), so that tail was a ghost of the card
       over a sharp, unblurred backdrop. InQuint holds the card and then lets
       it go, so the stretch below 30% is a frame or two. */
    onLeavingChanged: if (leaving) {
        revealAnim.duration = Style.animOut
        /* A swiped card is already faded by the gesture (to about a third,
           at the edge of the surface); holding it there before letting go,
           as a card closing from full opacity should, left it hanging at the
           edge for 0.3 s in a frame-by-frame capture. It finishes from where
           the swipe left it instead. */
        if (swipingOut) {
            revealAnim.easing.type = Easing.OutCubic
        } else if (Style.curveOut.length) {
            revealAnim.easing.bezierCurve = Style.curveOut
            revealAnim.easing.type = Easing.BezierSpline
        } else {
            revealAnim.easing.type = Easing.InQuint
        }
        reveal = 0.0
        slot = 0.0
    }

    /* The card's share of the column, 1 while it is up and 0 once it has
       gone. Cards below a leaving one used to hold still until its row was
       removed and then jump up a card's height in one frame; collapsing the
       slot alongside the exit lets them glide up into the gap while it
       leaves, which is how a macOS banner stack closes ranks. The stack turns
       it into a `lift` on every card below; this card's own layout is never
       touched. */
    property real slot: 1.0
    /* How far the cards above this one have collapsed, set by the stack. */
    property real lift: 0.0
    Behavior on slot {
        NumberAnimation {
            duration: Style.animOut
            easing.type: Easing.InOutCubic
        }
    }

    /* Rises into place, and sinks as it goes. A transform rather than a change
       of y, so the layout does not reflow every frame and the cards below stay
       put while this one arrives. */
    transform: Translate {
        /* Arrives from the right, as the macOS banner does, and rises only
           when it cannot slide. The two together read as a diagonal. */
        x: root.swipeX + (1.0 - root.reveal) * Style.slideTravel
        y: (Style.slideTravel > 0 ? 0 : (1.0 - root.reveal) * Style.animRise) - root.lift
    }

    /* A card that is destroyed without finishing its exit — Clear all, an
       app replacing its own notification, the model being reset — never
       reaches the reveal < 0.15 branch below, so its contribution stayed in
       the window's aggregate. The region is the union of every contribution,
       so the leftovers blur and lift a patch of backdrop where no card is
       drawn: invisible over a dark window, a grey slab over a white one.
       Observed as 320x308 of region for one 84px card. */
    Component.onDestruction: {
        const w = Window.window
        if (!w)
            return
        Surface.clearPanelRegion(w, card)
        for (let i = 0; i < sheets.count; ++i) {
            const s = sheets.itemAt(i)
            if (s) Surface.clearPanelRegion(w, s)
        }
    }

    /* Set once the blur region has been dropped, so it is not dropped again on
       every remaining frame of the exit. */
    property bool blurDropped: false
    property bool entrySent: false

    onRevealChanged: {
        /* Give the blurred region back before the card has finished fading,
           rather than when the delegate is destroyed.

           Compositor blur is binary — a region is blurred or it is not, there
           is no alpha to fade — so while the card's own pixels drop to nothing
           the rectangle behind it stays exactly as blurred as ever. The last
           frames of a dismissal are therefore a lit glass panel with no card
           in it, and it outlives the card by another frame or two while the
           region update makes its way to the compositor.

           When to drop it is Style.blurDropAt, because the right answer
           depends on whether the card leaves the screen or fades where it
           stands, and that is a theme's decision (motion.slideTravel).

           The default 0.15 is tuned for a card that fades in place. The exit
           accelerates (InQuint), so by the time reveal is this low the card is
           nearly gone and the blur going out from under it does not read as a
           change. It was 0.3, which at 120 Hz left two or three frames of
           faint card over an unblurred backdrop — visible as a ghost in a
           frame-by-frame capture.

           But that argument only weighs one side. Dropping the region also
           changes the *backdrop* in one step, and what hides that step is the
           card's own fill — card.background, alpha 0.83 in the macOS theme.
           So the later the drop, the more of the change shows through:

               drop at reveal 1.00 -> card alpha 0.83, 17% of it visible
               drop at reveal 0.15 -> card alpha 0.12, 88% of it visible

           Tuning for an invisible card therefore maximises the visible
           backdrop step, which is a flash at the moment of dismissal — plain
           over a light window, and the reason this is a theme value now.

           A theme whose cards slide off the screen edge should set it to 1.0
           and give the region back as the exit begins, while the card is still
           opaque enough to cover the change and is on its way out of sight
           anyway. That is what macOS does: the banner leaves the screen rather
           than fading where it stands, so there is never a moment where a
           stationary rectangle has to stop being blurred under the viewer's
           eye. */
        /* A card that slides off the edge keeps its region all the way out:
           Surface re-reads the card's live geometry every frame, so the blur
           travels with it and is clipped away at the surface edge, and there
           is never a stationary blurred patch to give back early. */
        if (root.leaving && Style.slideTravel <= 0 && reveal < Style.blurDropAt) {
            if (!root.blurDropped) {
                root.blurDropped = true
                if (Window.window) {
                    Surface.clearPanelRegion(Window.window, card)
                    for (let i = 0; i < sheets.count; ++i) {
                        const s = sheets.itemAt(i)
                        if (s) Surface.clearPanelRegion(Window.window, s)
                    }
                }
            }
            return
        }

        /* An arriving card sends its region twice: where it starts, and
           where it settles. Resending it on every frame of the rise is what
           made the blur unstable on the way out (below), and the same
           applies on the way in. */
        if (!root.leaving && reveal > 0 && reveal < 1) {
            /* A sliding card crosses the whole width of the surface, so a
               region sent at the start would sit blurred where the card is
               not, for the length of the flight. It gets its blur on
               landing instead. */
            if (Style.slideTravel > 0)
                return
            if (root.entrySent)
                return
            root.entrySent = true
        }

        /* A leaving card keeps its region where it is. It only sinks a few
           pixels on the way out, and chasing that every frame resent the
           window's whole blur each time, for every card at once on Clear all:
           frame-by-frame capture showed the blur gone from the first frame of
           the exit and the window stalling for 230 ms. The region is dropped
           above, near the end. */
        if (root.leaving)
            return

        /* Compositor blur cannot be animated, so the region has to be re-sent
           as the card travels or the blurred patch sits still while the card
           moves off it. Same reason the delegate refreshes when the ListView
           moves it. */
        refreshRegions()
    }

    /* The card and its sheets register their regions in window coordinates,
       and no binding notices when an ancestor moves them, so everything that
       moves this delegate has to say so: the relayout when a card above goes
       away, the rise on arrival, and a swipe.

       Guarded on blurDropped because the exit gives the region back early —
       re-registering it afterwards would put the blurred patch back on screen
       for the last frames of a dismissal, which is the bug that guard exists
       to prevent. */
    function refreshRegions() {
        if (root.blurDropped)
            return
        card.refreshBlur()
        for (let i = 0; i < sheets.count; ++i) {
            const s = sheets.itemAt(i)
            if (s) s.refreshSheet()
        }
    }

    /* Deferred, not immediate. refreshBlur() reads mapToItem(), which goes
       through this item's `transform` — and that Translate's x is a separate
       binding on swipeX. QML does not promise it has been re-evaluated by the
       time this handler runs, and it has not: measured mid-drag, every region
       was registered one change behind the card.

       While the finger is moving that is invisible, because the next change
       corrects it. On release it is not: endSwipe() assigns swipeRaw = 0 in a
       single step, so there is no next change, and the region is left a whole
       gesture behind — parked at the tanh saturation, Style.swipeTravel (40px)
       to the left of the card, blurring a slab of backdrop where no card is
       drawn. Qt.callLater runs it after the binding pass, so mapToItem sees
       the transform the card is actually drawn with, and it coalesces to one
       call per turn rather than one per property write. */
    onSwipeXChanged: Qt.callLater(refreshRegions)

    /* The ListView moves this delegate when a card above it goes away. The
       GlassPanel inside does not move relative to *us*, so none of its own
       geometry handlers fire, and the input region it registered — which is
       in window coordinates — keeps describing where the card used to be.
       The compositor is then left with a hole in the mask exactly where the
       card now is, so clicks fall through to the window behind and nothing
       even highlights on hover. Binding through mapToItem() does not help:
       QML does not re-evaluate it when an ancestor moves. The delegate's own
       y is the thing that actually changes, so drive it from here. */
    /* The sheets register in window coordinates too, so they go stale for the
       same reason the card does when the layout moves us. */
    onYChanged: refreshRegions()

    /* Settle back, or carry on out. Disabled while the finger is down so the
       card tracks the gesture rather than chasing it. */
    Behavior on swipeRaw {
        enabled: !swipe.active
        NumberAnimation {
            duration: root.swipingOut ? Style.animOut : Style.swipeSpring
            /* Out, not In: the finger has already given the card its speed,
               and an accelerating curve started it from rest again. */
            easing.type: Easing.OutCubic
        }
    }

    /* Let go: far enough is a dismissal, anything less springs back.

       Measured on the finger's travel rather than the card's, because the
       card saturates at Style.swipeTravel and every gesture past that point
       would otherwise look identical. Velocity counts too, so a quick flick
       does not have to be a long one — but distance alone is enough, which
       keeps the gesture usable if the velocity reads as zero. */
    function endSwipe() {
        /* Rightwards only, as macOS has it. A left swipe springs back however
           far it went: the gesture there means nothing, and dismissing on it
           made a card vanish when the pointer was only on its way somewhere
           else. */
        /* releaseVelocity rather than reading the handler: by the time this
           runs the drag is over, and a wheel gesture never touched the
           handler at all, so its centroid still holds whatever the last drag
           left there — which committed dismissals nobody asked for. */
        const committed = swipeRaw > 0
                       && (swipeRaw > root.width * Style.swipeCommit
                           || releaseVelocity > Style.swipeFlick)
        if (!committed) {
            swipeRaw = 0
            root.hoverChanged(cardHover.containsMouse)
            return
        }
        /* Dismiss now and let the exit fade run alongside the last of the
           travel: the model keeps the row for Style.animOut either way, so
           waiting for the slide to finish first would only double the wait. */
        swipingOut = true
        /* Far enough to clear the screen edge, not merely the card's own
           width: the surface now reaches the edge, so the card is clipped by
           it rather than stopping short. */
        swipeRaw = root.width + Style.shadowPad * 2
        root.dismissed()
    }
    signal dismissed()
    signal settingsRequested()
    signal snoozeRequested()
    signal moreRequested()
    signal actionTriggered(string key)
    signal replySent(string text)
    signal activated()
    signal hoverChanged(bool hovered)

    implicitHeight: card.implicitHeight
                    + (stackDepth > 0 ? Style.stackDrop(stackDepth - 1) : 0)

    // ---- stacked edges, drawn behind and below --------------------------
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
        anchors.fill: parent
        z: -1
        visible: root.stacked
        opacity: Style.cardBackground.a
        layer.enabled: root.stacked
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
            x: card.x
            y: card.y
            width: card.width
            height: card.height
            radius: card.radius
            antialiasing: true
            color: {
                const base = Style.opaque(Style.cardBackground)
                return cardHover.pressed ? Style.lift(base, 1.28) : base
            }
            Behavior on color { ColorAnimation { duration: 90 } }
        }

        Repeater {
            id: sheets
            model: root.stackDepth
            delegate: Rectangle {
                id: sheetRect
                required property int index

                z: -1 - index
                anchors.horizontalCenter: parent.horizontalCenter
                width: card.width - (index + 1) * Style.stackInset * 2
                y: Style.stackDrop(index)
                height: card.height
                radius: Style.stackRadius
                color: root.stacked ? Style.opaque(Style.cardStackEdge) : Style.cardStackEdge
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

    GlassPanel {
        id: card
        width: parent.width
        radius: Style.cardRadius
        /* Opaque while stacked: the root layer carries the alpha for the whole
           stack, so anything inside it that is translucent in its own right
           would be faded twice. */
        /* Lifts under the press. The whole card is a button — clicking the body
           runs the sender's default action — and it had no more acknowledgement
           than the action buttons did, so opening a chat from a notification
           looked like nothing until the other application got round to
           answering.

           A tint rather than a scale, deliberately. Compositor blur cannot be
           animated and the region is a fixed rectangle, so a card that changes
           size leaves its blurred patch behind; a colour change moves nothing
           and needs no region update. */
        /* Grouped, the fill is drawn by stackSurface beneath; this panel
           keeps only its edge and content. */
        surfaceColor: {
            if (root.stacked)
                return "transparent"
            const base = Style.cardBackground
            return cardHover.pressed ? Style.lift(base, 1.28) : base
        }
        Behavior on surfaceColor { ColorAnimation { duration: 90 } }
        /* Cards are glass too. At the default 0.975 alpha the backdrop effect
           is imperceptible, but a theme that lowers card.background needs the
           blur behind it or the card is merely see-through. */
        glass: true
        /* Only when this card is on its own. With sheets beneath it the whole
           stack casts one shadow, from the root — see below. A per-card shadow
           falls straight onto the sheets, and since they sit 9 and 18px below
           it, the nearest one lands in the darkest part of it and the next in
           a lighter part. That gradient across two sheets painted the same
           colour is what reads as "the first one is black and the second is
           grey", and it only shows over a bright backdrop, where a 55% black
           overlay has something to darken. */
        shadow: root.stackDepth === 0
        /* Every card in the stack keeps its own rounded corners. Squaring off
           the ones that meet something below closed the small wedge of
           backdrop between them, but at the price of a card with square
           bottom corners sitting above a sheet with square ones sitting above
           a sheet with round ones — three different shapes down one edge. The
           wedge is what a stack of rounded cards actually looks like; the
           mismatched shapes were something else entirely. */
        flatBottom: false
        implicitHeight: body.implicitHeight + Style.notifyPadTop + Style.notifyPadBottom

        Component.onCompleted: refreshBlur()
        onWidthChanged: refreshBlur()

        /* Swipe to dismiss, with a pointer or a finger.

           A DragHandler rather than MouseArea's own drag property: handlers
           see touch points natively instead of through synthesised mouse
           events, so the touchscreen works without a second code path, and a
           handler can take the exclusive grab away from the MouseArea below
           once the drag threshold is passed. That is what stops a swipe from
           also counting as a click and running the notification's default
           action on the way past. */
        /* macOS dismisses on a two-finger trackpad swipe with no click at
           all, which arrives here as horizontal wheel events. A trackpad
           brackets them with ScrollBegin/ScrollEnd; a plain mouse wheel does
           not, so that case is ended by a short timer instead. */
        WheelHandler {
            id: flick
            orientation: Qt.Horizontal
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            enabled: !replyInput.activeFocus && !swipe.active
            property bool gesturing: false

            onWheel: (event) => {
                const dx = event.pixelDelta.x !== 0 ? event.pixelDelta.x
                                                    : event.angleDelta.x / 8
                /* Qt::ScrollEnd is 3; this compared against 7, which no phase
                   ever equals, so the branch below was dead code. Nothing
                   ended a gesture but the fallback timer, and a zero-delta
                   event returned above without re-arming it. */
                const ending = event.phase === Qt.ScrollEnd

                /* The momentum tail arrives after the fingers are already up.
                   Once the gesture has ended it must not begin a new one, or
                   the card is dragged back out just after it has sprung home.
                   Not the cause of the blur-region halo — that was the
                   mapToItem lag above — but the same dead constant left this
                   unguarded, so it is fixed while the branch is live. */
                if (event.phase === Qt.ScrollMomentum && !gesturing)
                    return

                if (dx === 0 && !ending) {
                    /* Still inside the gesture. Keep the fallback armed
                       rather than letting it expire mid-flight. */
                    if (gesturing)
                        idle.restart()
                    return
                }
                if (!gesturing) {
                    gesturing = true
                    root.hoverChanged(true)
                }
                root.swipeRaw += dx
                if (ending) {
                    gesturing = false
                    root.releaseVelocity = 0
                    root.endSwipe()
                } else {
                    idle.restart()
                }
            }

        }

        /* Sibling of the handler, not a child of it: a pointer handler is not
           an Item and has no default property to put one in. Ends a gesture
           that never sent ScrollEnd — a mouse wheel, or a trackpad driver
           that does not phase its events. */
        Timer {
            id: idle
            interval: 120
            onTriggered: if (flick.gesturing) {
                flick.gesturing = false
                root.releaseVelocity = 0
                root.endSwipe()
            }
        }

        DragHandler {
            id: swipe
            /* Nothing for the handler to move. The card follows through
               swipeRaw instead, damped, because it cannot travel as far as
               the finger does — and because it is laid out by a ColumnLayout,
               which would overwrite a directly assigned x on the next pass. */
            target: null
            yAxis.enabled: false
            /* Once a reply is being typed, horizontal dragging belongs to the
               text field, for selecting what was typed. */
            enabled: !replyInput.activeFocus

            /* Followed imperatively rather than through a Binding with
               `when: active`. On release the binding's condition and this
               handler both fire, in an order QML does not define, so a
               spring-back assigned here could be overwritten by a binding
               that had not been torn down yet. Assigning leaves nothing to
               order. */
            onActiveTranslationChanged: {
                if (swipe.active)
                    root.swipeRaw = swipe.activeTranslation.x
            }

            onActiveChanged: {
                if (active) {
                    /* A finger produces no hover, so without this a slow
                       swipe could have the card expire underneath it. */
                    root.hoverChanged(true)
                    return
                }
                root.releaseVelocity = swipe.centroid.velocity.x
                root.endSwipe()
            }
        }

        MouseArea {
            id: cardHover
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            /* Pause the dwell timer while the pointer is over the card, and
               while the menu is open: reaching a row can take the pointer
               off the card, and a card that expired mid-menu would take the
               menu with it. */
            onContainsMouseChanged: root.hoverChanged(containsMouse || cardMenu.open)
            onClicked: (mouse) => {
                /* Right-click opens the card's own menu. Snooze and settings
                   used to sit on the card permanently, which is a lot of
                   furniture for two things wanted rarely. */
                if (mouse.button === Qt.RightButton) {
                    cardMenu.openAt(mouse.x, mouse.y)
                    return
                }
                /* A click anywhere else puts the menu away rather than
                   acting, the way dismissing a menu normally works. */
                if (cardMenu.open) {
                    cardMenu.close()
                    return
                }
                /* Clicking the body means the sender's "default" action when
                   it published one — that is what makes a chat notification
                   open the conversation. Middle-click always just dismisses,
                   and so does left-click when there is no default action. */
                if (mouse.button === Qt.LeftButton && root.entry.hasDefaultAction)
                    root.activated()
                else
                    root.dismissed()
            }
        }

        /* macOS layout: the app icon is a sibling of the text column rather
           than a member of the identity row, so it spans every line the card
           shows instead of sitting on the first one. Everything below the
           icon is the upstream content column, moved one level into textCol. */
        RowLayout {
            id: body
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Style.notifyPadding
            anchors.topMargin: Style.notifyPadTop
            anchors.rightMargin: Style.notifyPadRight
            spacing: Style.notifyIconGap

            /* The squircle container is for *symbolic* glyphs, which need
               something to sit in. An application icon is already a designed
               shape and gets no container; only our own monochrome glyphs do. */
            Rectangle {
                id: iconHolder
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: Style.notifyIconBox
                Layout.preferredHeight: Style.notifyIconBox
                visible: root.entry.iconSource !== ""
                         && Appearance.notifyHeader.includes("icon")
                radius: width * Style.chipRadiusRatio
                color: appIcon.isSymbolic ? Style.chipIdle : "transparent"

                Image {
                    id: appIcon
                    anchors.centerIn: parent
                    width: isSymbolic ? Style.notifyIconSize
                                      : Style.notifyAppIcon
                    height: width
                    source: root.entry.iconSource
                    /* Rasterised at the drawn size, not the box token, so an
                       SVG theme icon is never rendered small and scaled up. */
                    sourceSize: Qt.size(width * Screen.devicePixelRatio,
                                        width * Screen.devicePixelRatio)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    mipmap: true

                    readonly property bool isSymbolic:
                        Icons.isCustom(root.entry.iconSource)
                        || String(root.entry.iconSource).indexOf("symbolic") !== -1
                }
            }

            ColumnLayout {
                id: textCol
                Layout.fillWidth: true
                /* Apple's body line starts 1.5pt below a 16pt title line. */
                spacing: Style.notifyLinePx > 0 ? 2 : 3

                // ---- identity ------------------------------------
                /* Off by default in this layout: macOS says who sent it with
                   the icon and gives the bold line to the summary. Still
                   honoured when NotifyHeader asks for it. */
                Text {
                    Layout.fillWidth: true
                    visible: Appearance.notifyHeader.includes("appname")
                    text: root.entry.appName
                    /* macOS runs three lines — app, then sender or subject,
                       then the message — and makes the first one bold in
                       ordinary case. It was uppercase and letter-spaced,
                       which reads as a label above a card rather than as the
                       card's own first line. */
                    color: root.entry.urgency === 2 ? Style.critical
                                                    : Style.foreground
                    font.family: Style.fontFamily
                    font.pointSize: Style.fontSize
                    font.bold: true
                    elide: Text.ElideRight
                }

                /* First line: the summary, then the time — which the controls
                   replace while the pointer is on the card, the way a macOS
                   banner swaps its timestamp for Options on hover. */
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 7

                    Text {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: root.entry.summary
                        /* Critical urgency is marked on whichever line comes
                           first, so it is never on a line that is hidden. */
                        color: root.entry.urgency === 2
                               && !Appearance.notifyHeader.includes("appname")
                               ? Style.critical : Style.foreground
                        font.family: Style.fontFamily
                        font.pointSize: Style.notifyTextPx > 0 ? Style.notifyTextPx * 0.75
                                                               : Style.fontSize
                        lineHeightMode: Style.notifyLinePx > 0 ? Text.FixedHeight : Text.ProportionalHeight
                        lineHeight: Style.notifyLinePx > 0 ? Style.notifyLinePx : 1.0
                        /* Bold belongs to the first line. With the app name
                           above it this one is the middle tier and sets in
                           regular, the way a mail subject does under its
                           sender. */
                        font.bold: !Appearance.notifyHeader.includes("appname")
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }

                    /* Snooze and settings live in the card's menu now, so
                       nothing swaps in this cell and the time simply stays
                       put. What this replaced reflowed the row under the
                       pointer on hover. */
                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        visible: Appearance.notifyHeader.includes("time")
                        text: root.entry.when
                        color: Style.notifyTime
                        font.family: Style.fontFamily
                        font.pointSize: Style.notifyTimePx > 0 ? Style.notifyTimePx * 0.75
                                                               : Style.fontSize - 2
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: root.entry.body
                    /* A point below the title. macOS sets both at the same
                       size and separates them by weight, but that wraps a
                       chat message to three lines where two will do, and a
                       card that big stops being glanceable. */
                    color: Style.notifyBody
                    font.family: Style.fontFamily
                    font.pointSize: Style.notifyTextPx > 0 ? Style.notifyTextPx * 0.75
                                                           : Style.fontSize - 1
                    lineHeightMode: Style.notifyLinePx > 0 ? Text.FixedHeight : Text.ProportionalHeight
                    lineHeight: Style.notifyLinePx > 0 ? Style.notifyLinePx : 1.0
                    wrapMode: Text.WordWrap
                    maximumLineCount: Appearance.bodyLines
                    elide: Text.ElideRight
                    textFormat: Text.StyledText
                }

                /* A pill, matching the queue-overflow row exactly. "More exists
                   beyond what is shown" was previously two different shapes
                   depending on which mechanism produced it. */
                Rectangle {
                    Layout.topMargin: 6
                    visible: root.entry.groupCount > 1
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
                        /* "Show", because this one is a button and pressing it
                           opens them. The queue's pill in NotificationStack.qml
                           said the same words for something else entirely —
                           notifications waiting behind the display limit, which
                           you cannot act on — so the two are worded apart. */
                        text: "Show " + (root.entry.groupCount - 1) + " more"
                        color: Style.foreground
                        font.family: Style.fontFamily
                        font.pointSize: Style.fontSize - 2
                    }

                    MouseArea {
                        id: moreArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.moreRequested()
                    }
                }

                /* "value" hint rendered as a bar — what a file transfer or an
                   app-driven volume change actually wants to show. */
                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    Layout.preferredHeight: 5
                    visible: root.entry.progress >= 0
                    radius: height / 2
                    color: Style.trackColor

                    Rectangle {
                        height: parent.height
                        radius: parent.radius
                        width: parent.width * Math.max(0, Math.min(1, root.entry.progress / 100))
                        color: Style.accent
                        Behavior on width { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                    }
                }

                // ---- inline reply (KDE extension, sender-driven) -------------
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    spacing: 6
                    visible: root.entry.inlineReply === true

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
                            text: root.entry.replyPlaceholder
                            color: Style.foregroundDim
                            font.family: Style.fontFamily
                            font.pointSize: Style.fontSize - 1
                        }

                        TextInput {
                            id: replyInput
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            verticalAlignment: Text.AlignVCenter
                            color: Style.foreground
                            font.family: Style.fontFamily
                            font.pointSize: Style.fontSize - 1
                            selectByMouse: true
                            clip: true
                            onAccepted: root.replySent(text)
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
                            text: root.entry.replySubmitText
                            color: Style.foreground
                            font.family: Style.fontFamily
                            font.pointSize: Style.fontSize - 1
                        }

                        MouseArea {
                            id: sendHover
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: if (replyInput.text !== "") root.replySent(replyInput.text)
                        }
                    }
                }

                // ---- segmented actions --------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    spacing: 1
                    visible: root.entry.actions.length > 0

                    Repeater {
                        model: root.entry.actions
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
                            topRightRadius: index === root.entry.actions.length - 1 ? 9 : 0
                            bottomRightRadius: index === root.entry.actions.length - 1 ? 9 : 0

                            Text {
                                anchors.centerIn: parent
                                text: modelData.label
                                color: Style.foreground
                                font.family: Style.fontFamily
                                font.pointSize: Style.fontSize - 1
                            }

                            MouseArea {
                                id: actHover
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: root.actionTriggered(modelData.key)
                            }
                        }
                    }
                }
            }
        }

        /* Drawn inside the card rather than as a Qt popup. A popup is its
           own window, and this is a layer-shell surface with room for the
           card and its shadow and nothing else, so a real menu would be
           placed by the compositor somewhere we do not control. Two rows fit
           within the card comfortably. */
        Item {
            id: cardMenu
            anchors.fill: parent
            z: 5
            visible: open
            property bool open: false
            property real atX: 0
            property real atY: 0

            function openAt(x, y) {
                atX = x
                atY = y
                open = true
                root.hoverChanged(true)
            }
            function close() {
                open = false
                root.hoverChanged(cardHover.containsMouse)
            }

            /* Put away if the card starts leaving under it. */
            Connections {
                target: root
                function onLeavingChanged() { if (root.leaving) cardMenu.open = false }
            }

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
                            { label: "Snooze", key: "snooze", on: root.entry.stackTag === "" },
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
                                font.pointSize: Style.fontSize - 1
                            }

                            MouseArea {
                                id: rowHover
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    cardMenu.close()
                                    if (modelData.key === "snooze")
                                        root.snoozeRequested()
                                    else
                                        root.settingsRequested()
                                }
                            }
                        }
                    }
                }
            }
        }

        /* macOS puts the dismiss control on the top-left corner, overlapping
           the card edge, and only once the pointer has arrived. There is room
           to overlap because the window is shadowPad larger than the card on
           every side. Hiding it strands nothing: clicking the card, middle-
           clicking and swiping all still dismiss. */
        CardButton {
            icon: "close"
            visible: Appearance.notifyHeader.includes("close")
                     && cardHover.containsMouse
            anchors.horizontalCenter: parent.left
            anchors.verticalCenter: parent.top
            z: 3
            onActivated: root.dismissed()
        }
    }
}
