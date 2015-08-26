//
//  AssetServer.cpp
//
//  Created by Ryan Huffman on 2015/07/21
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//


#include "AssetServer.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QEventLoop>
#include <QRunnable>
#include <QString>

#include <NodeType.h>

const QString ASSET_SERVER_LOGGING_TARGET_NAME = "asset-server";

void writeError(NLPacketList* packetList, AssetServerError error) {
    packetList->writePrimitive(error);
}

SendAssetTask::SendAssetTask(MessageID messageID, const QByteArray& assetHash, QString filePath, DataOffset start, DataOffset end,
              const SharedNodePointer& sendToNode) :
    QRunnable(),
    _messageID(messageID),
    _assetHash(assetHash),
    _filePath(filePath),
    _start(start),
    _end(end),
    _sendToNode(sendToNode)
{
}

void SendAssetTask::run() {
    qDebug() << "Starting task to send asset: " << _assetHash << " for messageID " << _messageID;
    auto replyPacketList = std::unique_ptr<NLPacketList>(new NLPacketList(PacketType::AssetGetReply, QByteArray(), true, true));

    replyPacketList->write(_assetHash, HASH_HEX_LENGTH);

    replyPacketList->writePrimitive(_messageID);

    const int64_t MAX_LENGTH = 4294967296;

    if (_end <= _start) {
        writeError(replyPacketList.get(), AssetServerError::INVALID_BYTE_RANGE);
    } else if (_end - _start > MAX_LENGTH) {
        writeError(replyPacketList.get(), AssetServerError::INVALID_BYTE_RANGE);
    } else {
        QFile file { _filePath };
        qDebug() << "Opening file: " << QString(QFileInfo(_assetHash).fileName());

        if (file.open(QIODevice::ReadOnly)) {
            if (file.size() < _end) {
                writeError(replyPacketList.get(), AssetServerError::INVALID_BYTE_RANGE);
            } else {
                auto size = _end - _start;
                file.seek(_start);
                replyPacketList->writePrimitive(AssetServerError::NO_ERROR);
                replyPacketList->writePrimitive(size);
                while (file.pos() < file.size()) {
                    static const int chunkSize = 20000;
                    QByteArray data = file.read(chunkSize);
                    replyPacketList->write(data, chunkSize);
                }
                qDebug() << "Done reading";
            }
            file.close();
        } else {
            qDebug() << "Asset not found";
            writeError(replyPacketList.get(), AssetServerError::ASSET_NOT_FOUND);
        }
    }

    qDebug() << "Sending asset";
    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->sendPacketList(std::move(replyPacketList), *_sendToNode);
}

AssetServer::AssetServer(NLPacket& packet) :
        ThreadedAssignment(packet),
        _taskPool(this) {

    // Most of the work will be I/O bound, reading from disk and constructing packet objects,
    // so the ideal is greater than the number of cores on the system.
    _taskPool.setMaxThreadCount(20);

    auto& packetReceiver = DependencyManager::get<NodeList>()->getPacketReceiver();
    packetReceiver.registerListener(PacketType::AssetGet, this, "handleAssetGet");
    packetReceiver.registerListener(PacketType::AssetGetInfo, this, "handleAssetGetInfo");
    packetReceiver.registerMessageListener(PacketType::AssetUpload, this, "handleAssetUpload");
}

AssetServer::~AssetServer() {
}

void AssetServer::run() {
    ThreadedAssignment::commonInit(ASSET_SERVER_LOGGING_TARGET_NAME, NodeType::AssetServer);

    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->addNodeTypeToInterestSet(NodeType::Agent);

    _resourcesDirectory = QDir(QCoreApplication::applicationDirPath()).filePath("resources/assets");
    if (!_resourcesDirectory.exists()) {
        qDebug() << "Creating resources directory";
        _resourcesDirectory.mkpath(".");
    }
    qDebug() << "Serving files from: " << _resourcesDirectory.path();

    // Scan for new files
    qDebug() << "Looking for new files in asset directory";
    auto files = _resourcesDirectory.entryInfoList(QDir::Files);
    QRegExp filenameRegex { "^[a-f0-9]{32}(\\..+)?$" };
    for (auto fileInfo : files) {
        auto filename = fileInfo.fileName();
        if (!filenameRegex.exactMatch(filename)) {
            qDebug() << "Found file: " << filename;
            if (!fileInfo.isReadable()) {
                qDebug() << "\tCan't open file for reading: " << filename;
                continue;
            }

            // Read file
            QFile file { fileInfo.absoluteFilePath() };
            file.open(QFile::ReadOnly);
            QByteArray data = file.readAll();

            auto hash = hashData(data);

            qDebug() << "\tMoving " << filename << " to " << hash;

            file.rename(_resourcesDirectory.absoluteFilePath(hash));
        }
    }

    while (!_isFinished) {
        // since we're a while loop we need to help Qt's event processing
        QCoreApplication::processEvents();
    }
}

void AssetServer::handleAssetGetInfo(QSharedPointer<NLPacket> packet, SharedNodePointer senderNode) {
    if (packet->getPayloadSize() < HASH_HEX_LENGTH) {
        qDebug() << "ERROR bad file request";
        return;
    }

    QByteArray assetHash;
    MessageID messageID;
    packet->readPrimitive(&messageID);
    assetHash = packet->read(HASH_HEX_LENGTH);

    auto replyPacket = NLPacket::create(PacketType::AssetGetInfoReply);

    replyPacket->writePrimitive(messageID);
    replyPacket->write(assetHash, HASH_HEX_LENGTH);

    QFileInfo fileInfo { _resourcesDirectory.filePath(QString(assetHash)) };
    qDebug() << "Opening file: " << QString(QFileInfo(assetHash).fileName());

    if (fileInfo.exists() && fileInfo.isReadable()) {
        replyPacket->writePrimitive(AssetServerError::NO_ERROR);
        replyPacket->writePrimitive(fileInfo.size());
    } else {
        qDebug() << "Asset not found: " << assetHash;
        replyPacket->writePrimitive(AssetServerError::ASSET_NOT_FOUND);
    }

    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->sendPacket(std::move(replyPacket), *senderNode);
}

void AssetServer::handleAssetGet(QSharedPointer<NLPacket> packet, SharedNodePointer senderNode) {
    if (packet->getPayloadSize() < HASH_HEX_LENGTH) {
        qDebug() << "ERROR bad file request";
        return;
    }

    MessageID messageID;
    packet->readPrimitive(&messageID);

    QByteArray assetHash;
    assetHash = packet->read(HASH_HEX_LENGTH);

    DataOffset start;
    packet->readPrimitive(&start);

    DataOffset end;
    packet->readPrimitive(&end);

    qDebug() << "Received a request for the file: " << assetHash << " from " << start << " to " << end;

    // Queue task
    QString filePath = _resourcesDirectory.filePath(QString(assetHash));
    auto task = new SendAssetTask(messageID, assetHash, filePath, start, end, senderNode);
    _taskPool.start(task);
}

void AssetServer::handleAssetUpload(QSharedPointer<NLPacketList> packetList, SharedNodePointer senderNode) {
    auto data = packetList->getMessage();
    QBuffer buffer { &data };
    buffer.open(QIODevice::ReadOnly);

    MessageID messageID;
    buffer.read(reinterpret_cast<char*>(&messageID), sizeof(messageID));
    // buffer.readPrimitive(&messageID);

    uint8_t extensionLength;
    buffer.read(reinterpret_cast<char*>(&extensionLength), sizeof(extensionLength));
    // buffer.readPrimitive(&extensionLength);

    QByteArray extension = buffer.read(extensionLength);

    qDebug() << "Got extension: " << extension;

    uint64_t fileSize;
    buffer.read(reinterpret_cast<char*>(&fileSize), sizeof(fileSize));
    // buffer.readPrimitive(&fileSize);

    // const uint64_t MAX_LENGTH = 1024;
    // fileSize = std::min(MAX_LENGTH, fileSize);
    qDebug() << "Receiving a file of size " << fileSize;

    QByteArray fileData = buffer.read(fileSize);

    QString hash = hashData(fileData);

    qDebug() << "Got data: (" << hash << ") ";

    QFile file { _resourcesDirectory.filePath(QString(hash)) };

    if (file.exists()) {
        qDebug() << "[WARNING] This file already exists";
    } else {
        file.open(QIODevice::WriteOnly);
        file.write(fileData);
        file.close();
    }

    auto replyPacket = NLPacket::create(PacketType::AssetUploadReply);

    replyPacket->writePrimitive(messageID);

    replyPacket->writePrimitive(true);
    replyPacket->write(hash.toLatin1().constData(), HASH_HEX_LENGTH);

    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->sendPacket(std::move(replyPacket), *senderNode);
}

QString AssetServer::hashData(const QByteArray& data) {
    return QString(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

