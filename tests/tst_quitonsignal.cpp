/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    SIGTERM reaches the event loop as a signal, so the application quits
    the ordinary way and aboutToQuit (which saves history) runs. Before,
    the default action ended the process on the spot.
*/
#include "quitonsignal.h"

#include <QSignalSpy>
#include <QtTest>

#include <csignal>

class TestQuitOnSignal : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void sigtermIsDeliveredToTheEventLoop()
    {
        QuitOnSignal q;
        QVERIFY(q.isValid());
        QSignalSpy got(&q, &QuitOnSignal::received);
        ::raise(SIGTERM);   // with the default action this test would die here
        QTRY_COMPARE(got.size(), 1);
        QCOMPARE(got.first().at(0).toInt(), int(SIGTERM));
        ::raise(SIGHUP);
        QTRY_COMPARE(got.size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestQuitOnSignal)
#include "tst_quitonsignal.moc"
