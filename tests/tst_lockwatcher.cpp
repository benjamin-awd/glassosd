/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    The lock state, against a fake org.freedesktop.ScreenSaver on a private
    bus. Needs the throwaway bus ctest provides (tst_lockwatcher_bus).
*/
#include "lockwatcher.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QtTest>

namespace
{
/* Answers GetActive with `active`, but only when told to. */
class FakeScreenSaver : public QDBusVirtualObject
{
public:
    bool active = false;
    QList<QDBusMessage> pending;
    QDBusConnection bus{QString()};

    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.member() != QLatin1String("GetActive")) {
            return false;
        }
        bus = connection;
        message.setDelayedReply(true);
        pending << message;
        return true;
    }
    void answer()
    {
        for (const QDBusMessage &m : std::as_const(pending)) {
            bus.send(m.createReply(active));
        }
        pending.clear();
    }
};

#define REQUIRE_PRIVATE_BUS()                                                                  \
    if (!qEnvironmentVariableIsSet("GLASSOSD_PRIVATE_BUS"))                                    \
    QSKIP("needs the private session bus ctest provides (tst_lockwatcher_bus)")
} // namespace

class TestLockWatcher : public QObject
{
    Q_OBJECT

    QDBusConnection saverBus() { return QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("saver")); }

private Q_SLOTS:
    /* Until the service answers, the session counts as locked: a daemon
       restarted while locked showed full content to anything arriving
       before the reply. */
    void lockedUntilTheServiceAnswers()
    {
        REQUIRE_PRIVATE_BUS();
        FakeScreenSaver saver;
        QDBusConnection bus = saverBus();
        QVERIFY(bus.registerVirtualObject(QStringLiteral("/org/freedesktop/ScreenSaver"), &saver));
        QVERIFY(bus.registerService(QStringLiteral("org.freedesktop.ScreenSaver")));

        LockWatcher lock;
        QVERIFY(lock.available());
        QVERIFY(lock.locked());
        QTRY_COMPARE(saver.pending.size(), 1);
        saver.active = false;
        saver.answer();
        QTRY_VERIFY(!lock.locked());

        bus.unregisterService(QStringLiteral("org.freedesktop.ScreenSaver"));
        bus.unregisterObject(QStringLiteral("/org/freedesktop/ScreenSaver"));
    }

    /* No lock service at all: unlocked, as before. */
    void noServiceMeansUnlocked()
    {
        REQUIRE_PRIVATE_BUS();
        LockWatcher lock;
        QVERIFY(!lock.available());
        QVERIFY(!lock.locked());
    }
};

QTEST_GUILESS_MAIN(TestLockWatcher)
#include "tst_lockwatcher.moc"
