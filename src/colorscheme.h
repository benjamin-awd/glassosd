/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    Whether the desktop prefers dark or light, for Theme=auto.

    Read from the freedesktop Settings portal's org.freedesktop.appearance
    color-scheme (1 dark, 2 light, 0 no preference), which Plasma's portal
    derives from the active colour scheme and announces as SettingChanged
    when it switches. Every desktop with a portal answers the same question
    the same way, so nothing here is Plasma-specific.

    The answer is process-wide and static: Appearance and Theme are QML
    singletons the engine constructs, and both only need to read it when they
    reload. main.cpp reloads them on changed().
*/
#pragma once

#include <QDBusVariant>
#include <QObject>

class ColorScheme : public QObject
{
    Q_OBJECT

public:
    explicit ColorScheme(QObject *parent = nullptr);

    /* True until the portal says light, so a session without a portal keeps
       glassosd's dark default. */
    static bool prefersDark();
    static void setPrefersDarkForTesting(bool dark) { s_dark = dark; }

Q_SIGNALS:
    void changed();

private Q_SLOTS:
    void onSettingChanged(const QString &ns, const QString &key, const QDBusVariant &value);

private:
    void apply(const QVariant &value);
    static inline bool s_dark = true;
};
