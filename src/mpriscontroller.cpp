/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "mpriscontroller.h"

#include <QUrl>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusReply>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QStringList>

#include <memory>

namespace
{
constexpr const char *kPath = "/org/mpris/MediaPlayer2";
constexpr const char *kPlayerIface = "org.mpris.MediaPlayer2.Player";
constexpr const char *kRootIface = "org.mpris.MediaPlayer2";
constexpr const char *kPrefix = "org.mpris.MediaPlayer2.";

/* Properties.GetAll / Get, asynchronously. Everything here used to be a
   blocking call with a 500 ms timeout on the GUI thread, three or four per
   refresh, and a refresh ran on every PropertiesChanged from any sender on
   /org/mpris/MediaPlayer2: a hung player (a frozen browser tab) stalled
   animations and Notify replies for seconds at a time. */
QDBusPendingCall propertiesCall(const QString &service, const QString &method,
                                const QString &iface, const QString &name = {})
{
    QDBusMessage msg = QDBusMessage::createMethodCall(service, QLatin1String(kPath),
                                                      QStringLiteral("org.freedesktop.DBus.Properties"),
                                                      method);
    if (name.isEmpty()) {
        msg << iface;
    } else {
        msg << iface << name;
    }
    return QDBusConnection::sessionBus().asyncCall(msg, 2000);
}

/* A variant off the bus, unwrapped from its QDBusVariant. */
QVariant unwrap(QVariant v)
{
    if (v.canConvert<QDBusVariant>()) {
        v = v.value<QDBusVariant>().variant();
    }
    return v;
}

/* Reads an a{sv} only if it is one. Streaming a QDBusArgument as a type it
   does not hold is not an error Qt reports: it asserts inside libdbus, or
   reads garbage. Any player on the session bus can send a Metadata of the
   wrong shape (a{ss}, say), which is the same hole db02ce0 closed for
   image-data. */
QVariantMap readStringVariantMap(const QVariant &v)
{
    if (!v.canConvert<QDBusArgument>()) {
        return v.toMap();
    }
    const QDBusArgument arg = v.value<QDBusArgument>();
    if (arg.currentSignature() != QLatin1String("a{sv}")) {
        return {};
    }
    QVariantMap out;
    arg.beginMap();
    while (!arg.atEnd()) {
        QString key;
        QDBusVariant value;
        arg.beginMapEntry();
        arg >> key >> value;
        arg.endMapEntry();
        out.insert(key, value.variant());
    }
    arg.endMap();
    return out;
}
} // namespace

QVariantMap MprisController::parseMetadata(const QVariant &value)
{
    return readStringVariantMap(unwrap(value));
}

MprisController::MprisController(QObject *parent)
    : QObject(parent)
{
    /* Watch for players coming and going rather than polling. */
    m_watcher.setConnection(QDBusConnection::sessionBus());
    m_watcher.setWatchMode(QDBusServiceWatcher::WatchForOwnerChange);
    m_watcher.addWatchedService(QStringLiteral("org.mpris.MediaPlayer2*"));
    connect(&m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] {
        findPlayer();
    });

    /* Property changes arrive as a standard PropertiesChanged signal, from
       any sender on the path. Coalesced: a player that sends a burst of them
       (position, volume, metadata in turn) costs one round of reads. */
    m_refreshDebounce.setSingleShot(true);
    m_refreshDebounce.setInterval(30);
    connect(&m_refreshDebounce, &QTimer::timeout, this, &MprisController::refresh);
    QDBusConnection::sessionBus().connect(QString(), QLatin1String(kPath),
                                          QStringLiteral("org.freedesktop.DBus.Properties"),
                                          QStringLiteral("PropertiesChanged"),
                                          this, SLOT(scheduleRefresh()));
    findPlayer();
}

void MprisController::scheduleRefresh()
{
    m_refreshDebounce.start();
}

void MprisController::findPlayer()
{
    const quint64 gen = ++m_findGeneration;
    auto *namesWatcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().interface()->asyncCall(QStringLiteral("ListNames")), this);
    connect(namesWatcher, &QDBusPendingCallWatcher::finished, this, [this, gen](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (gen != m_findGeneration) {
            return;
        }
        const QDBusPendingReply<QStringList> reply = *w;
        QStringList players;
        if (!reply.isError()) {
            for (const QString &name : reply.value()) {
                if (name.startsWith(QLatin1String(kPrefix))) {
                    players << name;
                }
            }
        }
        if (players.isEmpty()) {
            m_service.clear();
            refresh();
            return;
        }
        /* Prefer a player that is actually playing over one merely open:
           ask them all at once and take the first that says so, or the last
           listed once every one has answered (the old order). */
        auto pending = std::make_shared<int>(players.size());
        auto decided = std::make_shared<bool>(false);
        const QString fallback = players.last();
        for (const QString &name : std::as_const(players)) {
            auto *pw = new QDBusPendingCallWatcher(
                propertiesCall(name, QStringLiteral("Get"), QLatin1String(kPlayerIface),
                               QStringLiteral("PlaybackStatus")), this);
            connect(pw, &QDBusPendingCallWatcher::finished, this,
                    [this, gen, name, pending, decided, fallback](QDBusPendingCallWatcher *p) {
                p->deleteLater();
                --*pending;
                if (gen != m_findGeneration || *decided) {
                    return;
                }
                const QDBusMessage reply = p->reply();
                const bool playing = reply.type() == QDBusMessage::ReplyMessage
                    && !reply.arguments().isEmpty()
                    && unwrap(reply.arguments().constFirst()).toString() == QLatin1String("Playing");
                if (playing || *pending == 0) {
                    *decided = true;
                    m_service = playing ? name : fallback;
                    refresh();
                }
            });
        }
    });
}

void MprisController::refresh()
{
    const quint64 gen = ++m_generation;
    if (m_service.isEmpty()) {
        m_title.clear();
        m_artist.clear();
        m_artUrl.clear();
        m_identity.clear();
        m_playing = false;
        Q_EMIT changed();
        return;
    }

    const QString service = m_service;
    auto *pw = new QDBusPendingCallWatcher(
        propertiesCall(service, QStringLiteral("GetAll"), QLatin1String(kPlayerIface)), this);
    connect(pw, &QDBusPendingCallWatcher::finished, this, [this, gen](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (gen != m_generation) {
            return;
        }
        const QDBusMessage reply = w->reply();
        QVariantMap props;
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            /* GetAll is a{sv} by definition, but from a sender we do not
               control the signature is checked all the same. */
            props = readStringVariantMap(reply.arguments().constFirst());
        }
        applyPlayerProperties(props);
    });

    auto *iw = new QDBusPendingCallWatcher(
        propertiesCall(service, QStringLiteral("Get"), QLatin1String(kRootIface),
                       QStringLiteral("Identity")), this);
    connect(iw, &QDBusPendingCallWatcher::finished, this, [this, gen](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (gen != m_generation) {
            return;
        }
        const QDBusMessage reply = w->reply();
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            m_identity = unwrap(reply.arguments().constFirst()).toString();
            Q_EMIT changed();
        }
    });
}

void MprisController::applyPlayerProperties(const QVariantMap &props)
{
    m_playing = props.value(QStringLiteral("PlaybackStatus")).toString() == QLatin1String("Playing");
    const QVariantMap meta = parseMetadata(props.value(QStringLiteral("Metadata")));
    m_title = meta.value(QStringLiteral("xesam:title")).toString();
    /* xesam:artist is a list; join it rather than showing only the first. */
    m_artist = meta.value(QStringLiteral("xesam:artist")).toStringList().join(QStringLiteral(", "));
    /* The player chooses this URL and the centre loads it, so only schemes
       that mean "a picture": a file, the web, or inline data. Anything else
       (qrc:, image://notifyimage/..., an image provider's own scheme) would
       let a player point the centre at glassosd's internals; dropped, the
       chip shows its placeholder.

       Remote art is kept, knowingly. It is a fetch the player asked for, to
       a host the player picked, and it is what Spotify and every browser
       send (https://i.scdn.co/..., the page's own artwork). Refusing it
       would cost the art of most players to save a request the player could
       make itself. It only happens while the centre is open. */
    const QString art = meta.value(QStringLiteral("mpris:artUrl")).toString();
    const QString scheme = QUrl(art).scheme().toLower();
    m_artUrl = (scheme == QLatin1String("file") || scheme == QLatin1String("https")
                || scheme == QLatin1String("http") || scheme == QLatin1String("data"))
                   ? art : QString();
    Q_EMIT changed();
}

void MprisController::call(const QString &method)
{
    if (m_service.isEmpty()) {
        return;
    }
    QDBusInterface player(m_service, QLatin1String(kPath), QLatin1String(kPlayerIface),
                          QDBusConnection::sessionBus());
    player.asyncCall(method);
}

void MprisController::playPause() { call(QStringLiteral("PlayPause")); }
void MprisController::next()      { call(QStringLiteral("Next")); }
void MprisController::previous()  { call(QStringLiteral("Previous")); }
