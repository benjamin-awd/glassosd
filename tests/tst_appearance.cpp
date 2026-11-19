/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    [Appearance] keys as the daemon reads them, from a throwaway
    XDG_CONFIG_HOME: the OSD's placement and number, and the centre's style.
*/
#include "appearance.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QTemporaryDir>
#include <QtTest>

class TestAppearance : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;

    void write(const QString &key, const QString &value)
    {
        auto cfg = KSharedConfig::openConfig(QStringLiteral("glassosdrc"));
        KConfigGroup(cfg, QStringLiteral("Appearance")).writeEntry(key, value);
        cfg->sync();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("XDG_CONFIG_HOME", m_dir.path().toUtf8());
        QStandardPaths::setTestModeEnabled(false);
    }

    /* top-right anchors top and right, where macOS 26 shows the volume and
       brightness module; the others stay as they were. */
    void osdPositions()
    {
        const QList<QPair<QString, int>> cases = {
            {QStringLiteral("top"), 1}, {QStringLiteral("bottom"), 2},
            {QStringLiteral("centre"), 0}, {QStringLiteral("top-right"), 1 | 8},
            {QStringLiteral("nonsense"), 1},
        };
        for (const auto &[value, anchors] : cases) {
            write(QStringLiteral("OsdPosition"), value);
            Appearance a;
            QCOMPARE(a.osdAnchor(), anchors);
        }
    }

    void osdPercentAndCentreStyle()
    {
        {
            Appearance a;
            QVERIFY(a.osdPercent());
            QVERIFY(a.centreCards());
        }
        write(QStringLiteral("OsdPercent"), QStringLiteral("false"));
        write(QStringLiteral("CentreStyle"), QStringLiteral("list"));
        {
            Appearance a;
            QVERIFY(!a.osdPercent());
            QVERIFY(!a.centreCards());
        }
        write(QStringLiteral("CentreStyle"), QStringLiteral("typo"));
        Appearance a;
        QVERIFY(a.centreCards());   // anything but "list" is the default
    }
};

QTEST_GUILESS_MAIN(TestAppearance)
#include "tst_appearance.moc"
