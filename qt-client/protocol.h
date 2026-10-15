#pragma once

#include <QByteArray>
#include <QString>
#include <QtEndian>

// LCH1: [4-byte body length][4-byte magic][4-byte type][payload].
// All integers are big endian; body length excludes its own four bytes.
namespace Wire {
constexpr quint32 Magic = 0x4C434831;
constexpr quint32 MaxPayload = 64 * 1024;
constexpr qsizetype HeaderSize = 12;
constexpr qsizetype MaxFrameSize = HeaderSize + MaxPayload;
enum Type : quint32 { Login = 1, Welcome, Users, Say, Chat, Bye, Error };

struct Frame {
    quint32 type = 0;
    QByteArray payload;
    QByteArray raw;
};

inline QString typeName(quint32 type) {
    switch (type) {
    case Login:
        return QStringLiteral("Login");
    case Welcome:
        return QStringLiteral("Welcome");
    case Users:
        return QStringLiteral("Users");
    case Say:
        return QStringLiteral("Say");
    case Chat:
        return QStringLiteral("Chat");
    case Bye:
        return QStringLiteral("Bye");
    case Error:
        return QStringLiteral("Error");
    default:
        return QStringLiteral("Unknown(%1)").arg(type);
    }
}

inline void put(QByteArray &bytes, quint32 value) {
    char encoded[4];
    qToBigEndian(value, encoded);
    bytes.append(encoded, 4);
}

inline QByteArray pack(quint32 type, const QByteArray &payload = {}) {
    if (payload.size() > MaxPayload)
        return {};
    QByteArray bytes;
    bytes.reserve(HeaderSize + payload.size());
    put(bytes, quint32(8 + payload.size()));
    put(bytes, Magic);
    put(bytes, type);
    bytes += payload;
    return bytes;
}

// 1 = complete frame consumed, 0 = more bytes needed, -1 = protocol error.
// Validate the length as soon as four bytes arrive, before allocating a payload.
// Unknown types are valid diagnostic data, not an application/login failure.
inline int take(QByteArray &buffer, Frame &frame, QString &error) {
    error.clear();
    if (buffer.size() < 4)
        return 0;
    const quint32 bodyLength = qFromBigEndian<quint32>(buffer.constData());
    if (bodyLength < 8 || bodyLength > MaxPayload + 8) {
        error = QStringLiteral("协议错误：长度字段 %1 非法，允许 8–65544（payload ≤ 64 KiB）")
                    .arg(bodyLength);
        return -1;
    }
    if (buffer.size() < 8)
        return 0;
    const quint32 magic = qFromBigEndian<quint32>(buffer.constData() + 4);
    if (magic != Magic) {
        error = QStringLiteral("协议错误：魔数 0x%1 不匹配，需要 LCH1 / 0x4C434831")
                    .arg(magic, 8, 16, QLatin1Char('0'));
        return -1;
    }
    const qsizetype frameLength = qsizetype(bodyLength) + 4;
    if (buffer.size() < frameLength)
        return 0;
    frame.type = qFromBigEndian<quint32>(buffer.constData() + 8);
    frame.payload = buffer.mid(HeaderSize, bodyLength - 8);
    frame.raw = buffer.first(frameLength);
    buffer.remove(0, frameLength);
    return 1;
}

inline bool validNick(const QByteArray &nick) {
    const QString decoded = QString::fromUtf8(nick);
    if (nick.isEmpty() || nick.size() > 32 || decoded.toUtf8() != nick ||
        decoded.trimmed() != decoded)
        return false;
    for (const QChar ch : decoded)
        if (ch.isSpace() || ch.category() == QChar::Other_Control)
            return false;
    return true;
}

inline QByteArray say(char mode, const QString &target, const QString &text) {
    QByteArray bytes(1, mode);
    if (mode == 'D') {
        bytes += target.toUtf8();
        bytes += '\0';
    }
    return bytes + text.toUtf8();
}

inline bool parseMessage(const QByteArray &bytes, char &mode, QString &sender, QString &text) {
    if (bytes.size() < 3 || (bytes[0] != 'B' && bytes[0] != 'D'))
        return false;
    const qsizetype end = bytes.indexOf('\0', 1);
    if (end < 2 || end == bytes.size() - 1)
        return false;
    const QByteArray senderBytes = bytes.mid(1, end - 1);
    const QByteArray textBytes = bytes.mid(end + 1);
    const QString decodedSender = QString::fromUtf8(senderBytes);
    const QString decodedText = QString::fromUtf8(textBytes);
    if (decodedSender.toUtf8() != senderBytes || decodedText.toUtf8() != textBytes)
        return false;
    mode = bytes[0];
    sender = decodedSender;
    text = decodedText;
    return true;
}
} // namespace Wire

inline QString typeName(quint32 type) {
    return Wire::typeName(type);
}
