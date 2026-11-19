/*
    SPDX-FileCopyrightText: 2026 glassosd contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "notificationmodel.h"

#include "soundplayer.h"

#include <memory>
#include <utility>

#include <QDateTime>

#include <KIdleTime>

#include <algorithm>
#include <KWaylandExtras>

namespace
{
/* Whether the card's picture is the message's own (an inline image, or an
   image-path such as a contact's avatar) rather than the sender's logo. A
   notification restored without senderIcon counts as having one, so a
   hidden card never shows a picture it cannot tell apart from a logo. */
bool hasPicture(const Notification &n)
{
    return n.image.isValid() || (!n.appIcon.isEmpty() && n.appIcon != n.senderIcon);
}

/* What a hidden card shows in place of its picture. image-path used to
   overwrite app_icon, so this "app icon" was the contact's avatar: the lock
   screen hid a message's text and showed whose photo it was. */
QString senderLogo(const Notification &n)
{
    const QString logo = hasPicture(n) ? n.senderIcon : n.appIcon;
    return logo.isEmpty() ? QString() : QStringLiteral("image://icon/") + logo;
}
constexpr int kTickMs = 250;
}

NotificationModel::NotificationModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_tick.setInterval(kTickMs);
    connect(&m_tick, &QTimer::timeout, this, &NotificationModel::expireTick);
    m_ageTick.setInterval(30000);
    connect(&m_ageTick, &QTimer::timeout, this, [this] {
        if (!m_displayed.isEmpty()) {
            Q_EMIT dataChanged(index(0), index(m_displayed.size() - 1), {WhenRole});
        }
    });

    /* Every way the live count can move goes through one of these: rows
       arriving or leaving, or a row being marked closing. */
    auto announceLive = [this] {
        const int live = liveCount();
        if (live != m_lastLiveCount) {
            m_lastLiveCount = live;
            Q_EMIT liveCountChanged();
        }
    };
    connect(this, &QAbstractItemModel::rowsInserted, this, announceLive);
    connect(this, &QAbstractItemModel::rowsRemoved, this, announceLive);
    connect(this, &QAbstractItemModel::modelReset, this, announceLive);
    connect(this, &QAbstractItemModel::dataChanged, this, announceLive);

    auto *kit = KIdleTime::instance();
    connect(kit, &KIdleTime::timeoutReached, this, [this](int id, int) {
        if (id != m_idleTimeoutId) {
            return;
        }
        m_userIdle = true;
        qWarning("glassosd: session went idle (threshold %d ms) - holding expiry", m_idleThresholdMs);
        KIdleTime::instance()->catchNextResumeEvent();
    });
    connect(kit, &KIdleTime::resumingFromIdle, this, [this] {
        if (!m_userIdle) {
            return;
        }
        m_userIdle = false;
        qWarning("glassosd: resumed from idle - restarting dwell timers");
        /* Restart every visible notification's dwell from the moment the user
           came back, so nothing vanishes before it has been seen. */
        const QDateTime stamp = now();
        for (Notification &n : m_displayed) {
            n.displayedAt = stamp;
        }
    });
}

int NotificationModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_displayed.size();
}

QVariant NotificationModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_displayed.size()) {
        return {};
    }
    const Notification &n = m_displayed.at(index.row());
    switch (role) {
    case GroupCountRole: return 1 + int(m_members.value(n.id).size());
    case MembersRole: {
        /* A live group's other notifications, newest first, each masked
           exactly as a card of its own would be: they are read through
           roleFor(), the same path a row is. */
        QVariantList out;
        for (const Notification &m : m_members.value(n.id)) {
            out.append(QVariantMap{
                {QStringLiteral("id"), m.id},
                {QStringLiteral("summary"), roleFor(m, SummaryRole)},
                {QStringLiteral("body"), roleFor(m, BodyRole)},
                {QStringLiteral("iconSource"), roleFor(m, IconSourceRole)},
                {QStringLiteral("badgeSource"), roleFor(m, BadgeSourceRole)},
                {QStringLiteral("when"), roleFor(m, WhenRole)},
                {QStringLiteral("actions"), roleFor(m, ActionsRole)},
                {QStringLiteral("hasDefaultAction"), roleFor(m, HasDefaultActionRole)},
                {QStringLiteral("repeatCount"), 1},
            });
        }
        return out;
    }
    default:
        return roleFor(n, role);
    }
}

QVariant NotificationModel::roleFor(const Notification &n, int role) const
{

    if (m_screenLocked) {
        /* Nothing on a lock screen acts on the sender's behalf. KWin routes
           pointer input to a lock-screen overlay, so a button drawn here could
           be pressed by whoever is at the machine — "Delete", "Approve",
           "Mark as read" — without unlocking it. The keyboard stays with the
           greeter, so a reply field could not be typed into anyway. */
        switch (role) {
        case ActionsRole:          return QVariantList();
        case HasDefaultActionRole: return false;
        case InlineReplyRole:      return false;
        default:                   break;
        }
    }
    if (privacyActive()) {
        /* Withheld rather than removed: the card still shows that something
           arrived, and unlocking reveals it without the sender resending.
           The inline image goes with the text: it is a chat avatar, a photo
           preview, a QR code — content, not branding. The app icon stays. */
        /* Hide too, though a Hide row should never be here: it is dropped
           on arrival and when privacy comes on. This is the backstop for
           any path that slips one through (an in-place update was one), so
           the worst case is a card that says "hidden", not one that says
           everything. */
        const auto privacy = effectiveLockPrivacy(n);
        if (privacy == Notification::LockPrivacy::HideContent
            || privacy == Notification::LockPrivacy::Hide) {
            switch (role) {
            case SummaryRole: return tr("Notification hidden");
            case BodyRole:    return QString();
            case IconSourceRole:
                return senderLogo(n);
            case BadgeSourceRole:
                return QString();
            /* While sharing the screen the buttons stay (the person at the
               keyboard can still act), but their labels and the reply
               field's placeholder are the sender's text too: \"Reply to
               Jane\", \"Join: Board review\". They read as generic. */
            case ActionsRole: {
                QVariantList out;
                const QVariantList real = roleFor(n, NotificationModel::RealActionsRole).toList();
                for (int i = 0; i < real.size(); ++i) {
                    QVariantMap a = real.at(i).toMap();
                    a.insert(QStringLiteral("label"), real.size() == 1 ? tr("Action") : tr("Action %1").arg(i + 1));
                    out << a;
                }
                return out;
            }
            case ReplyPlaceholderRole: return tr("Reply");
            default:          break;
            }
        }
        /* Who, not what: the summary is left alone (Slack's names the sender
           or the conversation) and the body is reduced to what the rule's
           lockscreen_keep chose, which is the sender in a group chat. */
        if (effectiveLockPrivacy(n) == Notification::LockPrivacy::HideBody) {
            switch (role) {
            case BodyRole:
                return n.lockKeep.isEmpty()
                    ? tr("Message hidden")
                    : tr("%1: message hidden").arg(n.lockKeep.toHtmlEscaped());
            case IconSourceRole:
                return senderLogo(n);
            case BadgeSourceRole:
                return QString();
            default:          break;
            }
        }
    }

    switch (role) {
    case IdRole:         return n.id;
    case ClosingRole:    return n.closing;
    case StackTagRole:   return n.stackTag;
    case AppNameRole:    return n.appName;
    case SummaryRole:    return n.summary;
    case BodyRole:       return n.body;
    case UrgencyRole:    return static_cast<int>(n.urgency);
    case RealActionsRole:
    case ActionsRole: {
        /* The wire format is a flat [key, label, key, label, ...] list. The
           "default" key is special: it is what activating the notification
           body means, and per the spec it must not be drawn as a button. */
        QVariantList out;
        for (int i = 0; i + 1 < n.actions.size(); i += 2) {
            const QString key = n.actions.at(i);
            if (key == QLatin1String("default")) {
                continue;
            }
            if (key == QLatin1String("inline-reply")) {
                continue;   // rendered as a text field, not a button
            }
            out.append(QVariantMap{{QStringLiteral("key"), key},
                                   {QStringLiteral("label"), n.actions.at(i + 1)}});
        }
        return out;
    }
    case HasDefaultActionRole: {
        for (int i = 0; i + 1 < n.actions.size(); i += 2) {
            if (n.actions.at(i) == QLatin1String("default")) {
                return true;
            }
        }
        return false;
    }
    case GroupKeyRole:   return n.groupKey();
    case ProgressRole:         return n.progressValue;
    case InlineReplyRole:      return n.inlineReply;
    case ReplyPlaceholderRole: return n.replyPlaceholder.isEmpty()
                                      ? QStringLiteral("Reply…") : n.replyPlaceholder;
    case ReplySubmitTextRole:  return n.replySubmitText.isEmpty()
                                      ? QStringLiteral("Send") : n.replySubmitText;
    case WhenRole: {
        /* Apple-style relative time: "now" while fresh, then minutes. */
        const qint64 secs = n.received.secsTo(now());
        if (secs < 60) {
            return QStringLiteral("now");
        }
        if (secs < 3600) {
            return QStringLiteral("%1m ago").arg(secs / 60);
        }
        return n.received.toLocalTime().toString(QStringLiteral("HH:mm"));
    }
    case BadgeSourceRole:
        /* The sender's icon, small on the corner of a picture, the way macOS
           badges a contact's photo with the app it came through. Only when
           there is a picture: on a plain app icon it would be the same icon
           twice. */
        if (hasPicture(n) && !n.senderIcon.isEmpty()) {
            return QStringLiteral("image://icon/") + n.senderIcon;
        }
        return QString();
    case IconSourceRole:
        /* image-data outranks a themed name, matching the spec's ordering and
           mako's behaviour: an inline image is what the app actually wants
           shown, whereas app_icon is often a generic fallback. */
        if (n.image.isValid()) {
            return QStringLiteral("image://notifyimage/%1/%2").arg(n.id).arg(n.imageRev);
        }
        return n.appIcon.isEmpty() ? QString() : QStringLiteral("image://icon/") + n.appIcon;
    default:
        return {};
    }
}

QHash<int, QByteArray> NotificationModel::roleNames() const
{
    return {
        {IdRole, "notifId"},
        {AppNameRole, "appName"},
        {SummaryRole, "summary"},
        {BodyRole, "body"},
        {IconSourceRole, "iconSource"},
        {UrgencyRole, "urgency"},
        {ActionsRole, "actions"},
        {HasDefaultActionRole, "hasDefaultAction"},
        {GroupCountRole, "groupCount"},
        {MembersRole, "members"},
        {ClosingRole, "closing"},
        {StackTagRole, "stackTag"},
        {GroupKeyRole, "groupKey"},
        {WhenRole, "when"},
        {ProgressRole, "progress"},
        {InlineReplyRole, "inlineReply"},
        {ReplyPlaceholderRole, "replyPlaceholder"},
        {ReplySubmitTextRole, "replySubmitText"},
        {BadgeSourceRole, "badgeSource"},
    };
}

ImageData NotificationModel::imageFor(uint id) const
{
    for (const Notification &n : m_displayed) {
        if (n.id == id) {
            return n.image;
        }
    }
    for (const QList<Notification> &list : m_members) {
        for (const Notification &m : list) {
            if (m.id == id) {
                return m.image;
            }
        }
    }
    for (const Notification &n : m_waiting) {
        if (n.id == id) {
            return n.image;
        }
    }
    return {};
}

int NotificationModel::defaultTimeoutFor(Urgency u) const
{
    /* Mirrors the timeouts already tuned in the existing dunstrc. */
    switch (u) {
    case Urgency::Low:      return m_timeoutLow;
    case Urgency::Critical: return m_timeoutCritical;   // 0 = never expires
    case Urgency::Normal:
    default:                return m_timeoutNormal;
    }
}

bool NotificationModel::isLive(uint id) const
{
    if (indexOfDisplayed(id) >= 0 || ownerOf(id) != 0) {
        return true;
    }
    for (const Notification &n : m_waiting) {
        if (n.id == id) {
            return true;
        }
    }
    return false;
}

bool NotificationModel::hasDefaultAction(uint id) const
{
    auto offers = [](const Notification &n) {
        for (int i = 0; i + 1 < n.actions.size(); i += 2) {
            if (n.actions.at(i) == QLatin1String("default")) {
                return true;
            }
        }
        return false;
    };
    const int row = indexOfDisplayed(id);
    if (row >= 0) {
        return offers(m_displayed.at(row));
    }
    for (const Notification &n : m_waiting) {
        if (n.id == id) {
            return offers(n);
        }
    }
    for (const QList<Notification> &list : m_members) {
        for (const Notification &m : list) {
            if (m.id == id) {
                return offers(m);
            }
        }
    }
    return false;
}

int NotificationModel::indexOfDisplayed(uint id) const
{
    for (int i = 0; i < m_displayed.size(); ++i) {
        /* A closing row is logically gone — it is only still here so the view
           can animate it away. Hiding it here means replace-by-id, coalescing
           and activation all treat it as absent, without each of them having
           to know that the state exists. */
        if (m_displayed.at(i).id == id && !m_displayed.at(i).closing) {
            return i;
        }
    }
    return -1;
}

bool NotificationModel::replaceById(const Notification &n)
{
    /* Deliberately emits no NotificationClosed. The id is unchanged, so this
       is an update of an existing notification rather than the closing of one
       — dunst's queues_notification_replace_id() does the same. */
    const int row = indexOfDisplayed(n.id);
    if (row >= 0) {
        Notification updated = n;
        updated.groupCount = m_displayed.at(row).groupCount;
        updated.displayedAt = now();  // restart the dwell
        if (updated.appIcon.isEmpty() && !updated.image.isValid()) {
            updated.appIcon = m_displayed.at(row).appIcon;
        }
        if (updated.senderIcon.isEmpty()) {
            updated.senderIcon = m_displayed.at(row).senderIcon;
        }
        m_displayed[row] = updated;
        Q_EMIT dataChanged(index(row), index(row));
        return true;
    }
    for (int i = 0; i < m_waiting.size(); ++i) {
        if (m_waiting.at(i).id == n.id) {
            Notification updated = n;
            updated.groupCount = m_waiting.at(i).groupCount;
            m_waiting[i] = updated;
            return true;
        }
    }
    /* A member of a group updating itself: in place, where it is. */
    if (const uint owner = ownerOf(n.id)) {
        for (Notification &m : m_members[owner]) {
            if (m.id == n.id) {
                const QString icon = m.appIcon, sender = m.senderIcon;
                m = n;
                if (m.appIcon.isEmpty() && !m.image.isValid()) {
                    m.appIcon = icon;
                }
                if (m.senderIcon.isEmpty()) {
                    m.senderIcon = sender;
                }
            }
        }
        groupChanged(owner);
        return true;
    }
    return false;
}

bool NotificationModel::replaceByTag(const Notification &n)
{
    if (n.stackTag.isEmpty()) {
        return false;
    }
    /* Matched on tag *and* app, per dunst — otherwise two apps using the same
       generic tag would clobber each other. Displayed is searched first. */
    /* Not a closing row. It is only still here to finish its exit; written
       over, it came back as a live card whose reveal had already run to 0,
       so it held a slot and expired without ever being seen, and its old id
       was announced closed a second time. PowerDevil re-sends its battery
       card often enough to land inside the exit of the one just dismissed. */
    auto matches = [&n](const Notification &old) {
        return !old.closing && !old.stackTag.isEmpty() && old.stackTag == n.stackTag
            && old.appName == n.appName;
    };

    for (int i = 0; i < m_displayed.size(); ++i) {
        if (matches(m_displayed.at(i))) {
            const Notification old = m_displayed.at(i);
            Notification updated = n;
            updated.groupCount = old.groupCount;
            /* Only restart the expiry clock when something visible actually
               changed.

               PowerDevil re-sends the identical "Device Battery Low (6%
               Remaining)" once or twice a second for as long as the device is
               low — 66 byte-identical copies in one afternoon here. Each was
               correctly collapsed onto the one card by the tag, but each also
               reset displayedAt, so the 8s timer restarted roughly eight times
               per expiry and the card could never die. The rule was working;
               the card was simply immortal, which looks exactly like the
               spam the rule was meant to stop.

               A tagged notification whose text has genuinely changed (12% ->
               9% -> 6%) still earns a fresh timer: that is new information and
               the point of replacing by tag. A repeat of what is already on
               screen is not. */
            const bool sameContent = old.summary == n.summary
                                  && old.body    == n.body;
            updated.displayedAt = (sameContent && old.displayedAt.isValid())
                                      ? old.displayedAt
                                      : now();
            if (updated.appIcon.isEmpty() && !updated.image.isValid()) {
                updated.appIcon = old.appIcon;   // inherit rather than go blank
            }
            if (updated.senderIcon.isEmpty()) {
                updated.senderIcon = old.senderIcon;
            }
            m_displayed[i] = updated;
            if (m_hovered.remove(old.id)) {
                m_hovered.insert(updated.id);   // same card, same pointer
            }
            Q_EMIT dataChanged(index(i), index(i));
            /* A different id is going away, so this one does need announcing.
               Expired, not Dismissed — the user never acted on it. */
            Q_EMIT notificationClosed(old.id, CloseReason::Expired);
            return true;
        }
    }
    for (int i = 0; i < m_waiting.size(); ++i) {
        if (matches(m_waiting.at(i))) {
            const Notification old = m_waiting.at(i);
            Notification updated = n;
            updated.groupCount = old.groupCount;
            if (updated.appIcon.isEmpty() && !updated.image.isValid()) {
                updated.appIcon = old.appIcon;
            }
            if (updated.senderIcon.isEmpty()) {
                updated.senderIcon = old.senderIcon;
            }
            m_waiting[i] = updated;
            Q_EMIT notificationClosed(old.id, CloseReason::Expired);
            return true;
        }
    }
    return false;
}

bool NotificationModel::coalesce(const Notification &n)
{
    /* Stack tags outrank coalescing: a tagged notification has explicitly
       asked to be replaced by its successor, not counted alongside it. */
    if (!n.stackTag.isEmpty() || m_coalesceThreshold <= 0) {
        return false;
    }

    const QString key = n.groupKey();
    const QDateTime stamp = now();

    QList<QDateTime> &times = m_recentByGroup[key];
    times.removeIf([&](const QDateTime &t) {
        return t.msecsTo(stamp) > m_coalesceWindowMs;
    });
    times.append(stamp);

    if (times.size() < m_coalesceThreshold) {
        return false;
    }

    /* Absorb every existing untagged card from this group, not just one. The
       burst's first messages arrived before the threshold was crossed and are
       already on screen as separate cards; without this they would linger
       beside the merged one and the count would understate the total. A card
       already closing was dismissed and is not part of the burst any more. */
    auto member = [&key](const Notification &old) {
        return old.groupKey() == key && old.stackTag.isEmpty() && !old.closing;
    };
    /* Everything of the group so far becomes a member of the new card, and
       stays live: each absorbed notification keeps its id, actions and reply
       with its sender, who is not told anything happened. Only the card on
       screen changes. */
    QList<Notification> members;
    auto absorb = [&](const Notification &old) {
        members << old;
        members << m_members.take(old.id);
    };
    QList<uint> onScreen;
    for (const Notification &old : std::as_const(m_displayed)) {
        if (member(old)) {
            onScreen << old.id;
        }
    }
    for (int i = m_waiting.size() - 1; i >= 0; --i) {
        if (member(m_waiting.at(i))) {
            absorb(m_waiting.takeAt(i));
        }
    }
    for (uint id : std::as_const(onScreen)) {
        absorb(m_displayed.at(indexOfDisplayed(id)));
    }
    /* Newest first. A burst arrives within the same millisecond often
       enough that the time alone left the order to chance; ids rise with
       arrival, so they break the tie. */
    std::sort(members.begin(), members.end(), [](const Notification &a, const Notification &b) {
        return a.received != b.received ? a.received > b.received : a.id > b.id;
    });
    while (members.size() > kMaxMembers) {
        Q_EMIT notificationClosed(members.takeLast().id, CloseReason::Expired);
    }

    Notification merged = n;
    merged.groupCount = 1 + int(members.size());
    m_members.insert(merged.id, members);

    /* The group is on screen: its newest card becomes the merged card where it
       stands, and the rest leave the screen (their notifications live on as
       members). These used to be deleted outright and the merged card queued
       afresh, so on screen the old card vanished mid-frame while the new one
       faded in thirty pixels away and the two overlapped (seen frame by
       frame). */
    if (!onScreen.isEmpty()) {
        const uint newest = m_newestFirst ? onScreen.takeFirst() : onScreen.takeLast();
        const int row = indexOfDisplayed(newest);
        merged.displayedAt = now();
        /* The card keeps its place and the pointer on it: the hold moves to
           the id it now shows, or an open group expired under the reader the
           moment another message of the burst arrived. */
        if (m_hovered.remove(newest)) {
            m_hovered.insert(merged.id);
        }
        m_displayed[row] = merged;
        Q_EMIT dataChanged(index(row), index(row));
        for (uint id : std::as_const(onScreen)) {
            retireRow(id);
        }
        return true;
    }

    /* Nothing of it on screen yet: front of the queue, so a continuing burst
       keeps one live card rather than re-popping behind whatever else is up. */
    m_waiting.prepend(merged);
    return true;
}

uint NotificationModel::ownerOf(uint id) const
{
    for (auto it = m_members.cbegin(); it != m_members.cend(); ++it) {
        for (const Notification &m : it.value()) {
            if (m.id == id) {
                return it.key();
            }
        }
    }
    return 0;
}

void NotificationModel::groupChanged(uint ownerId)
{
    const int row = indexOfDisplayed(ownerId);
    if (row >= 0) {
        m_displayed[row].groupCount = 1 + int(m_members.value(ownerId).size());
        Q_EMIT dataChanged(index(row), index(row), {GroupCountRole, MembersRole});
        return;
    }
    for (Notification &w : m_waiting) {
        if (w.id == ownerId) {
            w.groupCount = 1 + int(m_members.value(ownerId).size());
        }
    }
}

bool NotificationModel::closeMember(uint id, uint reason)
{
    const uint owner = ownerOf(id);
    if (owner == 0) {
        return false;
    }
    auto it = m_members.find(owner);
    it->removeIf([id](const Notification &m) { return m.id == id; });
    if (it->isEmpty()) {
        m_members.erase(it);
    }
    m_hovered.remove(id);
    Q_EMIT notificationClosed(id, reason);
    groupChanged(owner);
    return true;
}

void NotificationModel::retireRow(uint id)
{
    const int row = indexOfDisplayed(id);
    if (row < 0) {
        return;
    }
    m_hovered.remove(id);
    if (m_exitMs <= 0) {
        beginRemoveRows({}, row, row);
        m_displayed.removeAt(row);
        endRemoveRows();
        update();
        return;
    }
    m_displayed[row].closing = true;
    const QModelIndex ix = index(row, 0);
    Q_EMIT dataChanged(ix, ix, {ClosingRole});
    QTimer::singleShot(m_exitMs, this, [this, id] { finishClose(id); });
}

uint NotificationModel::insert(Notification n)
{
    n.received = now();
    n.imageRev = ++m_imageRev;
    if (n.timeoutMs < 0) {
        n.timeoutMs = defaultTimeoutFor(n.urgency);
    }

    /* An update in place has to meet the lock screen's Hide as an arrival
       does: the new version may ask to be hidden (a rule matching its new
       text, a portal update adding hide-on-lock-screen) when the old one
       did not. Without this it was drawn in full while locked or shared. */
    if ((n.id != 0 && replaceById(n)) || replaceByTag(n)) {
        if (privacyActive()) {
            dropHiddenForPrivacy();
        }
        update();
        return n.id;
    }
    /* An identical repeat, inside the window a rule asked for.

       set_stack_tag alone is not enough. It collapses repeats onto the card
       that is already up, but replaceByTag() searches only what is displayed
       or waiting — once the card expires, the next copy matches nothing and
       opens a fresh popup. A sender that repeats indefinitely therefore gets a
       new card every time the previous one times out. Measured here with
       PowerDevil: 66 byte-identical "Device Battery Low (6% Remaining)" in one
       afternoon, one or two every second.

       Keyed on the content rather than on the tag, so the same rule still lets
       12% -> 9% -> 6% through as three separate pieces of news. History is
       untouched: this suppresses the popup, not the record. */
    if (n.repeatWindowSec >= 0) {
        const QString key = n.appName + QLatin1Char('\x1f')
                          + n.summary + QLatin1Char('\x1f') + n.body;
        const QDateTime last = m_lastShownByContent.value(key);
        if (last.isValid() && last.secsTo(n.received) < n.repeatWindowSec) {
            Q_EMIT notificationClosed(n.id, CloseReason::Expired);
            return n.id;
        }
        m_lastShownByContent.insert(key, n.received);
        /* Bounded: one entry per distinct message, and a desktop does not
           produce unboundedly many. Trimmed anyway if it ever runs away. */
        if (m_lastShownByContent.size() > 512) {
            m_lastShownByContent.clear();
        }
    }

    /* The sender asked not to appear on a lock screen, and one is up. It is
       already in history, which is where it will be found after unlocking —
       the same bargain a rule's no-popup makes. */
    if (privacyActive() && effectiveLockPrivacy(n) == Notification::LockPrivacy::Hide) {
        Q_EMIT notificationClosed(n.id, CloseReason::Expired);
        return n.id;
    }

    /* A rule asked for no popup. The sender is still told the notification
       ended, so it does not sit waiting on a close signal that never comes. */
    if (n.skipDisplay) {
        Q_EMIT notificationClosed(n.id, CloseReason::Expired);
        return n.id;
    }

    /* Do Not Disturb, or an application asking for quiet, suppresses the popup
       but must never lose the message; history still receives it via the
       closed signal path. */
    if (quiet()) {
        Q_EMIT notificationClosed(n.id, CloseReason::Expired);
        return n.id;
    }

    /* Coalescing comes after every check that can suppress the popup, not
       before them. It used to run first, so the second message of a burst
       sailed past Do Not Disturb, an application's Inhibit() (a screen share),
       a rule's no-popup and the lock screen's Hide, and was shown merged —
       with its text — while all of those said nothing should appear. */
    if (coalesce(n)) {
        update();
        return n.id;
    }

    /* Before the queue, not after: a notification held back by the display
       limit has still arrived, and the sound is what tells you so. The player
       rate-limits, so a burst that coalesces into one card is one sound. */
    if (n.sound != QLatin1String("none")) {
        const QString name = n.sound.isEmpty()
            ? SoundPlayer::nameFor(n.category, int(n.urgency))
            : n.sound;
        if (!name.isEmpty()) {
            Q_EMIT soundWanted(name);
        }
    }

    m_waiting.append(n);
    update();
    return n.id;
}

bool NotificationModel::closeId(uint id, uint reason)
{
    if (closeMember(id, reason)) {
        return true;
    }

    /* A group's top card. Withdrawn by its sender (CloseNotification), only
       that one goes and the newest member takes its place, since the rest
       are still news. Any other end — it expired, the user dismissed the
       stack, Clear all — closes the whole group with it, each member's
       sender told the same reason. */
    if (m_members.contains(id)) {
        if (reason == CloseReason::Closed) {
            QList<Notification> rest = m_members.take(id);
            Notification next = rest.takeFirst();
            next.groupCount = 1 + int(rest.size());
            if (!rest.isEmpty()) {
                m_members.insert(next.id, rest);
            }
            const int at = indexOfDisplayed(id);
            if (at >= 0) {
                next.displayedAt = m_displayed.at(at).displayedAt;
                if (m_hovered.remove(id)) {
                    m_hovered.insert(next.id);
                }
                m_displayed[at] = next;
                Q_EMIT dataChanged(index(at), index(at));
            } else {
                for (Notification &w : m_waiting) {
                    if (w.id == id) {
                        w = next;
                    }
                }
            }
            Q_EMIT notificationClosed(id, reason);
            return true;
        }
        for (const Notification &m : m_members.take(id)) {
            m_hovered.remove(m.id);
            Q_EMIT notificationClosed(m.id, reason);
        }
    }

    const int row = indexOfDisplayed(id);
    if (row >= 0) {
        m_hovered.remove(id);
        /* Told to the sender straight away, whatever the animation does. The
           spec's NotificationClosed answers "has this been dealt with", and
           holding it back for the length of a fade would make every client
           wait on a detail of our rendering. */
        Q_EMIT notificationClosed(id, reason);

        if (m_exitMs <= 0) {
            beginRemoveRows({}, row, row);
            m_displayed.removeAt(row);
            endRemoveRows();
            update();
            return true;
        }

        m_displayed[row].closing = true;
        const QModelIndex ix = index(row, 0);
        Q_EMIT dataChanged(ix, ix, {ClosingRole});
        QTimer::singleShot(m_exitMs, this, [this, id] { finishClose(id); });
        return true;
    }
    for (int i = 0; i < m_waiting.size(); ++i) {
        if (m_waiting.at(i).id == id) {
            m_waiting.removeAt(i);
            Q_EMIT notificationClosed(id, reason);
            update();
            return true;
        }
    }
    return false;
}

int NotificationModel::closeEverything(uint reason)
{
    /* The queue goes first, and without update(). Closing a queued entry
       through closeId() runs update(), which promotes the next queued entry
       into a displayed row, which the loop then closes, which promotes the
       next: every queued notification became an on-screen card animating
       out, all at once. 400 queued from distinct senders made the popup
       surface 458x20295, past the GL size limit, and the daemon segfaulted
       in eglSwapBuffers — from pressing Clear all, or opening the centre. */
    const QList<Notification> waiting = std::exchange(m_waiting, {});
    for (const Notification &n : waiting) {
        for (const Notification &m : m_members.take(n.id)) {
            Q_EMIT notificationClosed(m.id, reason);
        }
        Q_EMIT notificationClosed(n.id, reason);
    }

    /* Ids are collected first: closeId() mutates m_displayed, so iterating
       it directly would invalidate the loop underneath itself. */
    QList<uint> ids;
    ids.reserve(m_displayed.size());
    for (const Notification &n : std::as_const(m_displayed)) {
        ids << n.id;
    }
    for (uint id : std::as_const(ids)) {
        closeId(id, reason);
    }
    update();
    return int(waiting.size() + ids.size());
}

int NotificationModel::dismissAll()
{
    return closeEverything(CloseReason::Dismissed);
}

int NotificationModel::hideForCentre()
{
    return closeEverything(CloseReason::Expired);
}

void NotificationModel::setHoverPause(bool on)
{
    if (m_hoverPause == on) {
        return;
    }
    m_hoverPause = on;
    if (!on) {
        /* Anything currently held by the pointer resumes counting from now,
           not from whenever it appeared — otherwise turning the setting off
           makes every hovered popup vanish at once. */
        const QDateTime stamp = now();
        for (uint id : std::as_const(m_hovered)) {
            const int row = indexOfDisplayed(id);
            if (row >= 0) {
                m_displayed[row].displayedAt = stamp;
            }
        }
        m_hovered.clear();
    }
}

void NotificationModel::setHovered(uint id, bool hovered)
{
    if (!m_hoverPause) {
        return;
    }
    /* Logged once per transition: this is the only way to confirm from
       outside that pointer events are reaching the card at all, which
       depends on the layer surface's input region being right. */
    qDebug("glassosd: notification %u hover=%s", id, hovered ? "in" : "out");
    if (hovered) {
        m_hovered.insert(id);
    } else {
        /* Restart the dwell from now rather than resuming mid-count, so a
           notification you just finished reading does not vanish instantly. */
        if (m_hovered.remove(id)) {
            const int row = indexOfDisplayed(id);
            if (row >= 0) {
                m_displayed[row].displayedAt = now();
            }
        }
    }
}

void NotificationModel::snooze(uint id)
{
    const int row = indexOfDisplayed(id);
    if (row < 0) {
        return;
    }
    /* Copied before closing: closeId() marks the row and a later sweep removes
       it, so a reference taken here would not survive the call. */
    const Notification n = m_displayed.at(row);
    Q_EMIT snoozeRequested(n);
    /* Dismissed, not Expired: the user acted on it. The sender is told it is
       closed and finds out nothing about the snooze, which is correct — a
       notification coming back later is our behaviour, not a change to the
       one it sent. */
    closeId(id, CloseReason::Dismissed);
}

void NotificationModel::dismiss(uint id)
{
    closeId(id, CloseReason::Dismissed);
}

void NotificationModel::sendReply(uint id, const QString &text)
{
    if (text.isEmpty() || m_screenLocked || !isLive(id)) {
        return;
    }
    Q_EMIT replied(id, text);
    closeId(id, CloseReason::Dismissed);
}

namespace
{
/* Long enough that the token normally arrives first, short enough that a
   compositor which never answers cannot make a button feel broken. */
constexpr int kActivationTokenWaitMs = 200;
} // namespace

const Notification *NotificationModel::find(uint id) const
{
    const int row = indexOfDisplayed(id);
    if (row >= 0) {
        return &m_displayed.at(row);
    }
    for (const Notification &n : m_waiting) {
        if (n.id == id) {
            return &n;
        }
    }
    for (const QList<Notification> &list : m_members) {
        for (const Notification &m : list) {
            if (m.id == id) {
                return &m;
            }
        }
    }
    return nullptr;
}

void NotificationModel::open(QQuickWindow *window, uint id)
{
    if (m_screenLocked || !isLive(id)) {
        return;
    }
    /* What a click on a banner means on macOS, every time: open it. The
       sender's own "default" action when it offers one; otherwise the app
       it came from, as a click in the centre already did. A click used to
       dismiss a card with no default action, or expand a stacked one,
       depending on state nobody could see. */
    if (hasDefaultAction(id)) {
        activate(window, id, QStringLiteral("default"));
        return;
    }
    const Notification *n = find(id);
    const QString entry = n ? n->desktopEntry : QString();
    if (entry.isEmpty() || !window) {
        if (!entry.isEmpty()) {
            Q_EMIT launchRequested(entry, QString());
        }
        closeId(id, CloseReason::Dismissed);
        return;
    }
    /* With a token, so the app's window may come to the front. Same bounded
       wait as activate(). */
    auto fired = std::make_shared<bool>(false);
    auto fire = [this, id, entry, fired](const QString &token) {
        if (*fired) {
            return;
        }
        *fired = true;
        Q_EMIT launchRequested(entry, token);
        if (isLive(id)) {
            closeId(id, CloseReason::Dismissed);
        }
    };
    auto future = KWaylandExtras::xdgActivationToken(window, 0, entry);
    future.then(this, [fire](const QString &token) { fire(token); });
    QTimer::singleShot(kActivationTokenWaitMs, this, [fire] { fire(QString()); });
}

void NotificationModel::activate(QQuickWindow *window, uint id, const QString &key)
{
    if (m_screenLocked || !isLive(id)) {
        return;   // see data(): acting needs the session unlocked
    }
    const Notification *found = find(id);
    const QString appId = found ? found->desktopEntry : QString();

    if (!window) {
        invokeAction(id, key);
        return;
    }

    /* Ask the compositor for a token tied to this click, then hand it to the
       sender just before the action itself so it can raise its window.

       Bounded, because the action used to wait on that round trip with nothing
       to stop it taking as long as it liked. The token is a courtesy — it lets
       the sender raise a window without tripping focus stealing prevention —
       and it is not worth making someone wait to find out whether their click
       did anything. If it has not arrived by the deadline the action goes
       without it, and a late token is dropped rather than invoking twice. */
    auto fired = std::make_shared<bool>(false);
    auto fire = [this, id, key, fired](const QString &token) {
        if (*fired) {
            return;
        }
        *fired = true;
        /* It may have expired or been replaced while the token was on its
           way, and its sender has then been told it closed. */
        if (!isLive(id)) {
            return;
        }
        if (!token.isEmpty()) {
            Q_EMIT activationToken(id, token);
        }
        invokeAction(id, key);
    };

    auto future = KWaylandExtras::xdgActivationToken(window, 0, appId);
    future.then(this, [fire](const QString &token) { fire(token); });
    QTimer::singleShot(kActivationTokenWaitMs, this, [fire] { fire(QString()); });
}

void NotificationModel::invokeAction(uint id, const QString &key)
{
    /* Only while it is live. A card stays clickable through its exit, so a
       double click, or a click during the fade, sent ActionInvoked a second
       time, after NotificationClosed — the spec's order the other way round,
       and some senders act twice. closeId() below makes the second one dead. */
    if (m_screenLocked || !isLive(id)) {
        return;
    }
    Q_EMIT actionInvoked(id, key);
    closeId(id, CloseReason::Dismissed);
}

int NotificationModel::liveCount() const
{
    return int(std::count_if(m_displayed.cbegin(), m_displayed.cend(),
                             [](const Notification &n) { return !n.closing; }));
}

int NotificationModel::effectiveLimit() const
{
    if (m_limit == 0) {
        return std::numeric_limits<int>::max();
    }
    /* Reserve a slot for the "+N more" row, but only when there is genuinely
       an overflow to indicate — dunst additionally requires limit > 1, else a
       limit of 1 would leave no room for any notification at all. */
    if (m_indicateHidden && m_limit > 1 && liveCount() + m_waiting.size() > m_limit) {
        return m_limit - 1;
    }
    return m_limit;
}

int NotificationModel::newestLiveRow() const
{
    const int n = m_displayed.size();
    for (int k = 0; k < n; ++k) {
        const int row = m_newestFirst ? k : n - 1 - k;
        if (!m_displayed.at(row).closing) {
            return row;
        }
    }
    return -1;
}

/* Reversing the rows keeps the stack consistent when the anchor flips (a
   position change at runtime): the cards already up would otherwise sit in
   the old order with new ones arriving at the other end. A reset, not moves:
   it is rare and it is a jump the user just asked for. */
void NotificationModel::setNewestFirst(bool on)
{
    if (m_newestFirst == on) {
        return;
    }
    m_newestFirst = on;
    if (m_displayed.size() > 1) {
        beginResetModel();
        std::reverse(m_displayed.begin(), m_displayed.end());
        endResetModel();
    }
}

void NotificationModel::update()
{
    const int limit = effectiveLimit();

    /* Counted as live cards only. A closing row is on its way out and only
       still here to be animated; counting it held a slot that nothing was
       using, and (below) made it a candidate to be queued. */
    while (liveCount() < limit && !m_waiting.isEmpty()) {
        Notification n = m_waiting.takeFirst();
        if (n.closing) {
            continue;   // never shown again; see below
        }
        n.displayedAt = now();
        const int at = m_newestFirst ? 0 : m_displayed.size();
        beginInsertRows({}, at, at);
        m_displayed.insert(at, n);
        endInsertRows();
    }

    /* Shrinking the limit (e.g. once the indicator slot is needed) pushes the
       newest live card back to the front of the queue rather than dropping it.

       Never a closing one. That was the ghost card: a burst arriving while
       the newest card faded out took that card back into the queue mid-fade,
       where its finishClose() could no longer find it. When it came out again
       it was still marked closing — drawn, but ignoring clicks, never
       expiring and invisible to Dismiss All, which all treat closing rows as
       already gone. */
    while (liveCount() > limit) {
        const int row = newestLiveRow();
        if (row < 0) {
            break;
        }
        beginRemoveRows({}, row, row);
        const Notification n = m_displayed.takeAt(row);
        endRemoveRows();
        m_waiting.prepend(n);
    }

    /* Compare against the last value actually emitted, not against the size
       on entry: insert() appends to m_waiting *before* calling update(), so an
       entry snapshot already contains the new arrival. When nothing then moves
       between the lists the two match, no signal fires, and the "+N more"
       label silently goes stale — it read "+2" while four were really queued. */
    if (m_waiting.size() != m_lastHiddenCount) {
        m_lastHiddenCount = m_waiting.size();
        Q_EMIT hiddenCountChanged();
    }
    syncTick();
}

void NotificationModel::syncTick()
{
    bool work = !m_recentByGroup.isEmpty();
    for (const Notification &n : std::as_const(m_displayed)) {
        if (work) {
            break;
        }
        work = n.timeoutMs > 0 && !n.closing;
    }
    if (work && !m_tick.isActive()) {
        m_tick.start();
    } else if (!work && m_tick.isActive()) {
        m_tick.stop();
    }
    const bool shown = !m_displayed.isEmpty();
    if (shown && !m_ageTick.isActive()) {
        m_ageTick.start();
    } else if (!shown && m_ageTick.isActive()) {
        m_ageTick.stop();
    }
}

/* The row's index will have moved if anything above it went first, and
   indexOfDisplayed() deliberately cannot see closing rows, so find it by
   hand. */
void NotificationModel::finishClose(uint id)
{
    for (int i = 0; i < m_displayed.size(); ++i) {
        if (m_displayed.at(i).id != id || !m_displayed.at(i).closing) {
            continue;
        }
        beginRemoveRows({}, i, i);
        m_displayed.removeAt(i);
        endRemoveRows();
        update();
        return;
    }
}

void NotificationModel::expireTick()
{
    /* Drop coalescing history for groups that have gone quiet. The per-group
       list is pruned on use, but the map entry itself was never removed, so
       every app that ever sent a notification left a key behind for the life
       of the session. */
    if (m_coalesceWindowMs > 0) {
        const QDateTime cutoff = now();
        for (auto it = m_recentByGroup.begin(); it != m_recentByGroup.end();) {
            it.value().removeIf([&](const QDateTime &t) {
                return t.msecsTo(cutoff) > m_coalesceWindowMs;
            });
            it = it.value().isEmpty() ? m_recentByGroup.erase(it) : std::next(it);
        }
    }

    /* Hold everything while the user is away. The point is "nobody is here to
       read these", which is a property of the session, not of any one
       message. */
    if (m_userIdle) {
        syncTick();
        return;
    }
    /* Likewise while locked. A card on the lock screen is waiting for someone
       to come back, and expiring it there means it was never read; on unlock
       setScreenLocked() restarts the dwell, as resuming from idle does. */
    if (m_screenLocked) {
        syncTick();
        return;
    }

    const QDateTime stamp = now();
    QList<uint> expired;
    for (const Notification &n : std::as_const(m_displayed)) {
        if (n.timeoutMs <= 0 || !n.displayedAt.isValid()) {
            continue;   // 0 means never expire, per the spec
        }
        if (m_hovered.contains(n.id)) {
            continue;   // being read
        }
        if (n.displayedAt.msecsTo(stamp) >= n.timeoutMs) {
            expired << n.id;
        }
    }
    for (uint id : std::as_const(expired)) {
        closeId(id, CloseReason::Expired);
    }
    syncTick();
}

void NotificationModel::setIdleThresholdMs(int ms)
{
    /* The Wayland idle protocol (ext_idle_notification_v1) is
       notification-based: there is no "how long has the session been idle"
       query, so KIdleTime::idleTime() simply returns 0 here. An earlier
       attempt compared against it and therefore never fired. The supported
       shape is a registered timeout plus the resume signal.

       The signals are connected once, in the constructor — Qt::UniqueConnection
       cannot be used with a lambda (it asserts at runtime), so re-connecting
       here on every settings reload would stack duplicate handlers. */
    auto *kit = KIdleTime::instance();
    if (m_idleTimeoutId >= 0) {
        kit->removeIdleTimeout(m_idleTimeoutId);
        m_idleTimeoutId = -1;
    }
    m_userIdle = false;
    m_idleThresholdMs = ms;
    if (ms > 0) {
        m_idleTimeoutId = kit->addIdleTimeout(ms);
    }
}

void NotificationModel::setInhibited(bool inhibited)
{
    if (m_inhibited == inhibited) {
        return;
    }
    m_inhibited = inhibited;
    Q_EMIT inhibitedChanged();
    /* Anything held back while inhibited is released the moment it lifts —
       update() promotes from the queue, and the queue is where a suppressed
       notification would be if the limit had also been reached. */
    update();
}

void NotificationModel::setBusyQuiet(bool on)
{
    if (m_busyQuiet == on) {
        return;
    }
    m_busyQuiet = on;
    /* Whatever was held back is released as soon as it lifts. */
    update();
}

void NotificationModel::setDoNotDisturb(bool dnd)
{
    if (m_dnd == dnd) {
        return;
    }
    m_dnd = dnd;
    Q_EMIT doNotDisturbChanged();
}

Notification::LockPrivacy NotificationModel::effectiveLockPrivacy(const Notification &n) const
{
    /* A sender that expressed a preference always wins: the configured
       default exists to cover the overwhelming majority that say nothing, not
       to overrule the few that do. */
    return n.lockPrivacy == Notification::LockPrivacy::Unset ? m_lockDefault : n.lockPrivacy;
}

void NotificationModel::setLockPrivacyDefault(Notification::LockPrivacy policy)
{
    if (m_lockDefault == policy) {
        return;
    }
    m_lockDefault = policy;
    /* A default switched to Hide while privacy is on turns every card that
       expressed no preference into one that must not be on screen. */
    if (privacyActive()) {
        dropHiddenForPrivacy();
    }
    if (privacyActive() && !m_displayed.isEmpty()) {
        Q_EMIT dataChanged(index(0), index(m_displayed.size() - 1),
                           {SummaryRole, BodyRole, IconSourceRole, BadgeSourceRole, ActionsRole, ReplyPlaceholderRole});
    }
}

void NotificationModel::dropHiddenForPrivacy()
{
    /* The queue first. Cards held back by the display limit were promoted
       by update() as the displayed ones below closed, and lived out their
       timeouts on the lock screen as if Hide had never been asked for. */
    for (int i = m_waiting.size() - 1; i >= 0; --i) {
        if (effectiveLockPrivacy(m_waiting.at(i)) == Notification::LockPrivacy::Hide) {
            const uint id = m_waiting.takeAt(i).id;
            Q_EMIT notificationClosed(id, CloseReason::Expired);
        }
    }
    QList<uint> hiddenMembers;
    for (const QList<Notification> &list : std::as_const(m_members)) {
        for (const Notification &m : list) {
            if (effectiveLockPrivacy(m) == Notification::LockPrivacy::Hide) {
                hiddenMembers << m.id;
            }
        }
    }
    for (uint id : std::as_const(hiddenMembers)) {
        closeMember(id, CloseReason::Expired);
    }
    const QList<Notification> shown = m_displayed;
    for (const Notification &n : shown) {
        if (!n.closing && effectiveLockPrivacy(n) == Notification::LockPrivacy::Hide) {
            closeId(n.id, CloseReason::Expired);
        }
    }
}

void NotificationModel::setScreenSharing(bool sharing)
{
    if (m_screenSharing == sharing) {
        return;
    }
    m_screenSharing = sharing;
    qInfo("glassosd: screen sharing %s, notification content %s",
          sharing ? "started" : "stopped", sharing ? "withheld" : "restored");
    if (sharing) {
        dropHiddenForPrivacy();
    }
    if (!m_displayed.isEmpty()) {
        Q_EMIT dataChanged(index(0), index(m_displayed.size() - 1),
                           {SummaryRole, BodyRole, IconSourceRole, BadgeSourceRole, ActionsRole, ReplyPlaceholderRole});
    }
}

void NotificationModel::setScreenLocked(bool locked)
{
    if (m_screenLocked == locked) {
        return;
    }
    m_screenLocked = locked;

    /* Locking with popups already on screen: anything asking to be hidden has
       to go now, not merely be withheld from here on. Closed rather than
       queued, because the notification has been delivered and is in history;
       re-showing it on unlock would be a second arrival the sender never
       made. */
    if (m_screenLocked) {
        dropHiddenForPrivacy();
    } else {
        /* Expiry was held while locked (see the tick). Whatever is still up
           gets its full dwell from now, when someone can finally read it. */
        const QDateTime stamp = now();
        for (Notification &n : m_displayed) {
            n.displayedAt = stamp;
        }
    }

    /* The rest only changed how they read, so redraw them in place. */
    if (!m_displayed.isEmpty()) {
        Q_EMIT dataChanged(index(0), index(m_displayed.size() - 1),
                           {SummaryRole, BodyRole, IconSourceRole, BadgeSourceRole, ActionsRole,
                            HasDefaultActionRole, InlineReplyRole});
    }
    Q_EMIT screenLockedChanged();
}
