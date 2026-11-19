/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "historymodel.h"

#include "jsonstore.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QDateTime>

#include <algorithm>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

HistoryModel::HistoryModel(QObject *parent)
    : QAbstractListModel(parent)
{
    /* Debounced: a burst of thirty notifications should produce one write,
       not thirty. */
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1500);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] {
        save();
    });
    m_ageTick.setInterval(30000);
    connect(&m_ageTick, &QTimer::timeout, this, [this] {
        if (!m_rows.isEmpty()) {
            Q_EMIT dataChanged(index(0), index(int(m_rows.size()) - 1), {WhenRole});
        }
    });

    /* Capacity before load, and read here rather than waited for.

       load() truncates to m_capacity as it reads, and this is a QML singleton
       the engine constructs, so main.cpp cannot hand a capacity to the
       constructor — it can only call setCapacity() afterwards, by which point
       the file has already been read and cut short. Anyone raising
       HistoryLength above the 200 default therefore kept the larger history
       for the session and silently lost everything past 200 on the next
       start, while glassosdctl status went on reporting the number they
       asked for.

       setCapacity() still exists for live changes; this only makes sure the
       first read already knows the answer. */
    m_capacity = qMax(1, KSharedConfig::openConfig(QStringLiteral("glassosdrc"))
                             ->group(QStringLiteral("Notifications"))
                             .readEntry("HistoryLength", 200));
    load();
}

QString HistoryModel::storePath() const
{
    /* GenericDataLocation + an explicit name, because AppDataLocation nests
       organisation *and* application and both are "glassosd", which produced
       ~/.local/share/glassosd/glassosd/. */
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/glassosd");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/history.json");
}

void HistoryModel::setCapacity(int n)
{
    m_capacity = qMax(1, n);
    while (m_all.size() > m_capacity) {
        m_all.removeLast();
    }
    rebuild();
    m_saveTimer.start();   // the entries changed; see rebuild()
}

void HistoryModel::load()
{
    const QJsonArray arr = JsonStore::read(storePath());
    m_all.clear();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Notification n;
        /* 64-bit: a portal id has the top bit set, and toInt() would
           read it back as a negative number. */
        n.id = uint(o.value(QStringLiteral("id")).toInteger());
        n.appName = o.value(QStringLiteral("app")).toString();
        n.desktopEntry = o.value(QStringLiteral("desktopEntry")).toString();
        n.appIcon = o.value(QStringLiteral("icon")).toString();
        n.summary = o.value(QStringLiteral("summary")).toString();
        n.body = o.value(QStringLiteral("body")).toString();
        n.urgency = static_cast<Urgency>(o.value(QStringLiteral("urgency")).toInt(1));
        n.received = QDateTime::fromString(o.value(QStringLiteral("at")).toString(), Qt::ISODate);
        n.repeatCount = o.value(QStringLiteral("repeats")).toInt(1);
        /* The sender's or rule's lock-screen wish, so the centre masks it
           while sharing after a restart as it did before. Unknown values
           (a newer build's) read as Unset, which falls back to the default. */
        const int privacy = o.value(QStringLiteral("privacy")).toInt(0);
        if (privacy > int(Notification::LockPrivacy::Unset)
            && privacy <= int(Notification::LockPrivacy::Hide)) {
            n.lockPrivacy = static_cast<Notification::LockPrivacy>(privacy);
        }
        n.lockKeep = o.value(QStringLiteral("lockKeep")).toString();
        /* Tracked separately: feeding a portal id back into the
           freedesktop sequence would push it over the top bit and make every
           subsequent Notify() collide with the portal's range. */
        if (NotificationId::isPortal(n.id)) {
            m_maxLoadedPortalId = qMax(m_maxLoadedPortalId, n.id);
        } else {
            m_maxLoadedId = qMax(m_maxLoadedId, n.id);
        }
        m_all.append(n);
        if (m_all.size() >= m_capacity) {
            break;
        }
    }

    /* Sort by arrival rather than trusting the file's order. Two reasons:
       a file written by an older build can be out of order (ids used to
       collide across restarts, so a new notification would overwrite an
       unrelated entry *in that entry's position*), and sorting is the only
       thing that repairs such a file. It also means the on-disk order is not
       load-bearing, so a hand-edited history still comes back sane. */
    std::stable_sort(m_all.begin(), m_all.end(),
                     [](const Notification &a, const Notification &b) {
                         return a.received > b.received;
                     });

    /* Collapse identical entries a previous build wrote as separate rows.

       record() merges on arrival, but a file written before it did carries the
       repeats one row each — 158 copies of the same low-battery warning here.
       Nothing else would ever fold those together, since only a *new* arrival
       triggers a merge. Doing it on load is the same reasoning as the sort
       above: the on-disk format is not load-bearing, and an old or hand-edited
       file should come back sane.

       Newest wins the position, and the counts add up. */
    QHash<QString, int> firstAt;
    for (int i = 0; i < m_all.size(); ) {
        const Notification &n = m_all.at(i);
        const QString key = n.appName + QLatin1Char('\x1f')
                          + n.summary + QLatin1Char('\x1f') + n.body;
        const auto seen = firstAt.constFind(key);
        if (seen == firstAt.constEnd()) {
            firstAt.insert(key, i);
            ++i;
            continue;
        }
        m_all[*seen].repeatCount += n.repeatCount;
        m_all.removeAt(i);   // indices after i shift down, but *seen < i so it stands
    }

    rebuild();
}

void HistoryModel::save() const
{
    QJsonArray arr;
    for (const Notification &n : m_all) {
        QJsonObject o;
        o[QStringLiteral("id")] = qint64(n.id);
        o[QStringLiteral("app")] = n.appName;
        o[QStringLiteral("desktopEntry")] = n.desktopEntry;
        o[QStringLiteral("icon")] = n.appIcon;
        o[QStringLiteral("summary")] = n.summary;
        o[QStringLiteral("body")] = n.body;
        o[QStringLiteral("urgency")] = int(n.urgency);
        o[QStringLiteral("at")] = n.received.toString(Qt::ISODate);
        if (n.repeatCount > 1) {
            o[QStringLiteral("repeats")] = n.repeatCount;
        }
        if (n.lockPrivacy != Notification::LockPrivacy::Unset) {
            o[QStringLiteral("privacy")] = int(n.lockPrivacy);
        }
        if (!n.lockKeep.isEmpty()) {
            o[QStringLiteral("lockKeep")] = n.lockKeep;
        }
        /* Inline image-data is deliberately not persisted: it is raw pixels
           and would bloat the file badly. Such entries fall back to the app
           icon after a restart. */
        arr.append(o);
    }
    if (!JsonStore::write(storePath(), arr)) {
        qWarning("glassosd: could not save %s", qPrintable(storePath()));
    }
}

void HistoryModel::setScreenSharing(bool sharing, Notification::LockPrivacy fallback)
{
    if (m_sharing == sharing && m_sharingFallback == fallback) {
        return;
    }
    m_sharing = sharing;
    m_sharingFallback = fallback;
    if (!m_rows.isEmpty()) {
        Q_EMIT dataChanged(index(0), index(m_rows.size() - 1),
                           {SummaryRole, BodyRole, IconSourceRole});
    }
    Q_EMIT changed();   // a group opened on a popup re-reads its masked rows
}

int HistoryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant HistoryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const Row &r = m_rows.at(index.row());

    switch (role) {
    case IsHeaderRole:   return r.header;
    case GroupKeyRole:   return r.groupKey;
    case GroupCountRole: return r.count;
    case CollapsedRole:  return r.collapsed;
    case StackDepthRole: return r.stackDepth;
    case GroupStartRole: return r.groupStart;
    case AppNameRole:    return r.header ? r.appName : r.entry.appName;
    /* Must be answered here: header rows return early below, so a case in
       the entry switch is unreachable for exactly the rows that need it. */
    case HeaderIconRole:
        return r.icon.isEmpty() ? QString() : QStringLiteral("image://icon/") + r.icon;
    default:
        break;
    }
    if (r.header) {
        return {};
    }

    const Notification &n = r.entry;
    if (const QVariant masked = maskedForSharing(n, role); masked.isValid()) {
        return masked;
    }
    switch (role) {
    case IdRole:      return n.id;
    case SummaryRole: return n.summary;
    case BodyRole:    return n.body;
    case UrgencyRole: return static_cast<int>(n.urgency);
    case WhenRole:    return Notification::relativeWhen(n.received, now());
    case RepeatCountRole: return n.repeatCount;
    case IconSourceRole: return iconSourceFor(n);
    default:
        return {};
    }
}

QString HistoryModel::iconSourceFor(const Notification &n)
{
    if (n.image.isValid()) {
        return QStringLiteral("image://notifyimage/h%1").arg(n.id);
    }
    return n.appIcon.isEmpty() ? QString() : QStringLiteral("image://icon/") + n.appIcon;
}

QVariant HistoryModel::maskedForSharing(const Notification &n, int role) const
{
    if (!m_sharing) {
        return {};
    }
    const auto policy = n.lockPrivacy == Notification::LockPrivacy::Unset ? m_sharingFallback
                                                                         : n.lockPrivacy;
    const bool hideAll = policy == Notification::LockPrivacy::HideContent
                      || policy == Notification::LockPrivacy::Hide;
    const bool hideBody = hideAll || policy == Notification::LockPrivacy::HideBody;
    switch (role) {
    case SummaryRole:
        if (hideAll) {
            return tr("Notification hidden");
        }
        break;
    case BodyRole:
        if (hideBody) {
            return QString();
        }
        break;
    case IconSourceRole:
        /* The picture is content (an avatar, a photo); the app's icon
           is not. Without a picture this is the app icon anyway. */
        if (hideBody && (n.image.isValid() || n.appIcon != n.senderIcon)) {
            return n.senderIcon.isEmpty() ? QString() : QStringLiteral("image://icon/") + n.senderIcon;
        }
        break;
    default:
        break;
    }
    return {};
}

QVariantList HistoryModel::groupMembers(const QString &key, uint excludeId, int max) const
{
    QVariantList out;
    if (key.isEmpty() || max <= 0) {
        return out;
    }
    for (const Notification &n : m_all) {   // newest first
        if (n.groupKey() != key || n.id == excludeId) {
            continue;
        }
        auto field = [&](int role, const QVariant &real) {
            const QVariant masked = maskedForSharing(n, role);
            return masked.isValid() ? masked : real;
        };
        const QString when = Notification::relativeWhen(n.received, now());
        out.append(QVariantMap{
            {QStringLiteral("id"), n.id},
            {QStringLiteral("summary"), field(SummaryRole, n.summary)},
            {QStringLiteral("body"), field(BodyRole, n.body)},
            {QStringLiteral("iconSource"), field(IconSourceRole, iconSourceFor(n))},
            {QStringLiteral("when"), when},
            {QStringLiteral("repeatCount"), n.repeatCount},
        });
        if (out.size() >= max) {
            break;
        }
    }
    return out;
}

void HistoryModel::activateId(uint id)
{
    for (const Notification &n : std::as_const(m_all)) {
        if (n.id == id) {
            Q_EMIT entryActivated(n.id, n.desktopEntry);
            return;
        }
    }
}

QHash<int, QByteArray> HistoryModel::roleNames() const
{
    return {
        {IdRole, "histId"},
        {AppNameRole, "appName"},
        {SummaryRole, "summary"},
        {BodyRole, "body"},
        {IconSourceRole, "iconSource"},
        {HeaderIconRole, "headerIcon"},
        {UrgencyRole, "urgency"},
        {WhenRole, "when"},
        {RepeatCountRole, "repeatCount"},
        {IsHeaderRole, "isHeader"},
        {GroupKeyRole, "groupKey"},
        {GroupCountRole, "groupCount"},
        {CollapsedRole, "collapsed"},
        {StackDepthRole, "stackDepth"},
        {GroupStartRole, "groupStart"},
    };
}

void HistoryModel::record(const Notification &n)
{
    if (n.historyIgnore) {
        return;
    }
    if (n.image.isValid()
        && (n.image.width > ImageData::maxHistoryEdge || n.image.height > ImageData::maxHistoryEdge)) {
        Notification smaller = n;
        smaller.image = ImageData::fromImage(n.image.toImage(), ImageData::maxHistoryEdge);
        record(smaller);
        return;
    }


    /* A replaces_id notification is the *same* message updating itself — a
       file-copy progress notification would otherwise leave a hundred entries
       behind. Update in place instead of prepending.

       The app name has to match as well as the id. Ids are only unique within
       one run of the daemon, but history outlives restarts, so id alone let a
       fresh notification overwrite an unrelated old entry — silently, and in
       that entry's old position, which is what put the file out of time
       order. Seeding the server's id counter past maxLoadedId() prevents the
       collision; this check is the belt to that pair of braces. */
    for (int i = 0; i < m_all.size(); ++i) {
        if (m_all.at(i).id == n.id && m_all.at(i).appName == n.appName) {
            m_all[i] = n;
            rebuild();
            m_saveTimer.start();   // the entries changed; see rebuild()
            return;
        }
    }

    /* Counted only for something new: a replaces_id update above is the
       same message changing (a copy's progress), and counting each step put
       a hundred on the badge for one file. Not counted while the centre is
       open, either — you are looking at it. */
    if (!m_panelOpen) {
        ++m_unread;
    }

    /* A byte-identical repeat becomes a count on the existing entry rather
       than another row.

       PowerDevil re-sends the same "Device Battery Low (6% Remaining)" once or
       twice a second for as long as the pen is low. repeat_window keeps that
       off the screen, but every copy still reached history, and 129 identical
       rows under one group header is the same spam in a different place.

       Matched on the text rather than on a window, because there is no honest
       window here: the interesting question is "have I already been told this",
       and the answer does not expire after five minutes. It is moved back to
       the front, since it did just happen again — history is ordered by when
       something last occurred, not by when it first did.

       Only exact matches merge. A changed percentage is a different message
       and gets its own row. */
    for (int i = 0; i < m_all.size(); ++i) {
        const Notification &old = m_all.at(i);
        if (old.appName == n.appName && old.summary == n.summary
            && old.body == n.body) {
            Notification merged = n;
            merged.repeatCount = old.repeatCount + 1;
            if (merged.appIcon.isEmpty() && !merged.image.isValid()) {
                merged.appIcon = old.appIcon;
            }
            m_all.removeAt(i);
            m_all.prepend(merged);
            /* So a cursor on the old entry can follow it (latestId()).
               Bounded; a cursor only ever needs the last few. */
            if (m_mergedInto.size() > 256) {
                m_mergedInto.clear();
            }
            m_mergedInto.insert(old.id, merged.id);
            rebuild();
            m_saveTimer.start();   // the entries changed; see rebuild()
            return;
        }
    }

    m_all.prepend(n);
    while (m_all.size() > m_capacity) {
        m_all.removeLast();
    }
    rebuild();
    m_saveTimer.start();   // the entries changed; see rebuild()
}

ImageData HistoryModel::imageFor(uint id) const
{
    for (const Notification &n : m_all) {
        if (n.id == id) {
            return n.image;
        }
    }
    return {};
}

bool HistoryModel::matchesSearch(const Notification &n) const
{
    if (m_search.isEmpty()) {
        return true;
    }
    /* App, summary and body — the three things somebody actually remembers
       about a notification they are trying to find again. Case-insensitive
       because nobody recalls the capitalisation of a subject line. */
    return n.appName.contains(m_search, Qt::CaseInsensitive)
        || n.summary.contains(m_search, Qt::CaseInsensitive)
        || n.body.contains(m_search, Qt::CaseInsensitive);
}

void HistoryModel::setSearch(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (m_search == trimmed) {
        return;
    }
    m_search = trimmed;
    /* Searching is a whole-history question, so drilling into one app and then
       typing would otherwise search only that app while looking like it
       searched everything. */
    if (!m_search.isEmpty()) {
        m_filter.clear();
    }
    rebuild();
}

void HistoryModel::setGroupFilter(const QString &key)
{
    if (m_filter == key) {
        return;
    }
    m_filter = key;
    rebuild();   // rebuild() resolves the label, so it stays correct as entries arrive
}

void HistoryModel::showGroup(const QString &key, bool withSettings)
{
    if (key.isEmpty()) {
        return;
    }
    m_appSettingsVisible = withSettings;

    /* Expanded, always. "Show more" that lands you on a collapsed header is
       asking you to click twice to do the thing you already asked for, and
       auto-collapse or an always_collapsed rule would otherwise fold exactly
       the group you opened deliberately. */
    m_expanded.insert(key);
    m_collapsed.remove(key);

    m_filter = key;
    rebuild();            // resolves the label and applies the expand
    setPanelOpen(true);
    Q_EMIT changed();
}

void HistoryModel::setPanelOpen(bool open)
{
    if (m_panelOpen == open) {
        return;
    }
    m_panelOpen = open;
    if (open) {
        m_ageTick.start();
        /* The ages moved on while it was shut. */
        if (!m_rows.isEmpty()) {
            Q_EMIT dataChanged(index(0), index(int(m_rows.size()) - 1), {WhenRole});
        }
    } else {
        m_ageTick.stop();
    }
    if (open) {
        m_unread = 0;
        /* changed(), not just panelOpenChanged(): the tray listens to the
           former, and without this the badge stayed up after the centre had
           been opened and closed again. */
        Q_EMIT changed();
    }
    if (!open) {
        /* Reopening should show everything, not silently still be filtered to
           whichever app was drilled into last time, and not still showing a
           settings panel opened three drill-ins ago. */
        /* One rebuild for whatever changed. Clearing the search without one
           left the rows filtered by it while the field showed empty, and the
           field's own reset to "" then matched and returned early: the centre
           reopened on one app's results with nothing to clear. */
        bool stale = !m_filter.isEmpty() || !m_search.isEmpty();
        m_filter.clear();
        m_search.clear();
        m_appSettingsVisible = false;

        /* "Always collapsed" has to mean every time the panel opens, not just
           the first. Expanding one of these groups is a look inside, not a
           change of setting, so the expand is dropped on close. Groups without
           the rule keep their expand — there the user is overriding an
           automatic guess, and re-folding it each time would be a nuisance. */
        if (!m_alwaysCollapsed.isEmpty()) {
            for (const Notification &n : std::as_const(m_all)) {
                if (m_alwaysCollapsed.contains(n.appName)) {
                    stale |= m_expanded.remove(n.groupKey());
                }
            }
        }
        if (stale) {
            rebuild();
        }
    }
    Q_EMIT panelOpenChanged();
}

void HistoryModel::setNewestFirst(bool on)
{
    if (m_newestFirst == on) {
        return;
    }
    m_newestFirst = on;
    rebuild();
}

void HistoryModel::rebuild()
{
    beginResetModel();
    m_rows.clear();

    /* Resolved here rather than when the filter is set: a filter can be
       applied before any matching notification exists, and the label would
       then stay empty forever. */
    m_filterLabel.clear();
    if (!m_filter.isEmpty()) {
        for (const Notification &n : std::as_const(m_all)) {
            if (n.groupKey() == m_filter) {
                m_filterLabel = n.appName;
                break;
            }
        }
    }

    /* Groups are ordered by their most recent member, and members keep their
       own order, so the newest thing is always at the top whichever group it
       belongs to. */
    QStringList order;
    QHash<QString, QList<Notification>> byGroup;
    m_matchCount = 0;
    for (const Notification &n : std::as_const(m_all)) {
        if (!m_filter.isEmpty() && n.groupKey() != m_filter) {
            continue;
        }
        if (!matchesSearch(n)) {
            continue;
        }
        ++m_matchCount;
        const QString key = n.groupKey();
        if (!byGroup.contains(key)) {
            order.append(key);
        }
        byGroup[key].append(n);
    }

    /* Newest-last is the default because the centre opens scrolled to the
       bottom, the way a message thread does: the thing you just heard arrive
       is under your eyes without scrolling. Groups reverse and so do their
       members, but the header still leads its own group — reversing the flat
       row list instead would put every header underneath its entries. */
    if (!m_newestFirst) {
        std::reverse(order.begin(), order.end());
        for (auto it = byGroup.begin(); it != byGroup.end(); ++it) {
            std::reverse(it.value().begin(), it.value().end());
        }
    }

    for (const QString &key : std::as_const(order)) {
        const QList<Notification> &items = byGroup.value(key);
        const bool collapsed = isCollapsed(key, items.first().appName, items.size());

        /* The newest member of the group. items are in display order, which
           is oldest-first when the newest is being shown at the bottom. */
        const Notification &newest = m_newestFirst ? items.first() : items.last();

        /* Occurrences, not rows. Since identical repeats collapse onto one
           entry, a group can be a single row that stands for 156 arrivals, and
           the two numbers stopped meaning the same thing. */
        int occurrences = 0;
        for (const Notification &it : items) {
            occurrences += qMax(1, it.repeatCount);
        }

        /* A group of one is a card on its own: no header. A deduplicated
           row standing for many arrivals used to keep a header so it would
           not read as part of the group above it; every group is now its
           own card or stack, spaced from the next, and the card says x156
           itself. */
        if (items.size() == 1 && (m_cards || occurrences == 1)) {
            Row row;
            row.groupKey = key;
            row.entry = items.first();
            row.count = occurrences;
            m_rows.append(row);
            continue;
        }

        /* Folded, a group is a stack, as in macOS Notification Centre: its
           newest entry on top with sheets beneath, and no header. It opens
           in place (toggleGroup) into a header and every entry. It used to
           be a header over the newest row, which read as a list of app names
           with a sample under each, not as a pile of notifications. */
        if (collapsed && m_cards) {
            Row row;
            row.groupKey = key;
            row.entry = newest;
            row.count = occurrences;
            row.collapsed = true;
            row.stackDepth = int(qMin<qsizetype>(2, items.size() - 1));
            m_rows.append(row);
            continue;
        }

        Row header;
        header.header = true;
        header.groupKey = key;
        header.appName = newest.appName;          // same app throughout the group
        header.icon = newest.appIcon;
        /* The number of notifications, which is what the user counts — not
           the number of rows we chose to draw them in. */
        header.count = occurrences;
        header.collapsed = collapsed;   // only ever true in the old list
        m_rows.append(header);
        /* The old list: a folded group is its header over its newest entry. */
        if (collapsed) {
            Row row;
            row.groupKey = key;
            row.entry = newest;
            m_rows.append(row);
            continue;
        }
        for (const Notification &n : items) {
            Row row;
            row.groupKey = key;
            row.entry = n;
            row.groupStart = false;
            m_rows.append(row);
        }
    }

    endResetModel();
    Q_EMIT changed();
    /* Not saved from here: rebuild() also runs for a search keystroke, a
       filter, a collapse or the centre opening, none of which changes what
       is on disk, and each rewrote history.json. The paths that change the
       entries start the save timer themselves. */
}

bool HistoryModel::isCollapsed(const QString &key, const QString &appName, int count) const
{
    /* An explicit expand still wins over always_collapsed — the rule sets how
       the group *opens*, not a lock. setPanelOpen() drops the expand again
       when the panel closes, which is what makes "always" literal. */
    if (m_expanded.contains(key)) {
        return false;
    }
    if (m_collapsed.contains(key)) {
        return true;
    }
    /* Drilled into one app, or searching: the point is to see the entries,
       and a stack would hide all but one of the matches. */
    if (m_cards && (!m_filter.isEmpty() || !m_search.isEmpty())) {
        return false;
    }
    if (m_alwaysCollapsed.contains(appName)) {
        return true;
    }
    /* Long groups start folded so a single noisy app cannot push everything
       else off the screen. */
    return m_autoCollapseOver > 0 && count > m_autoCollapseOver;
}

void HistoryModel::reloadRules()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("glassosdrc"));
    config->reparseConfiguration();

    QSet<QString> collapsed;
    const QStringList groups = config->groupList();
    for (const QString &groupName : groups) {
        if (!groupName.startsWith(QLatin1String("Rule "))) {
            continue;
        }
        KConfigGroup g(config, groupName);
        if (!g.readEntry("always_collapsed", false)) {
            continue;
        }
        /* Only an exact appname match is honoured. The rules engine matches
           globs, but a group is one concrete app, and expanding a glob across
           every app seen so far would make the panel's own toggle read back
           wrong for apps the user never configured. */
        const QString app = g.readEntry("appname", QString());
        if (!app.isEmpty()) {
            collapsed.insert(app);
        }
    }

    if (collapsed == m_alwaysCollapsed) {
        return;
    }
    m_alwaysCollapsed = collapsed;
    rebuild();
}

void HistoryModel::activateEntry(int row)
{
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).header) {
        return;
    }
    const Notification &n = m_rows.at(row).entry;
    Q_EMIT entryActivated(n.id, n.desktopEntry);
}

uint HistoryModel::latestId(uint id) const
{
    for (int hops = 0; hops < 256 && m_mergedInto.contains(id); ++hops) {
        id = m_mergedInto.value(id);
    }
    return id;
}

QVariantMap HistoryModel::rowInfo(int row) const
{
    if (row < 0 || row >= m_rows.size()) {
        return {};
    }
    const Row &r = m_rows.at(row);
    return QVariantMap{
        {QStringLiteral("isHeader"), r.header},
        {QStringLiteral("groupKey"), r.groupKey},
        {QStringLiteral("collapsed"), r.collapsed},
        {QStringLiteral("stacked"), r.stackDepth > 0},
        {QStringLiteral("id"), r.header ? 0u : r.entry.id},
    };
}

void HistoryModel::toggleGroup(const QString &key)
{
    /* Flip what the header shows. Working the state out again here used
       the header's count, which is occurrences (repeats included), while
       rebuild() decides on rows: a group of two rows standing for three
       arrivals showed open, read as folded here, and took a second press. */
    for (const Row &r : std::as_const(m_rows)) {
        if ((r.header || r.stackDepth > 0) && r.groupKey == key) {
            setGroupFolded(key, !r.collapsed);   // a header is open, a stack folded
            return;
        }
    }
    /* No header on screen (filtered out, or a group of one): the rows
       rebuild() would count. */
    int count = 0;
    for (const Notification &n : std::as_const(m_all)) {
        if (n.groupKey() == key) {
            ++count;
        }
    }
    /* The group key is the desktop entry where there is one, so it is not
       usable as the appname the always_collapsed rule is written under. */
    QString appName;
    for (const Notification &n : std::as_const(m_all)) {
        if (n.groupKey() == key) {
            appName = n.appName;
            break;
        }
    }
    setGroupFolded(key, !isCollapsed(key, appName, count));
}

void HistoryModel::setGroupFolded(const QString &key, bool fold)
{
    if (fold) {
        m_collapsed.insert(key);
        m_expanded.remove(key);
    } else {
        m_expanded.insert(key);
        m_collapsed.remove(key);
    }
    rebuild();
}

void HistoryModel::clearGroup(const QString &key)
{
    m_all.removeIf([&key](const Notification &n) {
        return n.groupKey() == key;
    });
    m_collapsed.remove(key);
    m_expanded.remove(key);
    rebuild();
    m_saveTimer.start();   // the entries changed; see rebuild()
}

void HistoryModel::removeIds(const QVariantList &ids)
{
    QSet<uint> gone;
    for (const QVariant &v : ids) {
        gone.insert(v.toUInt());
    }
    const qsizetype before = m_all.size();
    m_all.removeIf([&gone](const Notification &n) { return gone.contains(n.id); });
    if (m_all.size() == before) {
        return;
    }
    rebuild();
    m_saveTimer.start();
}

void HistoryModel::removeAt(int row)
{
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).header) {
        return;
    }
    const uint id = m_rows.at(row).entry.id;
    m_all.removeIf([id](const Notification &n) {
        return n.id == id;
    });
    rebuild();
    m_saveTimer.start();   // the entries changed; see rebuild()
}

void HistoryModel::clearAll()
{
    /* Clears what is on screen. With a filter active that means only the
       filtered app, which is what the button appears to promise. */
    if (m_filter.isEmpty()) {
        m_all.clear();
    } else {
        m_all.removeIf([this](const Notification &n) {
            return n.groupKey() == m_filter;
        });
    }
    rebuild();
    m_saveTimer.start();   // the entries changed; see rebuild()
}
