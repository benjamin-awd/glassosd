/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    The popup stack, rendered offscreen: the real NotificationStack.qml and
    NotificationCard.qml driven by the real NotificationModel.

    Every bug in the model named in a commit message has a test in
    tst_notificationmodel; the QML bugs had none, and they were most of them —
    cards popping in instead of sliding, ghosts left by exits, blur regions
    leaked by a destroyed card, a stack that failed to load at all. The parts
    that need a compositor (layer shell, blur, input regions) are behind the
    Surface singleton, which the test replaces with a stand-in that records
    which panels hold a region (tests/qml/stubs).

    Any QML warning fails the test it was raised in. That alone catches a
    file that does not load (bc200f0: a Timer inside a WheelHandler took the
    whole daemon down) and binding loops (3e2b275).
*/
#include "notificationmodel.h"

#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

#include <QPointer>

#include <functional>

namespace
{
uint g_nextId = 1;

Notification note(const QString &summary, const QString &app = QStringLiteral("app"))
{
    Notification n;
    n.id = g_nextId++;
    n.appName = app + QString::number(n.id);   // distinct, so nothing coalesces
    n.summary = summary;
    n.body = QStringLiteral("body ") + QString::number(n.id);
    return n;
}
} // namespace

class TestNotificationStack : public QObject
{
    Q_OBJECT

    NotificationModel *m_model = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
    QQuickWindow *m_win = nullptr;
    QStringList m_warnings;

    QObject *singleton(const char *name)
    {
        return m_engine->singletonInstance<QObject *>("org.glassosd.ui", name);
    }

    /* Repeater delegates are not QObject children of the window, so they are
       found by walking the item tree for the card's own properties. Ordered
       top to bottom on screen. */
    QList<QQuickItem *> cards()
    {
        QList<QQuickItem *> out;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
            for (QQuickItem *c : it->childItems()) {
                const QMetaObject *mo = c->metaObject();
                if (mo->indexOfProperty("leaving") >= 0 && mo->indexOfProperty("reveal") >= 0
                    && mo->indexOfProperty("slot") >= 0) {
                    out << c;
                } else {
                    walk(c);
                }
            }
        };
        walk(m_win->contentItem());
        std::sort(out.begin(), out.end(), [](QQuickItem *a, QQuickItem *b) { return a->y() < b->y(); });
        return out;
    }

    /* Where the card is drawn: its layout position plus the transform the
       stack and its own motion apply. */
    QRectF drawn(QQuickItem *card) const
    {
        return card->mapRectToScene(QRectF(0, 0, card->width(), card->height()));
    }

    int holders() { return singleton("Surface")->property("holders").toList().size(); }

    /* Theme values as a theme file would set them. Shorter motion than any
       real theme keeps the suite fast; tests about motion set their own. */
    void setTheme(const QVariantMap &values)
    {
        QObject *theme = singleton("Theme");
        theme->setProperty("values", values);
        theme->setProperty("revision", theme->property("revision").toInt() + 1);
    }

    void waitSettled()
    {
        QTRY_VERIFY_WITH_TIMEOUT(
            [this] {
                for (QQuickItem *c : cards()) {
                    if (c->property("reveal").toReal() != 1.0 || c->property("slot").toReal() != 1.0) {
                        return false;
                    }
                }
                return true;
            }(),
            3000);
    }

private Q_SLOTS:
    void init()
    {
        m_warnings.clear();
        m_model = new NotificationModel;
        m_model->setCoalesce(0, 0);
        m_model->setTimeouts(0, 0, 0);
        qmlRegisterSingletonInstance("org.glassosd.ui", 1, 0, "NotificationModel", m_model);

        m_engine = new QQmlApplicationEngine;
        m_engine->addImportPath(QStringLiteral(GLASSOSD_QML_TEST_IMPORTS));
        connect(m_engine, &QQmlEngine::warnings, this, [this](const QList<QQmlError> &list) {
            for (const QQmlError &e : list) {
                m_warnings << e.toString();
            }
        });
        m_engine->loadData(QByteArrayLiteral("import QtQuick\nimport org.glassosd.ui\nNotificationStack {}\n"));
        QVERIFY2(!m_engine->rootObjects().isEmpty(), qPrintable(m_warnings.join(u'\n')));
        m_win = qobject_cast<QQuickWindow *>(m_engine->rootObjects().constFirst());
        QVERIFY(m_win);
        setTheme({{QStringLiteral("motion.in"), 120}, {QStringLiteral("motion.out"), 80}});
    }

    void cleanup()
    {
        delete m_engine;
        m_engine = nullptr;
        delete m_model;
        m_model = nullptr;
        m_win = nullptr;
        /* Ignored: the idle-time plugin is absent offscreen, and says so. */
        m_warnings.removeIf([](const QString &w) { return w.contains(QLatin1String("idletime")); });
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join(u'\n')));
    }

    void firstCardAppearsAndRevealsFully()
    {
        m_model->insert(note(QStringLiteral("one")));
        QTRY_COMPARE(cards().size(), 1);
        QVERIFY(m_win->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(cards().constFirst()->property("reveal").toReal(), 1.0, 3000);
    }

    /* da10a41: a card arriving into a window already on screen must animate.
       Revealed inline, before its Behavior existed, it popped in at full
       size. */
    void secondCardSlidesInRatherThanPopping()
    {
        m_model->insert(note(QStringLiteral("one")));
        waitSettled();
        m_model->insert(note(QStringLiteral("two")));
        QTRY_COMPARE(cards().size(), 2);
        QQuickItem *arriving = nullptr;
        for (QQuickItem *c : cards()) {
            if (c->property("reveal").toReal() < 1.0) {
                arriving = c;
            }
        }
        QVERIFY2(arriving, "the new card was fully revealed on its first frame (popped in)");
        QTRY_COMPARE_WITH_TIMEOUT(arriving->property("reveal").toReal(), 1.0, 3000);
    }

    /* A removal while another card arrives leaves no ghost delegate, no card
       stuck half-way, and no panel still holding a blur region. */
    void removalDuringArrivalLeavesNoGhost()
    {
        m_model->insert(note(QStringLiteral("one")));
        m_model->insert(note(QStringLiteral("two")));
        QTRY_COMPARE(cards().size(), 2);
        m_model->dismiss(m_model->index(0).data(NotificationModel::IdRole).toUInt());
        m_model->insert(note(QStringLiteral("three")));
        QTRY_COMPARE_WITH_TIMEOUT(m_model->rowCount(), 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(cards().size(), 2, 3000);
        waitSettled();
        for (QQuickItem *c : cards()) {
            QCOMPARE(c->property("leaving").toBool(), false);
        }
        m_model->dismissAll();
        QTRY_COMPARE_WITH_TIMEOUT(cards().size(), 0, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(holders(), 0, 3000);
        QTRY_VERIFY(!m_win->isVisible());
    }

    /* ff29b19: a card destroyed without finishing its exit (Clear all, an
       app replacing its notification, a reset) must still give its region
       back, or the leftover blurs a patch of backdrop where nothing is drawn.
       exitMs 0 removes the row at once, so no exit animation ever runs. */
    void aCardDestroyedMidExitGivesItsRegionBack()
    {
        m_model->insert(note(QStringLiteral("one")));
        m_model->insert(note(QStringLiteral("two")));
        waitSettled();
        QVERIFY(holders() > 0);
        m_model->setExitMs(0);
        m_model->dismissAll();
        QTRY_COMPARE_WITH_TIMEOUT(cards().size(), 0, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(holders(), 0, 3000);
    }

    /* 7da5a1e: when a card leaves, the cards below it glide up into the gap
       while it leaves; they used to hold still and then jump a whole card
       height in one frame when the row was removed. Sampled every event-loop
       turn, no card below may move by more than a fraction of a card per
       frame. */
    void cardsBelowALeavingCardGlideRatherThanJump()
    {
        setTheme({{QStringLiteral("motion.in"), 120}, {QStringLiteral("motion.out"), 300}});
        for (int i = 0; i < 3; ++i) {
            m_model->insert(note(QStringLiteral("card %1").arg(i)));
        }
        QTRY_COMPARE(cards().size(), 3);
        waitSettled();
        const QList<QQuickItem *> before = cards();
        QQuickItem *bottom = before.last();
        const qreal cardH = bottom->height();
        /* Sampled at each frame's sync, while the GUI thread is blocked:
           that is the state the frame is drawn from, after polish has run
           the layout. Sampling between event-loop turns instead catches
           half-updated states no frame ever shows. */
        QList<qreal> ys;
        QPointer<QQuickItem> watched(bottom);
        auto conn = connect(m_win, &QQuickWindow::beforeSynchronizing, this, [&] {
            if (watched && watched->window()) {
                ys << drawn(watched).y();
            }
        }, Qt::DirectConnection);
        m_model->dismiss(m_model->index(0).data(NotificationModel::IdRole).toUInt());
        QTest::qWait(600);
        disconnect(conn);
        QVERIFY(ys.size() > 5);
        qreal worst = 0;
        for (int i = 1; i < ys.size(); ++i) {
            worst = qMax(worst, qAbs(ys.at(i) - ys.at(i - 1)));
        }
        QVERIFY2(worst < cardH * 0.5,
                 qPrintable(QStringLiteral("the bottom card jumped %1px in one step (card %2px)")
                                .arg(worst).arg(cardH)));
    }

    /* A card arriving at the top while the bottom one leaves: the card in
       between is pushed down by the arrival with nothing above it leaving,
       and must glide down rather than jump. */
    void arrivalDuringAnExitDoesNotJump()
    {
        setTheme({{QStringLiteral("motion.in"), 400}, {QStringLiteral("motion.out"), 250},
                  {QStringLiteral("motion.slideTravel"), 362},
                  {QStringLiteral("motion.curveIn"), QStringLiteral("0.35,0.85,0.35,1")},
                  {QStringLiteral("motion.curveOut"), QStringLiteral("0.15,0.05,0.30,1")}});
        m_model->insert(note(QStringLiteral("leaving")));
        m_model->insert(note(QStringLiteral("staying")));
        QTRY_COMPARE(cards().size(), 2);
        waitSettled();
        QQuickItem *staying = cards().constFirst();      // newest, at the top
        const qreal cardH = staying->height();
        QList<qreal> ys;
        QPointer<QQuickItem> watched(staying);
        auto conn = connect(m_win, &QQuickWindow::beforeSynchronizing, this, [&] {
            if (watched && watched->window()) {
                ys << drawn(watched).y();
            }
        }, Qt::DirectConnection);
        m_model->dismiss(m_model->index(1).data(NotificationModel::IdRole).toUInt());
        QTest::qWait(20);
        m_model->insert(note(QStringLiteral("arriving")));
        QTest::qWait(700);
        disconnect(conn);
        QVERIFY(ys.size() > 5);
        qreal worst = 0;
        QStringList trace;
        for (int i = 1; i < ys.size(); ++i) {
            worst = qMax(worst, qAbs(ys.at(i) - ys.at(i - 1)));
            trace << QString::number(ys.at(i), 'f', 0);
        }
        QVERIFY2(worst < cardH * 0.3,
                 qPrintable(QStringLiteral("the card jumped %1px in one frame: %2")
                                .arg(worst).arg(trace.join(u' '))));
        /* And it went down, by a card and a gap, not up. */
        QVERIFY(ys.last() > ys.first() + cardH * 0.9);
    }

    /* "Clear all" appears with the third live card and is gone with them:
       it was counted by rows, so it outlived cards still fading out. */
    void clearAllFollowsLiveCards()
    {
        QObject *pill = nullptr;
        for (int i = 0; i < 3; ++i) {
            m_model->insert(note(QStringLiteral("card %1").arg(i)));
        }
        waitSettled();
        std::function<QObject *(QQuickItem *)> find = [&](QQuickItem *it) -> QObject * {
            for (QQuickItem *c : it->childItems()) {
                if (c->metaObject()->indexOfProperty("held") >= 0 && c->metaObject()->indexOfProperty("waiting") >= 0) {
                    return c;
                }
                if (QObject *o = find(c)) {
                    return o;
                }
            }
            return nullptr;
        };
        pill = find(m_win->contentItem());
        QVERIFY(pill);
        QTRY_VERIFY_WITH_TIMEOUT(pill->property("shown").toBool(), 3000);
        m_model->dismissAll();
        QCOMPARE(pill->property("shown").toBool(), false);
    }

    /* Three live cards for an instant, as when one arrives just as another
       expires, is not a pile. Shown at once, the pill flashed up under the
       stack for ~100 ms and vanished (live capture, per-panel regions). */
    void clearAllDoesNotFlashForAMomentaryThirdCard()
    {
        m_model->insert(note(QStringLiteral("a")));
        m_model->insert(note(QStringLiteral("b")));
        waitSettled();
        QObject *pill = nullptr;
        std::function<QObject *(QQuickItem *)> find = [&](QQuickItem *it) -> QObject * {
            for (QQuickItem *c : it->childItems()) {
                if (c->metaObject()->indexOfProperty("held") >= 0 && c->metaObject()->indexOfProperty("waiting") >= 0) {
                    return c;
                }
                if (QObject *o = find(c)) {
                    return o;
                }
            }
            return nullptr;
        };
        pill = find(m_win->contentItem());
        QVERIFY(pill);
        bool everShown = false;
        auto conn = connect(m_win, &QQuickWindow::afterAnimating, this, [&] {
            everShown = everShown || pill->property("shown").toBool();
        });
        m_model->insert(note(QStringLiteral("c")));
        QTest::qWait(50);
        m_model->dismiss(m_model->index(m_model->rowCount() - 1).data(NotificationModel::IdRole).toUInt());
        QTest::qWait(500);
        disconnect(conn);
        QVERIFY2(!everShown, "Clear all flashed up for a momentary third card");
    }

    /* A committed swipe carries on at the speed it was let go. It used to
       fly a fixed 250 ms OutCubic plus the exit slide, so its first frame
       moved at ~8900 px/s whatever the finger did. */
    void aThrownCardKeepsTheReleaseVelocity_data()
    {
        QTest::addColumn<qreal>("velocity");
        QTest::newRow("slow release") << qreal(400);
        QTest::newRow("ordinary flick") << qreal(1200);
        QTest::newRow("hard flick") << qreal(6000);
    }
    void aThrownCardKeepsTheReleaseVelocity()
    {
        QFETCH(qreal, velocity);
        setTheme({{QStringLiteral("motion.in"), 120}, {QStringLiteral("motion.out"), 250},
                  {QStringLiteral("motion.slideTravel"), 362}});
        m_model->setExitMs(250);
        m_model->insert(note(QStringLiteral("swiped")));
        QTRY_COMPARE(cards().size(), 1);
        waitSettled();
        QQuickItem *card = cards().constFirst();
        card->setProperty("swipeRaw", 150.0);
        QTRY_COMPARE_WITH_TIMEOUT(card->property("swipeX").toReal(), 150.0, 2000);
        card->setProperty("releaseVelocity", velocity);
        QMetaObject::invokeMethod(card, "endSwipe");

        /* The flight's starting speed, from its duration and curve. */
        const qreal ms = card->property("throwMs").toReal();
        const QVariantList curve = card->property("throwCurve").toList();
        const qreal dist = card->width() + 2 * singleton("Style")->property("shadowPad").toReal() - 150.0;
        QVERIFY(ms > 0 && ms <= 250);
        const qreal start = 3 * curve.at(1).toReal() * dist / ms * 1000;
        QVERIFY2(start > velocity * 0.7 && start < velocity * 1.4,
                 qPrintable(QStringLiteral("released at %1 px/s, thrown at %2 px/s").arg(velocity).arg(start)));

        /* And it is off the screen before its row is removed. */
        QPointer<QQuickItem> watched(card);
        qreal lastLeft = 0;
        auto conn = connect(m_win, &QQuickWindow::beforeSynchronizing, this, [&] {
            if (watched && watched->window()) {
                lastLeft = drawn(watched).left();
            }
        }, Qt::DirectConnection);
        QTRY_COMPARE_WITH_TIMEOUT(cards().size(), 0, 2000);
        disconnect(conn);
        QVERIFY2(lastLeft >= m_win->width(),
                 qPrintable(QStringLiteral("last drawn at x=%1 in a %2px window").arg(lastLeft).arg(m_win->width())));
    }

    /* e099271: a settings change re-sends the position. With a sliding theme
       the right margin is folded into the layout, and passing it to the
       layer shell as well pushed the surface inward on every change. */
    void positionChangeDoesNotReapplyTheFoldedMargin()
    {
        setTheme({{QStringLiteral("motion.slideTravel"), 362}});
        QObject *appearance = singleton("Appearance");
        appearance->setProperty("notifyMarginX", 16);
        QMetaObject::invokeMethod(appearance, "changed");
        const QVariantList pos = singleton("Surface")->property("lastPosition").toList();
        QCOMPARE(pos.size(), 5);
        QCOMPARE(pos.at(2).toInt(), 0);   // right margin: folded into the layout
    }
};

QTEST_MAIN(TestNotificationStack)
#include "tst_notificationstack.moc"
