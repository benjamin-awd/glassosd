/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "theme.h"
#include "colorscheme.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QJsonArray>

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

namespace
{
/* Reference loops in a hand-edited file should degrade to the fallback, not
   hang the daemon. */
constexpr int kMaxRefDepth = 8;

/* rgba(20, 25, 25, 0.5) — the form every swaync theme is written in. Qt's
   own colour parser does not accept it, and asking people to convert their
   palette to #rrggbbaa by hand is exactly the friction we are removing. */
QColor parseColor(const QString &spec)
{
    const QString s = spec.trimmed();

    static const QRegularExpression rgba(
        QStringLiteral("^rgba?\\(\\s*([0-9.]+)\\s*,\\s*([0-9.]+)\\s*,\\s*([0-9.]+)\\s*"
                       "(?:,\\s*([0-9.]+)\\s*)?\\)$"),
        QRegularExpression::CaseInsensitiveOption);

    const auto m = rgba.match(s);
    if (m.hasMatch()) {
        const int r = qBound(0, int(m.captured(1).toDouble()), 255);
        const int g = qBound(0, int(m.captured(2).toDouble()), 255);
        const int b = qBound(0, int(m.captured(3).toDouble()), 255);
        /* CSS alpha is 0..1; accept 0..255 too, since people do write it. */
        qreal a = m.captured(4).isEmpty() ? 1.0 : m.captured(4).toDouble();
        if (a > 1.0) {
            a /= 255.0;
        }
        return QColor::fromRgbF(r / 255.0, g / 255.0, b / 255.0, qBound(0.0, a, 1.0));
    }

    const QColor c(s);
    return c.isValid() ? c : QColor();
}
} // namespace

Theme::Theme(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
{
    /* Editors that write via rename break a plain path watch, so re-arm on
       every event and debounce — a save can fire several times. */
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
        QTimer::singleShot(80, this, [this, path] {
            watchFile(path);
            reload();
        });
    });
    reload();
}

QStringList Theme::searchPaths()
{
    QStringList paths;
    const QString cfg = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    paths << cfg + QStringLiteral("/glassosd/themes");
    for (const QString &d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        paths << d + QStringLiteral("/glassosd/themes");
    }
    return paths;
}

void Theme::watchFile(const QString &path)
{
    if (!m_watcher->files().contains(path) && QFile::exists(path)) {
        m_watcher->addPath(path);
    }
}

void Theme::reload()
{
    m_values = {};
    m_name.clear();
    m_path.clear();
    m_loaded = false;

    auto cfg = KSharedConfig::openConfig(QStringLiteral("glassosdrc"));
    cfg->reparseConfiguration();
    const KConfigGroup g(cfg, QStringLiteral("Appearance"));
    /* With Theme=auto and the desktop in light mode, ThemeFileLight wins
       over ThemeFile, so one setting pair follows Plasma's colour scheme. An
       unset ThemeFileLight leaves ThemeFile in charge either way. */
    QString requested = g.readEntry("ThemeFile", QString());
    if (g.readEntry("Theme", QString()).compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0
        && !ColorScheme::prefersDark()) {
        const QString light = g.readEntry("ThemeFileLight", QString());
        if (!light.isEmpty()) {
            requested = light;
        }
    }

    QStringList candidates;
    if (!requested.isEmpty()) {
        /* An absolute path is taken literally so a theme can live in a
           dotfiles repo without being copied into place. */
        if (QFileInfo(requested).isAbsolute()) {
            candidates << requested;
        } else {
            for (const QString &dir : searchPaths()) {
                candidates << dir + QLatin1Char('/') + requested + QStringLiteral(".json");
                candidates << dir + QLatin1Char('/') + requested;
            }
        }
    } else {
        /* No ThemeFile set: an unnamed theme.json is still honoured, so
           dropping one file in place is enough to retheme. */
        const QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        candidates << cfgDir + QStringLiteral("/glassosd/theme.json");
    }

    for (const QString &path : std::as_const(candidates)) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError) {
            /* Name the file and the offset. A theme that silently does
               nothing is the single most annoying failure mode here. */
            qWarning("glassosd: %s is not valid JSON at offset %d: %s",
                     qUtf8Printable(path), err.offset, qUtf8Printable(err.errorString()));
            continue;
        }
        if (!doc.isObject()) {
            qWarning("glassosd: %s must contain a JSON object", qUtf8Printable(path));
            continue;
        }
        m_values = withBase(doc.object(), path, 0);
        m_path = path;
        m_name = QFileInfo(path).completeBaseName();
        m_loaded = true;
        watchFile(path);
        qInfo("glassosd: theme '%s' loaded from %s (%lld keys)",
              qUtf8Printable(m_name), qUtf8Printable(path),
              static_cast<long long>(m_values.size()));
        break;
    }

    applyMaterial();

    ++m_revision;
    Q_EMIT changed();
}

/* "extends": "name" loads another theme underneath this one: its keys are
   the defaults and this file's win. Without it a variant had to restate the
   whole theme, and did not: macos-light.json said "Geometry is shared with
   macos.json; only the palette differs", but nothing shared anything, so the
   light banner fell back to built-in metrics for nineteen keys — the motion
   (no slide), the padding, the icon box and the type sizes among them.

   Resolved next to the file first, then on the search path, like ThemeFile.
   Merged at the top level: a nested object in this file replaces the base's
   branch, and a flat "a.b" key overrides one leaf (lookup() prefers it). */
QJsonObject Theme::withBase(const QJsonObject &theme, const QString &path, int depth)
{
    const QString base = theme.value(QStringLiteral("extends")).toString();
    if (base.isEmpty()) {
        return theme;
    }
    if (depth >= 4) {
        qWarning("glassosd: %s: \"extends\" goes more than four deep; ignoring the rest",
                 qUtf8Printable(path));
        return theme;
    }
    QStringList candidates;
    if (QFileInfo(base).isAbsolute()) {
        candidates << base;
    } else {
        const QString here = QFileInfo(path).absolutePath();
        candidates << here + QLatin1Char('/') + base + QStringLiteral(".json")
                   << here + QLatin1Char('/') + base;
        for (const QString &dir : searchPaths()) {
            candidates << dir + QLatin1Char('/') + base + QStringLiteral(".json")
                       << dir + QLatin1Char('/') + base;
        }
    }
    for (const QString &candidate : std::as_const(candidates)) {
        if (QFileInfo(candidate).canonicalFilePath() == QFileInfo(path).canonicalFilePath()) {
            continue;   // a theme cannot extend itself
        }
        QFile f(candidate);
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning("glassosd: %s extends %s, which is not a valid theme",
                     qUtf8Printable(path), qUtf8Printable(candidate));
            return theme;
        }
        watchFile(candidate);   // editing the base re-themes the variant too
        QJsonObject merged = withBase(doc.object(), candidate, depth + 1);
        for (auto it = theme.constBegin(); it != theme.constEnd(); ++it) {
            merged.insert(it.key(), it.value());
        }
        merged.remove(QStringLiteral("extends"));
        return merged;
    }
    qWarning("glassosd: %s extends '%s', which was not found", qUtf8Printable(path),
             qUtf8Printable(base));
    return theme;
}

/* kwin-glassblur remaps the backdrop's luminance under glassosd's windows
   into [MaterialLumaMin, MaterialLumaMax] percent of kwinrc's
   [Effect-glassblur]. The right range depends on the theme — about 10-40 for
   a dark card, 60-100 for a light one — so a theme may carry it as
   "blur.materialLuma": [min, max], and loading the theme writes it there and
   asks the effect to re-read it. A theme without the key leaves kwinrc
   alone, so nothing changes for anyone not using glassblur. */
void Theme::applyMaterial()
{
    const QJsonArray range = lookup(QStringLiteral("blur.materialLuma")).toArray();
    if (range.size() != 2 || !range.at(0).isDouble() || !range.at(1).isDouble()) {
        return;
    }
    const int lo = qBound(0, range.at(0).toInt(), 100);
    const int hi = qBound(lo, range.at(1).toInt(), 100);
    auto kwinrc = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    KConfigGroup g(kwinrc, QStringLiteral("Effect-glassblur"));
    if (g.readEntry("MaterialLumaMin", -1) == lo && g.readEntry("MaterialLumaMax", -1) == hi) {
        return;
    }
    g.writeEntry("MaterialLumaMin", lo);
    g.writeEntry("MaterialLumaMax", hi);
    kwinrc->sync();
    /* Not from a test: the suite's kwinrc is a scratch file, and the real
       compositor has no business re-reading its own because of it. */
    if (QStandardPaths::isTestModeEnabled()) {
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/Effects"),
                                                       QStringLiteral("org.kde.kwin.Effects"),
                                                       QStringLiteral("reconfigureEffect"));
    call.setArguments({QStringLiteral("glassblur")});
    QDBusConnection::sessionBus().call(call, QDBus::NoBlock);
    qInfo("glassosd: glassblur material set to %d-%d%% for theme '%s'", lo, hi, qUtf8Printable(m_name));
}

QJsonValue Theme::lookup(const QString &key, int depth) const
{
    if (m_values.isEmpty() || depth > kMaxRefDepth) {
        return {};
    }

    QJsonValue v;
    /* Flat "a.b" wins over nested {a:{b:…}} so a theme can override one leaf
       without restating the branch it lives in. */
    if (m_values.contains(key)) {
        v = m_values.value(key);
    } else {
        const QStringList parts = key.split(QLatin1Char('.'));
        QJsonValue cur = m_values;
        for (const QString &p : parts) {
            if (!cur.isObject()) {
                return {};
            }
            const QJsonObject o = cur.toObject();
            if (!o.contains(p)) {
                return {};
            }
            cur = o.value(p);
        }
        v = cur;
    }

    if (v.isString()) {
        const QString s = v.toString();
        if (s.startsWith(QLatin1Char('@')) && s.size() > 1) {
            return lookup(s.mid(1), depth + 1);
        }
    }
    return v;
}

QColor Theme::color(const QString &key, const QColor &fallback) const
{
    const QJsonValue v = lookup(key);
    if (!v.isString()) {
        return fallback;
    }
    const QColor c = parseColor(v.toString());
    if (!c.isValid()) {
        qWarning("glassosd: theme key '%s' is not a colour: %s",
                 qUtf8Printable(key), qUtf8Printable(v.toString()));
        return fallback;
    }
    return c;
}

qreal Theme::num(const QString &key, qreal fallback) const
{
    const QJsonValue v = lookup(key);
    return v.isDouble() ? v.toDouble() : fallback;
}

QString Theme::str(const QString &key, const QString &fallback) const
{
    const QJsonValue v = lookup(key);
    return v.isString() ? v.toString() : fallback;
}

bool Theme::flag(const QString &key, bool fallback) const
{
    const QJsonValue v = lookup(key);
    return v.isBool() ? v.toBool() : fallback;
}
