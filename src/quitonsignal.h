/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    SIGTERM, SIGINT and SIGHUP as an ordinary quit.

    Qt leaves these at their default, which ends the process on the spot:
    no aboutToQuit, so whatever was waiting to be written went with it.
    History is saved on a 1.5 s debounce after each arrival and once more on
    aboutToQuit, so every `systemctl --user stop/restart`, and every logout,
    lost what had arrived in the last moment before it.

    The handler only writes the signal number down a pipe (the one thing
    that is safe in a signal handler); the event loop reads it and emits
    received(), which main() connects to quit().
*/
#pragma once

#include <QObject>

class QSocketNotifier;

class QuitOnSignal : public QObject
{
    Q_OBJECT
public:
    /* One per process: the handler writes to a pipe shared by all. */
    explicit QuitOnSignal(QObject *parent = nullptr);
    ~QuitOnSignal() override;

    bool isValid() const { return m_notifier != nullptr; }

Q_SIGNALS:
    void received(int signal);

private:
    QSocketNotifier *m_notifier = nullptr;
};
