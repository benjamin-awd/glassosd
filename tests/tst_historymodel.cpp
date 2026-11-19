/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    Notification history.

    Every test in here is a bug that actually happened. HistoryLength was read
    after the file had already been truncated, so anyone raising it lost the
    excess on the next start. Identical repeats were kept a row each, so one
    afternoon of PowerDevil produced 158 copies of the same line. A group of
    one row standing for 156 arrivals lost its header and read as a member of
    whatever was above it. None of these looked like a crash; they looked like
    the program working.
*/
#include "historymodel.h"
#include "testenv.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QJsonArray>
#include <QSignalSpy>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <sys/stat.h>

class TestHistoryModel : public QObject
{
    Q_OBJECT

private:
    static QString storePath()
    {
        return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + QStringLiteral("/glassosd/history.json");
    }

    /* Both files are shared by every test in the process, so each one starts
       by saying what it wants them to contain. */
    static void writeStore(const QJsonArray &arr)
    {
        const QString path = storePath();
        QDir().mkpath(QFileInfo(path).path());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(arr).toJson());
    }

    static void clearStore()
    {
        QFile::remove(storePath());
    }

    static void setHistoryLength(int n)
    {
        auto config = KSharedConfig::openConfig(QStringLiteral("glassosdrc"));
        config->group(QStringLiteral("Notifications")).writeEntry("HistoryLength", n);
        config->sync();
        config->reparseConfiguration();
    }

    static QJsonObject entry(const QString &app, const QString &summary,
                             const QString &body = {}, int id = 0,
                             const QDateTime &at = QDateTime::currentDateTimeUtc())
    {
        return QJsonObject{
            {QStringLiteral("id"), int(id)},
            {QStringLiteral("app"), app},
            {QStringLiteral("summary"), summary},
            {QStringLiteral("body"), body},
            {QStringLiteral("urgency"), 1},
            {QStringLiteral("at"), at.toString(Qt::ISODate)},
        };
    }

    /* Ids come from a counter because the notification server hands out a
       fresh one for every arrival, and record() treats a repeated id as the
       same message updating itself. Two unrelated notifications sharing an id
       is not a case that occurs; two sharing their text very much is. */
    static Notification note(const QString &app, const QString &summary,
                             const QString &body = {}, uint id = 0)
    {
        static uint next = 1000;
        Notification n;
        n.id = id ? id : next++;
        n.appName = app;
        n.summary = summary;
        n.body = body;
        n.received = QDateTime::currentDateTimeUtc();
        return n;
    }

private Q_SLOTS:
    /* The bug: load() truncates to m_capacity as it reads, and the model is a
       QML singleton, so main.cpp could only call setCapacity() after the file
       had already been cut short. Raising HistoryLength therefore worked for
       the session and silently lost everything past 200 on the next start,
       while glassosdctl went on reporting the larger number. */
    /* History keeps a picture at the size the centre draws it, not the
       512px the popup copy is allowed: 200 entries at 512^2 RGBA was a
       ~200 MB ceiling for one session. */
    void historyKeepsImagesSmall()
    {
        clearStore();
        HistoryModel h;
        Notification n = note(QStringLiteral("App"), QStringLiteral("photo"));
        QImage big(512, 384, QImage::Format_RGBA8888);
        big.fill(Qt::blue);
        n.image = ImageData::fromImage(big);
        QCOMPARE(n.image.width, 512);
        h.record(n);
        const ImageData kept = h.imageFor(n.id);
        QVERIFY(kept.isValid());
        QCOMPARE(kept.width, ImageData::maxHistoryEdge);
        QCOMPARE(kept.height, ImageData::maxHistoryEdge * 3 / 4);
    }

    void capacityIsReadBeforeLoading()
    {
        setHistoryLength(250);
        QJsonArray arr;
        for (int i = 0; i < 240; ++i) {
            arr.append(entry(QStringLiteral("App"), QStringLiteral("message %1").arg(i)));
        }
        writeStore(arr);

        HistoryModel h;
        QCOMPARE(h.total(), 240);   // 200 was the bug
    }

    void capacityTruncatesOnLoad()
    {
        setHistoryLength(5);
        QJsonArray arr;
        for (int i = 0; i < 20; ++i) {
            arr.append(entry(QStringLiteral("App"), QStringLiteral("message %1").arg(i)));
        }
        writeStore(arr);

        HistoryModel h;
        QCOMPARE(h.total(), 5);
    }

    /* An older file has the repeats one row each; nothing but a pass on load
       would ever fold them together, since only a new arrival triggers a
       merge. */
    void loadCollapsesDuplicatesWrittenByOlderBuilds()
    {
        setHistoryLength(200);
        QJsonArray arr;
        for (int i = 0; i < 6; ++i) {
            arr.append(entry(QStringLiteral("Battery Monitor"),
                             QStringLiteral("Device Battery Low (6% Remaining)")));
        }
        arr.append(entry(QStringLiteral("Battery Monitor"),
                         QStringLiteral("Device Battery Low (9% Remaining)")));
        writeStore(arr);

        HistoryModel h;
        QCOMPARE(h.total(), 2);   // one per distinct message
    }

    /* Ids are only unique within one run of the daemon, so the server has to
       start above whatever the file already used or a fresh notification
       overwrites an unrelated old entry. */
    void maxLoadedIdIsTheHighestInTheFile()
    {
        setHistoryLength(200);
        writeStore({entry(QStringLiteral("A"), QStringLiteral("one"), {}, 4),
                    entry(QStringLiteral("A"), QStringLiteral("two"), {}, 91),
                    entry(QStringLiteral("A"), QStringLiteral("three"), {}, 12)});

        HistoryModel h;
        QCOMPARE(h.maxLoadedId(), 91u);
    }

    void missingFileIsNotAnError()
    {
        clearStore();
        HistoryModel h;
        QCOMPARE(h.total(), 0);
        QCOMPARE(h.rowCount(), 0);
    }

    /* A byte-identical repeat becomes a count, not another row. */
    void recordMergesIdenticalRepeats()
    {
        clearStore();
        HistoryModel h;
        for (int i = 0; i < 3; ++i) {
            h.record(note(QStringLiteral("Battery Monitor"),
                          QStringLiteral("Device Battery Low (6% Remaining)")));
        }
        QCOMPARE(h.total(), 1);

        /* One card, which says x3 itself: every group is its own card or
           stack now, so it no longer needs a header to keep it from reading
           as part of the group above. The count is still occurrences. */
        QCOMPARE(h.rowCount(), 1);
        QVERIFY(!h.index(0).data(HistoryModel::IsHeaderRole).toBool());
        QCOMPARE(h.index(0).data(HistoryModel::GroupCountRole).toInt(), 3);
        QCOMPARE(h.index(0).data(HistoryModel::RepeatCountRole).toInt(), 3);
    }

    /* One toggle folds a group, whatever its repeats. toggleGroup worked
       out the current state from the header's occurrence count while the
       rows were built from the row count, so a group of two rows standing
       for three arrivals (one a repeat) looked open, toggled to "open", and
       took a second press to fold. */
    void oneToggleFoldsAGroupWithRepeats()
    {
        clearStore();
        HistoryModel h;
        h.setAutoCollapseOver(2);
        h.record(note(QStringLiteral("Mail"), QStringLiteral("one")));
        h.record(note(QStringLiteral("Mail"), QStringLiteral("two")));
        h.record(note(QStringLiteral("Mail"), QStringLiteral("two")));   // a repeat
        QCOMPARE(h.rowCount(), 3);   // header, two rows
        QVERIFY(!h.rowInfo(0).value(QStringLiteral("collapsed")).toBool());
        h.toggleGroup(h.rowInfo(0).value(QStringLiteral("groupKey")).toString());
        QVERIFY(h.rowInfo(0).value(QStringLiteral("collapsed")).toBool());
        QVERIFY(h.rowCount() < 3);
    }

    /* Times read as macOS writes them, in the centre as on the banner:
       the centre wrote 23:22 under a card that said "now". */
    void timesAreRelative()
    {
        const QDateTime now = QDateTime(QDate(2026, 11, 19), QTime(15, 0)).toUTC();
        auto at = [&](qint64 secsAgo) { return Notification::relativeWhen(now.addSecs(-secsAgo), now); };
        QCOMPARE(at(10), QStringLiteral("now"));
        QCOMPARE(at(5 * 60), QStringLiteral("5m ago"));
        QCOMPARE(at(2 * 3600 + 60), QStringLiteral("2h ago"));
        const QDateTime yesterdayEvening = QDateTime(QDate(2026, 11, 18), QTime(22, 0)).toUTC();
        QCOMPARE(Notification::relativeWhen(yesterdayEvening, now), QStringLiteral("Yesterday"));
        const QDate monday(2026, 11, 16);
        QCOMPARE(Notification::relativeWhen(QDateTime(monday, QTime(9, 0)).toUTC(), now),
                 QLocale().dayName(1, QLocale::LongFormat));
        QCOMPARE(Notification::relativeWhen(QDateTime(QDate(2026, 10, 3), QTime(9, 0)).toUTC(), now),
                 QLocale().toString(QDate(2026, 10, 3), QStringLiteral("d MMM")));

        clearStore();
        HistoryModel h;
        QDateTime clock = QDateTime::currentDateTimeUtc();
        h.setClockForTesting([&clock] { return clock; });
        h.record(note(QStringLiteral("App"), QStringLiteral("hello")));
        QCOMPARE(h.index(0).data(HistoryModel::WhenRole).toString(), QStringLiteral("now"));
        clock = clock.addSecs(7 * 60);
        QCOMPARE(h.index(0).data(HistoryModel::WhenRole).toString(), QStringLiteral("7m ago"));
        QVERIFY(!h.ageTickActiveForTesting());
        h.setPanelOpen(true);
        QVERIFY(h.ageTickActiveForTesting());
        h.setPanelOpen(false);
        QVERIFY(!h.ageTickActiveForTesting());
    }

    /* A folded group is a stack: its newest entry, no header, sheets for
       up to two more; opening it gives a header and every entry. Searching
       or drilling into the app opens it, so no match is hidden under it. */
    void aFoldedGroupIsAStack()
    {
        clearStore();
        HistoryModel h;
        h.setAutoCollapseOver(1);
        for (int i = 0; i < 4; ++i) {
            h.record(note(QStringLiteral("Mail"), QStringLiteral("m%1").arg(i)));
        }
        h.record(note(QStringLiteral("Chat"), QStringLiteral("alone")));
        QCOMPARE(h.rowCount(), 2);   // the chat card, the mail stack
        QCOMPARE(h.index(0).data(HistoryModel::StackDepthRole).toInt(), 0);
        QCOMPARE(h.index(1).data(HistoryModel::StackDepthRole).toInt(), 2);
        QCOMPARE(h.index(1).data(HistoryModel::GroupCountRole).toInt(), 4);
        QCOMPARE(h.index(1).data(HistoryModel::SummaryRole).toString(), QStringLiteral("m3"));
        QVERIFY(h.index(1).data(HistoryModel::GroupStartRole).toBool());
        const QString mail = h.index(1).data(HistoryModel::GroupKeyRole).toString();

        h.setSearch(QStringLiteral("m"));
        QCOMPARE(h.rowCount(), 5);   // header and every match
        h.setSearch(QString());
        QCOMPARE(h.rowCount(), 2);

        h.toggleGroup(mail);
        QCOMPARE(h.rowCount(), 6);   // chat, header, four entries
        QVERIFY(h.index(1).data(HistoryModel::IsHeaderRole).toBool());
        QVERIFY(!h.index(3).data(HistoryModel::GroupStartRole).toBool());
        h.toggleGroup(mail);
        QCOMPARE(h.rowCount(), 2);
    }

    /* CentreStyle=list keeps the old centre's rows exactly: a folded group
       is its header over its newest entry, a repeated single entry keeps a
       header, and a search does not open folded groups. */
    void theListStyleKeepsTheOldRows()
    {
        clearStore();
        HistoryModel h;
        h.setCentreCards(false);
        h.setAutoCollapseOver(1);
        for (int i = 0; i < 4; ++i) {
            h.record(note(QStringLiteral("Mail"), QStringLiteral("m%1").arg(i)));
        }
        h.record(note(QStringLiteral("Beep"), QStringLiteral("again")));
        h.record(note(QStringLiteral("Beep"), QStringLiteral("again")));
        /* Beep: header, entry x2. Mail: header (folded), newest entry. */
        QCOMPARE(h.rowCount(), 4);
        QVERIFY(h.index(0).data(HistoryModel::IsHeaderRole).toBool());
        QVERIFY(h.index(2).data(HistoryModel::IsHeaderRole).toBool());
        QVERIFY(h.index(2).data(HistoryModel::CollapsedRole).toBool());
        QCOMPARE(h.index(3).data(HistoryModel::StackDepthRole).toInt(), 0);
        h.setSearch(QStringLiteral("m"));
        QCOMPARE(h.rowCount(), 2);   // still folded: header, newest
        h.setSearch(QString());
        h.setCentreCards(true);
        QCOMPARE(h.rowCount(), 2);   // the Beep card, the Mail stack
    }

    /* The rest of a popup's group, for opening it on the card: newest
       first, the card's own entry left out, capped, other apps ignored. */
    void groupMembersListsTheRestOfTheGroup()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("Chat"), QStringLiteral("one"), {}, 501));
        h.record(note(QStringLiteral("Other"), QStringLiteral("elsewhere"), {}, 502));
        h.record(note(QStringLiteral("Chat"), QStringLiteral("two"), QStringLiteral("b2"), 503));
        h.record(note(QStringLiteral("Chat"), QStringLiteral("three"), {}, 504));
        const QString key = h.index(0).data(HistoryModel::GroupKeyRole).toString();

        const QVariantList all = h.groupMembers(key, 504, 10);
        QCOMPARE(all.size(), 2);
        QCOMPARE(all.at(0).toMap().value(QStringLiteral("summary")).toString(), QStringLiteral("two"));
        QCOMPARE(all.at(0).toMap().value(QStringLiteral("body")).toString(), QStringLiteral("b2"));
        QCOMPARE(all.at(0).toMap().value(QStringLiteral("id")).toUInt(), 503u);
        QCOMPARE(all.at(1).toMap().value(QStringLiteral("summary")).toString(), QStringLiteral("one"));
        QCOMPARE(all.at(1).toMap().value(QStringLiteral("when")).toString(), QStringLiteral("now"));

        QCOMPARE(h.groupMembers(key, 504, 1).size(), 1);
        QCOMPARE(h.groupMembers(key, 504, 0).size(), 0);
        QCOMPARE(h.groupMembers(QString(), 504, 10).size(), 0);
    }

    /* While the screen is shared the rows are masked as the centre's are. */
    void groupMembersAreMaskedWhileSharing()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("Chat"), QStringLiteral("secret"), QStringLiteral("body"), 601));
        h.record(note(QStringLiteral("Chat"), QStringLiteral("newest"), {}, 602));
        const QString key = h.index(0).data(HistoryModel::GroupKeyRole).toString();

        QSignalSpy changed(&h, &HistoryModel::changed);
        h.setScreenSharing(true, Notification::LockPrivacy::HideContent);
        QVERIFY(changed.count() >= 1);   // an open group re-reads its rows
        const QVariantMap row = h.groupMembers(key, 602, 10).value(0).toMap();
        QCOMPARE(row.value(QStringLiteral("summary")).toString(), QStringLiteral("Notification hidden"));
        QCOMPARE(row.value(QStringLiteral("body")).toString(), QString());

        h.setScreenSharing(false, Notification::LockPrivacy::HideContent);
        QCOMPARE(h.groupMembers(key, 602, 10).value(0).toMap().value(QStringLiteral("summary")).toString(),
                 QStringLiteral("secret"));
    }

    /* A changed percentage is news, not a repeat. */
    void recordKeepsChangedContentSeparate()
    {
        clearStore();
        HistoryModel h;
        for (const QString &pct : {QStringLiteral("12"), QStringLiteral("9"),
                                   QStringLiteral("6")}) {
            h.record(note(QStringLiteral("Battery Monitor"),
                          QStringLiteral("Device Battery Low (%1% Remaining)").arg(pct)));
        }
        QCOMPARE(h.total(), 3);
    }

    /* The single most annoying shape of a full history panel: one app's
       entries pushing everything else off the screen. A group of one arrival
       has no header at all — with one entry the header says nothing the entry
       does not already say. */
    void singleArrivalGetsNoHeader()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("Signal"), QStringLiteral("hello")));
        QCOMPARE(h.rowCount(), 1);
        QVERIFY(!h.index(0).data(HistoryModel::IsHeaderRole).toBool());
    }

    /* The header count is arrivals, which is what a person counts — not the
       number of rows we happened to draw them in. */
    void headerCountsOccurrencesNotRows()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("App"), QStringLiteral("first")));
        h.record(note(QStringLiteral("App"), QStringLiteral("second")));
        h.record(note(QStringLiteral("App"), QStringLiteral("second")));
        h.record(note(QStringLiteral("App"), QStringLiteral("second")));

        QCOMPARE(h.total(), 2);   // two rows
        QVERIFY(h.index(0).data(HistoryModel::IsHeaderRole).toBool());
        QCOMPARE(h.index(0).data(HistoryModel::GroupCountRole).toInt(), 4);
    }

    void historyIgnoreIsNotRecorded()
    {
        clearStore();
        HistoryModel h;
        Notification n = note(QStringLiteral("Steam"), QStringLiteral("friend online"));
        n.historyIgnore = true;
        h.record(n);
        QCOMPARE(h.total(), 0);
        QCOMPARE(h.unread(), 0);
    }

    /* replaces_id is the same message updating itself — a file copy would
       otherwise leave a hundred entries behind. */
    void replacesIdUpdatesInPlace()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("Dolphin"), QStringLiteral("Copying"),
                      QStringLiteral("20%"), 7));
        h.record(note(QStringLiteral("Dolphin"), QStringLiteral("Copying"),
                      QStringLiteral("80%"), 7));
        QCOMPARE(h.total(), 1);
        QCOMPARE(h.index(0).data(HistoryModel::BodyRole).toString(), QStringLiteral("80%"));
    }

    /* The app name has to match as well as the id. History outlives daemon
       restarts and ids do not, so id alone let a fresh notification overwrite
       an unrelated old entry — silently, and in that entry's old position,
       which is what put the file out of time order in the first place. */
    void replacesIdRequiresTheSameApp()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("Dolphin"), QStringLiteral("Copying"),
                      QStringLiteral("20%"), 7));
        h.record(note(QStringLiteral("Signal"), QStringLiteral("hello"),
                      QStringLiteral("there"), 7));
        QCOMPARE(h.total(), 2);
    }

    void capacityTrimsTheOldest()
    {
        clearStore();
        HistoryModel h;
        h.setCapacity(2);
        h.record(note(QStringLiteral("App"), QStringLiteral("one")));
        h.record(note(QStringLiteral("App"), QStringLiteral("two")));
        h.record(note(QStringLiteral("App"), QStringLiteral("three")));

        QCOMPARE(h.total(), 2);
        h.setSearch(QStringLiteral("one"));
        QCOMPARE(h.matchCount(), 0);      // the oldest is the one that went
        h.setSearch(QStringLiteral("three"));
        QCOMPARE(h.matchCount(), 1);
    }

    /* A capacity of zero would be a config typo that emptied history on every
       arrival, so it is clamped rather than obeyed. */
    void capacityIsClamped()
    {
        clearStore();
        HistoryModel h;
        h.setCapacity(0);
        h.record(note(QStringLiteral("App"), QStringLiteral("one")));
        QCOMPARE(h.total(), 1);
    }

    void searchLooksAtAppSummaryAndBody()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("Thunderbird"), QStringLiteral("Invoice"),
                      QStringLiteral("due Friday")));
        h.record(note(QStringLiteral("Signal"), QStringLiteral("hello"),
                      QStringLiteral("about the invoice")));
        h.record(note(QStringLiteral("Steam"), QStringLiteral("Sale"),
                      QStringLiteral("50% off")));

        h.setSearch(QStringLiteral("invoice"));    // summary, body, insensitive
        QCOMPARE(h.matchCount(), 2);

        h.setSearch(QStringLiteral("steam"));      // app name
        QCOMPARE(h.matchCount(), 1);

        h.setSearch(QStringLiteral("nothing here"));
        QCOMPARE(h.matchCount(), 0);
        QCOMPARE(h.rowCount(), 0);

        h.setSearch(QString());
        QCOMPARE(h.matchCount(), 3);
    }

    /* Typing into search while drilled into one app must search everything,
       or it looks like it searched everything and quietly did not. */
    void searchClearsTheGroupFilter()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("Thunderbird"), QStringLiteral("Invoice")));
        h.record(note(QStringLiteral("Signal"), QStringLiteral("Invoice")));

        h.setGroupFilter(QStringLiteral("Signal"));
        QCOMPARE(h.matchCount(), 1);

        h.setSearch(QStringLiteral("invoice"));
        QCOMPARE(h.matchCount(), 2);
        QVERIFY(h.groupFilter().isEmpty());
    }

    /* unread answers "is there anything new", which total() cannot: total is
       the whole backlog and is almost never zero. */
    void unreadStopsCountingWhileTheCentreIsOpen()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("App"), QStringLiteral("one")));
        h.record(note(QStringLiteral("App"), QStringLiteral("two")));
        QCOMPARE(h.unread(), 2);

        h.setPanelOpen(true);
        QCOMPARE(h.unread(), 0);
        h.record(note(QStringLiteral("App"), QStringLiteral("three")));
        QCOMPARE(h.unread(), 0);          // you are looking at it

        h.setPanelOpen(false);
        h.record(note(QStringLiteral("App"), QStringLiteral("four")));
        QCOMPARE(h.unread(), 1);
    }

    /* Closing the centre has to drop the drill-in, or reopening shows one
       app's notifications while looking like it shows all of them. */
    void closingTheCentreResetsFilterAndSearch()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("Signal"), QStringLiteral("hello")));
        h.record(note(QStringLiteral("Steam"), QStringLiteral("sale")));

        h.setPanelOpen(true);
        h.setGroupFilter(QStringLiteral("Signal"));
        h.setPanelOpen(false);

        QVERIFY(h.groupFilter().isEmpty());
        QVERIFY(h.search().isEmpty());
        QCOMPARE(h.matchCount(), 2);
    }

    void clearAllEmptiesEverything()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("App"), QStringLiteral("one")));
        h.record(note(QStringLiteral("App"), QStringLiteral("two")));
        h.clearAll();
        QCOMPARE(h.total(), 0);
        QCOMPARE(h.rowCount(), 0);
    }

    /* Round trip: what save() writes, load() has to read back. The counts on
       deduplicated entries are the part that is easy to drop, because they
       live nowhere but that field. */
    /* An entry's lock-screen wish survives a restart, so the centre still
       masks it while the screen is shared. */
    void lockPrivacySurvivesAReload()
    {
        clearStore();
        {
            HistoryModel h;
            Notification n = note(QStringLiteral("Chat"), QStringLiteral("secret"), QStringLiteral("b"), 701);
            n.lockPrivacy = Notification::LockPrivacy::Hide;
            h.record(n);
            h.record(note(QStringLiteral("Chat"), QStringLiteral("newest"), {}, 702));
            h.save();
        }
        HistoryModel reloaded;
        reloaded.setScreenSharing(true, Notification::LockPrivacy::Show);
        const QString key = reloaded.index(0).data(HistoryModel::GroupKeyRole).toString();
        const QVariantMap row = reloaded.groupMembers(key, 702, 5).value(0).toMap();
        QCOMPARE(row.value(QStringLiteral("summary")).toString(), QStringLiteral("Notification hidden"));
    }

    /* A replaces_id update is the same message changing, not a new one. */
    void anUpdateInPlaceIsNotUnread()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("Copy"), QStringLiteral("10%"), {}, 801));
        h.record(note(QStringLiteral("Copy"), QStringLiteral("50%"), {}, 801));
        h.record(note(QStringLiteral("Copy"), QStringLiteral("100%"), {}, 801));
        QCOMPARE(h.unread(), 1);
    }

    void savedHistoryReloads()
    {
        clearStore();
        {
            HistoryModel h;
            h.record(note(QStringLiteral("Battery Monitor"), QStringLiteral("Low")));
            h.record(note(QStringLiteral("Battery Monitor"), QStringLiteral("Low")));
            h.record(note(QStringLiteral("Signal"), QStringLiteral("hello"),
                          QStringLiteral("there")));
            h.save();
        }

        HistoryModel reloaded;
        QCOMPARE(reloaded.total(), 2);
        reloaded.setSearch(QStringLiteral("Low"));
        QCOMPARE(reloaded.rowCount(), 1);   // one card, x2
        QCOMPARE(reloaded.index(0).data(HistoryModel::RepeatCountRole).toInt(), 2);
    }

    /* A save interrupted part way — power cut, full disk, a kill during the
       write — used to leave a truncated file, because the old one was emptied
       first and then rewritten. The replacement now goes to a new file that
       is renamed over the old one, so a reader sees one or the other, never
       half. A new inode is how that shows from outside. */
    void saveReplacesTheFileRatherThanRewritingIt()
    {
        clearStore();
        HistoryModel h;
        h.record(note(QStringLiteral("App"), QStringLiteral("one")));
        h.save();
        struct stat st1{};
        QCOMPARE(::stat(QFile::encodeName(storePath()).constData(), &st1), 0);

        h.record(note(QStringLiteral("App"), QStringLiteral("two")));
        h.save();
        struct stat st2{};
        QCOMPARE(::stat(QFile::encodeName(storePath()).constData(), &st2), 0);
        QVERIFY(st1.st_ino != st2.st_ino);
    }

    /* A history file that does not parse used to load as empty, and the next
       save — a second and a half after the next notification — wrote the
       empty list over it. Everything was gone with no message. Now the bad
       file is kept beside the new one and the problem is logged. */
    void corruptFileIsSetAsideNotLost()
    {
        clearStore();
        QFile::remove(storePath() + QStringLiteral(".corrupt"));
        const QByteArray damaged = "[{\"id\":1,\"app\":\"App\",\"summary\":\"kept\"},{\"id\":2,\"ap";
        {
            QDir().mkpath(QFileInfo(storePath()).path());
            QFile f(storePath());
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(damaged);
        }

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("could not be read")));
        HistoryModel h;
        QCOMPARE(h.total(), 0);

        QFile kept(storePath() + QStringLiteral(".corrupt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), damaged);
    }
};

GLASSOSD_TEST_MAIN(TestHistoryModel)
#include "tst_historymodel.moc"
