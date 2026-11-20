/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QDir>
#include <QHash>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

/* Resolve a desktop-entry id to the name and icon in its .desktop file.

   xdg-desktop-portal forwards a sandboxed app's notification with an empty
   app_name and app_icon, so everything arriving that way — which is every
   Flatpak — would otherwise be an anonymous card. The desktop-entry hint is
   usually still present and is enough to recover both. */
struct DesktopInfo {
    QString name;
    QString icon;
};

inline DesktopInfo lookupDesktopEntry(const QString &entry)
{
    if (entry.isEmpty()) {
        return {};
    }
    /* Cached: a burst from one app would otherwise stat the whole XDG data
       path once per message. */
    static QHash<QString, DesktopInfo> cache;
    const auto cached = cache.constFind(entry);
    if (cached != cache.constEnd()) {
        return *cached;
    }

    QString id = entry;
    if (id.endsWith(QLatin1String(".desktop"))) {
        id.chop(8);
    }

    QString path = QStandardPaths::locate(QStandardPaths::ApplicationsLocation,
                                          id + QStringLiteral(".desktop"));
    if (path.isEmpty()) {
        /* Flatpaks install under their full reverse-DNS id but some apps send
           only the last component, so fall back to a suffix match. */
        for (const QString &dir : QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation)) {
            const QStringList hits =
                QDir(dir).entryList({QStringLiteral("*") + id + QStringLiteral(".desktop")}, QDir::Files);
            if (!hits.isEmpty()) {
                path = dir + QLatin1Char('/') + hits.first();
                break;
            }
        }
    }

    DesktopInfo info;
    if (!path.isEmpty()) {
        QSettings df(path, QSettings::IniFormat);
        df.beginGroup(QStringLiteral("Desktop Entry"));
        info.name = df.value(QStringLiteral("Name")).toString();
        info.icon = df.value(QStringLiteral("Icon")).toString();
    }
    cache.insert(entry, info);
    return info;
}

/* The card's image://icon URL for a notification's own icon.

   A themed name gets the app's desktop-entry icon attached as "?app=", so the
   provider can swap a monochrome glyph for the app's colour icon when a theme
   asks for that (macOS always shows the app's icon, never a generic glyph).
   Files and the entry's own icon need no fallback and get none. QML removes
   the parameter again for themes that keep the glyph: see Icons.themed(). */
inline QString notificationIconUrl(const QString &appIcon, const QString &desktopEntry)
{
    if (appIcon.isEmpty()) {
        return {};
    }
    QString url = QStringLiteral("image://icon/") + appIcon;
    const bool themedName = !QDir::isAbsolutePath(appIcon) && !appIcon.startsWith(QLatin1String("file:"));
    if (themedName && !desktopEntry.isEmpty()) {
        const QString app = lookupDesktopEntry(desktopEntry).icon;
        if (!app.isEmpty() && app != appIcon && !app.contains(QLatin1Char('?'))) {
            url += QStringLiteral("?app=") + app;
        }
    }
    return url;
}
