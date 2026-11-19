/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    The notification centre's keyboard, rendered offscreen: the real
    HistoryPanel.qml over the real HistoryModel, with the compositor-facing
    singletons stood in for as tst_notificationstack does (tests/qml/stubs,
    plus tests/qml/centre for the centre's own).

    The centre had keyboard interactivity for a long time with nothing inside
    it ever holding focus, so none of this could be reached: Esc worked only
    once the search field had been clicked, typing went nowhere, and there
    was no way to move between entries at all. These tests drive it the way
    a person would, by key presses at the window, and check what the model
    was asked to do.

    As in the stack's tests, any QML warning fails the test it was raised in.
*/
#include "historymodel.h"
#include "testenv.h"

#include <QQmlApplicationEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest>

#include <functional>

namespace
{
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

Notification note(uint id, const QString &app, const QString &summary)
{
    Notification n;
    n.id = id;
    n.appName = app;
    n.summary = summary;
    n.body = QStringLiteral("body ") + QString::number(id);
    n.received = QDateTime::currentDateTimeUtc();
    return n;
}
} // namespace

class TestHistoryPanel : public QObject
{
    Q_OBJECT

    HistoryModel *m_model = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
    QQuickWindow *m_win = nullptr;
    QStringList m_warnings;

    QQuickItem *find(const char *prop)
    {
        std::function<QQuickItem *(QQuickItem *)> walk = [&](QQuickItem *it) -> QQuickItem * {
            for (QQuickItem *c : it->childItems()) {
                if (c->metaObject()->indexOfProperty(prop) >= 0) {
                    return c;
                }
                if (QQuickItem *f = walk(c)) {
                    return f;
                }
            }
            return nullptr;
        };
        return walk(m_win->contentItem());
    }

    /* The ListView, found by the cursor property the navigation adds. */
    QQuickItem *list() { return find("cursorKey"); }
    int cursor() { return list()->property("cursor").toInt(); }
    QQuickItem *focused() { return m_win->activeFocusItem(); }
    bool searchFocused()
    {
        QQuickItem *f = focused();
        return f && QByteArray(f->metaObject()->className()).startsWith("QQuickTextInput");
    }

    /* Rows drawn as the keyboard's current one. */
    int highlighted()
    {
        int n = 0;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
            for (QQuickItem *c : it->childItems()) {
                if (c->metaObject()->indexOfProperty("current") >= 0 && c->property("current").toBool()) {
                    ++n;
                }
                walk(c);
            }
        };
        walk(m_win->contentItem());
        return n;
    }

    void key(int k) { QTest::keyClick(m_win, Qt::Key(k)); }
    /* QTest has keyClicks for widgets only. */
    void type(const QString &text)
    {
        for (const QChar c : text) {
            QTest::keyClick(m_win, c.toLatin1());
        }
    }

    void open()
    {
        m_model->setPanelOpen(true);
        QTRY_VERIFY(m_win->isVisible());
        m_win->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(m_win));
        QTRY_VERIFY(list() && list()->property("count").toInt() == m_model->rowCount());
    }

private Q_SLOTS:
    void init()
    {
        m_warnings.clear();
        m_model = new HistoryModel;
        m_model->clearAll();
        /* Rows, newest group first:
             0  chat entry        (a group of one: no header)
             1  Mail header
             2  mail two
             3  mail one */
        m_model->record(note(1, QStringLiteral("Mail"), QStringLiteral("mail one")));
        m_model->record(note(2, QStringLiteral("Mail"), QStringLiteral("mail two")));
        m_model->record(note(3, QStringLiteral("Chat"), QStringLiteral("chat")));
        QCOMPARE(m_model->rowCount(), 4);
        qmlRegisterSingletonInstance("org.glassosd.ui", 1, 0, "HistoryModel", m_model);

        m_engine = new QQmlApplicationEngine;
        m_engine->addImportPath(QStringLiteral(GLASSOSD_QML_TEST_IMPORTS));
        m_engine->addImageProvider(QStringLiteral("icon"), new BlankImages);
        m_engine->addImageProvider(QStringLiteral("notifyimage"), new BlankImages);
        connect(m_engine, &QQmlEngine::warnings, this, [this](const QList<QQmlError> &list) {
            for (const QQmlError &e : list) {
                m_warnings << e.toString();
            }
        });
        m_engine->loadData(QByteArrayLiteral("import QtQuick\nimport org.glassosd.ui\nHistoryPanel {}\n"));
        QVERIFY2(!m_engine->rootObjects().isEmpty(), qPrintable(m_warnings.join(u'\n')));
        m_win = qobject_cast<QQuickWindow *>(m_engine->rootObjects().constFirst());
        QVERIFY(m_win);
        /* The layer shell sizes the real surface to the screen; offscreen
           nothing does, and a zero-sized list creates no delegates. */
        m_win->resize(1000, 900);
        open();
    }

    void cleanup()
    {
        delete m_engine;
        m_engine = nullptr;
        delete m_model;
        m_model = nullptr;
        m_win = nullptr;
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join(u'\n')));
    }

    void openingFocusesSearchSoTypingSearches()
    {
        QVERIFY(searchFocused());
        type(QStringLiteral("mail"));
        QCOMPARE(m_model->search(), QStringLiteral("mail"));
        QCOMPARE(m_model->rowCount(), 3);
    }

    void escClearsSearchThenCloses()
    {
        type(QStringLiteral("xyz"));
        QCOMPARE(m_model->search(), QStringLiteral("xyz"));
        key(Qt::Key_Escape);
        QCOMPARE(m_model->search(), QString());
        QVERIFY(m_model->panelOpen());
        key(Qt::Key_Escape);
        QVERIFY(!m_model->panelOpen());
    }

    /* Esc is the panel's, not the field's: it has to work from the list. */
    void escWorksFromTheList()
    {
        key(Qt::Key_Down);
        QVERIFY(!searchFocused());
        key(Qt::Key_Escape);
        QVERIFY(!m_model->panelOpen());
    }

    /* Someone using the mouse alone never sees it; one arrow key and it is
       on exactly one row. */
    void highlightOnlyOnceAKeyIsPressed()
    {
        QVERIFY(!list()->property("keyNav").toBool());
        QCOMPARE(highlighted(), 0);
        key(Qt::Key_Down);
        QVERIFY(list()->property("keyNav").toBool());
        QCOMPARE(cursor(), 0);
        QCOMPARE(highlighted(), 1);
        key(Qt::Key_Down);
        QCOMPARE(highlighted(), 1);
        key(Qt::Key_Up);
        key(Qt::Key_Up);
        QCOMPARE(highlighted(), 0);   // back in the search field
    }

    void downThenReturnActivatesAndCloses()
    {
        QSignalSpy activated(m_model, &HistoryModel::entryActivated);
        key(Qt::Key_Down);
        key(Qt::Key_Return);
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.at(0).at(0).toUInt(), 3u);   // the chat entry
        QVERIFY(!m_model->panelOpen());
    }

    void upFromFirstRowReturnsToSearch()
    {
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        QCOMPARE(cursor(), 1);
        key(Qt::Key_Up);
        key(Qt::Key_Up);
        QVERIFY(searchFocused());
        QCOMPARE(cursor(), -1);
    }

    /* Deleting "mail two" leaves Mail a group of one, so its header goes
       too and every row below moves: the cursor lands on what took its
       place, and a second Delete carries on through the list. */
    void deleteRemovesAndKeepsAUsableCursor()
    {
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        QCOMPARE(cursor(), 2);
        key(Qt::Key_Delete);
        QCOMPARE(m_model->total(), 2);
        QCOMPARE(m_model->rowCount(), 2);
        QCOMPARE(cursor(), 1);
        QCOMPARE(m_model->rowInfo(1).value(QStringLiteral("id")).toUInt(), 1u);
        key(Qt::Key_Backspace);   // Backspace too, away from the search field
        QCOMPARE(m_model->total(), 1);
        QCOMPARE(cursor(), 0);
    }

    /* Delete over a header does nothing: clearing a whole group is the
       header's own button, not a key pressed while passing over it. */
    void deleteOnHeaderIsIgnored()
    {
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        key(Qt::Key_Delete);
        QCOMPARE(m_model->total(), 3);
    }

    void returnOnHeaderTogglesTheGroup()
    {
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        key(Qt::Key_Return);
        QVERIFY(m_model->rowInfo(1).value(QStringLiteral("collapsed")).toBool());
        QVERIFY(m_model->panelOpen());
        QCOMPARE(cursor(), 1);
        key(Qt::Key_Return);
        QVERIFY(!m_model->rowInfo(1).value(QStringLiteral("collapsed")).toBool());
    }

    /* Left from an entry folds its group and moves to the header; a second
       Left does not unfold it again; Right unfolds. */
    void leftRightFoldFromHeaderOrEntry()
    {
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        QCOMPARE(cursor(), 3);
        key(Qt::Key_Left);
        QVERIFY(m_model->rowInfo(1).value(QStringLiteral("collapsed")).toBool());
        QCOMPARE(cursor(), 1);
        key(Qt::Key_Left);
        QVERIFY(m_model->rowInfo(1).value(QStringLiteral("collapsed")).toBool());
        key(Qt::Key_Right);
        QVERIFY(!m_model->rowInfo(1).value(QStringLiteral("collapsed")).toBool());
        key(Qt::Key_Right);
        QVERIFY(!m_model->rowInfo(1).value(QStringLiteral("collapsed")).toBool());
    }

    /* A notification arriving renumbers every row; the highlight stays on
       the entry it was on. */
    void cursorFollowsItsEntryAcrossARebuild()
    {
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        QCOMPARE(m_model->rowInfo(cursor()).value(QStringLiteral("id")).toUInt(), 2u);
        m_model->record(note(4, QStringLiteral("Other"), QStringLiteral("new")));
        QCOMPARE(m_model->rowInfo(cursor()).value(QStringLiteral("id")).toUInt(), 2u);
    }

    void typingOnTheListGoesToSearch()
    {
        key(Qt::Key_Down);
        type(QStringLiteral("ch"));
        QVERIFY(searchFocused());
        QCOMPARE(m_model->search(), QStringLiteral("ch"));
    }
};

GLASSOSD_TEST_MAIN(TestHistoryPanel)
#include "tst_historypanel.moc"
