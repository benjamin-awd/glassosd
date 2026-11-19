/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "systemcontrols.h"

#include <QDebug>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

SystemControls::SystemControls(QObject *parent)
    : QObject(parent)
{
    detect();

    /* Slow: this is the fallback. Real responsiveness comes from the
       subscription below. */
    m_poll.setInterval(1500);
    connect(&m_poll, &QTimer::timeout, this, &SystemControls::refresh);

    /* A single volume keypress produces several change events in a row.
       Coalescing them keeps this to one wpctl call per burst. */
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(40);
    connect(&m_debounce, &QTimer::timeout, this, &SystemControls::refresh);

    refresh();
}

void SystemControls::detect()
{
    /* wpctl first: on a PipeWire system pactl is a compatibility shim, and
       asking the shim for a value the native tool owns is a needless hop. */
    for (const QString &t : {QStringLiteral("wpctl"), QStringLiteral("pactl")}) {
        if (!QStandardPaths::findExecutable(t).isEmpty()) {
            m_volumeTool = t;
            break;
        }
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("brightnessctl")).isEmpty()) {
        m_brightnessTool = QStringLiteral("brightnessctl");
    }

    if (m_volumeTool.isEmpty()) {
        qInfo("glassosd: no wpctl or pactl — the volume widget will stay hidden");
    }
    if (m_brightnessTool.isEmpty()) {
        qInfo("glassosd: no brightnessctl — the brightness widget will stay hidden");
    }
}

void SystemControls::runAsync(const QString &cmd, std::function<void(const QString &)> done)
{
    auto *p = new QProcess(this);
    auto finished = std::make_shared<bool>(false);
    auto complete = [p, done, finished](const QString &out) {
        if (*finished) {
            return;
        }
        *finished = true;
        p->deleteLater();
        done(out);
    };
    connect(p, &QProcess::finished, this, [p, complete](int code, QProcess::ExitStatus status) {
        complete(status == QProcess::NormalExit && code == 0
                     ? QString::fromUtf8(p->readAllStandardOutput()).trimmed()
                     : QString());
    });
    connect(p, &QProcess::errorOccurred, this, [complete](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            complete(QString());
        }
    });
    /* Short: these are local queries. A hung audio server must not leave a
       reading pending forever; a stale value for one tick is harmless. */
    QTimer::singleShot(400, p, [p, complete] {
        p->kill();
        complete(QString());
    });
    p->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmd});
}

void SystemControls::startSubscription()
{
    if (m_subscription || QStandardPaths::findExecutable(QStringLiteral("pactl")).isEmpty()) {
        return;
    }
    m_subscription = new QProcess(this);
    connect(m_subscription, &QProcess::readyReadStandardOutput, this, [this] {
        const QByteArray out = m_subscription->readAllStandardOutput();
        /* Sink events cover volume and mute; source events cover the mic.
           With the '#': "on sink-input #" (any app's stream starting,
           stopping or changing level) matched "on sink" too and refreshed
           on every one. */
        if (out.contains("on sink #") || out.contains("on source #")) {
            m_debounce.start();
        }
    });
    m_subscription->start(QStringLiteral("pactl"), {QStringLiteral("subscribe")});
}

void SystemControls::setPolling(bool on)
{
    if (on) {
        refresh();
        startSubscription();
        m_poll.start();
    } else {
        m_poll.stop();
        if (m_subscription) {
            /* Not waited for: the centre closing must not stall on it. */
            QProcess *sub = m_subscription;
            m_subscription = nullptr;
            sub->disconnect(this);
            connect(sub, &QProcess::finished, sub, &QObject::deleteLater);
            sub->kill();
        }
    }
}

void SystemControls::refresh()
{
    if (m_refreshing) {
        m_refreshAgain = true;   // one more once this finishes, not one per call
        return;
    }
    m_refreshing = true;
    m_oldVol = m_volume;
    m_oldBri = m_brightness;
    m_oldMuted = m_muted;
    m_oldVolAvail = m_volumeAvailable;
    m_oldBriAvail = m_brightnessAvailable;
    m_refreshPending = 0;
    /* Which slider moves this reading predates. One taken before a set
       landed after it and put the old level back (see readingIsStale()). */
    const quint64 volGen = m_volumeGen, briGen = m_brightnessGen;

    if (m_volumeTool == QLatin1String("wpctl")) {
        ++m_refreshPending;
        // "Volume: 0.35" or "Volume: 0.35 [MUTED]"
        runAsync(QStringLiteral("wpctl get-volume @DEFAULT_AUDIO_SINK@"), [this, volGen](const QString &out) {
            static const QRegularExpression re(QStringLiteral("Volume:\\s*([0-9.]+)"));
            const auto m = re.match(out);
            if (volumeReadingIsStale(volGen)) {
                // the slider's value stands; a fresh read follows the write
            } else if (m.hasMatch()) {
                m_volume = qBound(0.0, m.captured(1).toDouble(), 1.0);
                m_muted = out.contains(QLatin1String("MUTED"));
                m_volumeAvailable = true;
            } else {
                m_volumeAvailable = false;
            }
            finishRefresh();
        });
    } else if (m_volumeTool == QLatin1String("pactl")) {
        ++m_refreshPending;
        runAsync(QStringLiteral("pactl get-sink-volume @DEFAULT_SINK@; echo; pactl get-sink-mute @DEFAULT_SINK@"),
                 [this, volGen](const QString &out) {
            static const QRegularExpression re(QStringLiteral("(\\d+)%"));
            const auto m = re.match(out);
            if (volumeReadingIsStale(volGen)) {
                // as above
            } else if (m.hasMatch()) {
                m_volume = qBound(0.0, m.captured(1).toDouble() / 100.0, 1.0);
                m_muted = out.contains(QLatin1String("Mute: yes"));
                m_volumeAvailable = true;
            } else {
                m_volumeAvailable = false;
            }
            finishRefresh();
        });
    }

    if (!m_brightnessTool.isEmpty()) {
        ++m_refreshPending;
        /* One call: -m prints device,class,current,percent,max. */
        runAsync(QStringLiteral("brightnessctl -m"), [this, briGen](const QString &out) {
            const QStringList f = out.section(QLatin1Char('\n'), 0, 0).split(QLatin1Char(','));
            bool okc = false, okm = false;
            const double c = f.value(2).toDouble(&okc);
            const double mx = f.value(4).toDouble(&okm);
            if (readingIsStale(briGen, m_brightnessGen, m_brightnessSending, m_brightnessTarget)) {
                // as for volume
            } else if (okc && okm && mx > 0) {
                m_brightness = qBound(0.0, c / mx, 1.0);
                m_brightnessAvailable = true;
            } else {
                m_brightnessAvailable = false;
            }
            finishRefresh();
        });
    }

    if (m_refreshPending == 0) {
        m_refreshing = false;
    }
}

bool SystemControls::readingIsStale(quint64 gen, quint64 current, bool sending, int target)
{
    /* Taken before the slider last moved, or while a set is still on its
       way: either way it may hold the level from before, and applying it
       snapped the slider back (or, when it matched the refresh's own
       snapshot, silently left the model disagreeing with the slider). The
       slider's value stands. Something may still have changed outside, so
       read again: after the write lands if one is in flight, else at once. */
    if (gen == current && !sending && target < 0) {
        return false;
    }
    if (sending || target >= 0) {
        m_rereadAfterSend = true;
    } else {
        m_refreshAgain = true;
    }
    return true;
}

bool SystemControls::volumeReadingIsStale(quint64 gen)
{
    return readingIsStale(gen, m_volumeGen, m_volumeSending, m_volumeTarget);
}

void SystemControls::rereadIfIdle()
{
    if (m_rereadAfterSend && !m_volumeSending && m_volumeTarget < 0
        && !m_brightnessSending && m_brightnessTarget < 0) {
        m_rereadAfterSend = false;
        refresh();
    }
}

void SystemControls::finishRefresh()
{
    if (--m_refreshPending > 0) {
        return;
    }
    m_refreshing = false;
    if (!qFuzzyCompare(m_oldVol + 1, m_volume + 1) || !qFuzzyCompare(m_oldBri + 1, m_brightness + 1)
        || m_oldMuted != m_muted || m_oldVolAvail != m_volumeAvailable
        || m_oldBriAvail != m_brightnessAvailable) {
        Q_EMIT changed();
    }
    if (m_refreshAgain) {
        m_refreshAgain = false;
        refresh();
    }
}

void SystemControls::setVolume(qreal v)
{
    if (m_volumeTool.isEmpty()) {
        return;
    }
    m_volumeTarget = qBound(0, int(v * 100 + 0.5), 100);
    m_volume = m_volumeTarget / 100.0;
    ++m_volumeGen;
    Q_EMIT changed();
    sendVolume();
}

void SystemControls::sendVolume()
{
    if (m_volumeSending || m_volumeTarget < 0) {
        return;
    }
    const int pct = m_volumeTarget;
    m_volumeTarget = -1;
    m_volumeSending = true;
    const QString cmd = m_volumeTool == QLatin1String("wpctl")
        ? QStringLiteral("wpctl set-volume @DEFAULT_AUDIO_SINK@ %1%").arg(pct)
        : QStringLiteral("pactl set-sink-volume @DEFAULT_SINK@ %1%").arg(pct);
    runAsync(cmd, [this](const QString &) {
        m_volumeSending = false;
        sendVolume();   // the latest value a drag reached while this ran
        rereadIfIdle();
    });
}

void SystemControls::toggleMute()
{
    if (m_volumeTool.isEmpty()) {
        return;
    }
    const QString cmd = m_volumeTool == QLatin1String("wpctl")
        ? QStringLiteral("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle")
        : QStringLiteral("pactl set-sink-mute @DEFAULT_SINK@ toggle");
    runAsync(cmd, [this](const QString &) { refresh(); });
}

void SystemControls::setBrightness(qreal v)
{
    if (m_brightnessTool.isEmpty()) {
        return;
    }
    /* Never let a slider drag take the screen to literal zero — on most
       panels that is indistinguishable from the machine having died. */
    m_brightnessTarget = qBound(1, int(v * 100 + 0.5), 100);
    m_brightness = m_brightnessTarget / 100.0;
    ++m_brightnessGen;
    Q_EMIT changed();
    sendBrightness();
}

void SystemControls::sendBrightness()
{
    if (m_brightnessSending || m_brightnessTarget < 0) {
        return;
    }
    const int pct = m_brightnessTarget;
    m_brightnessTarget = -1;
    m_brightnessSending = true;
    runAsync(QStringLiteral("brightnessctl -q set %1%").arg(pct), [this](const QString &) {
        m_brightnessSending = false;
        sendBrightness();
        rereadIfIdle();
    });
}
