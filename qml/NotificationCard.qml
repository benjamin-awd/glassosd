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

    /* Sheets actually drawn: none while the group is open in place, since
       the rows it would stand for are then on the card itself. */
    readonly property int shownDepth: groupProgress > 0 ? 0 : stackDepth
    readonly property bool stacked: shownDepth > 0

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
    /* A swiped card is carried off the edge by the swipe itself, so it
       stays opaque all the way out like any object thrown off a surface. */
    opacity: (Style.slideTravel > 0 ? (swipingOut ? 1.0 : Math.min(1.0, reveal * 4.0)) : reveal)
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
       is. A card arriving in a window that is already up starts at once.

       Always one event-loop turn late, never inline: a Behavior does not
       animate until its object is finalized, which is after onCompleted, and
       both triggers below can fire before that. windowReady flips while the
       card is being attached to an already-exposed window, inside creation.
       Called inline, reveal jumped straight to 1, so every card arriving
       while another was on screen popped in at full size (3 frames instead
       of ~24 in a QSG_RENDER_TIMING capture). Only the first card of a stack
       animated, because its window was not exposed yet. */
    function startReveal() {
        Qt.callLater(root.revealNow)
    }
    function revealNow() {
        if (reveal === 0.0 && !leaving) {
            reveal = 1.0
            slot = 1.0
        }
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
        slotAnim.duration = Style.animOut
        slotAnim.easing.type = Easing.InOutCubic
        reveal = 0.0
        slot = 0.0
    }

    /* The card's share of the column: 0 before it arrives, 1 while it is up,
       0 again once it has gone. Cards below a leaving one used to hold still
       until its row was removed and then jump up a card's height in one
       frame; collapsing the slot alongside the exit lets them glide up into
       the gap while it leaves, which is how a macOS banner stack closes
       ranks. The stack turns it into a `lift` on every card below; this
       card's own layout is never touched.

       Arrival is the same thing run forwards. The newest card is inserted at
       the top of a top-anchored stack (NotificationModel.newestFirst), as
       macOS does, and the layout makes room for it at once; starting at 0
       holds the cards below where they were, and growing to 1 on the arrival
       curve lets them glide down as this one slides in. revealNow() starts
       it, together with the slide; the exit swaps in the exit timing. */
    property real slot: 0.0
    /* How far the cards above this one have collapsed, set by the stack. */
    property real lift: 0.0
    Behavior on slot {
        NumberAnimation {
            id: slotAnim
            duration: Style.animIn
            easing.type: Style.curveIn.length ? Easing.BezierSpline : Easing.OutQuint
            easing.bezierCurve: Style.curveIn
        }
    }

    /* Rises into place, and sinks as it goes. A transform rather than a change
       of y, so the layout does not reflow every frame and the cards below stay
       put while this one arrives. */
    transform: Translate {
        /* Arrives from the right, as the macOS banner does, and rises only
           when it cannot slide. The two together read as a diagonal. */
        /* Not once swiped: the swipe carries the card past the edge on its
           own, and adding the exit slide on top doubled the distance it had
           to cover in the same time. */
        x: root.swipeX + (root.swipingOut ? 0 : (1.0 - root.reveal) * Style.slideTravel)
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
        Surface.clearPanelRegion(w, closeButton)
        for (let i = 0; i < stackSurface.sheets.count; ++i) {
            const s = stackSurface.sheets.itemAt(i)
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
                    for (let i = 0; i < stackSurface.sheets.count; ++i) {
                        const s = stackSurface.sheets.itemAt(i)
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
        for (let i = 0; i < stackSurface.sheets.count; ++i) {
            const s = stackSurface.sheets.itemAt(i)
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
            duration: root.swipingOut ? root.throwMs : Style.swipeSpring
            /* The spring back is a plain OutCubic. The throw continues at
               the speed the finger let go with: see endSwipe(). */
            easing.type: root.swipingOut ? Easing.BezierSpline : Easing.OutCubic
            easing.bezierCurve: root.throwCurve
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
            root.hoverChanged(root.pointerOn)
            return
        }
        /* Dismiss now and let the exit fade run alongside the last of the
           travel: the model keeps the row for Style.animOut either way, so
           waiting for the slide to finish first would only double the wait. */
        /* Far enough to clear the screen edge, not merely the card's own
           width: the surface now reaches the edge, so the card is clipped by
           it rather than stopping short. */
        const target = Math.max(swipeRaw, root.width + Style.shadowPad * 2)
        /* Thrown at the speed it was let go. It used to fly the rest of the
           way on a fixed 250 ms OutCubic, plus the exit slide on top, whose
           first frame moves at 3 x distance / duration: released at the
           commit point of a 398px card that was ~8900 px/s against a real
           flick of 0.5-1.5k, and the card was past the edge ~40 ms later,
           a snap rather than a throw.

           The flight is a bezier whose starting slope matches the release
           velocity: with the first control point at x = 1/3 the slope is
           3 * y1, and in normalised units the velocity is v * T / distance.
           A fast flick shortens the flight so OutCubic's start still
           matches it; a slow release (or a trackpad gesture with no
           velocity) eases in from rest. It never takes longer than the
           model keeps the row (Style.animOut), so the card is off the
           screen before its delegate goes. */
        const dist = Math.max(1, target - swipeRaw)
        const v = Math.max(0, releaseVelocity) / 1000   // px per ms
        let ms = Style.animOut
        if (v > 0)
            ms = Math.max(60, Math.min(Style.animOut, 3 * dist / v))
        const y1 = Math.min(1.0, v * ms / dist / 3)
        throwCurve = [0.333, y1, 0.667, 1.0, 1.0, 1.0]
        throwMs = ms
        swipingOut = true
        swipeRaw = target
        root.dismissed()
    }
    /* The body opened in place by its chevron. The card grows; the cards
       below move with the layout, and their regions follow (Surface reads
       live geometry every frame). */
    property bool expanded: false
    readonly property int expandedLines: 20

    /* The open and close are animated: the card's content is clipped to a
       height that runs between the folded and opened text on the arrival
       curve, so the extra lines are uncovered rather than appearing at once.

       textOpen is what the Text elements read, not `expanded`. Opening sets
       both at once (the text lays out in full and the clip uncovers it);
       closing keeps the text open until the clip has shut over it, and only
       then folds it, or the lines would vanish before the card had shrunk.
       heldBodyH is -1 except while that runs, when it stands in for the
       body's own height. */
    property bool textOpen: false
    property real foldedBodyH: 0
    /* The clip runs from wherever it was to a live target, as a 0..1
       progress, rather than between two numbers fixed at the click: the
       opened text's height is only known once the layout has polished, a
       frame later, and a target read at the click was still the folded one. */
    property bool bodyMoving: false
    property real bodyFrom: 0
    property real bodyT: 0
    readonly property real bodyTo: expanded ? body.implicitHeight : foldedBodyH
    readonly property real heldBodyH: bodyMoving ? bodyFrom + (bodyTo - bodyFrom) * bodyT : -1
    function setExpanded(open) {
        if (open === expanded)
            return
        const from = bodyMoving ? heldBodyH : body.implicitHeight
        bodyAnim.stop()
        if (open) {
            if (!textOpen)
                foldedBodyH = body.implicitHeight
            expanded = true
            textOpen = true
        } else {
            expanded = false
        }
        bodyFrom = from
        bodyT = 0
        bodyMoving = true
        bodyAnim.start()
    }
    NumberAnimation {
        id: bodyAnim
        target: root
        property: "bodyT"
        from: 0
        to: 1
        duration: Style.animIn
        easing.type: Style.curveIn.length ? Easing.BezierSpline : Easing.OutQuint
        easing.bezierCurve: Style.curveIn
        onStopped: {
            if (root.bodyT < 1)
                return   // interrupted by a reversal, which has taken over
            if (!root.expanded)
                root.textOpen = false
            root.bodyMoving = false
        }
    }

    /* The group opened in place, as macOS opens a stack in Notification
       Centre: a header with the app's name, Show less and a clear button
       above the top card, and each earlier notification as a card of its own
       below it, sliding out from under the top card where the sheets were.

       The popup model only counts a group's members (coalescing closes them
       with their senders as it merges), so those cards are read from history,
       which recorded each one on arrival. Never on the lock screen: the way
       in is withheld there, and a group left open must not keep showing what
       the lock screen hides. */
    property bool groupOpen: false
    readonly property bool groupShown: groupOpen && root.entry.groupCount > 1
                                       && !NotificationModel.screenLocked
    /* Cards before the rest are left to the centre. Past five the stack
       stops being a glance; a link under them opens the whole group. */
    readonly property int groupRowsMax: 5
    /* 0 closed, 1 open, animated on the arrival curve. Everything about the
       open group is a function of it, so opening and closing are the same
       motion run in opposite directions. */
    property real groupProgress: groupShown ? 1.0 : 0.0
    Behavior on groupProgress {
        NumberAnimation {
            duration: Style.animIn
            easing.type: Style.curveIn.length ? Easing.BezierSpline : Easing.OutQuint
            easing.bezierCurve: Style.curveIn
        }
    }
    /* Read when the group opens and when history changes, not bound to
       progress: a new array rebuilds every member card, and closing has to
       keep them while they slide back in. Cleared once fully closed. */
    property var groupMembers: []
    /* The open group's own pointer: on a member card or the header. */
    property int groupHovers: 0
    function refreshMembers() {
        if (!groupShown)
            return
        groupMembers = HistoryModel.groupMembers(root.entry.groupKey, root.entry.notifId,
                                                 Math.min(root.entry.groupCount - 1, groupRowsMax))
    }
    onGroupProgressChanged: if (groupProgress === 0 && !groupShown) groupMembers = []
    Connections {
        target: HistoryModel
        function onChanged() { root.refreshMembers() }
    }
    Connections {
        target: root.entry
        ignoreUnknownSignals: true
        function onGroupCountChanged() { root.refreshMembers() }
    }
    /* An open group is being read: it stays until closed or cleared, as an
       expanded stack in Notification Centre does, rather than expiring the
       moment the pointer wanders off it. */
    onGroupShownChanged: {
        refreshMembers()
        root.hoverChanged(pointerOn || cardMenu.open || groupShown)
    }

    /* The header's height is fixed rather than read from the loaded item, so
       cardTop has something to animate towards from the first frame. */
    readonly property int groupHeaderBar: 30
    readonly property real groupHeaderH: groupHeaderBar + Style.notifySpacing
    /* Where the top card sits: pushed down by the header as it opens. */
    readonly property real cardTop: groupHeaderH * groupProgress
    /* Total height of the member cards and the gaps above them, open. */
    readonly property real membersH: {
        let h = 0
        for (let i = 0; i < memberCards.count; ++i) {
            const m = memberCards.itemAt(i)
            if (m) h += Style.notifySpacing + m.implicitHeight
        }
        if (centreLink.item)
            h += Style.notifySpacing + centreLink.item.implicitHeight
        return h
    }

    /* The committed swipe's flight, set by endSwipe(). */
    property int throwMs: Style.animOut
    property var throwCurve: [0.333, 0.0, 0.667, 1.0, 1.0, 1.0]

    signal dismissed()
    signal settingsRequested()
    signal snoozeRequested()
    signal moreRequested()
    signal memberActivated(int id)
    signal actionTriggered(string key)
    signal replySent(string text)
    signal activated()
    signal hoverChanged(bool hovered)

    /* Whether the pointer is on the card: anywhere on it, its buttons
       included, or on the close button hanging off its corner.

       It was the card's MouseArea's containsMouse, but a hover-enabled
       MouseArea on top (an action, Show N more, the close button) takes the
       hover from the one beneath. Resting the pointer on an action told the
       model it had left, the dwell timer resumed, and the card could expire
       under the pointer. A HoverHandler is passive and stays hovered over
       the card's children. */
    readonly property bool pointerOn: cardPointer.hovered || closeButton.hovered || groupHovers > 0
    /* The row can change id in place (coalescing, a stack tag). The hold
       is sent by id, so say it again for the new one. */
    readonly property var shownId: entry ? entry.notifId : 0
    onShownIdChanged: if (pointerOn || cardMenu.open || groupShown) root.hoverChanged(true)
    onPointerOnChanged: {
        root.hoverChanged(pointerOn || cardMenu.open || groupShown)
        if (!pointerOn)
            closeGrace.restart()
    }

    readonly property real closedHeight: card.implicitHeight
                    + (stackDepth > 0 ? Style.stackDrop(stackDepth - 1) : 0)
    implicitHeight: groupProgress > 0
                    ? closedHeight + (groupHeaderH + membersH + card.implicitHeight - closedHeight)
                                     * groupProgress
                    : card.implicitHeight + (shownDepth > 0 ? Style.stackDrop(shownDepth - 1) : 0)

    // ---- stacked edges, drawn behind and below --------------------------
    CardStackSurface {
        id: stackSurface
        anchors.fill: parent
        z: -1
        card: card
        depth: root.shownDepth
        pressed: cardHover.pressed
    }

    GlassPanel {
        id: card
        width: parent.width
        y: root.cardTop
        radius: Style.notifyRadius
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
        shadow: root.shownDepth === 0
        /* Every card in the stack keeps its own rounded corners. Squaring off
           the ones that meet something below closed the small wedge of
           backdrop between them, but at the price of a card with square
           bottom corners sitting above a sheet with square ones sitting above
           a sheet with round ones — three different shapes down one edge. The
           wedge is what a stack of rounded cards actually looks like; the
           mismatched shapes were something else entirely. */
        flatBottom: false
        implicitHeight: (root.heldBodyH >= 0 ? root.heldBodyH : body.implicitHeight)
                        + Style.notifyPadTop + Style.notifyPadBottom

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
            enabled: !reply.inputFocused && !swipe.active
            property bool gesturing: false
            /* Recent (time, dx) pairs, for the release velocity. A wheel
               gesture has no handler velocity to read, so it was always
               released at 0 and a quick two-finger flick had to travel the
               whole commit distance to dismiss. */
            property var samples: []
            function velocity() {
                if (samples.length < 2)
                    return 0
                const last = samples[samples.length - 1].t
                let sum = 0
                let first = last
                for (let i = samples.length - 1; i >= 0 && last - samples[i].t <= 100; --i) {
                    sum += samples[i].dx
                    first = samples[i].t
                }
                /* One event's worth of time beyond the span, so two events
                   a millisecond apart do not read as a huge speed. */
                const span = (last - first) + 8
                return sum / span * 1000
            }
            function release() {
                gesturing = false
                root.releaseVelocity = velocity()
                samples = []
                root.endSwipe()
            }

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
                    samples = []
                    root.hoverChanged(true)
                }
                root.swipeRaw += dx
                if (dx !== 0) {
                    const now = Date.now()
                    samples.push({ t: now, dx: dx })
                    while (samples.length > 0 && now - samples[0].t > 200)
                        samples.shift()
                }
                if (ending) {
                    release()
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
            onTriggered: if (flick.gesturing) flick.release()
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
            enabled: !reply.inputFocused

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

        HoverHandler {
            id: cardPointer
        }

        MouseArea {
            id: cardHover
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            /* Whether the pointer is here is root.pointerOn's job, which
               also keeps the dwell paused while the menu is open: reaching a
               row can take the pointer off the card, and a card that expired
               mid-menu would take the menu with it. */
            onClicked: (mouse) => {
                /* Right-click opens the card's own menu. Snooze and settings
                   used to sit on the card permanently, which is a lot of
                   furniture for two things wanted rarely. */
                if (mouse.button === Qt.RightButton) {
                    /* Snooze and settings open the centre, which the lock
                       screen does not draw; it would be waiting after unlock. */
                    if (!NotificationModel.screenLocked)
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
                /* A closed stack opens on a click, as macOS opens one in
                   Notification Centre: the click is asking what is under the
                   top card. Its default action is a click away once open. */
                if (mouse.button === Qt.LeftButton && root.stacked
                    && root.entry.groupCount > 1 && !NotificationModel.screenLocked) {
                    root.groupOpen = true
                    return
                }
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
            /* See setExpanded(): the clip that opens and shuts the text. */
            height: root.heldBodyH >= 0 ? root.heldBodyH : implicitHeight
            clip: root.heldBodyH >= 0
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
                    /* A picture of the message's own (a contact's avatar) is
                       drawn as a circle, the way macOS shows a sender. */
                    readonly property bool isPicture: (root.entry.badgeSource || "") !== ""
                    visible: !isPicture
                }

                /* The circular avatar: the same image through a round mask. */
                Item {
                    id: avatarMask
                    anchors.fill: appIcon
                    visible: false
                    layer.enabled: appIcon.isPicture
                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                    }
                }
                MultiEffect {
                    anchors.fill: appIcon
                    visible: appIcon.isPicture
                    source: appIcon
                    maskEnabled: true
                    maskSource: avatarMask
                    maskThresholdMin: 0.5
                    maskSpreadAtMin: 1.0
                }

                /* The sender's icon on the avatar's corner, as macOS badges a
                   contact's photo with the app it came through. */
                Image {
                    id: badge
                    visible: appIcon.isPicture
                    width: Math.round(appIcon.width * 0.5)
                    height: width
                    anchors.right: appIcon.right
                    anchors.bottom: appIcon.bottom
                    anchors.rightMargin: -Math.round(width * 0.2)
                    anchors.bottomMargin: -Math.round(width * 0.2)
                    source: root.entry.badgeSource || ""
                    sourceSize: Qt.size(width * Screen.devicePixelRatio,
                                        width * Screen.devicePixelRatio)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    mipmap: true
                }
            }

            ColumnLayout {
                id: textCol
                Layout.fillWidth: true
                /* Pinned to the top while the clip is moving, so the text
                   stays put and is uncovered; centred against the icon
                   otherwise, as before. */
                Layout.alignment: root.heldBodyH >= 0 ? Qt.AlignTop : Qt.AlignVCenter
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
                    textFormat: Text.PlainText    // app_name is not markup and is not sanitised
                    /* macOS runs three lines — app, then sender or subject,
                       then the message — and makes the first one bold in
                       ordinary case. It was uppercase and letter-spaced,
                       which reads as a label above a card rather than as the
                       card's own first line. */
                    color: root.entry.urgency === 2 ? Style.critical
                                                    : Style.foreground
                    font.family: Style.fontFamily
                    font.pointSize: Style.notifyFontSize
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
                        textFormat: Text.StyledText   // sanitised by the daemon; AutoText guessed per message
                        /* Critical urgency is marked on whichever line comes
                           first, so it is never on a line that is hidden. */
                        color: root.entry.urgency === 2
                               && !Appearance.notifyHeader.includes("appname")
                               ? Style.critical : Style.foreground
                        font.family: Style.fontFamily
                        font.pointSize: Style.notifyTextPx > 0 ? Style.notifyTextPx * 0.75
                                                               : Style.notifyFontSize
                        lineHeightMode: Style.notifyLinePx > 0 ? Text.FixedHeight : Text.ProportionalHeight
                        lineHeight: Style.notifyLinePx > 0 ? Style.notifyLinePx : 1.0
                        /* Bold belongs to the first line. With the app name
                           above it this one is the middle tier and sets in
                           regular, the way a mail subject does under its
                           sender. */
                        font.bold: !Appearance.notifyHeader.includes("appname")
                        wrapMode: Text.WordWrap
                        maximumLineCount: root.textOpen ? 6 : 2
                        elide: Text.ElideRight
                    }

                    /* The time, which an Options control replaces while the
                       pointer is on the card, as a macOS banner swaps its
                       timestamp for Options on hover. It opens the same menu
                       as a right-click (snooze, settings), which had no
                       visible way in.

                       Both sit in one cell as wide as the wider of the two,
                       and swap by opacity, so nothing reflows under the
                       pointer: what this cell held before reflowed the row
                       on hover. */
                    Item {
                        id: timeCell
                        Layout.alignment: Qt.AlignVCenter
                        readonly property bool showTime: Appearance.notifyHeader.includes("time")
                        readonly property bool offerOptions: !NotificationModel.screenLocked
                        readonly property bool optionsUp: offerOptions && (root.pointerOn || cardMenu.open)
                        visible: showTime || offerOptions
                        implicitWidth: Math.max(showTime ? timeText.implicitWidth : 0,
                                                offerOptions ? options.implicitWidth : 0)
                        implicitHeight: Math.max(timeText.implicitHeight, options.implicitHeight)

                        Text {
                            id: timeText
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            visible: timeCell.showTime
                            opacity: timeCell.optionsUp ? 0 : 1
                            Behavior on opacity { NumberAnimation { duration: 120 } }
                            text: root.entry.when
                            color: Style.notifyTime
                            font.family: Style.fontFamily
                            font.pointSize: Style.notifyTimePx > 0 ? Style.notifyTimePx * 0.75
                                                                   : Style.notifyFontSize - 2
                        }

                        Rectangle {
                            id: options
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            opacity: timeCell.optionsUp ? 1 : 0
                            visible: opacity > 0
                            Behavior on opacity { NumberAnimation { duration: 120 } }
                            implicitWidth: optionsLabel.implicitWidth + 16
                            implicitHeight: optionsLabel.implicitHeight + 4
                            width: implicitWidth
                            height: implicitHeight
                            radius: height / 2
                            color: optionsArea.pressed ? Style.controlFillActive
                                 : optionsArea.containsMouse ? Style.controlFillHover
                                                             : Style.controlFill
                            Behavior on color { ColorAnimation { duration: 100 } }

                            Text {
                                id: optionsLabel
                                anchors.centerIn: parent
                                text: "Options"
                                color: Style.foreground
                                font.family: Style.fontFamily
                                font.pointSize: Style.notifyTimePx > 0 ? Style.notifyTimePx * 0.75
                                                                       : Style.notifyFontSize - 2
                            }

                            MouseArea {
                                id: optionsArea
                                anchors.fill: parent
                                hoverEnabled: true
                                enabled: timeCell.optionsUp
                                onClicked: {
                                    if (cardMenu.open) {
                                        cardMenu.close()
                                        return
                                    }
                                    const p = options.mapToItem(cardMenu, 0, options.height + 4)
                                    cardMenu.openAt(cardMenu.width, p.y)   // clamped to the right edge
                                }
                            }
                        }
                    }
                }

                Text {
                    id: bodyText
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
                                                           : Style.notifyFontSize - 1
                    lineHeightMode: Style.notifyLinePx > 0 ? Text.FixedHeight : Text.ProportionalHeight
                    lineHeight: Style.notifyLinePx > 0 ? Style.notifyLinePx : 1.0
                    wrapMode: Text.WordWrap
                    maximumLineCount: root.textOpen ? root.expandedLines : Appearance.bodyLines
                    elide: Text.ElideRight
                    textFormat: Text.StyledText

                    /* Shown on hover when there is more than fits, as macOS
                       offers a chevron on a banner whose text is cut off.
                       Opens the text in place instead of sending you to the
                       centre or the app. Its own MouseArea, so the press does
                       not reach the card's (which would run the default
                       action or dismiss). */
                    Rectangle {
                        id: expander
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.rightMargin: -2
                        width: 20
                        height: 16
                        radius: 8
                        visible: root.pointerOn && (bodyText.truncated || root.textOpen)
                                 && !NotificationModel.screenLocked
                        color: expanderArea.containsMouse ? Style.controlFillHover : Style.opaque(Style.cardBackground)
                        border.color: Style.controlEdge
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            anchors.verticalCenterOffset: root.expanded ? 1 : -1
                            text: "\u203A"   // a single chevron, turned to point down (or up)
                            rotation: root.expanded ? -90 : 90
                            color: Style.foreground
                            font.family: Style.fontFamily
                            font.pointSize: Style.notifyFontSize
                        }

                        MouseArea {
                            id: expanderArea
                            anchors.fill: parent
                            anchors.margins: -4
                            hoverEnabled: true
                            onClicked: root.setExpanded(!root.expanded)
                        }
                    }
                }

                /* A pill, matching the queue-overflow row exactly. "More exists
                   beyond what is shown" was previously two different shapes
                   depending on which mechanism produced it. */
                Rectangle {
                    Layout.topMargin: 6
                    /* Closed groups only: once open, "Show less" lives in the
                       group's header, where macOS puts it. */
                    visible: root.entry.groupCount > 1 && !root.groupOpen
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
                        font.pointSize: Style.notifyFontSize - 2
                    }

                    MouseArea {
                        id: moreArea
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: !NotificationModel.screenLocked
                        onClicked: root.groupOpen = true
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
                CardReply {
                    id: reply
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    visible: root.entry.inlineReply === true
                    placeholder: root.entry.replyPlaceholder
                    submitText: root.entry.replySubmitText
                    onSent: text => root.replySent(text)
                }

                // ---- segmented actions --------------------------------------
                CardActions {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    visible: root.entry.actions.length > 0
                    actions: root.entry.actions
                    onTriggered: key => root.actionTriggered(key)
                }
            }
        }

        /* Snooze and settings, opened by right-click or Options. */
        CardMenu {
            id: cardMenu
            anchors.fill: parent
            z: 5
            stackTag: root.entry.stackTag
            leaving: root.leaving
            onSnoozeRequested: root.snoozeRequested()
            onSettingsRequested: root.settingsRequested()
            /* Keeps the dwell paused while open: reaching a row can take the
               pointer off the card. */
            onOpenChanged: root.hoverChanged(open || root.pointerOn || root.groupShown)
        }
    }

    // ---- the open group: header above, member cards below ---------------
    /* Loaded only while the group is open or animating: a GlassPanel
       registers its blur and input region as soon as it exists, visible or
       not, so a header kept around while closed would leave a blurred patch
       above the card. */
    Loader {
        id: groupHeader
        width: parent.width
        active: root.groupProgress > 0
        height: root.groupHeaderBar
        opacity: Math.min(1, root.groupProgress * 2.5)
        sourceComponent: Item {

            HoverHandler {
                id: headerPointer
                onHoveredChanged: root.groupHovers += hovered ? 1 : -1
            }
            Component.onDestruction: if (headerPointer.hovered) root.groupHovers -= 1

            /* The app's name, as macOS titles an open stack. Bare text, with
               the card shadow under it to hold it off whatever window is
               behind the popup. */
            Text {
                id: groupTitle
                anchors.left: parent.left
                anchors.leftMargin: 4
                anchors.right: lessButton.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: root.entry.appName
                textFormat: Text.PlainText
                color: Style.foreground
                font.family: Style.fontFamily
                font.pointSize: Style.notifyFontSize + 2
                font.bold: true
                elide: Text.ElideRight
                layer.enabled: true
                layer.effect: MultiEffect {
                    shadowEnabled: true
                    shadowColor: Style.shadowColor
                    shadowBlur: 0.6
                    shadowOpacity: 0.8
                    shadowVerticalOffset: 1
                    autoPaddingEnabled: true
                }
            }

            GlassPanel {
                id: lessButton
                anchors.right: clearButton.left
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: lessLabel.implicitWidth + 20
                implicitHeight: 24
                width: implicitWidth
                height: implicitHeight
                radius: height / 2
                glass: true
                surfaceColor: lessArea.pressed ? Style.lift(Style.cardBackground, 1.28)
                            : lessArea.containsMouse ? Style.lift(Style.cardBackground, 1.12)
                                                     : Style.cardBackground

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
                    onClicked: root.groupOpen = false
                }
            }

            /* Clears the group: the popup goes, as the X on an open stack
               clears it in Notification Centre. History keeps every entry. */
            GlassPanel {
                id: clearButton
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 24
                height: 24
                radius: 12
                glass: true
                surfaceColor: Style.cardBackground

                CardButton {
                    anchors.fill: parent
                    icon: "close"
                    color: "transparent"
                    border.width: 0
                    onActivated: root.dismissed()
                }
            }
        }
    }

    Repeater {
        id: memberCards
        /* A count, not the array: a fresh array from groupMembers() (on every
           history change, from any app, or a keystroke in the centre's
           search) made the Repeater destroy and rebuild every member card,
           shadows and all, which flickered. With a count the cards stay and
           only their entry rebinds; they are rebuilt only when the number
           of members changes. */
        model: root.groupMembers.length

        MemberCard {
            id: member
            required property int index
            readonly property var modelData: root.groupMembers[index] || ({})
            entry: modelData
            z: -2

            /* Under the top card it stands in for sheet `index` (or the last
               sheet, past the stack's depth); open, it is a card of its own
               below the one before it. */
            readonly property int sheet: Math.min(index, Math.max(root.stackDepth - 1, 0))
            readonly property bool wasSheet: index < root.stackDepth
            readonly property real closedWidth: root.width - (sheet + 1) * Style.stackInset * 2
            readonly property real closedY: Math.max(root.cardTop,
                root.cardTop + card.height + (root.stackDepth > 0 ? Style.stackDrop(sheet) : 0) - height)
            readonly property real openY: {
                let y = root.cardTop + card.height + Style.notifySpacing
                for (let j = 0; j < index; ++j) {
                    const m = memberCards.itemAt(j)
                    if (m) y += m.implicitHeight + Style.notifySpacing
                }
                return y
            }
            readonly property real p: root.groupProgress
            open: p
            width: closedWidth + (root.width - closedWidth) * p
            x: (root.width - width) / 2
            y: closedY + (openY - closedY) * p
            opacity: wasSheet ? 1 : p

            onHoveredChanged: root.groupHovers += hovered ? 1 : -1
            Component.onDestruction: if (hovered) root.groupHovers -= 1
            onActivated: root.memberActivated(modelData.id)
        }
    }

    /* The rest of the group, and its settings, in the centre: for members
       past groupRowsMax, and any that history no longer holds (cleared, or
       aged out past its cap). */
    Loader {
        id: centreLink
        active: root.groupProgress > 0
                && root.entry.groupCount - 1 > root.groupMembers.length
        x: 0
        y: {
            let y = root.cardTop + card.height
            for (let j = 0; j < memberCards.count; ++j) {
                const m = memberCards.itemAt(j)
                if (m) y += Style.notifySpacing + m.implicitHeight
            }
            return y + Style.notifySpacing
        }
        opacity: root.groupProgress
        sourceComponent: GlassPanel {
            implicitWidth: linkLabel.implicitWidth + 24
            implicitHeight: 26
            radius: height / 2
            glass: true
            surfaceColor: linkArea.containsMouse ? Style.lift(Style.cardBackground, 1.12)
                                                 : Style.cardBackground
            Text {
                id: linkLabel
                anchors.centerIn: parent
                text: (root.entry.groupCount - 1 - root.groupMembers.length)
                      + " more in Notification Centre"
                color: Style.foreground
                font.family: Style.fontFamily
                font.pointSize: Style.notifyFontSize - 2
            }
            MouseArea {
                id: linkArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.moreRequested()
            }
        }
    }

    /* macOS puts the dismiss control on the top-left corner, overlapping
       the card edge, and only once the pointer has arrived. There is room
       to overlap because the window is shadowPad larger than the card on
       every side. Hiding it strands nothing: clicking the card, middle-
       clicking and swiping all still dismiss. */
    /* Two things kept half of it out of reach. Input followed the card
       exactly, so the half hanging off the corner was not in the input
       region and a click there went to the window behind. And it was
       shown only while the card was hovered, so moving the pointer onto
       that half hid it. It now registers its own input-only region
       while shown (not blurred), and its own hover keeps it up.

       A child of the delegate, not of the card: the card carries a layer
       effect for its shadow, and Qt delivers no pointer events to a layered
       item's children outside its bounds, so the outer half could never be
       hovered or clicked whatever the input region said. */
    /* A moment's grace after the pointer leaves the card, so a quick
       move onto the overhanging half finds the button still there. */
    Timer {
        id: closeGrace
        interval: 250
    }
    CardButton {
        id: closeButton
        icon: "close"
        /* Always offered on hover, as macOS does. It used to need `close`
           in NotifyHeader, so a header trimmed to icon,time (to drop the app
           name) silently took the only visible way to dismiss with it. */
        visible: !root.leaving
                 && (root.pointerOn || closeGrace.running)
        anchors.horizontalCenter: card.left
        anchors.verticalCenter: card.top
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
    }
}
