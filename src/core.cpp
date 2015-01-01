/*
    Copyright (C) 2013 by Maxim Biro <nurupo.contributions@gmail.com>

    This file is part of Tox Qt GUI.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

    See the COPYING file for more details.
*/

#include "core.hpp"
#include "Settings/settings.hpp"

#include <cstdint>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>
#include <QThread>

const QString Core::CONFIG_FILE_NAME = "data.tox";

Core::Core() :
    tox(nullptr)
{
    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, &Core::process);
    connect(&Settings::getInstance(), &Settings::dhtServerListChanged, this, &Core::bootstrapDht);
}

Core::~Core()
{
    if (tox) {
        saveConfiguration();
        tox_kill(tox);
    }
}

void Core::onFriendRequest(Tox*/* tox*/, const uint8_t* cUserId, const uint8_t* cMessage, uint16_t cMessageSize, void* core)
{
    emit static_cast<Core*>(core)->friendRequestReceived(CUserId::toString(cUserId), CString::toString(cMessage, cMessageSize));
}

void Core::onFriendMessage(Tox*/* tox*/, int friendId, const uint8_t* cMessage, uint16_t cMessageSize, void* core)
{
    emit static_cast<Core*>(core)->friendMessageReceived(friendId, CString::toString(cMessage, cMessageSize));
}

void Core::onFriendNameChange(Tox*/* tox*/, int friendId, const uint8_t* cName, uint16_t cNameSize, void* core)
{
    emit static_cast<Core*>(core)->friendUsernameChanged(friendId, CString::toString(cName, cNameSize));
}

void Core::onFriendTypingChange(Tox*/* tox*/, int friendId, uint8_t isTyping, void *core)
{
    emit static_cast<Core*>(core)->friendTypingChanged(friendId, isTyping ? true : false);
}

void Core::onStatusMessageChanged(Tox*/* tox*/, int friendId, const uint8_t* cMessage, uint16_t cMessageSize, void* core)
{
    emit static_cast<Core*>(core)->friendStatusMessageChanged(friendId, CString::toString(cMessage, cMessageSize));
}

void Core::onUserStatusChanged(Tox*/* tox*/, int friendId, uint8_t userstatus, void* core)
{
    Status status;
    switch (userstatus) {
        case TOX_USERSTATUS_NONE:
            status = Status::Online;
            break;
        case TOX_USERSTATUS_AWAY:
            status = Status::Away;
            break;
        case TOX_USERSTATUS_BUSY:
            status = Status::Busy;
            break;
        default:
            status = Status::Online;
            break;
    }
    emit static_cast<Core*>(core)->friendStatusChanged(friendId, status);
}

void Core::onConnectionStatusChanged(Tox*/* tox*/, int friendId, uint8_t status, void* core)
{
    Status friendStatus = status ? Status::Online : Status::Offline;
    emit static_cast<Core*>(core)->friendStatusChanged(friendId, friendStatus);
    if (friendStatus == Status::Offline) {
        static_cast<Core*>(core)->checkLastOnline(friendId);
    }
}

void Core::onAction(Tox*/* tox*/, int friendId, const uint8_t *cMessage, uint16_t cMessageSize, void *core)
{
    emit static_cast<Core*>(core)->actionReceived(friendId, CString::toString(cMessage, cMessageSize));
}

void Core::acceptFriendRequest(const QString& userId)
{
    int friendId = tox_add_friend_norequest(tox, CUserId(userId).data());
    if (friendId == -1) {
        emit failedToAddFriend(userId);
    } else {
        emit friendAdded(friendId, userId);
    }
}

void Core::requestFriendship(const QString& friendAddress, const QString& message)
{
    CString cMessage(message);

    int friendId = tox_add_friend(tox, CFriendAddress(friendAddress).data(), cMessage.data(), cMessage.size());
    const QString userId = friendAddress.mid(0, TOX_CLIENT_ID_SIZE * 2);
    // TODO: better error handling
    if (friendId < 0) {
        emit failedToAddFriend(userId);
    } else {
        emit friendAdded(friendId, userId);
    }
}

void Core::sendMessage(int friendId, const QString& message)
{
    QByteArray byteArray = message.toUtf8();

    int messageOffset = 0;
    int absoluteMaxLength = TOX_MAX_MESSAGE_LENGTH + messageOffset;
    // keep splitting message while it's too big
    while (byteArray.size() > absoluteMaxLength) {
        int absoluteMaxPosition = absoluteMaxLength - 1;

        static const char multibyteCodepoint = 1 << 7; // 1xxxxxxx mask
        static const char multibyteCodepointStart = 1 << 6; // x1xxxxxx mask
        int lastCodepointStart = -1;
        // find a codepoint start position
        // UTF-8 codepoints are maximum of 4 bytes in length
        for (int i = absoluteMaxPosition; i > absoluteMaxPosition - 4; i --) {
            // if we are at multibyte codepoint but not at its start yet -- keep moving back
            if ((byteArray[i] & multibyteCodepoint) && !(byteArray[i] & multibyteCodepointStart)) {
                continue;
            }
            // we are either at a single-byte codepoint or at the beginning of a multibyte one -- split the message
            lastCodepointStart = i;
            break;
        }

        // try to split on whitespace or punctuation instead of just cutting off at a codepoint
        static const char *splitOn = " .,-";
        int splitPosition = -1;
        for (int i = lastCodepointStart; i > absoluteMaxPosition - TOX_MAX_MESSAGE_LENGTH / 4; i --) {
            // if we are at multibyte codepoint -- keep moving back
            if (byteArray[i] & multibyteCodepoint) {
                continue;
            }
            // we are at single-byte codepoint, check if it matches any of splitOn characters
            for (const char *splitChar = splitOn; *splitChar != 0; splitChar ++) {
                if (byteArray[i] == *splitChar) {
                    // keep split char on old line
                    splitPosition = i + 1;
                    goto found;
                }
            }
        }
found:
        if (splitPosition <= messageOffset || splitPosition > absoluteMaxLength) {
            splitPosition = lastCodepointStart;
        }

        int messageId = tox_send_message(tox, friendId, reinterpret_cast<uint8_t*>(byteArray.data() + messageOffset), splitPosition - messageOffset);
        emit messageSentResult(friendId, QString::fromUtf8(byteArray.data() + messageOffset, splitPosition - messageOffset), messageId);

        messageOffset = splitPosition;
        absoluteMaxLength = TOX_MAX_MESSAGE_LENGTH + messageOffset;
    }

    if (byteArray.size() - messageOffset > 0) {
        int messageId = tox_send_message(tox, friendId, reinterpret_cast<uint8_t*>(byteArray.data() + messageOffset), byteArray.size() - messageOffset);
        emit messageSentResult(friendId, QString::fromUtf8(byteArray.data() + messageOffset, byteArray.size() - messageOffset), messageId);
    }

}

void Core::sendAction(int friendId, const QString &action)
{
    CString cMessage(action);
    int ret = tox_send_action(tox, friendId, cMessage.data(), cMessage.size());
    emit actionSentResult(friendId, action, ret);
}

void Core::sendTyping(int friendId, bool typing)
{
    int ret = tox_set_user_is_typing(tox, friendId, typing);
    if (ret == -1)
        emit failedToSetTyping(typing);
}

void Core::removeFriend(int friendId)
{
    if (tox_del_friend(tox, friendId) == -1) {
        emit failedToRemoveFriend(friendId);
    } else {
        emit friendRemoved(friendId);
    }
}

void Core::setUsername(const QString& username)
{
    CString cUsername(username);

    if (tox_set_name(tox, cUsername.data(), cUsername.size()) == -1) {
        emit failedToSetUsername(username);
    } else {
        emit usernameSet(username);
    }
}

void Core::setStatusMessage(const QString& message)
{
    CString cMessage(message);

    if (tox_set_status_message(tox, cMessage.data(), cMessage.size()) == -1) {
        emit failedToSetStatusMessage(message);
    } else {
        emit statusMessageSet(message);
    }
}

void Core::setStatus(Status status)
{
    TOX_USERSTATUS userstatus;
    switch (status) {
        case Status::Online:
            userstatus = TOX_USERSTATUS_NONE;
            break;
        case Status::Away:
            userstatus = TOX_USERSTATUS_AWAY;
            break;
        case Status::Busy:
            userstatus = TOX_USERSTATUS_BUSY;
            break;
        default:
            userstatus = TOX_USERSTATUS_INVALID;
            break;
    }

    if (tox_set_user_status(tox, userstatus) == 0) {
        emit statusSet(status);
    } else {
        emit failedToSetStatus(status);
    }
}

void Core::bootstrapDht()
{
    const Settings& s = Settings::getInstance();
    QList<Settings::DhtServer> dhtServerList = s.getDhtServerList();

    for (const Settings::DhtServer& dhtServer : dhtServerList) {
       tox_bootstrap_from_address(tox, dhtServer.address.toLatin1().data(), dhtServer.port, CUserId(dhtServer.userId).data());
    }
}

void Core::process()
{
    tox_do(tox);
#ifdef DEBUG
    //we want to see the debug messages immediately
    fflush(stdout);
#endif
    checkConnection();
    timer->start(tox_do_interval(tox));
}

void Core::checkConnection()
{
    static bool isConnected = false;

    if (tox_isconnected(tox) && !isConnected) {
        emit connected();
        isConnected = true;
    } else if (!tox_isconnected(tox) && isConnected) {
        emit disconnected();
        isConnected = false;
    }
}

void Core::loadConfiguration()
{
    QString path = Settings::getSettingsDirPath() + '/' + CONFIG_FILE_NAME;

    QFile configurationFile(path);

    if (!configurationFile.exists()) {
        qWarning() << "The Tox configuration file was not found";
        return;
    }

    if (!configurationFile.open(QIODevice::ReadOnly)) {
        qCritical() << "File " << path << " cannot be opened";
        return;
    }

    qint64 fileSize = configurationFile.size();
    if (fileSize > 0) {
        QByteArray data = configurationFile.readAll();
        tox_load(tox, reinterpret_cast<uint8_t *>(data.data()), data.size());
    }

    configurationFile.close();

    loadFriends();
}

void Core::saveConfiguration()
{
    QString path = Settings::getSettingsDirPath();

    QDir directory(path);

    if (!directory.exists() && !directory.mkpath(directory.absolutePath())) {
        qCritical() << "Error while creating directory " << path;
        return;
    }

    path += '/' + CONFIG_FILE_NAME;
    QSaveFile configurationFile(path);
    if (!configurationFile.open(QIODevice::WriteOnly)) {
        qCritical() << "File " << path << " cannot be opened";
        return;
    }

    uint32_t fileSize = tox_size(tox);
    if (fileSize > 0 && fileSize <= INT32_MAX) {
        uint8_t *data = new uint8_t[fileSize];
        tox_save(tox, data);
        configurationFile.write(reinterpret_cast<char *>(data), fileSize);
        configurationFile.commit();
        delete[] data;
    }
}

void Core::loadFriends()
{
    const uint32_t friendCount = tox_count_friendlist(tox);
    if (friendCount > 0) {
        // assuming there are not that many friends to fill up the whole stack
        int32_t *ids = new int32_t[friendCount];
        tox_get_friendlist(tox, ids, friendCount);
        uint8_t clientId[TOX_CLIENT_ID_SIZE];
        for (int32_t i = 0; i < static_cast<int32_t>(friendCount); ++i) {
            if (tox_get_client_id(tox, ids[i], clientId) == 0) {
                emit friendAdded(ids[i], CUserId::toString(clientId));

                const int nameSize = tox_get_name_size(tox, ids[i]);
                if (nameSize > 0) {
                    uint8_t *name = new uint8_t[nameSize];
                    if (tox_get_name(tox, ids[i], name) == nameSize) {
                        emit friendUsernameLoaded(ids[i], CString::toString(name, nameSize));
                    }
                    delete[] name;
                }

                const int statusMessageSize = tox_get_status_message_size(tox, ids[i]);
                if (statusMessageSize > 0) {
                    uint8_t *statusMessage = new uint8_t[statusMessageSize];
                    if (tox_get_status_message(tox, ids[i], statusMessage, statusMessageSize) == statusMessageSize) {
                        emit friendStatusMessageLoaded(ids[i], CString::toString(statusMessage, statusMessageSize));
                    }
                    delete[] statusMessage;
                }

                checkLastOnline(ids[i]);
            }

        }
        delete[] ids;
    }
}

void Core::checkLastOnline(int friendId) {
    const uint64_t lastOnline = tox_get_last_online(tox, friendId);
    if (lastOnline > 0) {
        emit friendLastSeenChanged(friendId, QDateTime::fromTime_t(lastOnline));
    }
}

void Core::start()
{
    const Settings &settings = Settings::getInstance();

    Tox_Options options;
    options.ipv6enabled = settings.isIPv6Enabled();
    options.proxy_type = TOX_PROXY_NONE;
    options.udp_disabled = 0;

    tox = tox_new(&options);

    // if failed to initialize -- try to fallback to ipv4
    if (tox == nullptr && settings.isIPv6Enabled() && settings.isIPv4FallbackEnabled()) {
          options.ipv6enabled = 0;
          tox = tox_new(&options);
    }

    // if still didn't manage to initialize -- throw an error
    if (tox == nullptr) {
        emit failedToStart();
        return;
    }

    loadConfiguration();

    tox_callback_friend_request(tox, onFriendRequest, this);
    tox_callback_friend_message(tox, onFriendMessage, this);
    tox_callback_friend_action(tox, onAction, this);
    tox_callback_name_change(tox, onFriendNameChange, this);
    tox_callback_typing_change(tox, onFriendTypingChange, this);
    tox_callback_status_message(tox, onStatusMessageChanged, this);
    tox_callback_user_status(tox, onUserStatusChanged, this);
    tox_callback_connection_status(tox, onConnectionStatusChanged, this);

    uint8_t friendAddress[TOX_FRIEND_ADDRESS_SIZE];
    tox_get_address(tox, friendAddress);

    emit friendAddressGenerated(CFriendAddress::toString(friendAddress));

    CString cUsername(settings.getUsername());
    tox_set_name(tox, cUsername.data(), cUsername.size());

    CString cStatusMessage(settings.getStatusMessage());
    tox_set_status_message(tox, cStatusMessage.data(), cStatusMessage.size());

    bootstrapDht();

    timer->start(tox_do_interval(tox));
}


// CData

Core::CData::CData(const QString &data, uint16_t byteSize)
{
    cData = new uint8_t[byteSize];
    cDataSize = fromString(data, cData);
}

Core::CData::~CData()
{
    delete[] cData;
}

uint8_t* Core::CData::data()
{
    return cData;
}

uint16_t Core::CData::size()
{
    return cDataSize;
}

QString Core::CData::toString(const uint8_t *cData, const uint16_t cDataSize)
{
    return QString(QByteArray(reinterpret_cast<const char*>(cData), cDataSize).toHex()).toUpper();
}

uint16_t Core::CData::fromString(const QString& data, uint8_t* cData)
{
    QByteArray arr = QByteArray::fromHex(data.toLower().toLatin1());
    memcpy(cData, reinterpret_cast<uint8_t*>(arr.data()), arr.size());
    return arr.size();
}


// CUserId

Core::CUserId::CUserId(const QString &userId) :
    CData(userId, SIZE)
{
    // intentionally left empty
}

QString Core::CUserId::toString(const uint8_t* cUserId)
{
    return CData::toString(cUserId, SIZE);
}


// CFriendAddress

Core::CFriendAddress::CFriendAddress(const QString &friendAddress) :
    CData(friendAddress, SIZE)
{
    // intentionally left empty
}

QString Core::CFriendAddress::toString(const uint8_t *cFriendAddress)
{
    return CData::toString(cFriendAddress, SIZE);
}


// CString

Core::CString::CString(const QString& string)
{
    cString = new uint8_t[string.length() * MAX_SIZE_OF_UTF8_ENCODED_CHARACTER]();
    cStringSize = fromString(string, cString);
}

Core::CString::~CString()
{
    delete[] cString;
}

uint8_t* Core::CString::data()
{
    return cString;
}

uint16_t Core::CString::size()
{
    return cStringSize;
}

QString Core::CString::toString(const uint8_t* cString, uint16_t cStringSize)
{
    return QString::fromUtf8(reinterpret_cast<const char*>(cString), cStringSize);
}

uint16_t Core::CString::fromString(const QString& string, uint8_t* cString)
{
    QByteArray byteArray = QByteArray(string.toUtf8());
    memcpy(cString, reinterpret_cast<uint8_t*>(byteArray.data()), byteArray.size());
    return byteArray.size();
}
