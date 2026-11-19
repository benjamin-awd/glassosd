/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "colorscheme.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace
{
constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kIface = "org.freedesktop.portal.Settings";
constexpr auto kNamespace = "org.freedesktop.appearance";
constexpr auto kKey = "color-scheme";

/* ReadOne answers v, the deprecated Read answered v-in-v, and a signal's
   value arrives as a QDBusVariant: peel until something that is not one. */
QVariant unwrap(QVariant v)
{
    for (int i = 0; i < 4 && v.canConvert<QDBusVariant>(); ++i) {
        v = v.value<QDBusVariant>().variant();
    }
    return v;
}
} // namespace

ColorScheme::ColorScheme(QObject *parent)
    : QObject(parent)
{
    auto bus = QDBusConnection::sessionBus();
    bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface),
                QStringLiteral("SettingChanged"), this,
                SLOT(onSettingChanged(QString, QString, QDBusVariant)));

    QDBusMessage call = QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath),
                                                       QLatin1String(kIface), QStringLiteral("ReadOne"));
    call.setArguments({QLatin1String(kNamespace), QLatin1String(kKey)});
    /* Asynchronous: the portal is D-Bus activated and can take a moment to
       start at login, and the daemon must not stall waiting for it. */
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *w;
        if (reply.isError()) {
            return;   // no portal: stay dark, as glassosd always was
        }
        apply(reply.value().variant());
    });
}

bool ColorScheme::prefersDark()
{
    return s_dark;
}

void ColorScheme::onSettingChanged(const QString &ns, const QString &key, const QDBusVariant &value)
{
    if (ns == QLatin1String(kNamespace) && key == QLatin1String(kKey)) {
        apply(value.variant());
    }
}

void ColorScheme::apply(const QVariant &value)
{
    /* 0 is "no preference", which is not a request for light. */
    const bool dark = unwrap(value).toUInt() != 2;
    if (dark == s_dark) {
        return;
    }
    s_dark = dark;
    qInfo("glassosd: desktop colour scheme is now %s", dark ? "dark" : "light");
    Q_EMIT changed();
}
