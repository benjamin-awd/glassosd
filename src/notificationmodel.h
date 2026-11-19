/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    The visible notification stack, plus the waiting queue behind it.

    Behaviour is modelled on dunst's queues.c (read as raw source, not a
    summary), because dunst's popup-limiting and replace-by-tag are the reason
    it was chosen over swaync in the first place.
*/
#pragma once

#include "notification.h"

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QList>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTimer>

#include <functional>

class NotificationModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /* Number queued behind the visible ones — the "+N more" indicator. */
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY hiddenCountChanged)
    /* Cards on screen that are not already on their way out. */
    Q_PROPERTY(int liveCount READ liveCount NOTIFY liveCountChanged)
    Q_PROPERTY(bool doNotDisturb READ doNotDisturb WRITE setDoNotDisturb NOTIFY doNotDisturbChanged)
    /* Read by the card to put away what only makes sense unlocked: its menu
       (snooze, settings) and "Show N more", both of which open windows the
       lock screen will not draw. */
    Q_PROPERTY(bool screenLocked READ screenLocked NOTIFY screenLockedChanged)
    Q_PROPERTY(int exitMs READ exitMs WRITE setExitMs)
    /* Where a newly shown card goes: row 0 (true) or the end (false). The
       stack sets it so the newest card is always nearest the anchored corner,
       as macOS stacks its banners: top-anchored stacks put it first. */
    Q_PROPERTY(bool newestFirst READ newestFirst WRITE setNewestFirst)
    /* How long "Remind me" defers a notification, for the Options label.
       SnoozeStore reads the same SnoozeMinutes key. */
    Q_PROPERTY(int snoozeMinutes READ snoozeMinutes NOTIFY snoozeMinutesChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        ClosingRole,
        StackTagRole,
        AppNameRole,
        SummaryRole,
        BodyRole,
        IconSourceRole,
        UrgencyRole,
        ActionsRole,
        HasDefaultActionRole,
        GroupCountRole,
        GroupKeyRole,
        WhenRole,
        InlineReplyRole,
        ReplyPlaceholderRole,
        ReplySubmitTextRole,
        ProgressRole,
        BadgeSourceRole,
        RealActionsRole,   // the actions with their real labels, for data() itself
        MembersRole,       // a group's other live notifications, as maps, newest first
        PersistentRole,    // stays until acted on (critical, or timeout 0): an alert, not a banner
    };
    Q_ENUM(Roles)

    explicit NotificationModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int hiddenCount() const { return m_waiting.size(); }
    int liveCount() const;   // displayed rows not already closing

    /* For the image://notifyimage provider — inline pixels live on the
       notification, not on disk. */
    ImageData imageFor(uint id) const;
    bool doNotDisturb() const { return m_dnd; }
    void setDoNotDisturb(bool dnd);

    /* Applications asking for quiet, via the notification server's Inhibit
       method. Kept apart from Do Not Disturb rather than folded into it: one
       is the user's choice and the other is an application's request, and
       turning DND off should not silently cancel a screen recording's
       inhibition. Popups are suppressed while either is on. */
    bool inhibited() const { return m_inhibited; }
    void setInhibited(bool inhibited);
    /* Something is holding the screen awake — a video, a game, a
       presentation. A third reason, kept apart from the other two so that
       turning any one of them off does not cancel the others. */
    bool busyQuiet() const { return m_busyQuiet; }
    void setBusyQuiet(bool on);
    bool quiet() const { return m_dnd || m_inhibited || m_busyQuiet; }

    /* Clamped at 0 (dunst's "unlimited"). A negative limit is an easy typo
       in a config file and would make effectiveLimit() negative, so update()
       would promote nothing and every notification would queue forever with
       no popup and no error. */
    /* Per-urgency dwell time in ms. 0 means never expire, which is what the
       spec mandates for critical. Apps that send their own expire_timeout
       still win; these are the fallback when they send -1. */
    void setTimeouts(int low, int normal, int critical)
    {
        m_timeoutLow = qMax(0, low);
        m_timeoutNormal = qMax(0, normal);
        m_timeoutCritical = qMax(0, critical);
    }

    void setLimit(int limit) { m_limit = qMax(0, limit); update(); }
    void setIndicateHidden(bool on) { m_indicateHidden = on; update(); }
    /* dunst's idle_threshold: while the session has been idle longer than
       this, nothing is allowed to expire. Stepping away for coffee should not
       mean returning to an empty screen. 0 disables it. */
    void setIdleThresholdMs(int ms);

    void setCoalesce(int threshold, int windowMs)
    {
        m_coalesceThreshold = qMax(0, threshold);   // 0 disables coalescing
        m_coalesceWindowMs = qMax(0, windowMs);
    }

    /* Returns the id actually used (may be the replaced one). */
    uint insert(Notification n);
    /* Closes this one notification. A group's top card hands the group to
       its newest member instead of taking the group with it. */
    bool closeOne(uint id, uint reason);
    bool closeId(uint id, uint reason);

    /* How long a card is kept on screen after it is closed, so it can animate
       away. Set from QML so the theme's motion.out stays the one place the
       duration lives; 0 removes it immediately, as before. */
    int exitMs() const { return m_exitMs; }
    void setExitMs(int ms) { m_exitMs = qBound(0, ms, 2000); }

    /* Test seams. Every dwell, hover and coalescing decision reads the time
       through now(), so a test can replace the clock and step it by hand,
       then run the expiry check itself instead of sleeping through the 250ms
       tick. The daemon never calls these; the default clock is the wall clock. */
    bool newestFirst() const { return m_newestFirst; }
    void setNewestFirst(bool on);

    void setClockForTesting(std::function<QDateTime()> clock) { m_clock = std::move(clock); }
    void expireForTesting() { expireTick(); }
    bool tickActiveForTesting() const { return m_tick.isActive(); }
    bool ageTickActiveForTesting() const { return m_ageTick.isActive(); }

    /* Put this one away and let it come back later. The notification itself
       is handed out rather than kept: persistence belongs to SnoozeStore, and
       a woken notification comes back through insert() like any other so it
       meets the rules, coalescing and the queue on the way in. */
    Q_INVOKABLE void snooze(uint id);
    Q_INVOKABLE void dismiss(uint id);                       // user clicked away
    /* Options' "Mute for 1 hour" / "Mute for today" (minutes < 0): this
       card goes, and the app's popups are held back until then, as Do Not
       Disturb holds everyone's: still recorded in history, still told they
       closed. In memory only; a restart ends it, which errs towards being
       told. */
    Q_INVOKABLE void mute(uint id, int minutes);
    bool isMuted(const Notification &n) const;
    int snoozeMinutes() const { return m_snoozeMinutes; }
    void setSnoozeMinutes(int m)
    {
        m = qMax(1, m);
        if (m != m_snoozeMinutes) {
            m_snoozeMinutes = m;
            Q_EMIT snoozeMinutesChanged();
        }
    }
    /* Clear every popup on screen and everything queued behind them.
       A notification with expire_timeout 0, or critical urgency, is required
       by the spec to stay until acted on — correct, but it means a misbehaving
       app can pin a card indefinitely with no way out but a restart. */
    Q_INVOKABLE int dismissAll();
    /* Take the popups off screen because the centre is now showing the same
       notifications properly. Closed as Expired rather than Dismissed: the
       user opened a window, they did not act on any particular notification,
       and telling every sender otherwise would be a lie about intent. */
    int hideForCentre();
    /* Hovering pauses the dwell timer: a notification should not expire out
       from under someone who is visibly reading it. Off by request for people
       who would rather popups clear on schedule regardless. */
    Q_INVOKABLE void setHovered(uint id, bool hovered);
    bool isHovered(uint id) const { return m_hovered.contains(id); }
    void setHoverPause(bool on);
    bool hoverPause() const { return m_hoverPause; }
    Q_INVOKABLE void invokeAction(uint id, const QString &key);

    /* Lock screen privacy. The watcher owns the state; the model only needs
       to know, because it is what decides whether a row is shown and what its
       summary and body read as. */
    void setScreenLocked(bool locked);
    bool screenLocked() const { return m_screenLocked; }
    /* The screen is being cast. Withholds content exactly as the lock screen
       does, but nothing else: the user is at the machine, so actions, replies
       and timeouts behave as normal. */
    void setScreenSharing(bool sharing);
    bool screenSharing() const { return m_screenSharing; }
    /* What to do with notifications whose sender expressed no preference. */
    void setLockPrivacyDefault(Notification::LockPrivacy policy);
    Notification::LockPrivacy lockPrivacyDefault() const { return m_lockDefault; }
    /* True while the notification is still displayed or queued, which is the
       only window in which its sender is still listening for an action. */
    bool isLive(uint id) const;
    /* Live and offering a "default" action, i.e. a click on it means
       something to its sender. */
    Q_INVOKABLE bool hasDefaultAction(uint id) const;
    Q_INVOKABLE void sendReply(uint id, const QString &text);

    /* Activating a notification has to hand the sender an XDG activation
       token, otherwise focus-stealing prevention stops it raising its window
       and clicking the notification appears to do nothing. */
    Q_INVOKABLE void activate(QQuickWindow *window, uint id, const QString &key);
    /* A click on a card or a group member: its default action if it has
       one, otherwise launch its app (launchRequested) and dismiss it. */
    Q_INVOKABLE void open(QQuickWindow *window, uint id);

Q_SIGNALS:
    void hiddenCountChanged();
    void snoozeMinutesChanged();
    void liveCountChanged();
    void doNotDisturbChanged();
    void screenLockedChanged();
    void inhibitedChanged();
    void notificationClosed(uint id, uint reason);
    void actionInvoked(uint id, const QString &key);
    void replied(uint id, const QString &text);
    void activationToken(uint id, const QString &token);
    /* Wired to SnoozeStore in main.cpp. */
    void snoozeRequested(const Notification &n);
    /* Wired to SoundPlayer in main.cpp. Emitted only for notifications that
       will actually be seen, so a muted app, Do Not Disturb or an inhibition
       silences the sound along with the popup — a notification nobody is shown
       should not announce itself. */
    void soundWanted(const QString &name);
    /* Wired in main.cpp to the same launcher a centre click uses. */
    void launchRequested(const QString &desktopEntry, const QString &activationToken);

private:
    void update();
    int closeEverything(uint reason);   // dismissAll() and hideForCentre()
    /* Bumped per arrival or replacement so the image URL changes with the
       pixels; QML caches image://notifyimage/<id> by URL otherwise. */
    uint m_imageRev = 0;
    void expireTick();
    /* The tick runs only while it has work: a card that can expire, or a
       burst still inside the coalescing window. It used to run from startup
       for the life of the session, four wakeups a second with nothing on
       screen. Called wherever either can change: update() (every arrival,
       close and promotion ends there) and the tick itself. */
    void syncTick();
    void finishClose(uint id);
    QDateTime now() const { return m_clock(); }
    /* Row of the newest card that is not closing, or -1. */
    int newestLiveRow() const;
    int indexOfDisplayed(uint id) const;

    /* Precedence, in dunst's order: an explicit replaces_id wins, then a stack
       tag, then plain insertion. */
    bool replaceById(const Notification &n);
    bool replaceByTag(const Notification &n);

    /* Per-app burst grouping. This is the capability neither dunst nor swaync
       has, and the actual fix for wake-spam: notification_limit only converts
       a burst into a drip, and a stack tag hides everything but the newest
       without saying how many there were. */
    bool coalesce(const Notification &n);

    int effectiveLimit() const;
    int defaultTimeoutFor(Urgency u) const;

    QList<Notification> m_displayed;
    QList<Notification> m_waiting;

    /* A group's other notifications, keyed by the id of the one on show
       (displayed or queued), newest first.

       They stay live, as a macOS stack's do: coalescing used to tell each
       sender its older notifications had closed, so an open group's older
       cards could only launch the app, never open their own message or run
       their own action. Kept here rather than inside Notification, which
       would have to hold a list of itself. */
    QHash<uint, QList<Notification>> m_members;
    QHash<QString, QDateTime> m_mutedUntil;   // by group key
    int m_snoozeMinutes = 10;
    /* More than this and the oldest close as before: a 300-message wake-up
       burst does not keep 300 senders waiting. */
    static constexpr int kMaxMembers = 50;
    QVariant roleFor(const Notification &n, int role) const;
    /* Any live notification by id: a row, a queued one, or a member. */
    const Notification *find(uint id) const;
    /* The representative whose group holds `id`, or 0. */
    uint ownerOf(uint id) const;
    /* Close one member: the sender is told, the group shrinks. */
    bool closeMember(uint id, uint reason);
    /* Tell a row's view its group changed (count and members). */
    void groupChanged(uint ownerId);
    /* Take a row off the screen without telling its sender, for a card
       whose notification lives on as a member of another. */
    void retireRow(uint id);
    /* A group follows its top card to a new id. */
    void moveMembers(uint from, uint to);

    int m_limit = 4;
    int m_timeoutLow = 4000;
    int m_timeoutNormal = 6000;
    int m_timeoutCritical = 0;
    bool m_indicateHidden = true;
    bool m_dnd = false;
    bool m_screenLocked = false;
    bool m_screenSharing = false;
    /* Somebody other than the user may be looking: locked, or sharing. */
    bool privacyActive() const { return m_screenLocked || m_screenSharing; }
    /* Close what asked to be hidden, now that privacy has come on. */
    void dropHiddenForPrivacy();
    /* HideContent, as macOS does by default: the lock screen says something
       arrived and from which app, and the words wait for whoever unlocks. */
    Notification::LockPrivacy m_lockDefault = Notification::LockPrivacy::HideContent;
    Notification::LockPrivacy effectiveLockPrivacy(const Notification &n) const;
    bool m_inhibited = false;
    bool m_busyQuiet = false;
    int m_lastHiddenCount = 0;

    int m_idleThresholdMs = 0;
    int m_idleTimeoutId = -1;
    bool m_userIdle = false;
    int m_exitMs = 0;
    bool m_newestFirst = false;
    int m_lastLiveCount = 0;   // set from QML; 0 keeps the old instant removal
    int m_coalesceThreshold = 3;
    int m_coalesceWindowMs = 20000;
    QHash<QString, QList<QDateTime>> m_recentByGroup;
    /* When each distinct piece of content was last shown, for rules carrying
       repeat_window. Keyed on app + summary + body, so "9% remaining" and
       "6% remaining" are different content and each gets its own popup. */
    QHash<QString, QDateTime> m_lastShownByContent;
    QTimer m_tick;
    /* Re-announces WhenRole so "now" becomes "1m ago" on a card that stays
       up (critical, timeout 0, or held by the pointer). Runs only while
       something is on screen. */
    QTimer m_ageTick;
    std::function<QDateTime()> m_clock = [] { return QDateTime::currentDateTimeUtc(); };
    QSet<uint> m_hovered;
    bool m_hoverPause = true;
};
