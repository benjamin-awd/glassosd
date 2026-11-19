/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    The centre's volume and brightness controls, against stand-in wpctl and
    brightnessctl scripts on PATH. They used to run on the UI thread and wait
    for each command; they are asynchronous now, so these check that readings
    arrive, that a slow tool does not block the caller, and that a slider
    drag is coalesced into few writes.
*/
#include "systemcontrols.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class TestSystemControls : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QByteArray m_oldPath;

    void tool(const QString &name, const QByteArray &body)
    {
        QFile f(m_dir.filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("#!/bin/sh\n" + body);
        f.close();
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
    }
    QString log() const
    {
        QFile f(m_dir.filePath(QStringLiteral("calls")));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_oldPath = qgetenv("PATH");
        /* The stand-ins only, plus what /bin/sh needs: no real wpctl or
           pactl can be found and nothing reaches the desktop's audio. */
        qputenv("PATH", (m_dir.path() + QStringLiteral(":/usr/bin:/bin")).toUtf8());
    }
    void cleanupTestCase() { qputenv("PATH", m_oldPath); }

    void readingsArriveWithoutBlocking()
    {
        const QByteArray logPath = m_dir.filePath(QStringLiteral("calls")).toUtf8();
        tool(QStringLiteral("wpctl"),
             "echo \"wpctl $*\" >> " + logPath + "\n"
             "case \"$1\" in get-volume) sleep 0.2; echo 'Volume: 0.42 [MUTED]';; esac\n");
        tool(QStringLiteral("brightnessctl"),
             "echo \"brightnessctl $*\" >> " + logPath + "\n"
             "[ \"$1\" = -m ] && echo 'intel_backlight,backlight,48000,25%,192000'\n");

        QElapsedTimer t;
        t.start();
        SystemControls c;
        QVERIFY2(t.elapsed() < 150, "constructing waited for the tools");
        QTRY_VERIFY(c.volumeAvailable());
        QCOMPARE(c.volume(), 0.42);
        QVERIFY(c.muted());
        QTRY_VERIFY(c.brightnessAvailable());
        QCOMPARE(c.brightness(), 0.25);
    }

    /* A drag sends the first value at once and then only the latest one
       reached while that ran, not one command per step. */
    void aDragIsCoalesced()
    {
        const QByteArray logPath = m_dir.filePath(QStringLiteral("calls")).toUtf8();
        QFile::remove(QString::fromUtf8(logPath));
        tool(QStringLiteral("wpctl"),
             "echo \"wpctl $*\" >> " + logPath + "\n"
             "case \"$1\" in set-volume) sleep 0.1;; get-volume) echo 'Volume: 0.10';; esac\n");
        SystemControls c;
        QTRY_VERIFY(c.volumeAvailable());
        QElapsedTimer t;
        t.start();
        for (int i = 1; i <= 20; ++i) {
            c.setVolume(i / 100.0);
        }
        QVERIFY2(t.elapsed() < 100, "setVolume waited for wpctl");
        QCOMPARE(c.volume(), 0.20);
        QTRY_VERIFY(log().contains(QStringLiteral("set-volume @DEFAULT_AUDIO_SINK@ 20%")));
        QTest::qWait(250);
        QVERIFY(log().count(QStringLiteral("set-volume")) <= 3);
    }
};

QTEST_GUILESS_MAIN(TestSystemControls)
#include "tst_systemcontrols.moc"
