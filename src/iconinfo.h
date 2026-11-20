/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "iconprovider.h"

#include <QObject>
#include <QQmlEngine>

/* What the icon provider will draw for a URL, asked ahead of time. A card
   needs to know when a notification's glyph is replaced by its app's colour
   icon (notify.appIconFirst): the glyph would be badged with that same app
   icon and masked round as a "picture", and the app icon alone needs
   neither. */
class IconInfo : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    using QObject::QObject;

    Q_INVOKABLE bool prefersApp(const QString &url) const { return IconProvider::prefersApp(url); }
};
