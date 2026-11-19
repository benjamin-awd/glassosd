/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "quitonsignal.h"

#include <QSocketNotifier>

#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace
{
int g_pipe[2] = {-1, -1};
constexpr int kSignals[] = {SIGTERM, SIGINT, SIGHUP};

void handler(int sig)
{
    const unsigned char b = static_cast<unsigned char>(sig);
    const ssize_t n = ::write(g_pipe[1], &b, 1);   // async-signal-safe
    (void)n;
}
} // namespace

QuitOnSignal::QuitOnSignal(QObject *parent)
    : QObject(parent)
{
    if (g_pipe[0] < 0 && ::pipe2(g_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        g_pipe[0] = g_pipe[1] = -1;
        return;
    }
    m_notifier = new QSocketNotifier(g_pipe[0], QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, [this] {
        unsigned char b = 0;
        while (::read(g_pipe[0], &b, 1) == 1) {
            Q_EMIT received(int(b));
        }
    });
    struct sigaction sa = {};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (int sig : kSignals) {
        ::sigaction(sig, &sa, nullptr);
    }
}

QuitOnSignal::~QuitOnSignal()
{
    /* Back to the default, so a signal after this ends the process as it
       would have, rather than writing into a pipe nobody reads. */
    for (int sig : kSignals) {
        ::signal(sig, SIG_DFL);
    }
}
