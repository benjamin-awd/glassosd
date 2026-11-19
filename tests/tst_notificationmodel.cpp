/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    The popup queue.

    This is where the notifications a person actually sees are decided, and
    where the worst bugs have been — the ones that look like correct behaviour
    until you count. A tagged notification whose repeat reset the dwell clock
    produced a card that could never expire. A queue that compared its hidden
    count against a stale snapshot left "+2 more" on screen while four were
    waiting. Neither crashed; both were only findable by watching.

    Expiry is tested on a fake clock (FakeClock below): the test steps time by
    hand and runs the expiry check itself, so a dwell is checked to the
    millisecond and nothing sleeps. These used to wait on the real 250ms tick,
    which made them the slowest tests here (~6s of an 8s run) and only as exact
    as their safety margins. The immortal card is still the single worst bug
    this project has had; it is now pinned at exactly 2000ms, not "within 1.5s".
    Exit animations still use the model's real timer, set to 1ms.
*/
#include "notificationmodel.h"
#include "testenv.h"

#include <QSignalSpy>
#include <QTimeZone>

namespace
{
/* A clock the test moves by hand. Starts at an arbitrary fixed instant so
   nothing depends on when the suite runs. */
struct FakeClock {
    QDateTime t = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    void install(NotificationModel &m) { m.setClockForTesting([this] { return t; }); }
    /* Advance, then let the model notice, as its tick would have. */
    void advance(NotificationModel &m, int ms)
    {
        t = t.addMSecs(ms);
        m.expireForTesting();
    }
};

Notification note(const QString &app, const QString &summary = {},
                  const QString &body = {}, uint id = 0)
{
    Notification n;
    n.id = id;
    n.appName = app;
    n.summary = summary;
    n.body = body;
    return n;
}
} // namespace

class TestNotificationModel : public QObject
{
    Q_OBJECT

private:
    /* Every test wants a model that is not trying to be clever: no burst
       grouping unless it asks for it, and no expiry unless it asks for it. */
    static void plain(NotificationModel &m)
    {
        m.setCoalesce(0, 0);
        m.setTimeouts(0, 0, 0);
    }

    /* True while any card is still playing its exit. */
    static bool anyClosing(const NotificationModel &m)
    {
        for (int r = 0; r < m.rowCount(); ++r) {
            if (m.index(r).data(NotificationModel::ClosingRole).toBool()) {
                return true;
            }
        }
        return false;
    }

    static QString summaryAt(const NotificationModel &m, int row)
    {
        return m.index(row).data(NotificationModel::SummaryRole).toString();
    }

private Q_SLOTS:
    /* An update in place meets the lock screen's Hide as an arrival does:
       a card already up whose new version asks to be hidden goes, rather
       than being drawn in full. */
    void anUpdateAskingToHideIsDroppedWhileLocked()
    {
        NotificationModel m;
        plain(m);
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("hi"), QStringLiteral("body"), 7));
        m.setScreenLocked(true);
        QCOMPARE(m.liveCount(), 1);

        Notification update = note(QStringLiteral("Chat"), QStringLiteral("secret"), QStringLiteral("body"), 7);
        update.lockPrivacy = Notification::LockPrivacy::Hide;
        m.insert(update);
        QCOMPARE(m.liveCount(), 0);
    }

    /* Switching the default to Hide while locked drops the cards that
       expressed no preference, rather than showing them unmasked. */
    void aDefaultSwitchedToHideWhileLockedDrops()
    {
        NotificationModel m;
        plain(m);
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("hi"), {}, 8));
        m.setScreenLocked(true);
        m.setLockPrivacyDefault(Notification::LockPrivacy::Hide);
        QCOMPARE(m.liveCount(), 0);
    }

    /* A stack-tag repeat during the exit of the card it would replace is a
       new card, not a revival of the leaving one. */
    void aTagRepeatDuringAnExitIsANewCard()
    {
        NotificationModel m;
        plain(m);
        m.setExitMs(1000);
        Notification a = note(QStringLiteral("PowerDevil"), QStringLiteral("6%"), {}, 20);
        a.stackTag = QStringLiteral("battery");
        m.insert(a);
        m.dismiss(20);
        QVERIFY(anyClosing(m));

        QSignalSpy closed(&m, &NotificationModel::notificationClosed);
        Notification b = note(QStringLiteral("PowerDevil"), QStringLiteral("5%"), {}, 21);
        b.stackTag = QStringLiteral("battery");
        m.insert(b);
        QCOMPARE(m.liveCount(), 1);
        QCOMPARE(m.rowCount(), 2);               // the leaving card, and the new one
        for (const QList<QVariant> &c : std::as_const(closed)) {
            QVERIFY(c.at(0).toUInt() != 20u);    // not announced closed a second time
        }
    }

    /* A second click, or one during the exit, sends nothing. */
    void anActionIsSentOnlyOnce()
    {
        NotificationModel m;
        plain(m);
        m.setExitMs(1000);
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("hi"), {}, 30));
        QSignalSpy actions(&m, &NotificationModel::actionInvoked);
        QSignalSpy replies(&m, &NotificationModel::replied);
        m.invokeAction(30, QStringLiteral("default"));
        m.invokeAction(30, QStringLiteral("default"));
        m.sendReply(30, QStringLiteral("late"));
        QCOMPARE(actions.count(), 1);
        QCOMPARE(replies.count(), 0);
    }

    /* The pointer's hold follows the card when a stack tag swaps its id. */
    void theHoverHoldFollowsATagReplacement()
    {
        NotificationModel m;
        plain(m);
        Notification a = note(QStringLiteral("App"), QStringLiteral("one"), {}, 40);
        a.stackTag = QStringLiteral("t");
        m.insert(a);
        m.setHovered(40, true);
        Notification b = note(QStringLiteral("App"), QStringLiteral("two"), {}, 41);
        b.stackTag = QStringLiteral("t");
        m.insert(b);
        QVERIFY(m.isHovered(41));
        QVERIFY(!m.isHovered(40));
    }

    /* The expiry tick sleeps when there is nothing for it to do: nothing on
       screen that can expire and no burst inside the coalescing window. */
    /* A coalesced group's earlier notifications stay live: their senders
       are not told anything, each can be acted on or closed alone, and the
       group's end closes them with it. */
    void groupMembersStayLive()
    {
        NotificationModel m;
        m.setCoalesce(3, 20000);
        m.setTimeouts(0, 0, 0);
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);
        for (uint id = 101; id <= 103; ++id) {
            Notification n = note(QStringLiteral("Mail"), QStringLiteral("m%1").arg(id), {}, id);
            n.actions = {QStringLiteral("default"), QString()};
            m.insert(n);
        }
        QCOMPARE(m.liveCount(), 1);
        QCOMPARE(closed.size(), 0);                  // nobody told anything closed
        QVERIFY(m.isLive(101) && m.isLive(102) && m.isLive(103));
        QCOMPARE(m.index(0).data(NotificationModel::GroupCountRole).toInt(), 3);
        const QVariantList members = m.index(0).data(NotificationModel::MembersRole).toList();
        QCOMPARE(members.size(), 2);
        QCOMPARE(members.first().toMap().value(QStringLiteral("id")).toUInt(), 102u);   // newest first

        QSignalSpy actions(&m, &NotificationModel::actionInvoked);
        m.invokeAction(101, QStringLiteral("default"));
        QCOMPARE(actions.size(), 1);
        QCOMPARE(actions.first().at(0).toUInt(), 101u);
        QCOMPARE(m.index(0).data(NotificationModel::GroupCountRole).toInt(), 2);

        /* A member updating itself is updated in place. */
        m.insert(note(QStringLiteral("Mail"), QStringLiteral("edited"), {}, 102));
        QCOMPARE(m.index(0).data(NotificationModel::MembersRole).toList().first().toMap()
                     .value(QStringLiteral("summary")).toString(), QStringLiteral("edited"));

        closed.clear();
        m.dismiss(103);                              // the stack: all of it
        QCOMPARE(closed.size(), 2);
        QVERIFY(!m.isLive(102));
    }

    /* The sender withdrawing the top card closes only it: the newest member
       takes its place. */
    void withdrawingTheTopCardPromotesAMember()
    {
        NotificationModel m;
        m.setCoalesce(3, 20000);
        m.setTimeouts(0, 0, 0);
        for (uint id = 201; id <= 203; ++id) {
            m.insert(note(QStringLiteral("Chat"), QStringLiteral("c%1").arg(id), {}, id));
        }
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);
        m.closeId(203, CloseReason::Closed);
        QCOMPARE(closed.size(), 1);
        QCOMPARE(m.liveCount(), 1);
        QCOMPARE(m.index(0).data(NotificationModel::IdRole).toUInt(), 202u);
        QCOMPARE(m.index(0).data(NotificationModel::GroupCountRole).toInt(), 2);
    }

    /* While locked, a member that asks to be hidden goes, as a card would. */
    void aHiddenMemberIsDroppedWhenLocked()
    {
        NotificationModel m;
        m.setCoalesce(3, 20000);
        m.setTimeouts(0, 0, 0);
        Notification secret = note(QStringLiteral("Chat"), QStringLiteral("secret"), {}, 301);
        secret.lockPrivacy = Notification::LockPrivacy::Hide;
        m.insert(secret);
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("b"), {}, 302));
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("c"), {}, 303));
        m.setScreenLocked(true);
        QVERIFY(!m.isLive(301));
        QCOMPARE(m.index(0).data(NotificationModel::GroupCountRole).toInt(), 2);
    }

    /* The one card not playing its exit. */
    static int liveRow(const NotificationModel &m)
    {
        for (int r = 0; r < m.rowCount(); ++r) {
            if (!m.index(r).data(NotificationModel::ClosingRole).toBool()) {
                return r;
            }
        }
        return -1;
    }

    /* Each id is told it closed once, and only once. */
    static QMap<uint, int> closeCounts(const QSignalSpy &spy)
    {
        QMap<uint, int> counts;
        for (const QList<QVariant> &args : spy) {
            ++counts[args.at(0).toUInt()];
        }
        return counts;
    }

    /* The burst crossed the threshold after its earlier cards had gone, so
       the merged card has no members. Its sender withdrawing it (a chat
       marking the message read) took the first of an empty list and crashed
       the daemon. */
    void withdrawingAMergedCardWithNoMembers()
    {
        NotificationModel m;
        m.setCoalesce(3, 20000);
        m.setTimeouts(0, 0, 0);
        m.setExitMs(0);
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("a"), {}, 401));
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("b"), {}, 402));
        m.dismiss(401);
        m.dismiss(402);
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("c"), {}, 403));
        QCOMPARE(m.index(0).data(NotificationModel::GroupCountRole).toInt(), 1);
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);
        QVERIFY(m.closeId(403, CloseReason::Closed));
        QCOMPARE(closed.size(), 1);
        QCOMPARE(m.liveCount(), 0);
    }

    /* Acting on a stack's top card (opening it, an action, a reply, a
       snooze) deals with that one message. The rest of the group is still
       unread: the newest takes its place, and nobody is told the others
       were dismissed. */
    void actingOnTheTopCardLeavesTheRest()
    {
        auto group = [](NotificationModel &m) {
            m.setCoalesce(3, 20000);
            m.setTimeouts(0, 0, 0);
            for (uint id = 801; id <= 803; ++id) {
                Notification n = note(QStringLiteral("Chat"), QStringLiteral("c%1").arg(id), {}, id);
                n.actions = {QStringLiteral("default"), QString()};
                n.inlineReply = true;
                m.insert(n);
            }
        };
        const QList<std::function<void(NotificationModel &)>> acts = {
            [](NotificationModel &m) { m.invokeAction(803, QStringLiteral("default")); },
            [](NotificationModel &m) { m.open(nullptr, 803); },
            [](NotificationModel &m) { m.sendReply(803, QStringLiteral("ok")); },
            [](NotificationModel &m) { m.snooze(803); },
        };
        for (const auto &act : acts) {
            NotificationModel m;
            group(m);
            QSignalSpy closed(&m, &NotificationModel::notificationClosed);
            act(m);
            QCOMPARE(closed.size(), 1);
            QCOMPARE(closed.first().at(0).toUInt(), 803u);
            QVERIFY(m.isLive(801) && m.isLive(802));
            QCOMPARE(m.liveCount(), 1);
            QCOMPARE(m.index(liveRow(m)).data(NotificationModel::IdRole).toUInt(), 802u);
            QCOMPARE(m.index(liveRow(m)).data(NotificationModel::GroupCountRole).toInt(), 2);
        }
    }

    /* A queued group whose top card asks to be hidden on the lock screen
       passes the group on, as one on screen does. It used to close only the
       top: the members had no card, were never closed, and their senders
       waited for ever. */
    void aHiddenQueuedTopCardPassesTheGroupOn()
    {
        NotificationModel m;
        m.setCoalesce(3, 20000);
        m.setTimeouts(0, 0, 0);
        m.setExitMs(0);
        m.setLimit(1);
        m.insert(note(QStringLiteral("Other"), QStringLiteral("x"), {}, 500));
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("a"), {}, 501));
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("b"), {}, 502));
        Notification secret = note(QStringLiteral("Chat"), QStringLiteral("secret"), {}, 503);
        secret.lockPrivacy = Notification::LockPrivacy::Hide;
        m.insert(secret);
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);
        m.setScreenLocked(true);
        QCOMPARE(closeCounts(closed), (QMap<uint, int>{{503, 1}}));
        QVERIFY(m.isLive(501) && m.isLive(502));
        m.setScreenLocked(false);
        m.dismissAll();
        QCOMPARE(closeCounts(closed), (QMap<uint, int>{{500, 1}, {501, 1}, {502, 1}, {503, 1}}));
        QVERIFY(!m.isLive(501) && !m.isLive(502));
    }

    /* Same on screen: the hidden top card goes, the members it can show stay. */
    void aHiddenTopCardOnScreenPassesTheGroupOn()
    {
        NotificationModel m;
        m.setCoalesce(3, 20000);
        m.setTimeouts(0, 0, 0);
        m.setExitMs(0);
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("a"), {}, 511));
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("b"), {}, 512));
        Notification secret = note(QStringLiteral("Chat"), QStringLiteral("secret"), {}, 513);
        secret.lockPrivacy = Notification::LockPrivacy::Hide;
        m.insert(secret);
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);
        m.setScreenLocked(true);
        QCOMPARE(closeCounts(closed), (QMap<uint, int>{{513, 1}}));
        QVERIFY(m.isLive(511) && m.isLive(512));
        QCOMPARE(m.index(liveRow(m)).data(NotificationModel::IdRole).toUInt(), 512u);
    }

    /* A group's top card replaced through its stack tag keeps its members.
       They were left behind under the old id, live with no card for ever. */
    void aTagReplaceKeepsTheGroup()
    {
        NotificationModel m;
        m.setCoalesce(3, 20000);
        m.setTimeouts(0, 0, 0);
        m.setExitMs(0);
        for (uint id = 601; id <= 603; ++id) {
            m.insert(note(QStringLiteral("Chat"), QStringLiteral("c%1").arg(id), {}, id));
        }
        Notification tagged = note(QStringLiteral("Chat"), QStringLiteral("tagged"), {}, 603);
        tagged.stackTag = QStringLiteral("t");
        m.insert(tagged);                            // replaces_id: gains the tag
        Notification next = note(QStringLiteral("Chat"), QStringLiteral("next"), {}, 604);
        next.stackTag = QStringLiteral("t");
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);
        m.insert(next);
        QCOMPARE(m.index(0).data(NotificationModel::GroupCountRole).toInt(), 3);
        m.dismissAll();
        QCOMPARE(closeCounts(closed), (QMap<uint, int>{{601, 1}, {602, 1}, {603, 1}, {604, 1}}));
        QVERIFY(!m.isLive(601) && !m.isLive(602));
    }

    /* Mute takes the app's other cards and queue with it, not just the card
       it was chosen on, and lets a critical one through: muting the low
       battery warning must not hide the critical one, nor a call. */
    void muteClearsTheAppButNotItsCriticals()
    {
        NotificationModel m;
        plain(m);
        m.setExitMs(0);
        m.setLimit(2);
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("a"), {}, 701));
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("b"), {}, 702));
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("c"), {}, 703));   // queued
        m.insert(note(QStringLiteral("Mail"), QStringLiteral("d"), {}, 704));   // queued
        m.mute(701, 60);
        QVERIFY(!m.isLive(701) && !m.isLive(702) && !m.isLive(703));
        QVERIFY(m.isLive(704));
        m.insert(note(QStringLiteral("Chat"), QStringLiteral("muted"), {}, 705));
        QVERIFY(!m.isLive(705));
        Notification call = note(QStringLiteral("Chat"), QStringLiteral("call"), {}, 706);
        call.urgency = Urgency::Critical;
        m.insert(call);
        QVERIFY(m.isLive(706));
    }

    /* The age tick refreshes the members' "when" too, which an open
       group's member cards read: they stayed at the age they had when the
       group opened, however long it was held. */
    void theAgeTickAgesGroupMembers()
    {
        FakeClock clock;
        NotificationModel m;
        clock.install(m);
        m.setCoalesce(3, 20000);
        m.setTimeouts(0, 0, 0);
        for (uint id = 901; id <= 903; ++id) {
            m.insert(note(QStringLiteral("Chat"), QStringLiteral("c%1").arg(id), {}, id));
        }
        auto memberWhen = [&m] {
            return m.index(0).data(NotificationModel::MembersRole).toList().first().toMap()
                .value(QStringLiteral("when")).toString();
        };
        QCOMPARE(memberWhen(), QStringLiteral("now"));
        QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);
        clock.t = clock.t.addSecs(120);
        m.ageTickForTesting();
        QCOMPARE(changed.size(), 1);
        const auto roles = changed.first().at(2).value<QList<int>>();
        QVERIFY(roles.contains(NotificationModel::WhenRole));
        QVERIFY2(roles.contains(NotificationModel::MembersRole), "members' ages were not refreshed");
        QVERIFY(memberWhen() != QStringLiteral("now"));
    }

    /* A card that stays up is told the time has moved on. */
    void theAgeTickRunsWhileCardsAreShown()
    {
        NotificationModel m;
        plain(m);
        QVERIFY(!m.ageTickActiveForTesting());
        m.insert(note(QStringLiteral("App"), QStringLiteral("a"), {}, 90));
        QVERIFY(m.ageTickActiveForTesting());
        m.dismiss(90);
        QTRY_COMPARE(m.rowCount(), 0);
        QVERIFY(!m.ageTickActiveForTesting());
    }

    void tickRunsOnlyWhileThereIsWork()
    {
        FakeClock clock;
        NotificationModel m;
        clock.install(m);
        m.setCoalesce(3, 1000);
        m.setTimeouts(500, 500, 0);
        QVERIFY(!m.tickActiveForTesting());   // idle from startup

        m.insert(note(QStringLiteral("App"), QStringLiteral("a"), {}, 1));
        QVERIFY(m.tickActiveForTesting());

        clock.advance(m, 600);                // the card expires...
        QTRY_COMPARE(m.liveCount(), 0);
        clock.advance(m, 600);                // ...and the burst window lapses
        QVERIFY(!m.tickActiveForTesting());

        /* A card that never expires is not work. */
        m.setCoalesce(0, 0);
        Notification critical = note(QStringLiteral("App"), QStringLiteral("c"), {}, 2);
        critical.urgency = Urgency::Critical;
        m.insert(critical);
        QCOMPARE(m.liveCount(), 1);
        QVERIFY(!m.tickActiveForTesting());
    }

    void insertedNotificationIsShown()
    {
        NotificationModel m;
        plain(m);
        m.insert(note(QStringLiteral("Signal"), QStringLiteral("hello"), {}, 1));

        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(summaryAt(m, 0), QStringLiteral("hello"));
        QCOMPARE(m.hiddenCount(), 0);
        QVERIFY(m.isLive(1));
    }

    /* dunst's notification_limit, plus its rule that the indicator only takes
       a slot when there is genuinely an overflow to indicate. */
    void limitQueuesTheRestBehindAnIndicator()
    {
        NotificationModel m;
        plain(m);
        m.setLimit(4);
        m.setIndicateHidden(true);

        for (uint i = 1; i <= 4; ++i) {
            m.insert(note(QStringLiteral("App"), QStringLiteral("n%1").arg(i), {}, i));
        }
        QCOMPARE(m.rowCount(), 4);       // no overflow yet, so no reserved slot
        QCOMPARE(m.hiddenCount(), 0);

        m.insert(note(QStringLiteral("App"), QStringLiteral("n5"), {}, 5));
        QCOMPARE(m.rowCount(), 3);       // one slot given back to "+N more"
        QCOMPARE(m.hiddenCount(), 2);
    }

    /* macOS order: in a top-anchored stack the newest card is at the top. */
    void newestFirstPutsEachArrivalAtTheTop()
    {
        NotificationModel m;
        plain(m);
        m.setNewestFirst(true);
        for (uint i = 1; i <= 3; ++i) {
            m.insert(note(QStringLiteral("App%1").arg(i), QStringLiteral("n%1").arg(i), {}, i));
        }
        QCOMPARE(summaryAt(m, 0), QStringLiteral("n3"));
        QCOMPARE(summaryAt(m, 1), QStringLiteral("n2"));
        QCOMPARE(summaryAt(m, 2), QStringLiteral("n1"));
    }

    /* The limit gives a slot back by queueing the newest card, which is row 0
       now, not the last row. Taking the last would queue the oldest instead
       and reorder what the user is reading. */
    void newestFirstQueuesTheNewestCardNotTheLastRow()
    {
        NotificationModel m;
        plain(m);
        m.setNewestFirst(true);
        m.setLimit(4);
        m.setIndicateHidden(true);
        for (uint i = 1; i <= 5; ++i) {
            m.insert(note(QStringLiteral("App%1").arg(i), QStringLiteral("n%1").arg(i), {}, i));
        }
        /* Same cards on screen as in append order (n1..n3), newest at the top. */
        QCOMPARE(m.rowCount(), 3);
        QCOMPARE(m.hiddenCount(), 2);
        QCOMPARE(summaryAt(m, 0), QStringLiteral("n3"));
        QCOMPARE(summaryAt(m, 2), QStringLiteral("n1"));

        /* Freeing room brings the queue back in order, each above the last. */
        m.dismissAll();
        QCOMPARE(m.rowCount(), 0);
    }

    /* A burst still merges into its newest card where it stands; with the
       newest at the top, that is the top Discord card. */
    void newestFirstBurstMergesIntoTheTopCard()
    {
        NotificationModel m;
        m.setTimeouts(0, 0, 0);
        m.setCoalesce(3, 20000);
        m.setNewestFirst(true);

        m.insert(note(QStringLiteral("Discord"), QStringLiteral("msg 1"), {}, 1));
        m.insert(note(QStringLiteral("Signal"), QStringLiteral("other"), {}, 2));
        m.insert(note(QStringLiteral("Discord"), QStringLiteral("msg 2"), {}, 3));
        m.insert(note(QStringLiteral("Discord"), QStringLiteral("msg 3"), {}, 4));

        QCOMPARE(m.rowCount(), 2);
        QCOMPARE(summaryAt(m, 0), QStringLiteral("msg 3"));
        QCOMPARE(m.index(0).data(NotificationModel::GroupCountRole).toInt(), 3);
        QCOMPARE(summaryAt(m, 1), QStringLiteral("other"));
    }

    /* Moving the stack between top and bottom keeps it newest-nearest-corner
       for the cards already up, not just for the next arrival. */
    void flippingNewestFirstReversesTheStack()
    {
        NotificationModel m;
        plain(m);
        for (uint i = 1; i <= 3; ++i) {
            m.insert(note(QStringLiteral("App%1").arg(i), QStringLiteral("n%1").arg(i), {}, i));
        }
        QCOMPARE(summaryAt(m, 0), QStringLiteral("n1"));
        m.setNewestFirst(true);
        QCOMPARE(summaryAt(m, 0), QStringLiteral("n3"));
        QCOMPARE(summaryAt(m, 2), QStringLiteral("n1"));
        m.insert(note(QStringLiteral("App4"), QStringLiteral("n4"), {}, 4));
        QCOMPARE(summaryAt(m, 0), QStringLiteral("n4"));
    }

    void indicatorSlotCanBeTurnedOff()
    {
        NotificationModel m;
        plain(m);
        m.setLimit(4);
        m.setIndicateHidden(false);

        for (uint i = 1; i <= 5; ++i) {
            m.insert(note(QStringLiteral("App"), QStringLiteral("n%1").arg(i), {}, i));
        }
        QCOMPARE(m.rowCount(), 4);
        QCOMPARE(m.hiddenCount(), 1);
    }

    void limitZeroMeansUnlimited()
    {
        NotificationModel m;
        plain(m);
        m.setLimit(0);
        for (uint i = 1; i <= 10; ++i) {
            m.insert(note(QStringLiteral("App"), QStringLiteral("n%1").arg(i), {}, i));
        }
        QCOMPARE(m.rowCount(), 10);
        QCOMPARE(m.hiddenCount(), 0);
    }

    /* A negative limit is an easy typo in a config file. Unclamped it would
       make the effective limit negative, so nothing would ever be promoted:
       every notification queued forever, no popup, and no error anywhere. */
    void negativeLimitDoesNotSwallowEverything()
    {
        NotificationModel m;
        plain(m);
        m.setLimit(-5);
        for (uint i = 1; i <= 3; ++i) {
            m.insert(note(QStringLiteral("App"), QStringLiteral("n%1").arg(i), {}, i));
        }
        QCOMPARE(m.rowCount(), 3);
    }

    /* The "+N more" label went stale because update() compared the queue
       against its size on entry — and insert() appends before calling it, so
       the snapshot already contained the new arrival. */
    void hiddenCountIsAnnouncedEveryTimeTheQueueGrows()
    {
        NotificationModel m;
        plain(m);
        m.setLimit(2);
        QSignalSpy spy(&m, &NotificationModel::hiddenCountChanged);

        for (uint i = 1; i <= 3; ++i) {
            m.insert(note(QStringLiteral("App"), QStringLiteral("n%1").arg(i), {}, i));
        }
        QCOMPARE(m.hiddenCount(), 2);
        const int before = spy.count();

        m.insert(note(QStringLiteral("App"), QStringLiteral("n4"), {}, 4));
        QCOMPARE(m.hiddenCount(), 3);
        QVERIFY(spy.count() > before);   // nothing moved between the lists
    }

    /* replaces_id is an update of an existing notification, not the closing
       of one, so no NotificationClosed goes out — dunst does the same. */
    void replaceByIdUpdatesInPlaceAndAnnouncesNothing()
    {
        NotificationModel m;
        plain(m);
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);

        m.insert(note(QStringLiteral("Dolphin"), QStringLiteral("Copying"),
                      QStringLiteral("20%"), 7));
        m.insert(note(QStringLiteral("Dolphin"), QStringLiteral("Copying"),
                      QStringLiteral("80%"), 7));

        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(m.index(0).data(NotificationModel::BodyRole).toString(),
                 QStringLiteral("80%"));
        QCOMPARE(closed.count(), 0);
    }

    /* An update that arrives without an icon keeps the one it had, rather
       than going blank halfway through a progress notification. */
    void replaceByIdInheritsTheIcon()
    {
        NotificationModel m;
        plain(m);
        Notification first = note(QStringLiteral("Dolphin"), QStringLiteral("Copying"),
                                  {}, 7);
        first.appIcon = QStringLiteral("folder");
        m.insert(first);
        m.insert(note(QStringLiteral("Dolphin"), QStringLiteral("Copying"),
                      QStringLiteral("80%"), 7));

        QCOMPARE(m.index(0).data(NotificationModel::IconSourceRole).toString(),
                 QStringLiteral("image://icon/folder"));
    }

    /* A stack tag replaces a *different* notification, so that one does have
       to be announced — Expired, because the user never acted on it. */
    void replaceByTagClosesTheOneItReplaced()
    {
        NotificationModel m;
        plain(m);
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);

        Notification a = note(QStringLiteral("PowerDevil"), QStringLiteral("12%"), {}, 1);
        a.stackTag = QStringLiteral("battery");
        Notification b = note(QStringLiteral("PowerDevil"), QStringLiteral("9%"), {}, 2);
        b.stackTag = QStringLiteral("battery");

        m.insert(a);
        m.insert(b);

        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(summaryAt(m, 0), QStringLiteral("9%"));
        QCOMPARE(closed.count(), 1);
        QCOMPARE(closed.at(0).at(0).toUInt(), 1u);
        QCOMPARE(closed.at(0).at(1).toUInt(), CloseReason::Expired);
    }

    /* Matched on tag *and* app, or two apps picking the same generic tag
       would clobber each other. */
    void replaceByTagRequiresTheSameApp()
    {
        NotificationModel m;
        plain(m);
        Notification a = note(QStringLiteral("AppOne"), QStringLiteral("one"), {}, 1);
        a.stackTag = QStringLiteral("status");
        Notification b = note(QStringLiteral("AppTwo"), QStringLiteral("two"), {}, 2);
        b.stackTag = QStringLiteral("status");

        m.insert(a);
        m.insert(b);
        QCOMPARE(m.rowCount(), 2);
    }

    /* The immortal card.

       PowerDevil re-sends the identical "Device Battery Low (6% Remaining)"
       once or twice a second for as long as the device is low. The tag
       correctly collapsed all of them onto one card — and every copy also
       reset the dwell clock, so an 8s timer restarted about eight times per
       expiry and the card could never die. The rule was working; the card was
       simply immortal, which looks exactly like the spam it was meant to stop. */
    void identicalTagRepeatDoesNotRestartTheDwell()
    {
        NotificationModel m;
        FakeClock clock;
        clock.install(m);
        m.setCoalesce(0, 0);
        m.setTimeouts(2000, 2000, 2000);

        Notification n = note(QStringLiteral("PowerDevil"),
                              QStringLiteral("Device Battery Low"),
                              QStringLiteral("6% Remaining"), 1);
        n.stackTag = QStringLiteral("battery");
        m.insert(n);
        QCOMPARE(m.rowCount(), 1);

        clock.advance(m, 1200);
        n.id = 2;
        m.insert(n);                 // byte-identical, 1200ms in
        QCOMPARE(m.rowCount(), 1);

        /* It must die on the original clock, at 2000ms. With the clock reset
           it would live until 3200ms. */
        clock.advance(m, 799);
        QCOMPARE(m.rowCount(), 1);
        clock.advance(m, 1);
        QCOMPARE(m.rowCount(), 0);
    }

    /* The other half of the same rule: 12% -> 9% is new information and does
       earn a fresh timer, which is the entire point of replacing by tag. */
    void changedTagContentDoesRestartTheDwell()
    {
        NotificationModel m;
        FakeClock clock;
        clock.install(m);
        m.setCoalesce(0, 0);
        m.setTimeouts(2000, 2000, 2000);

        Notification n = note(QStringLiteral("PowerDevil"),
                              QStringLiteral("Device Battery Low"),
                              QStringLiteral("9% Remaining"), 1);
        n.stackTag = QStringLiteral("battery");
        m.insert(n);

        clock.advance(m, 1200);
        n.id = 2;
        n.body = QStringLiteral("6% Remaining");
        m.insert(n);

        clock.advance(m, 1000);      // 2200ms after the first, 1000 after the second
        QCOMPARE(m.rowCount(), 1);
        clock.advance(m, 1000);      // a full dwell after the replacement
        QCOMPARE(m.rowCount(), 0);
    }

    /* set_stack_tag alone is not enough for a sender that repeats forever:
       replaceByTag only searches what is on screen or queued, so once the card
       expires the next copy matches nothing and opens a fresh popup. */
    void repeatWindowSuppressesAPopupAfterTheCardIsGone()
    {
        NotificationModel m;
        plain(m);
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);

        Notification n = note(QStringLiteral("PowerDevil"),
                              QStringLiteral("Device Battery Low"),
                              QStringLiteral("6% Remaining"), 1);
        n.repeatWindowSec = 300;
        m.insert(n);
        QCOMPARE(m.rowCount(), 1);

        m.closeId(1, CloseReason::Expired);
        QCOMPARE(m.rowCount(), 0);

        n.id = 2;
        m.insert(n);
        QCOMPARE(m.rowCount(), 0);   // suppressed, not shown again

        /* The sender is still told, so it is not left waiting on a close
           signal that never comes. */
        QCOMPARE(closed.count(), 2);
        QCOMPARE(closed.at(1).at(0).toUInt(), 2u);
        QCOMPARE(closed.at(1).at(1).toUInt(), CloseReason::Expired);
    }

    /* Keyed on the content, so the same rule still lets a changed percentage
       through as a genuinely new piece of news. */
    void repeatWindowLetsChangedContentThrough()
    {
        NotificationModel m;
        plain(m);

        Notification n = note(QStringLiteral("PowerDevil"),
                              QStringLiteral("Device Battery Low"),
                              QStringLiteral("9% Remaining"), 1);
        n.repeatWindowSec = 300;
        m.insert(n);
        m.closeId(1, CloseReason::Expired);

        n.id = 2;
        n.body = QStringLiteral("6% Remaining");
        m.insert(n);
        QCOMPARE(m.rowCount(), 1);
    }

    void repeatWindowIsOffByDefault()
    {
        NotificationModel m;
        plain(m);
        Notification n = note(QStringLiteral("App"), QStringLiteral("same"), {}, 1);
        m.insert(n);
        m.closeId(1, CloseReason::Expired);
        n.id = 2;
        m.insert(n);
        QCOMPARE(m.rowCount(), 1);
    }

    /* Burst grouping — the capability neither dunst nor swaync has. Thirty
       notifications should become one card that says thirty. */
    void burstsCoalesceIntoOneCountedCard()
    {
        NotificationModel m;
        m.setTimeouts(0, 0, 0);
        m.setCoalesce(3, 20000);

        for (uint i = 1; i <= 3; ++i) {
            m.insert(note(QStringLiteral("Discord"), QStringLiteral("msg %1").arg(i), {}, i));
        }
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(m.index(0).data(NotificationModel::GroupCountRole).toInt(), 3);

        /* A continuing burst keeps absorbing rather than starting a second
           card behind the first. */
        m.insert(note(QStringLiteral("Discord"), QStringLiteral("msg 4"), {}, 4));
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(m.index(0).data(NotificationModel::GroupCountRole).toInt(), 4);
    }

    /* How a burst turns into one card, as drawn. The absorbed cards used to
       be deleted on the spot and the merged card added afresh at the end of
       the queue: a frame-by-frame capture showed the old card vanishing
       mid-frame while the new one faded in thirty pixels away, the two
       overlapping. Now the newest card of the group becomes the merged card
       where it stands, and the others fade out like any dismissed card. */
    void aBurstMergesIntoItsNewestCardInPlace()
    {
        NotificationModel m;
        m.setTimeouts(0, 0, 0);
        m.setCoalesce(3, 20000);
        m.setExitMs(1);

        m.insert(note(QStringLiteral("Discord"), QStringLiteral("msg 1"), {}, 1));
        m.insert(note(QStringLiteral("Signal"), QStringLiteral("other"), {}, 2));
        m.insert(note(QStringLiteral("Discord"), QStringLiteral("msg 2"), {}, 3));
        m.insert(note(QStringLiteral("Discord"), QStringLiteral("msg 3"), {}, 4));

        /* Same three rows: nothing jumped. The first Discord card is fading,
           the Signal card is untouched, the newest Discord card is now the
           group. */
        QCOMPARE(m.rowCount(), 3);
        QVERIFY(m.index(0).data(NotificationModel::ClosingRole).toBool());
        QCOMPARE(summaryAt(m, 1), QStringLiteral("other"));
        QCOMPARE(summaryAt(m, 2), QStringLiteral("msg 3"));
        QCOMPARE(m.index(2).data(NotificationModel::GroupCountRole).toInt(), 3);
        QVERIFY(!m.index(2).data(NotificationModel::ClosingRole).toBool());

        QTRY_COMPARE_WITH_TIMEOUT(m.rowCount(), 2, 1500);
        QCOMPARE(summaryAt(m, 1), QStringLiteral("msg 3"));
    }

    /* Below the threshold nothing is grouped: two messages are two messages. */
    void belowTheThresholdNothingIsGrouped()
    {
        NotificationModel m;
        m.setTimeouts(0, 0, 0);
        m.setCoalesce(3, 20000);
        m.insert(note(QStringLiteral("Discord"), QStringLiteral("one"), {}, 1));
        m.insert(note(QStringLiteral("Discord"), QStringLiteral("two"), {}, 2));
        QCOMPARE(m.rowCount(), 2);
    }

    void coalescingIsPerApp()
    {
        NotificationModel m;
        m.setTimeouts(0, 0, 0);
        m.setCoalesce(2, 20000);
        m.insert(note(QStringLiteral("Discord"), QStringLiteral("a"), {}, 1));
        m.insert(note(QStringLiteral("Signal"), QStringLiteral("b"), {}, 2));
        QCOMPARE(m.rowCount(), 2);   // one each, neither reached the threshold
    }

    /* Grouped by desktop entry where there is one: the same application can
       send under more than one display name. */
    void coalescingPrefersTheDesktopEntry()
    {
        NotificationModel m;
        m.setTimeouts(0, 0, 0);
        m.setCoalesce(2, 20000);

        Notification a = note(QStringLiteral("Firefox"), QStringLiteral("a"), {}, 1);
        a.desktopEntry = QStringLiteral("firefox");
        Notification b = note(QStringLiteral("Mozilla Firefox"), QStringLiteral("b"), {}, 2);
        b.desktopEntry = QStringLiteral("firefox");

        m.insert(a);
        m.insert(b);
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(m.index(0).data(NotificationModel::GroupCountRole).toInt(), 2);
    }

    /* A tagged notification has asked to be replaced by its successor, not
       counted alongside it, so tags outrank coalescing. */
    void stackTagsAreNotCoalesced()
    {
        NotificationModel m;
        m.setTimeouts(0, 0, 0);
        m.setCoalesce(2, 20000);

        Notification tagged = note(QStringLiteral("App"), QStringLiteral("a"), {}, 1);
        tagged.stackTag = QStringLiteral("t");
        m.insert(tagged);
        m.insert(note(QStringLiteral("App"), QStringLiteral("b"), {}, 2));

        QCOMPARE(m.rowCount(), 2);
        QCOMPARE(m.index(0).data(NotificationModel::GroupCountRole).toInt(), 1);
    }

    void coalescingCanBeDisabled()
    {
        NotificationModel m;
        m.setTimeouts(0, 0, 0);
        m.setCoalesce(0, 20000);
        m.setLimit(0);   // the display limit is a separate question
        for (uint i = 1; i <= 5; ++i) {
            m.insert(note(QStringLiteral("Discord"), QStringLiteral("n%1").arg(i), {}, i));
        }
        QCOMPARE(m.rowCount(), 5);
    }

    /* A rule said "history only". The sender is still told it ended. */
    void skipDisplayShowsNothingButStillAnswersTheSender()
    {
        NotificationModel m;
        plain(m);
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);

        Notification n = note(QStringLiteral("Steam"), QStringLiteral("friend online"), {}, 1);
        n.skipDisplay = true;
        m.insert(n);

        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(closed.count(), 1);
        QCOMPARE(closed.at(0).at(1).toUInt(), CloseReason::Expired);
    }

    void doNotDisturbSuppressesThePopup()
    {
        NotificationModel m;
        plain(m);
        m.setDoNotDisturb(true);
        m.insert(note(QStringLiteral("App"), QStringLiteral("hello"), {}, 1));
        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(m.hiddenCount(), 0);   // suppressed, not queued for later
    }

    /* Three separate reasons for quiet, kept apart on purpose: turning Do Not
       Disturb off must not silently cancel a screen recording's inhibition. */
    void theThreeQuietSourcesAreIndependent()
    {
        NotificationModel m;
        plain(m);
        QVERIFY(!m.quiet());

        m.setDoNotDisturb(true);
        m.setInhibited(true);
        m.setBusyQuiet(true);
        QVERIFY(m.quiet());

        m.setDoNotDisturb(false);
        QVERIFY(m.quiet());          // the application is still asking
        m.setInhibited(false);
        QVERIFY(m.quiet());          // something is still holding the screen awake
        m.setBusyQuiet(false);
        QVERIFY(!m.quiet());
    }

    /* Sound goes with the popup: a notification nobody is shown should not
       announce itself. */
    void quietSilencesTheSoundToo()
    {
        NotificationModel m;
        plain(m);
        QSignalSpy sound(&m, &NotificationModel::soundWanted);

        m.insert(note(QStringLiteral("App"), QStringLiteral("one"), {}, 1));
        QCOMPARE(sound.count(), 1);
        QCOMPARE(sound.at(0).at(0).toString(), QStringLiteral("message"));

        m.setDoNotDisturb(true);
        m.insert(note(QStringLiteral("App"), QStringLiteral("two"), {}, 2));
        QCOMPARE(sound.count(), 1);
    }

    void ruleSoundOverridesTheDefaultAndNoneMeansSilence()
    {
        NotificationModel m;
        plain(m);
        QSignalSpy sound(&m, &NotificationModel::soundWanted);

        Notification chosen = note(QStringLiteral("App"), QStringLiteral("one"), {}, 1);
        chosen.sound = QStringLiteral("bell");
        m.insert(chosen);
        QCOMPARE(sound.count(), 1);
        QCOMPARE(sound.at(0).at(0).toString(), QStringLiteral("bell"));

        Notification silent = note(QStringLiteral("App"), QStringLiteral("two"), {}, 2);
        silent.sound = QStringLiteral("none");
        m.insert(silent);
        QCOMPARE(sound.count(), 1);

        /* Low urgency has no sound at all, so nothing is emitted rather than
           an empty name being handed to the player. */
        Notification low = note(QStringLiteral("App"), QStringLiteral("three"), {}, 3);
        low.urgency = Urgency::Low;
        m.insert(low);
        QCOMPARE(sound.count(), 1);
    }

    /* A held-back notification has still arrived, and the sound is what tells
       you so — hence before the queue, not after. */
    void queuedNotificationsStillMakeTheirSound()
    {
        NotificationModel m;
        plain(m);
        m.setLimit(1);
        QSignalSpy sound(&m, &NotificationModel::soundWanted);

        m.insert(note(QStringLiteral("App"), QStringLiteral("one"), {}, 1));
        m.insert(note(QStringLiteral("App"), QStringLiteral("two"), {}, 2));
        QVERIFY(m.hiddenCount() > 0);
        QCOMPARE(sound.count(), 2);
    }

    /* 0 means never expire, which the spec mandates for critical. */
    void criticalDoesNotExpireOnItsOwn()
    {
        NotificationModel m;
        FakeClock clock;
        clock.install(m);
        m.setCoalesce(0, 0);
        m.setTimeouts(200, 200, 0);

        Notification n = note(QStringLiteral("App"), QStringLiteral("disk full"), {}, 1);
        n.urgency = Urgency::Critical;
        m.insert(n);

        Notification ordinary = note(QStringLiteral("App"), QStringLiteral("hello"), {}, 2);
        m.insert(ordinary);

        clock.advance(m, 200);
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(summaryAt(m, 0), QStringLiteral("disk full"));
        clock.advance(m, 24 * 60 * 60 * 1000);   // still there a day later
        QCOMPARE(m.rowCount(), 1);
    }

    /* An app that sent its own expire_timeout still wins, including over the
       critical default — the fallbacks only apply when it sent -1. */
    void senderTimeoutBeatsTheDefault()
    {
        NotificationModel m;
        FakeClock clock;
        clock.install(m);
        m.setCoalesce(0, 0);
        m.setTimeouts(0, 0, 0);

        Notification n = note(QStringLiteral("App"), QStringLiteral("brief"), {}, 1);
        n.urgency = Urgency::Critical;
        n.timeoutMs = 200;
        m.insert(n);

        clock.advance(m, 199);
        QCOMPARE(m.rowCount(), 1);
        clock.advance(m, 1);
        QCOMPARE(m.rowCount(), 0);
    }

    /* Hovering pauses the dwell timer: a notification should not expire out
       from under someone who is visibly reading it. */
    void hoveringHoldsANotificationOpen()
    {
        NotificationModel m;
        FakeClock clock;
        clock.install(m);
        m.setCoalesce(0, 0);
        m.setTimeouts(300, 300, 300);

        m.insert(note(QStringLiteral("App"), QStringLiteral("read me"), {}, 1));
        m.setHovered(1, true);

        clock.advance(m, 700);       // well past its dwell, but being read
        QCOMPARE(m.rowCount(), 1);

        /* Leaving restarts the dwell from now: a full 300ms more, not 0. */
        m.setHovered(1, false);
        clock.advance(m, 299);
        QCOMPARE(m.rowCount(), 1);
        clock.advance(m, 1);
        QCOMPARE(m.rowCount(), 0);
    }

    /* Turning the setting off must not make every hovered popup vanish at
       once — they resume counting from now, not from when they appeared. */
    void disablingHoverPauseRestartsRatherThanExpires()
    {
        NotificationModel m;
        FakeClock clock;
        clock.install(m);
        m.setCoalesce(0, 0);
        m.setTimeouts(400, 400, 400);

        m.insert(note(QStringLiteral("App"), QStringLiteral("read me"), {}, 1));
        m.setHovered(1, true);
        clock.advance(m, 700);

        /* Not swept away by the next tick after the flag flips (the old
           version of this test never ran one, so it could not have failed),
           and still gone a full dwell later. */
        m.setHoverPause(false);
        clock.advance(m, 0);
        QCOMPARE(m.rowCount(), 1);
        clock.advance(m, 399);
        QCOMPARE(m.rowCount(), 1);
        clock.advance(m, 1);
        QCOMPARE(m.rowCount(), 0);
    }

    void dismissAllClearsScreenAndQueue()
    {
        NotificationModel m;
        plain(m);
        m.setLimit(2);
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);

        for (uint i = 1; i <= 5; ++i) {
            m.insert(note(QStringLiteral("App"), QStringLiteral("n%1").arg(i), {}, i));
        }
        QCOMPARE(m.dismissAll(), 5);
        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(m.hiddenCount(), 0);
        QCOMPARE(closed.count(), 5);
        QCOMPARE(closed.at(0).at(1).toUInt(), CloseReason::Dismissed);
    }

    /* Opening the centre is not acting on any particular notification, and
       telling every sender otherwise would be a lie about intent. */
    void hideForCentreClosesAsExpiredNotDismissed()
    {
        NotificationModel m;
        plain(m);
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);

        m.insert(note(QStringLiteral("App"), QStringLiteral("one"), {}, 1));
        QCOMPARE(m.hideForCentre(), 1);
        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(closed.at(0).at(1).toUInt(), CloseReason::Expired);
    }

    void invokingAnActionAnnouncesItThenCloses()
    {
        NotificationModel m;
        plain(m);
        QSignalSpy invoked(&m, &NotificationModel::actionInvoked);
        QSignalSpy closed(&m, &NotificationModel::notificationClosed);

        Notification n = note(QStringLiteral("PowerDevil"), QStringLiteral("Low battery"), {}, 1);
        n.actions = {QStringLiteral("save"), QStringLiteral("Switch to Power Save")};
        m.insert(n);
        m.invokeAction(1, QStringLiteral("save"));

        QCOMPARE(invoked.count(), 1);
        QCOMPARE(invoked.at(0).at(1).toString(), QStringLiteral("save"));
        QCOMPARE(closed.at(0).at(1).toUInt(), CloseReason::Dismissed);
        QVERIFY(!m.isLive(1));
    }

    /* The wire format is a flat [key, label, ...] list. "default" is what
       activating the body means and the spec says it must not be drawn as a
       button; inline-reply is rendered as a text field, not one either. */
    void actionsRoleHidesDefaultAndInlineReply()
    {
        NotificationModel m;
        plain(m);
        Notification n = note(QStringLiteral("Element"), QStringLiteral("message"), {}, 1);
        n.actions = {QStringLiteral("default"),      QStringLiteral("Open"),
                     QStringLiteral("inline-reply"), QStringLiteral("Reply"),
                     QStringLiteral("mark-read"),    QStringLiteral("Mark as read")};
        m.insert(n);

        const QVariantList actions =
            m.index(0).data(NotificationModel::ActionsRole).toList();
        QCOMPARE(actions.size(), 1);
        QCOMPARE(actions.at(0).toMap().value(QStringLiteral("key")).toString(),
                 QStringLiteral("mark-read"));
        QVERIFY(m.index(0).data(NotificationModel::HasDefaultActionRole).toBool());
    }

    /* An odd-length actions list is malformed input from a sender, and must
       not be read past the end of. */
    void malformedActionListIsIgnoredNotRead()
    {
        NotificationModel m;
        plain(m);
        Notification n = note(QStringLiteral("App"), QStringLiteral("x"), {}, 1);
        n.actions = {QStringLiteral("only-a-key")};
        m.insert(n);
        QCOMPARE(m.index(0).data(NotificationModel::ActionsRole).toList().size(), 0);
    }

    /* A closing row is logically gone — it is still here only so the view can
       animate it away. Everything that looks a notification up has to treat it
       as absent, without each caller having to know the state exists. */
    void aClosingRowIsInvisibleToLookups()
    {
        NotificationModel m;
        plain(m);
        m.setExitMs(1);
        m.insert(note(QStringLiteral("App"), QStringLiteral("bye"), {}, 1));

        m.closeId(1, CloseReason::Dismissed);
        QCOMPARE(m.rowCount(), 1);                  // still drawn, mid-animation
        QVERIFY(m.index(0).data(NotificationModel::ClosingRole).toBool());
        QVERIFY(!m.isLive(1));

        QTRY_COMPARE_WITH_TIMEOUT(m.rowCount(), 0, 1500);
    }

    /* The ghost card. A burst arrives while one card is fading out: the
       overflow takes a slot for the "+N more" row, and the slot it took back
       was the newest card, which was the one closing. Moved to the queue
       mid-fade, its removal could no longer find it; when it came back out
       it was a card already marked closing — drawn, but deaf to clicks, never
       expiring, and skipped by Dismiss All. Seen live after waking with
       twenty notifications waiting. */
    void aClosingCardIsNeverMovedToTheQueue()
    {
        NotificationModel m;
        plain(m);
        m.setExitMs(1);
        m.setLimit(4);
        m.setIndicateHidden(true);

        for (uint i = 1; i <= 4; ++i) {
            m.insert(note(QStringLiteral("App%1").arg(i), QStringLiteral("n%1").arg(i), {}, i));
        }
        m.closeId(4, CloseReason::Dismissed);          // the newest, mid-fade
        m.insert(note(QStringLiteral("App5"), QStringLiteral("n5"), {}, 5));
        m.insert(note(QStringLiteral("App6"), QStringLiteral("n6"), {}, 6));

        /* Let the fade finish, then clear the screen the way a user would. */
        QTRY_VERIFY_WITH_TIMEOUT(!anyClosing(m), 1000);
        for (int round = 0; round < 10 && m.rowCount() > 0; ++round) {
            for (int r = 0; r < m.rowCount(); ++r) {
                m.closeId(m.index(r).data(NotificationModel::IdRole).toUInt(), CloseReason::Dismissed);
            }
            QTRY_VERIFY_WITH_TIMEOUT(!anyClosing(m), 1000);
        }

        /* Nothing left drawn, nothing left queued. */
        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(m.hiddenCount(), 0);
    }

    /* What the Clear all pill follows: a card fading out no longer counts,
       so the pill can leave with the cards instead of lingering after them. */
    void liveCountLeavesOutClosingCards()
    {
        NotificationModel m;
        plain(m);
        m.setExitMs(300);
        QSignalSpy spy(&m, &NotificationModel::liveCountChanged);
        for (uint i = 1; i <= 3; ++i) {
            m.insert(note(QStringLiteral("App%1").arg(i), QStringLiteral("n%1").arg(i), {}, i));
        }
        QCOMPARE(m.property("liveCount").toInt(), 3);
        const int before = spy.count();

        m.dismissAll();
        QCOMPARE(m.rowCount(), 3);                       // still fading
        QCOMPARE(m.property("liveCount").toInt(), 0);    // but none of them live
        QVERIFY(spy.count() > before);
    }

    void snoozeHandsTheNotificationOnAndTakesItOffScreen()
    {
        NotificationModel m;
        plain(m);
        Notification received;
        int count = 0;
        connect(&m, &NotificationModel::snoozeRequested, this,
                [&](const Notification &n) { received = n; ++count; });

        m.insert(note(QStringLiteral("Signal"), QStringLiteral("later"), {}, 1));
        m.snooze(1);

        QCOMPARE(count, 1);
        QCOMPARE(received.summary, QStringLiteral("later"));
        QCOMPARE(m.rowCount(), 0);
    }

    void closingAnUnknownIdIsHarmless()
    {
        NotificationModel m;
        plain(m);
        QVERIFY(!m.closeId(999, CloseReason::Closed));
        QCOMPARE(m.dismissAll(), 0);
        QCOMPARE(m.hideForCentre(), 0);
    }
};

GLASSOSD_TEST_MAIN(TestNotificationModel)
#include "tst_notificationmodel.moc"
