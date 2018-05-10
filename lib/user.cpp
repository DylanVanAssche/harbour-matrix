/******************************************************************************
 * Copyright (C) 2015 Felix Rohrbach <kde@fxrh.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "user.h"

#include "connection.h"
#include "room.h"
#include "avatar.h"
#include "events/event.h"
#include "events/roommemberevent.h"
#include "jobs/setroomstatejob.h"
#include "jobs/generated/profile.h"
#include "jobs/generated/content-repo.h"

#include <QtCore/QTimer>
#include <QtCore/QRegularExpression>
#include <QtCore/QPointer>
#include <QtCore/QStringBuilder>
#include <QtCore/QElapsedTimer>

#include <functional>

using namespace QMatrixClient;
using namespace std::placeholders;
using std::move;

class User::Private
{
    public:
        static Avatar makeAvatar(QUrl url)
        {
            static const QIcon icon
                { QIcon::fromTheme(QStringLiteral("user-available")) };
            return Avatar(move(url), icon);
        }

        Private(QString userId, Connection* connection)
            : userId(move(userId)), connection(connection)
        { }

        QString userId;
        Connection* connection;

        QString bridged;
        QString mostUsedName;
        QMultiHash<QString, const Room*> otherNames;
        Avatar mostUsedAvatar { makeAvatar({}) };
        std::vector<Avatar> otherAvatars;
        auto otherAvatar(QUrl url)
        {
            return std::find_if(otherAvatars.begin(), otherAvatars.end(),
                    [&url] (const auto& av) { return av.url() == url; });
        }
        QMultiHash<QUrl, const Room*> avatarsToRooms;

        mutable int totalRooms = 0;

        QString nameForRoom(const Room* r, const QString& hint = {}) const;
        void setNameForRoom(const Room* r, QString newName, QString oldName);
        QUrl avatarUrlForRoom(const Room* r, const QUrl& hint = {}) const;
        void setAvatarForRoom(const Room* r, const QUrl& newUrl,
                              const QUrl& oldUrl);

        void setAvatarOnServer(QString contentUri, User* q);

};


QString User::Private::nameForRoom(const Room* r, const QString& hint) const
{
    // If the hint is accurate, this function is O(1) instead of O(n)
    if (hint == mostUsedName || otherNames.contains(hint, r))
        return hint;
    return otherNames.key(r, mostUsedName);
}

static constexpr int MIN_JOINED_ROOMS_TO_LOG = 20;

void User::Private::setNameForRoom(const Room* r, QString newName,
                                   QString oldName)
{
    Q_ASSERT(oldName != newName);
    Q_ASSERT(oldName == mostUsedName || otherNames.contains(oldName, r));
    if (totalRooms < 2)
    {
        Q_ASSERT_X(totalRooms > 0 && otherNames.empty(), __FUNCTION__,
                   "Internal structures inconsistency");
        mostUsedName = move(newName);
        return;
    }
    otherNames.remove(oldName, r);
    if (newName != mostUsedName)
    {
        // Check if the newName is about to become most used.
        if (otherNames.count(newName) >= totalRooms - otherNames.size())
        {
            Q_ASSERT(totalRooms > 1);
            QElapsedTimer et;
            if (totalRooms > MIN_JOINED_ROOMS_TO_LOG)
            {
                qCDebug(MAIN) << "Switching the most used name of user" << userId
                              << "from" << mostUsedName << "to" << newName;
                qCDebug(MAIN) << "The user is in" << totalRooms << "rooms";
                et.start();
            }

            for (auto* r1: connection->roomMap())
                if (nameForRoom(r1) == mostUsedName)
                    otherNames.insert(mostUsedName, r1);

            mostUsedName = newName;
            otherNames.remove(newName);
            if (totalRooms > MIN_JOINED_ROOMS_TO_LOG)
                qCDebug(PROFILER) << et << "to switch the most used name";
        }
        else
            otherNames.insert(newName, r);
    }
}

QUrl User::Private::avatarUrlForRoom(const Room* r, const QUrl& hint) const
{
    // If the hint is accurate, this function is O(1) instead of O(n)
    if (hint == mostUsedAvatar.url() || avatarsToRooms.contains(hint, r))
        return hint;
    auto it = std::find(avatarsToRooms.begin(), avatarsToRooms.end(), r);
    return it == avatarsToRooms.end() ? mostUsedAvatar.url() : it.key();
}

void User::Private::setAvatarForRoom(const Room* r, const QUrl& newUrl,
                                     const QUrl& oldUrl)
{
    Q_ASSERT(oldUrl != newUrl);
    Q_ASSERT(oldUrl == mostUsedAvatar.url() ||
             avatarsToRooms.contains(oldUrl, r));
    if (totalRooms < 2)
    {
        Q_ASSERT_X(totalRooms > 0 && otherAvatars.empty(), __FUNCTION__,
                   "Internal structures inconsistency");
        mostUsedAvatar.updateUrl(newUrl);
        return;
    }
    avatarsToRooms.remove(oldUrl, r);
    if (!avatarsToRooms.contains(oldUrl))
    {
        auto it = otherAvatar(oldUrl);
        if (it != otherAvatars.end())
            otherAvatars.erase(it);
    }
    if (newUrl != mostUsedAvatar.url())
    {
        // Check if the new avatar is about to become most used.
        if (avatarsToRooms.count(newUrl) >= totalRooms - avatarsToRooms.size())
        {
            QElapsedTimer et;
            if (totalRooms > MIN_JOINED_ROOMS_TO_LOG)
            {
                qCDebug(MAIN) << "Switching the most used avatar of user" << userId
                              << "from" << mostUsedAvatar.url().toDisplayString()
                              << "to" << newUrl.toDisplayString();
                et.start();
            }
            avatarsToRooms.remove(newUrl);
            auto nextMostUsedIt = otherAvatar(newUrl);
            Q_ASSERT(nextMostUsedIt != otherAvatars.end());
            std::swap(mostUsedAvatar, *nextMostUsedIt);
            for (const auto* r1: connection->roomMap())
                if (avatarUrlForRoom(r1) == nextMostUsedIt->url())
                    avatarsToRooms.insert(nextMostUsedIt->url(), r1);

            if (totalRooms > MIN_JOINED_ROOMS_TO_LOG)
                qCDebug(PROFILER) << et << "to switch the most used avatar";
        } else {
            if (otherAvatar(newUrl) == otherAvatars.end())
                otherAvatars.emplace_back(makeAvatar(newUrl));
            avatarsToRooms.insert(newUrl, r);
        }
    }
}

User::User(QString userId, Connection* connection)
    : QObject(connection), d(new Private(move(userId), connection))
{
    setObjectName(userId);
}

User::~User() = default;

QString User::id() const
{
    return d->userId;
}

bool User::isGuest() const
{
    Q_ASSERT(!d->userId.isEmpty() && d->userId.startsWith('@'));
    auto it = std::find_if_not(d->userId.begin() + 1, d->userId.end(),
                               [] (QChar c) { return c.isDigit(); });
    Q_ASSERT(it != d->userId.end());
    return *it == ':';
}

QString User::name(const Room* room) const
{
    return d->nameForRoom(room);
}

QString User::rawName(const Room* room) const
{
    return d->bridged.isEmpty() ? name(room) :
                                  name(room) % " (" % d->bridged % ')';
}

void User::updateName(const QString& newName, const Room* room)
{
    updateName(newName, d->nameForRoom(room), room);
}

void User::updateName(const QString& newName, const QString& oldName,
                      const Room* room)
{
    Q_ASSERT(oldName == d->mostUsedName || d->otherNames.contains(oldName, room));
    if (newName != oldName)
    {
        emit nameAboutToChange(newName, oldName, room);
        d->setNameForRoom(room, newName, oldName);
        setObjectName(displayname());
        emit nameChanged(newName, oldName, room);
    }
}

void User::updateAvatarUrl(const QUrl& newUrl, const QUrl& oldUrl,
                           const Room* room)
{
    Q_ASSERT(oldUrl == d->mostUsedAvatar.url() ||
             d->avatarsToRooms.contains(oldUrl, room));
    if (newUrl != oldUrl)
    {
        d->setAvatarForRoom(room, newUrl, oldUrl);
        setObjectName(displayname());
        emit avatarChanged(this, room);
    }

}

void User::rename(const QString& newName)
{
    auto job = d->connection->callApi<SetDisplayNameJob>(id(), newName);
    connect(job, &BaseJob::success, this, [=] { updateName(newName); });
}

void User::rename(const QString& newName, const Room* r)
{
    if (!r)
    {
        qCWarning(MAIN) << "Passing a null room to two-argument User::rename()"
                           "is incorrect; client developer, please fix it";
        rename(newName);
        return;
    }
    Q_ASSERT_X(r->memberJoinState(this) == JoinState::Join, __FUNCTION__,
               "Attempt to rename a user that's not a room member");
    MemberEventContent evtC;
    evtC.displayName = newName;
    auto job = d->connection->callApi<SetRoomStateJob>(
                r->id(), id(), RoomMemberEvent(move(evtC)));
    connect(job, &BaseJob::success, this, [=] { updateName(newName, r); });
}

bool User::setAvatar(const QString& fileName)
{
    return avatarObject().upload(d->connection, fileName,
                std::bind(&Private::setAvatarOnServer, d.data(), _1, this));
}

bool User::setAvatar(QIODevice* source)
{
    return avatarObject().upload(d->connection, source,
                std::bind(&Private::setAvatarOnServer, d.data(), _1, this));
}

void User::requestDirectChat()
{
    Q_ASSERT(d->connection);
    d->connection->requestDirectChat(d->userId);
}

void User::Private::setAvatarOnServer(QString contentUri, User* q)
{
    auto* j = connection->callApi<SetAvatarUrlJob>(userId, contentUri);
    connect(j, &BaseJob::success, q,
            [=] { q->updateAvatarUrl(contentUri, avatarUrlForRoom(nullptr)); });
}

QString User::displayname(const Room* room) const
{
    auto name = d->nameForRoom(room);
    return name.isEmpty() ? d->userId :
           room ? room->roomMembername(this) : name;
}

QString User::fullName(const Room* room) const
{
    auto name = d->nameForRoom(room);
    return name.isEmpty() ? d->userId : name % " (" % d->userId % ')';
}

QString User::bridged() const
{
    return d->bridged;
}

const Avatar& User::avatarObject(const Room* room) const
{
    auto it = d->otherAvatar(d->avatarUrlForRoom(room));
    return it != d->otherAvatars.end() ? *it : d->mostUsedAvatar;
}

QImage User::avatar(int dimension, const Room* room)
{
    return avatar(dimension, dimension, room);
}

QImage User::avatar(int width, int height, const Room* room)
{
    return avatar(width, height, room, []{});
}

QImage User::avatar(int width, int height, const Room* room,
                    Avatar::get_callback_t callback)
{
    return avatarObject(room).get(d->connection, width, height,
                [=] { emit avatarChanged(this, room); callback(); });
}

QString User::avatarMediaId(const Room* room) const
{
    return avatarObject(room).mediaId();
}

QUrl User::avatarUrl(const Room* room) const
{
    return avatarObject(room).url();
}

void User::processEvent(RoomMemberEvent* event, const Room* room)
{
    if (event->membership() != MembershipType::Invite &&
            event->membership() != MembershipType::Join)
        return;

    auto aboutToEnter = room->memberJoinState(this) == JoinState::Leave &&
            (event->membership() == MembershipType::Join ||
             event->membership() == MembershipType::Invite);
    if (aboutToEnter)
        ++d->totalRooms;

    auto newName = event->displayName();
    // `bridged` value uses the same notification signal as the name;
    // it is assumed that first setting of the bridge occurs together with
    // the first setting of the name, and further bridge updates are
    // exceptionally rare (the only reasonable case being that the bridge
    // changes the naming convention). For the same reason room-specific
    // bridge tags are not supported at all.
    QRegularExpression reSuffix(" \\((IRC|Gitter|Telegram)\\)$");
    auto match = reSuffix.match(newName);
    if (match.hasMatch())
    {
        if (d->bridged != match.captured(1))
        {
            if (!d->bridged.isEmpty())
                qCWarning(MAIN) << "Bridge for user" << id() << "changed:"
                                << d->bridged << "->" << match.captured(1);
            d->bridged = match.captured(1);
        }
        newName.truncate(match.capturedStart(0));
    }
    if (event->prevContent())
    {
        // FIXME: the hint doesn't work for bridged users
        auto oldNameHint =
                d->nameForRoom(room, event->prevContent()->displayName);
        updateName(newName, oldNameHint, room);
        updateAvatarUrl(event->avatarUrl(),
                        d->avatarUrlForRoom(room, event->prevContent()->avatarUrl),
                        room);
    } else {
        updateName(newName, room);
        updateAvatarUrl(event->avatarUrl(), d->avatarUrlForRoom(room), room);
    }
}
