/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    Resolves "image://icon/audio-volume-high" against the icon theme.

    Phase 1 only needs the standard audio/brightness/keyboard icons, which
    Breeze always ships, so plain QIcon::fromTheme (which follows the theme's
    own Inherits= chain down to hicolor) is enough. The Papirus-Dark leg of the
    dunstrc chain exists for third-party *application* icons and is therefore a
    Phase 3 concern, handled when notifications arrive.
*/
#pragma once

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QImageReader>
#include <QQuickImageProvider>
#include <QSet>
#include <QUrl>

class IconProvider : public QQuickImageProvider
{
public:
    IconProvider()
        : QQuickImageProvider(QQuickImageProvider::Pixmap)
    {
    }

    /* "<name or path>?fg=RRGGBB": recolour the icon to that colour if it
       turns out to be a single flat colour.

       The icon theme is the desktop's, not ours, and the two need not agree
       on light and dark. With breeze-dark as the desktop theme every Breeze
       monochrome icon (mail-message, the envelope Thunderbird sends) is drawn
       near-white, and on a light glassosd card it vanished: white on white.
       The mirror case — Breeze light under a dark glassosd — painted a dark
       glyph on a dark card. Neither name says "symbolic", so the QML cannot
       know to tint it; only the pixels say so.

       So the decision is made here, on what was actually loaded. Artwork has
       more than one colour and is returned untouched; a glyph has one, and is
       repainted in the colour the surface asked for. The colour is part of
       the URL, so a theme change is a new URL and the image cache cannot hand
       back the old tint. */
    QPixmap requestPixmap(const QString &id, QSize *size, const QSize &requested) override
    {
        /* Parameters ride on the end of the id: "?app=" first (from the
           model), then "?fg=" or "&fg=" (from Icons.themed()). Peeled off
           from the right, so a name or path is never split. */
        QString base = id;
        const QString fgHex = takeParam(base, QStringLiteral("fg"));
        const QString app = takeParam(base, QStringLiteral("app"));
        const QColor fg = fgHex.isEmpty() ? QColor() : QColor(QLatin1Char('#') + fgHex);

        /* "?app=<the sender's desktop-entry icon>": the notification named a
           monochrome glyph, and the theme would rather show the app that sent
           it, in colour, as macOS does. Decided by the same cached test the
           card uses (prefersApp), so the two never disagree. */
        if (!app.isEmpty() && isGlyph(base) && isArtwork(app)) {
            return resolve(app, size, requested, /*fallback=*/false);
        }

        QPixmap pm = resolve(base, size, requested);
        if (pm.isNull() || !fg.isValid()) {
            return pm;
        }
        QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
        if (!isFlatColour(img)) {
            return pm;
        }

        const QRgb rgb = fg.rgb() & 0x00ffffff;
        for (int y = 0; y < img.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                line[x] = (line[x] & 0xff000000) | rgb;
            }
        }
        QPixmap out = QPixmap::fromImage(img);
        out.setDevicePixelRatio(pm.devicePixelRatio());
        return out;
    }

    /* Removes "?key=value" or "&key=value" from the end of id and returns
       value, or returns empty and leaves id alone. */
    static QString takeParam(QString &id, const QString &key)
    {
        for (const QChar sep : {QLatin1Char('&'), QLatin1Char('?')}) {
            const QString mark = sep + key + QLatin1Char('=');
            const qsizetype at = id.lastIndexOf(mark);
            if (at < 0) {
                continue;
            }
            const QString value = id.mid(at + mark.size());
            if (value.contains(QLatin1Char('&')) || value.contains(QLatin1Char('?'))) {
                continue;   // not the last parameter: leave it for a later call
            }
            id.truncate(at);
            return value;
        }
        return {};
    }

    /* Whether an image://icon URL carrying "?app=" will be drawn as the
       app's icon rather than its own: its own resolves to a one-colour glyph
       (mail-message, view-calendar-upcoming-events) and the app's to colour
       artwork. An app whose own icon is a glyph, or is missing, keeps the
       glyph the notification sent. The card asks too (IconInfo), so it can
       drop the badge and the avatar mask that the glyph alone would get. */
    static bool prefersApp(const QString &url)
    {
        QString base = url;
        if (base.startsWith(QLatin1String("image://icon/"))) {
            base.remove(0, 13);
        }
        takeParam(base, QStringLiteral("fg"));
        const QString app = takeParam(base, QStringLiteral("app"));
        return !app.isEmpty() && isGlyph(base) && isArtwork(app);
    }

    /* Both cached per name for the process: rendering an icon to look at its
       pixels is cheap once and pointless twice, and icon themes do not
       change under a running daemon often enough to matter. */
    static bool isGlyph(const QString &name) { return classify(name) == Kind::Glyph; }
    static bool isArtwork(const QString &name) { return classify(name) == Kind::Artwork; }

    /* One colour, give or take antialiasing: every pixel that is more than
       faintly visible lies within a small box in RGB. A full-colour icon
       fails at once; so does a monochrome glyph with an accent (a blue
       unread dot), which is then shown as drawn rather than flattened. */
    static bool isFlatColour(const QImage &img)
    {
        int lo[3] = {255, 255, 255};
        int hi[3] = {0, 0, 0};
        bool any = false;
        for (int y = 0; y < img.height(); ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const QRgb p = line[x];
                if (qAlpha(p) < 96) {
                    continue;
                }
                any = true;
                const int c[3] = {qRed(p), qGreen(p), qBlue(p)};
                for (int i = 0; i < 3; ++i) {
                    lo[i] = qMin(lo[i], c[i]);
                    hi[i] = qMax(hi[i], c[i]);
                }
            }
        }
        constexpr int spread = 40;
        return any && hi[0] - lo[0] <= spread && hi[1] - lo[1] <= spread && hi[2] - lo[2] <= spread;
    }

private:
    enum class Kind { Missing, Glyph, Artwork };
    static Kind classify(const QString &name)
    {
        static QHash<QString, Kind> cache;
        const auto hit = cache.constFind(name);
        if (hit != cache.constEnd()) {
            return *hit;
        }
        const QPixmap pm = resolve(name, nullptr, QSize(32, 32), /*fallback=*/false);
        const Kind k = pm.isNull() ? Kind::Missing
                     : isFlatColour(pm.toImage().convertToFormat(QImage::Format_ARGB32)) ? Kind::Glyph
                                                                                        : Kind::Artwork;
        cache.insert(name, k);
        return k;
    }

    static QPixmap resolve(const QString &id, QSize *size, const QSize &requested, bool fallback = true)
    {
        /* Every step of the chain, because each one has been the answer at
           least once and a missing icon is silent otherwise.

           The -symbolic retry is for the loader that needs it. Plasma
           announces a headset as "portable-symbolic", a name Breeze does not
           ship any file for, while it does ship "portable". KDE's icon engine
           resolves that on its own — measured, it returns an icon for every
           -symbolic name tried — so this line does nothing when the engine is
           present. Qt's built-in loader does no such thing, and that is
           exactly the configuration where icons were reported broken. */
        /* A path is a file to load, not a name to look up: QIcon::fromTheme
           only knows names, and given a path Qt's own loader finds nothing.
           Notifications carry paths through app_icon and image-path. A file
           that will not load falls through to the theme, and so to the
           fallback icon, like any other missing icon. */
        const QString path = id.startsWith(QLatin1String("file:")) ? QUrl(id).toLocalFile() : id;
        /* Regular files only. This runs on the GUI thread (a Pixmap provider
           is synchronous), and opening a FIFO blocks until a writer appears:
           an image-path naming one froze the whole daemon indefinitely. */
        if (QDir::isAbsolutePath(path) && QFileInfo(path).isFile()) {
            QImageReader reader(path);
            const int asked = qMax(requested.width(), requested.height());
            const QSize full = reader.size();
            if (asked > 0 && full.isValid() && (full.width() > asked || full.height() > asked)) {
                reader.setScaledSize(full.scaled(asked, asked, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)));
            }
            const QImage img = reader.read();
            if (!img.isNull()) {
                if (size) {
                    *size = img.size();
                }
                return QPixmap::fromImage(img);
            }
        }

        /* A path that did not load is not handed to the theme lookup:
           QIcon::fromTheme() treats an absolute name as a file and opens it
           itself, which is the same blocking open on a FIFO by another door
           (seen from the history list's delegates). */
        QIcon icon = QDir::isAbsolutePath(path) ? QIcon() : QIcon::fromTheme(id);
        if (icon.isNull() && id.endsWith(QLatin1String("-symbolic"))) {
            icon = QIcon::fromTheme(id.chopped(9));
        }
        if (icon.isNull() && !fallback) {
            return {};
        }
        if (icon.isNull()) {
            icon = QIcon::fromTheme(QStringLiteral("dialog-information"));
        }
        if (icon.isNull()) {
            /* Nothing resolved, which means no usable icon theme rather than
               one missing name — every theme has dialog-information. Said
               once per name: a notification storm must not become a log
               storm, and the first line is the one that explains it. */
            static QSet<QString> reported;
            if (!reported.contains(id)) {
                reported.insert(id);
                qWarning("glassosd: no icon for '%s' and no dialog-information either — "
                         "icon theme '%s' is not resolving. Is an icon theme installed?",
                         qPrintable(id), qPrintable(QIcon::themeName()));
            }
            return {};
        }

        /* Ask for what the caller wants, but never for more than the theme
           actually has.

           Which matters only when KDE's icon engine is absent. With it — it
           arrives with plasma-integration — Breeze's SVGs are rendered at
           whatever size is asked for, and this cap never binds. Without it,
           Qt's own loader is limited to the sizes a theme ships on disk, and
           Breeze has no scalable directory: audio-volume-high exists at 16,
           22 and 24px and nothing larger, measured on a current Breeze. Ask
           that loader for 90 and it stretches the 24px drawing to 90.

           Taking the largest the theme really has means the scene graph does
           any scaling once, with its own filtering, rather than receiving
           something already stretched. It cannot invent detail the theme
           never drew — an icon that only exists at 24px will still look soft
           at 60 — but it is no longer degraded twice on the way.

           The old floor of 64 is gone with it: raising a request cannot
           improve an icon whose source size is fixed, and on the built-in
           loader it was what triggered the upscale. */
        const int asked = qMax(requested.width(), requested.height());
        const int want = asked > 0 ? asked : 64;
        const QSize available = icon.actualSize(QSize(want, want));
        const int edge = available.isEmpty() ? want : qMin(want, qMax(available.width(),
                                                                     available.height()));

        QPixmap pm = icon.pixmap(QSize(edge, edge));
        if (size) {
            *size = pm.size();
        }
        return pm;
    }
};
