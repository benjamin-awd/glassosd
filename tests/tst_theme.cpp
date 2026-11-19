/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
/*
    Theme files: "extends" layers one theme over another, so a light variant
    can share its dark sibling's geometry and motion instead of silently
    dropping to the built-in values for every key it did not restate.
*/
#include "theme.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QtTest>

class TestTheme : public QObject
{
    Q_OBJECT

    QString themesDir() const
    {
        return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
            + QStringLiteral("/glassosd/themes");
    }

    void write(const QString &name, const QByteArray &json)
    {
        QDir().mkpath(themesDir());
        QFile f(themesDir() + QLatin1Char('/') + name + QStringLiteral(".json"));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(json);
    }

    void select(const QString &name)
    {
        auto cfg = KSharedConfig::openConfig(QStringLiteral("glassosdrc"));
        KConfigGroup(cfg, QStringLiteral("Appearance")).writeEntry("ThemeFile", name);
        cfg->sync();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QDir(themesDir()).removeRecursively();
    }

    void aVariantInheritsWhatItDoesNotRestate()
    {
        write(QStringLiteral("base"), R"j({"motion.slideTravel": 362, "radius.card": 16,
                                         "text.primary": "rgba(255,255,255,1)"})j");
        write(QStringLiteral("light"), R"j({"extends": "base", "text.primary": "rgba(0,0,0,1)"})j");
        select(QStringLiteral("light"));
        Theme t;
        QVERIFY(t.loaded());
        QCOMPARE(t.name(), QStringLiteral("light"));
        QCOMPARE(t.num(QStringLiteral("motion.slideTravel"), 0), 362.0);
        QCOMPARE(t.num(QStringLiteral("radius.card"), 0), 16.0);
        QCOMPARE(t.color(QStringLiteral("text.primary"), Qt::red), QColor(0, 0, 0));
    }

    void chainsResolveAndTheNearestWins()
    {
        write(QStringLiteral("a"), R"j({"x": 1, "y": 1, "z": 1})j");
        write(QStringLiteral("b"), R"j({"extends": "a", "y": 2, "z": 2})j");
        write(QStringLiteral("c"), R"j({"extends": "b", "z": 3})j");
        select(QStringLiteral("c"));
        Theme t;
        QCOMPARE(t.num(QStringLiteral("x"), 0), 1.0);
        QCOMPARE(t.num(QStringLiteral("y"), 0), 2.0);
        QCOMPARE(t.num(QStringLiteral("z"), 0), 3.0);
    }

    /* A hand-edited loop must load what it can, not hang. */
    void aLoopTerminates()
    {
        write(QStringLiteral("p"), R"j({"extends": "q", "p": 1})j");
        write(QStringLiteral("q"), R"j({"extends": "p", "q": 1})j");
        select(QStringLiteral("p"));
        Theme t;
        QVERIFY(t.loaded());
        QCOMPARE(t.num(QStringLiteral("p"), 0), 1.0);
        QCOMPARE(t.num(QStringLiteral("q"), 0), 1.0);
    }

    void aMissingBaseStillLoadsTheVariant()
    {
        write(QStringLiteral("orphan"), R"j({"extends": "nowhere", "k": 5})j");
        select(QStringLiteral("orphan"));
        Theme t;
        QVERIFY(t.loaded());
        QCOMPARE(t.num(QStringLiteral("k"), 0), 5.0);
    }

    void cleanupTestCase()
    {
        QDir(themesDir()).removeRecursively();
    }
};

QTEST_GUILESS_MAIN(TestTheme)
#include "tst_theme.moc"
