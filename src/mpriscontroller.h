/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    MPRIS2 media player, for the notification centre.

    swaync's control centre can host a player widget and ours could not. This
    watches the session bus for org.mpris.MediaPlayer2.* and drives whichever
    player is currently active.
*/
#pragma once

#include <QDBusServiceWatcher>
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <QQmlEngine>
#include <QString>

class QDBusInterface;

class MprisController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString artist READ artist NOTIFY changed)
    Q_PROPERTY(QString artUrl READ artUrl NOTIFY changed)
    Q_PROPERTY(bool playing READ playing NOTIFY changed)
    Q_PROPERTY(QString identity READ identity NOTIFY changed)

public:
    explicit MprisController(QObject *parent = nullptr);

    bool available() const { return !m_service.isEmpty(); }
    QString title() const { return m_title; }
    QString artist() const { return m_artist; }
    QString artUrl() const { return m_artUrl; }
    bool playing() const { return m_playing; }
    QString identity() const { return m_identity; }

    Q_INVOKABLE void playPause();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();

Q_SIGNALS:
    void changed();

private Q_SLOTS:
    /* Connected by name from the PropertiesChanged signal, so it has to be a
       real slot rather than a plain member. */
    void refresh();
    void scheduleRefresh();

public:
    /* Metadata as it came off the bus, demarshalled only if it is the a{sv}
       the spec says it is. Exposed for the tests. */
    static QVariantMap parseMetadata(const QVariant &value);

private:
    void findPlayer();
    void call(const QString &method);
    void applyPlayerProperties(const QVariantMap &props);

    QDBusServiceWatcher m_watcher;
    QString m_service;
    QString m_title;
    QString m_artist;
    QString m_artUrl;
    QString m_identity;
    bool m_playing = false;
    /* Every bus read is asynchronous; a reply for a request that has since
       been superseded is dropped by generation. */
    quint64 m_generation = 0;       // refresh() reads
    quint64 m_findGeneration = 0;   // findPlayer(); separate, or a refresh
                                    // landing mid-discovery discarded it
    QTimer m_refreshDebounce;
};
