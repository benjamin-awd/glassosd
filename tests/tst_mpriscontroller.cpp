/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    The MPRIS player widget's reads, against fake players on a private bus.

    Any program on the session bus can claim org.mpris.MediaPlayer2.*, so the
    player is as untrusted as a notification sender. Two ways one hurt the
    daemon: Metadata of the wrong shape was demarshalled as a{sv} regardless
    (an abort inside libdbus), and every read was a blocking call on the GUI
    thread, so a player that never answered froze animations and Notify
    replies for half a second per read.

    Needs the throwaway bus ctest provides (tst_mpriscontroller_bus); skips
    otherwise rather than taking names on a desktop session's bus.
*/
#include "mpriscontroller.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QtTest>

using StringMap = QMap<QString, QString>;

namespace
{
/* A player that answers Properties.Get/GetAll with whatever it is given, or
   never answers at all. */
class FakePlayer : public QDBusVirtualObject
{
public:
    QVariant metadata;
    bool silent = false;
    QList<QDBusMessage> held;   // the calls a silent player never answers

    QString introspect(const QString &) const override { return {}; }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() != QLatin1String("org.freedesktop.DBus.Properties")) {
            return false;
        }
        if (silent) {
            message.setDelayedReply(true);
            held << message;
            return true;
        }
        const QString member = message.member();
        const QString iface = message.arguments().value(0).toString();
        if (member == QLatin1String("GetAll")) {
            QVariantMap props;
            if (iface == QLatin1String("org.mpris.MediaPlayer2.Player")) {
                props.insert(QStringLiteral("PlaybackStatus"), QStringLiteral("Playing"));
                props.insert(QStringLiteral("Metadata"), metadata);
            }
            connection.send(message.createReply(QVariant::fromValue(props)));
            return true;
        }
        if (member == QLatin1String("Get")) {
            const QString name = message.arguments().value(1).toString();
            QVariant v;
            if (name == QLatin1String("PlaybackStatus")) {
                v = QStringLiteral("Playing");
            } else if (name == QLatin1String("Identity")) {
                v = QStringLiteral("Fake Player");
            } else if (name == QLatin1String("Metadata")) {
                v = metadata;
            }
            connection.send(message.createReply(QVariant::fromValue(QDBusVariant(v))));
            return true;
        }
        return false;
    }
};

#define REQUIRE_PRIVATE_BUS()                                                                  \
    if (!qEnvironmentVariableIsSet("GLASSOSD_PRIVATE_BUS"))                                    \
    QSKIP("needs the private session bus ctest provides (tst_mpriscontroller_bus)")
} // namespace

class TestMprisController : public QObject
{
    Q_OBJECT

    /* The player is on a connection of its own, as a separate process would
       be, so its replies travel through the bus rather than a local loop. */
    QDBusConnection playerBus() { return QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("player")); }

    void publish(FakePlayer *player)
    {
        QDBusConnection bus = playerBus();
        QVERIFY(bus.isConnected());
        QVERIFY(bus.registerVirtualObject(QStringLiteral("/org/mpris/MediaPlayer2"), player));
        QVERIFY(bus.registerService(QStringLiteral("org.mpris.MediaPlayer2.fake")));
    }

    void unpublish()
    {
        QDBusConnection bus = playerBus();
        bus.unregisterService(QStringLiteral("org.mpris.MediaPlayer2.fake"));
        bus.unregisterObject(QStringLiteral("/org/mpris/MediaPlayer2"));
    }

private Q_SLOTS:
    void initTestCase()
    {
        qDBusRegisterMetaType<StringMap>();
    }

    void cleanup()
    {
        unpublish();
    }

    void wellFormedMetadataIsRead()
    {
        REQUIRE_PRIVATE_BUS();
        FakePlayer player;
        QVariantMap meta;
        meta.insert(QStringLiteral("xesam:title"), QStringLiteral("A Song"));
        meta.insert(QStringLiteral("xesam:artist"), QStringList{QStringLiteral("One"), QStringLiteral("Two")});
        player.metadata = QVariant::fromValue(meta);
        publish(&player);

        MprisController mpris;
        QTRY_COMPARE_WITH_TIMEOUT(mpris.title(), QStringLiteral("A Song"), 3000);
        QVERIFY(mpris.playing());
        QTRY_COMPARE(mpris.identity(), QStringLiteral("Fake Player"));
    }

    /* Metadata as a{ss}. Read as a{sv} it aborted the daemon. */
    void wrongShapeMetadataIsIgnored()
    {
        REQUIRE_PRIVATE_BUS();
        FakePlayer player;
        StringMap meta;
        meta.insert(QStringLiteral("xesam:title"), QStringLiteral("not a variant"));
        player.metadata = QVariant::fromValue(meta);
        publish(&player);

        MprisController mpris;
        QTRY_VERIFY_WITH_TIMEOUT(mpris.playing(), 3000);   // the rest of the reply still counts
        QCOMPARE(mpris.title(), QString());
    }

    /* A player that never answers costs the GUI thread nothing. */
    void aSilentPlayerDoesNotBlock()
    {
        REQUIRE_PRIVATE_BUS();
        FakePlayer player;
        player.silent = true;
        publish(&player);

        QElapsedTimer t;
        t.start();
        MprisController mpris;
        for (int i = 0; i < 5; ++i) {
            QMetaObject::invokeMethod(&mpris, "refresh");
        }
        QVERIFY2(t.elapsed() < 100, qPrintable(QStringLiteral("blocked for %1 ms").arg(t.elapsed())));
        QTRY_VERIFY_WITH_TIMEOUT(!player.held.isEmpty(), 3000);   // it was asked
    }
};

QTEST_GUILESS_MAIN(TestMprisController)
#include "tst_mpriscontroller.moc"
