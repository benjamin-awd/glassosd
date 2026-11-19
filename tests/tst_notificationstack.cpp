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
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>
#include <QtTest>

#include <QPointer>

#include <functional>

namespace
{
uint g_nextId = 1;

/* image://icon and image://notifyimage are served by the daemon; here any
   request gets a blank square. */
class BlankImages : public QQuickImageProvider
{
public:
    BlankImages() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &, QSize *size, const QSize &requested) override
    {
        const QSize s = requested.isValid() ? requested : QSize(32, 32);
        if (size) {
            *size = s;
        }
        QImage img(s, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::gray);
        return img;
    }
};

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
        m_engine->addImageProvider(QStringLiteral("icon"), new BlankImages);
        m_engine->addImageProvider(QStringLiteral("notifyimage"), new BlankImages);
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

    /* A two-finger trackpad flick arrives as phased horizontal wheel events
       and has no handler velocity. It was released at 0, so a quick flick
       had to travel the whole commit distance; now its speed counts. */
    void aQuickTrackpadFlickDismisses()
    {
        m_model->insert(note(QStringLiteral("flicked")));
        QTRY_COMPARE(cards().size(), 1);
        waitSettled();
        QQuickItem *card = cards().constFirst();
        const qreal commit = card->width() * 0.28;
        const QPointF at = drawn(card).center();
        auto wheel = [&](int dx, Qt::ScrollPhase phase) {
            QWheelEvent e(at, m_win->mapToGlobal(at), QPoint(dx, 0), QPoint(dx * 8, 0),
                          Qt::NoButton, Qt::NoModifier, phase, false);
            QCoreApplication::sendEvent(m_win, &e);
        };
        wheel(0, Qt::ScrollBegin);
        int travelled = 0;
        for (int i = 0; i < 8; ++i) {           // 80px in ~64ms: ~1250 px/s
            wheel(10, Qt::ScrollUpdate);
            travelled += 10;
            QTest::qWait(8);
        }
        QVERIFY(travelled < commit);            // distance alone would not do it
        wheel(0, Qt::ScrollEnd);
        QVERIFY2(card->property("releaseVelocity").toReal() > 600,
                 qPrintable(QStringLiteral("released at %1 px/s").arg(card->property("releaseVelocity").toReal())));
        QTRY_COMPARE_WITH_TIMEOUT(cards().size(), 0, 2000);
    }

    /* The same distance dragged out slowly is not a flick, and springs back. */
    void aSlowTrackpadSwipeSpringsBack()
    {
        m_model->insert(note(QStringLiteral("kept")));
        QTRY_COMPARE(cards().size(), 1);
        waitSettled();
        QQuickItem *card = cards().constFirst();
        const QPointF at = drawn(card).center();
        auto wheel = [&](int dx, Qt::ScrollPhase phase) {
            QWheelEvent e(at, m_win->mapToGlobal(at), QPoint(dx, 0), QPoint(dx * 8, 0),
                          Qt::NoButton, Qt::NoModifier, phase, false);
            QCoreApplication::sendEvent(m_win, &e);
        };
        wheel(0, Qt::ScrollBegin);
        for (int i = 0; i < 8; ++i) {           // 80px over ~400ms: ~200 px/s
            wheel(10, Qt::ScrollUpdate);
            QTest::qWait(50);
        }
        wheel(0, Qt::ScrollEnd);
        QTest::qWait(400);
        QCOMPARE(cards().size(), 1);
        QCOMPARE(m_model->rowCount(), 1);
    }

    /* The close button straddles the card's top-left corner. Its outer half
       was outside the input region, and it hid as soon as the pointer left
       the card for that half. */
    void closeButtonStaysReachableOffTheCardEdge()
    {
        singleton("Appearance")->setProperty("notifyHeader", QStringList{QStringLiteral("icon"), QStringLiteral("time"), QStringLiteral("close")});
        m_model->insert(note(QStringLiteral("closable")));
        QTRY_COMPARE(cards().size(), 1);
        waitSettled();
        const QRectF r = drawn(cards().constFirst());
        QObject *surface = singleton("Surface");
        QTest::mouseMove(m_win, (r.center()).toPoint());
        QTRY_COMPARE(surface->property("inputHolders").toList().size(), 1);
        /* Across the button's inner half to its outer half: up and left of
           the card's corner, on the button and off the card. */
        QTest::mouseMove(m_win, (r.topLeft() + QPointF(3, 3)).toPoint());
        QTest::qWait(20);
        QTest::mouseMove(m_win, (r.topLeft() - QPointF(5, 5)).toPoint());
        QTest::qWait(400);   // past the grace period: its own hover holds it now
        QCOMPARE(surface->property("inputHolders").toList().size(), 1);
        QTest::mouseClick(m_win, Qt::LeftButton, Qt::NoModifier, (r.topLeft() - QPointF(5, 5)).toPoint());
        QTRY_COMPARE_WITH_TIMEOUT(m_model->rowCount(), 0, 2000);
        QTRY_COMPARE(surface->property("inputHolders").toList().size(), 0);
    }

    /* The compositor closes a layer surface whose output goes away, and
       layer-shell-qt closes the window. A card that never expires stayed
       invisible until the next notification; the stack shows itself again. */
    void aStackClosedFromOutsideComesBack()
    {
        m_model->insert(note(QStringLiteral("critical, never expires")));
        QTRY_COMPARE(cards().size(), 1);
        QTRY_VERIFY(m_win->isVisible());
        m_win->close();
        QVERIFY(!m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(m_win->isVisible(), 2000);
        QCOMPARE(cards().size(), 1);
    }

    /* A contact's avatar is drawn round and badged with the sender's icon. */
    void anAvatarCarriesTheSendersBadge()
    {
        Notification n = note(QStringLiteral("Jane"));
        n.appIcon = QStringLiteral("/tmp/avatar.png");
        n.senderIcon = QStringLiteral("org.example.Chat");
        m_model->insert(n);
        QTRY_COMPARE(cards().size(), 1);
        QQuickItem *badge = nullptr;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
            for (QQuickItem *c : it->childItems()) {
                if (c->property("source").toString() == QLatin1String("image://icon/org.example.Chat")) {
                    badge = c;
                }
                walk(c);
            }
        };
        walk(cards().constFirst());
        QVERIFY2(badge, "no badge image");
        QVERIFY(badge->isVisible());
    }

    /* The dwell timer pauses while the pointer is anywhere on the card,
       including on its buttons. A button's own MouseArea took the hover
       from the card's, so resting on an action reported the pointer gone
       and the card could expire under it. */
    void restingOnAnActionKeepsTheCardPaused()
    {
        Notification n = note(QStringLiteral("with an action"));
        n.actions = QStringList{QStringLiteral("reply"), QStringLiteral("Reply")};
        m_model->insert(n);
        QTRY_COMPARE(cards().size(), 1);
        waitSettled();
        QQuickItem *button = nullptr;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
            for (QQuickItem *c : it->childItems()) {
                if (c->property("text").toString() == QLatin1String("Reply")) {
                    button = c->parentItem();
                }
                walk(c);
            }
        };
        walk(cards().constFirst());
        QVERIFY(button);
        const QPointF on = button->mapToScene(QPointF(button->width() / 2, button->height() / 2));
        QTest::mouseMove(m_win, drawn(cards().constFirst()).center().toPoint());
        QTRY_VERIFY(m_model->isHovered(n.id));
        QTest::mouseMove(m_win, on.toPoint());
        QTest::qWait(50);
        QVERIFY2(m_model->isHovered(n.id), "the pointer is on the card's button, but the card was told it left");
    }

    /* Hovering a card swaps its time for Options, which opens the card's
       menu; the cell keeps its width so the row does not reflow. */
    void optionsReplacesTheTimeOnHover()
    {
        m_model->insert(note(QStringLiteral("hover me")));
        QTRY_COMPARE(cards().size(), 1);
        waitSettled();
        QQuickItem *card = cards().constFirst();
        QQuickItem *options = nullptr;
        QQuickItem *menu = nullptr;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
            for (QQuickItem *c : it->childItems()) {
                if (c->property("text").toString() == QLatin1String("Options")) {
                    options = c->parentItem();
                }
                if (c->metaObject()->indexOfProperty("atX") >= 0) {
                    menu = c;
                }
                walk(c);
            }
        };
        walk(card);
        QVERIFY(options && menu);
        const qreal cellWidth = options->parentItem()->width();
        QVERIFY(!options->isVisible());
        QTest::mouseMove(m_win, drawn(card).center().toPoint());
        QTRY_COMPARE(options->opacity(), 1.0);
        QCOMPARE(options->parentItem()->width(), cellWidth);
        const QPoint at = options->mapToScene(QPointF(options->width() / 2, options->height() / 2)).toPoint();
        QTest::mouseMove(m_win, at);
        QTest::mouseClick(m_win, Qt::LeftButton, Qt::NoModifier, at);
        QTRY_VERIFY(menu->property("open").toBool());
        QCOMPARE(m_model->rowCount(), 1);   // the click opened the menu, it did not dismiss
    }

    /* A body longer than the card shows offers a chevron on hover that
       opens it in place; the card grows and is not dismissed. */
    void aLongBodyExpandsInPlace()
    {
        Notification n = note(QStringLiteral("long"));
        n.body = QStringLiteral("word ").repeated(200);
        m_model->insert(n);
        QTRY_COMPARE(cards().size(), 1);
        waitSettled();
        QQuickItem *card = cards().constFirst();
        QQuickItem *expander = nullptr;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
            for (QQuickItem *c : it->childItems()) {
                if (c->property("text").toString() == QStringLiteral("\u203A")) {
                    expander = c->parentItem();
                }
                walk(c);
            }
        };
        walk(card);
        QVERIFY(expander);
        QVERIFY(!expander->isVisible());
        QTest::mouseMove(m_win, drawn(card).center().toPoint());
        QTRY_VERIFY(expander->isVisible());
        const qreal before = card->height();
        const QPoint at = expander->mapToScene(QPointF(expander->width() / 2, expander->height() / 2)).toPoint();
        QTest::mouseMove(m_win, at);
        QTest::mouseClick(m_win, Qt::LeftButton, Qt::NoModifier, at);
        QTRY_VERIFY(card->property("expanded").toBool());
        QTRY_VERIFY(card->height() > before + 20);
        QCOMPARE(m_model->rowCount(), 1);
    }

    /* A group opens in place the way macOS opens a stack: a click on the
       stacked card (or "Show N more") folds the sheets away, a header with
       the app's name, Show less and a clear button appears above the card,
       and each earlier notification becomes a card of its own below it,
       read from history. A member card's click goes to history's
       activation; Show less closes the group again. Nothing is dismissed and
       the centre is not opened. */
    void aGroupOpensInPlace()
    {
        m_model->setCoalesce(3, 20000);
        for (int i = 0; i < 3; ++i) {
            Notification n = note(QStringLiteral("chat"));
            n.appName = QStringLiteral("Chat");
            m_model->insert(n);
        }
        QTRY_COMPARE(cards().size(), 1);
        waitSettled();
        QQuickItem *card = cards().constFirst();
        QCOMPARE(card->property("stackDepth").toInt(), 2);
        QCOMPARE(card->property("shownDepth").toInt(), 2);

        QObject *history = singleton("HistoryModel");
        history->setProperty("members", QVariantList{
            QVariantMap{{QStringLiteral("id"), 41}, {QStringLiteral("summary"), QStringLiteral("Second message")},
                        {QStringLiteral("body"), QStringLiteral("hi again")}, {QStringLiteral("iconSource"), QString()},
                        {QStringLiteral("when"), QStringLiteral("now")}, {QStringLiteral("repeatCount"), 1}},
            QVariantMap{{QStringLiteral("id"), 40}, {QStringLiteral("summary"), QStringLiteral("First message")},
                        {QStringLiteral("body"), QString()}, {QStringLiteral("iconSource"), QString()},
                        {QStringLiteral("when"), QStringLiteral("1m ago")}, {QStringLiteral("repeatCount"), 3}},
        });

        auto findText = [&](const QString &text) -> QQuickItem * {
            QQuickItem *found = nullptr;
            std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
                for (QQuickItem *c : it->childItems()) {
                    if (!found && c->property("text").toString() == text && c->isVisible()) {
                        found = c;
                    }
                    walk(c);
                }
            };
            walk(card);
            return found;
        };
        auto click = [&](QQuickItem *item) {
            const QPoint at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
            QTest::mouseMove(m_win, at);
            QTest::mouseClick(m_win, Qt::LeftButton, Qt::NoModifier, at);
        };
        auto settledOpen = [&](qreal target) {
            QTRY_COMPARE(card->property("groupProgress").toReal(), target);
        };

        QVERIFY(findText(QStringLiteral("Show 2 more")));
        const qreal closedHeight = card->height();

        /* A click on the stacked card itself opens it. */
        QTest::mouseMove(m_win, drawn(card).center().toPoint());
        QTest::mouseClick(m_win, Qt::LeftButton, Qt::NoModifier, drawn(card).center().toPoint());
        QTRY_VERIFY(card->property("groupShown").toBool());
        settledOpen(1.0);
        QCOMPARE(card->property("shownDepth").toInt(), 0);
        QVERIFY(!findText(QStringLiteral("Show 2 more")));   // Show less is in the header now
        QVERIFY(card->height() > closedHeight + 60);
        QCOMPARE(m_model->rowCount(), 1);                     // opening dismissed nothing

        QQuickItem *title = findText(QStringLiteral("Chat"));
        QQuickItem *less = findText(QStringLiteral("Show less"));
        QQuickItem *second = findText(QStringLiteral("Second message"));
        QVERIFY(title && less && second);
        QVERIFY(findText(QStringLiteral("First message  ×3")));
        /* Order down the screen: header, the top card's text, then members. */
        const qreal titleY = title->mapToScene(QPointF(0, 0)).y();
        const qreal secondY = second->mapToScene(QPointF(0, 0)).y();
        QVERIFY(titleY < secondY);
        QVERIFY(secondY > card->mapToScene(QPointF(0, card->property("cardTop").toReal())).y() + 20);
        /* Both members are cards of their own: in the window's regions. */
        QVERIFY(holders() >= 5);   // card, two members, Show less, clear

        click(second);
        QTRY_COMPARE(history->property("lastActivated").toInt(), 41);
        QCOMPARE(history->property("lastGroup").toString(), QString());   // the centre stayed shut
        QCOMPARE(m_model->rowCount(), 1);

        click(less);
        QTRY_VERIFY(!card->property("groupShown").toBool());
        settledOpen(0.0);
        QCOMPARE(card->property("shownDepth").toInt(), 2);
        QTRY_COMPARE(card->property("groupMembers").toList().size(), 0);
        QTRY_COMPARE(card->height(), closedHeight);
        QVERIFY(!findText(QStringLiteral("Show less")));

        /* "Show N more" opens it too. */
        click(findText(QStringLiteral("Show 2 more")));
        QTRY_VERIFY(card->property("groupShown").toBool());
    }

    /* More members than the card shows: the rest are a link to the centre. */
    void aBigGroupLinksToTheCentre()
    {
        m_model->setCoalesce(3, 20000);
        for (int i = 0; i < 9; ++i) {
            Notification n = note(QStringLiteral("mail"));
            n.appName = QStringLiteral("Mail");
            m_model->insert(n);
        }
        QTRY_COMPARE(cards().size(), 1);
        waitSettled();
        QQuickItem *card = cards().constFirst();
        QVariantList members;
        for (int i = 0; i < 8; ++i) {
            members << QVariantMap{{QStringLiteral("id"), 100 + i},
                                   {QStringLiteral("summary"), QStringLiteral("Mail %1").arg(i)},
                                   {QStringLiteral("body"), QString()}, {QStringLiteral("iconSource"), QString()},
                                   {QStringLiteral("when"), QStringLiteral("now")}, {QStringLiteral("repeatCount"), 1}};
        }
        singleton("HistoryModel")->setProperty("members", members);
        card->setProperty("groupOpen", true);
        QTRY_COMPARE(card->property("groupProgress").toReal(), 1.0);
        QCOMPARE(card->property("groupMembers").toList().size(), 5);
        QQuickItem *link = nullptr;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
            for (QQuickItem *c : it->childItems()) {
                if (c->property("text").toString() == QStringLiteral("3 more in Notification Centre")) {
                    link = c;
                }
                walk(c);
            }
        };
        walk(card);
        QVERIFY(link);
        const QPoint at = link->mapToScene(QPointF(link->width() / 2, link->height() / 2)).toPoint();
        QTest::mouseMove(m_win, at);
        QTest::mouseClick(m_win, Qt::LeftButton, Qt::NoModifier, at);
        QTRY_COMPARE(singleton("HistoryModel")->property("lastGroup").toString(), QStringLiteral("Mail"));
    }

    /* A short body offers nothing to expand. */
    void aShortBodyHasNoChevron()
    {
        m_model->insert(note(QStringLiteral("short")));
        QTRY_COMPARE(cards().size(), 1);
        waitSettled();
        QQuickItem *card = cards().constFirst();
        QTest::mouseMove(m_win, drawn(card).center().toPoint());
        QTest::qWait(50);
        bool shown = false;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
            for (QQuickItem *c : it->childItems()) {
                if (c->property("text").toString() == QStringLiteral("\u203A")) {
                    shown = c->parentItem()->isVisible();
                }
                walk(c);
            }
        };
        walk(card);
        QVERIFY(!shown);
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
