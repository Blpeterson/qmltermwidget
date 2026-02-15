/*
    Copyright (c) 2026 Alex Fabri
    https://fromhelloworld.com
    https://github.com/hotbit9

    This file is part of CRT Plus.

    CRT Plus is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    CRT Plus is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with CRT Plus.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "DaemonClient.h"
#include "PersistentPty.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QDateTime>
#include <QDebug>

#include <unistd.h>  // getuid()

namespace Konsole {

// ---------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------

DaemonClient *DaemonClient::instance()
{
    static DaemonClient *s_instance = new DaemonClient();
    return s_instance;
}

// ---------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------

DaemonClient::DaemonClient(QObject *parent)
    : QObject(parent)
    , _socket(nullptr)
    , _pendingCreatePty(nullptr)
    , _heartbeatTimer(new QTimer(this))
    , _capabilities(0)
    , _authenticated(false)
{
    _heartbeatTimer->setInterval(30000);  // 30 seconds
    connect(_heartbeatTimer, &QTimer::timeout, this, &DaemonClient::sendPing);
}

DaemonClient::~DaemonClient()
{
    disconnectFromDaemon();
}

// ---------------------------------------------------------------
// Socket path (mirrors daemon's get_socket_dir() + "/sessiond.sock")
// ---------------------------------------------------------------

QString DaemonClient::_socketPath()
{
#if defined(Q_OS_MAC)
    QString tmpdir = QDir::tempPath();
    if (tmpdir.isEmpty())
        tmpdir = QStringLiteral("/tmp");
    return QStringLiteral("%1/crt-plus-%2/sessiond.sock")
        .arg(tmpdir)
        .arg(static_cast<unsigned>(getuid()));
#else
    QString xdg = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!xdg.isEmpty()) {
        return xdg + QStringLiteral("/crt-plus/sessiond.sock");
    }
    return QStringLiteral("/tmp/crt-plus-%1/sessiond.sock")
        .arg(static_cast<unsigned>(getuid()));
#endif
}

// ---------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------

bool DaemonClient::connectToDaemon()
{
    if (_socket && _socket->state() == QLocalSocket::ConnectedState)
        return true;

    // Clean up any previous socket
    if (_socket) {
        _socket->disconnect(this);
        _socket->deleteLater();
    }

    _socket = new QLocalSocket(this);
    _recvBuf.clear();
    _authenticated = false;
    _capabilities = 0;

    connect(_socket, &QLocalSocket::readyRead, this, &DaemonClient::_onReadyRead);
    connect(_socket, &QLocalSocket::disconnected, this, &DaemonClient::_onDisconnected);

    QString path = _socketPath();
    _socket->connectToServer(path);

    if (!_socket->waitForConnected(3000)) {
        QString err = QStringLiteral("Failed to connect to daemon at %1: %2")
            .arg(path, _socket->errorString());
        qWarning() << "DaemonClient:" << err;
        emit connectionError(err);
        return false;
    }

    // --- HELLO handshake (synchronous) ---
    // Block the readyRead signal during handshake so _onReadyRead doesn't
    // consume the HELLO_OK response before we can read it here.
    _socket->blockSignals(true);

    // HELLO payload: [1B version][4B capabilities][4B client_pid]
    uint8_t hello[9];
    hello[0] = PROTOCOL_VERSION;
    write_u32_le(hello + 1, DAEMON_CAPABILITIES);
    write_u32_le(hello + 5, static_cast<uint32_t>(QCoreApplication::applicationPid()));

    _sendRawMessage(MSG_HELLO, hello, sizeof(hello));

    // Wait for HELLO_OK
    if (!_socket->waitForReadyRead(3000)) {
        QString err = QStringLiteral("Daemon did not respond to HELLO");
        qWarning() << "DaemonClient:" << err;
        emit connectionError(err);
        _socket->blockSignals(false);
        _socket->disconnectFromServer();
        return false;
    }

    // Read the response
    _recvBuf.append(_socket->readAll());

    // Need at least HEADER_SIZE bytes to parse
    if (static_cast<size_t>(_recvBuf.size()) < HEADER_SIZE) {
        QString err = QStringLiteral("Incomplete HELLO_OK response");
        qWarning() << "DaemonClient:" << err;
        emit connectionError(err);
        _socket->blockSignals(false);
        _socket->disconnectFromServer();
        return false;
    }

    const uint8_t *buf = reinterpret_cast<const uint8_t *>(_recvBuf.constData());
    uint8_t type = buf[0];
    uint32_t payloadLen = read_u32_le(buf + 1);

    if (type == MSG_ERROR) {
        QString err = QStringLiteral("Daemon rejected HELLO");
        if (payloadLen >= 3 && static_cast<size_t>(_recvBuf.size()) >= HEADER_SIZE + payloadLen) {
            const uint8_t *p = buf + HEADER_SIZE;
            // Error payload: [1B code][2B msg_len][msg...]
            uint16_t msgLen = read_u16_le(p + 1);
            if (3 + msgLen <= payloadLen)
                err = QStringLiteral("Daemon rejected HELLO: %1")
                    .arg(QString::fromUtf8(reinterpret_cast<const char *>(p + 3), msgLen));
        }
        qWarning() << "DaemonClient:" << err;
        emit connectionError(err);
        _recvBuf.clear();
        _socket->blockSignals(false);
        _socket->disconnectFromServer();
        return false;
    }

    if (type != MSG_HELLO_OK || static_cast<size_t>(_recvBuf.size()) < HEADER_SIZE + payloadLen) {
        QString err = QStringLiteral("Unexpected response to HELLO (type=0x%1)")
            .arg(type, 2, 16, QLatin1Char('0'));
        qWarning() << "DaemonClient:" << err;
        emit connectionError(err);
        _recvBuf.clear();
        _socket->blockSignals(false);
        _socket->disconnectFromServer();
        return false;
    }

    // Parse HELLO_OK: [1B version][4B capabilities][4B daemon_pid]
    const uint8_t *payload = buf + HEADER_SIZE;
    if (payloadLen >= 9) {
        // uint8_t daemonVersion = payload[0];
        _capabilities = read_u32_le(payload + 1);
        // uint32_t daemonPid = read_u32_le(payload + 5);
    }
    _authenticated = true;

    // Consume the HELLO_OK message from the receive buffer
    _recvBuf.remove(0, static_cast<int>(HEADER_SIZE + payloadLen));

    // Unblock signals now that handshake is complete
    _socket->blockSignals(false);

    // Start heartbeat timer
    _heartbeatTimer->start();

    qDebug() << "DaemonClient: connected to daemon, capabilities=" << Qt::hex << _capabilities;
    emit connected();
    return true;
}

void DaemonClient::disconnectFromDaemon()
{
    _heartbeatTimer->stop();
    _authenticated = false;
    _capabilities = 0;
    _pendingCreatePty = nullptr;
    _pendingAttachPty = nullptr;
    _recvBuf.clear();

    if (_socket) {
        _socket->disconnect(this);
        if (_socket->state() == QLocalSocket::ConnectedState)
            _socket->disconnectFromServer();
        _socket->deleteLater();
        _socket = nullptr;
    }
}

bool DaemonClient::isConnected() const
{
    return _socket &&
           _socket->state() == QLocalSocket::ConnectedState &&
           _authenticated;
}

uint32_t DaemonClient::negotiatedCapabilities() const
{
    return _capabilities;
}

// ---------------------------------------------------------------
// Session registration
// ---------------------------------------------------------------

void DaemonClient::registerSession(const QByteArray &uuid, PersistentPty *pty)
{
    _sessions.insert(uuid, pty);
}

void DaemonClient::unregisterSession(const QByteArray &uuid)
{
    _sessions.remove(uuid);
}

// ---------------------------------------------------------------
// Raw message sending
// ---------------------------------------------------------------

void DaemonClient::_sendRawMessage(uint8_t type, const uint8_t *payload, uint32_t len)
{
    if (!_socket || _socket->state() != QLocalSocket::ConnectedState) {
        qWarning() << "DaemonClient: cannot send, not connected";
        return;
    }

    QByteArray msg;
    msg.resize(static_cast<int>(HEADER_SIZE + len));
    uint8_t *dst = reinterpret_cast<uint8_t *>(msg.data());
    write_header(dst, type, len);
    if (len > 0 && payload)
        memcpy(dst + HEADER_SIZE, payload, len);

    _socket->write(msg);
    _socket->flush();
}

// ---------------------------------------------------------------
// Protocol message senders
// ---------------------------------------------------------------

void DaemonClient::sendCreate(PersistentPty *pty, const QString &shell,
                               const QStringList &args, const QStringList &env,
                               const QString &cwd, uint16_t rows, uint16_t cols)
{
    if (!isConnected()) return;

    QByteArray shellUtf8 = shell.toUtf8();
    QByteArray cwdUtf8 = cwd.toUtf8();

    // Calculate payload size:
    // [2B len][shell] + [2B count] + sum([2B len][arg]) + [2B count] + sum([2B len][env]) +
    // [2B len][cwd] + [2B rows][2B cols]
    uint32_t payloadSize = 0;
    payloadSize += 2 + static_cast<uint32_t>(shellUtf8.size());
    payloadSize += 2;  // args count
    QList<QByteArray> argsUtf8;
    for (const QString &a : args) {
        QByteArray u = a.toUtf8();
        payloadSize += 2 + static_cast<uint32_t>(u.size());
        argsUtf8.append(u);
    }
    payloadSize += 2;  // env count
    QList<QByteArray> envUtf8;
    for (const QString &e : env) {
        QByteArray u = e.toUtf8();
        payloadSize += 2 + static_cast<uint32_t>(u.size());
        envUtf8.append(u);
    }
    payloadSize += 2 + static_cast<uint32_t>(cwdUtf8.size());
    payloadSize += 4;  // rows + cols

    QByteArray payload;
    payload.resize(static_cast<int>(payloadSize));
    uint8_t *p = reinterpret_cast<uint8_t *>(payload.data());
    size_t pos = 0;

    // Shell
    pos += write_string(p + pos, shellUtf8.constData(),
                         static_cast<size_t>(shellUtf8.size()));

    // Args
    write_u16_le(p + pos, static_cast<uint16_t>(argsUtf8.size()));
    pos += 2;
    for (const QByteArray &a : argsUtf8) {
        pos += write_string(p + pos, a.constData(), static_cast<size_t>(a.size()));
    }

    // Env
    write_u16_le(p + pos, static_cast<uint16_t>(envUtf8.size()));
    pos += 2;
    for (const QByteArray &e : envUtf8) {
        pos += write_string(p + pos, e.constData(), static_cast<size_t>(e.size()));
    }

    // Cwd
    pos += write_string(p + pos, cwdUtf8.constData(),
                         static_cast<size_t>(cwdUtf8.size()));

    // Rows + cols
    write_u16_le(p + pos, rows); pos += 2;
    write_u16_le(p + pos, cols); pos += 2;

    _pendingCreatePty = pty;
    _sendRawMessage(MSG_CREATE, reinterpret_cast<const uint8_t *>(payload.constData()),
                    payloadSize);
}

void DaemonClient::sendAttach(const QByteArray &uuid)
{
    if (!isConnected()) return;
    if (uuid.size() != static_cast<int>(SESSION_ID_LEN)) return;
    _pendingAttachPty = _sessions.value(uuid, nullptr);
    _sendRawMessage(MSG_ATTACH, reinterpret_cast<const uint8_t *>(uuid.constData()),
                    SESSION_ID_LEN);
}

void DaemonClient::sendDetach(const QByteArray &uuid)
{
    if (!isConnected()) return;
    if (uuid.size() != static_cast<int>(SESSION_ID_LEN)) return;
    _sendRawMessage(MSG_DETACH, reinterpret_cast<const uint8_t *>(uuid.constData()),
                    SESSION_ID_LEN);
}

void DaemonClient::sendDestroy(const QByteArray &uuid)
{
    if (!isConnected()) return;
    if (uuid.size() != static_cast<int>(SESSION_ID_LEN)) return;
    _sendRawMessage(MSG_DESTROY, reinterpret_cast<const uint8_t *>(uuid.constData()),
                    SESSION_ID_LEN);
}

void DaemonClient::sendInput(const QByteArray &uuid, const char *data, int len)
{
    if (!isConnected()) return;
    if (uuid.size() != static_cast<int>(SESSION_ID_LEN)) return;
    if (!data || len <= 0) return;

    QByteArray payload;
    payload.resize(static_cast<int>(SESSION_ID_LEN) + len);
    memcpy(payload.data(), uuid.constData(), SESSION_ID_LEN);
    memcpy(payload.data() + SESSION_ID_LEN, data, static_cast<size_t>(len));

    _sendRawMessage(MSG_INPUT, reinterpret_cast<const uint8_t *>(payload.constData()),
                    static_cast<uint32_t>(payload.size()));
}

void DaemonClient::sendResize(const QByteArray &uuid, uint16_t rows, uint16_t cols)
{
    if (!isConnected()) return;
    if (uuid.size() != static_cast<int>(SESSION_ID_LEN)) return;

    uint8_t payload[SESSION_ID_LEN + 4];
    memcpy(payload, uuid.constData(), SESSION_ID_LEN);
    write_u16_le(payload + SESSION_ID_LEN, rows);
    write_u16_le(payload + SESSION_ID_LEN + 2, cols);

    _sendRawMessage(MSG_RESIZE, payload, sizeof(payload));
}

void DaemonClient::sendSignal(const QByteArray &uuid, int signal)
{
    if (!isConnected()) return;
    if (uuid.size() != static_cast<int>(SESSION_ID_LEN)) return;

    uint8_t payload[SESSION_ID_LEN + 4];
    memcpy(payload, uuid.constData(), SESSION_ID_LEN);
    write_u32_le(payload + SESSION_ID_LEN, static_cast<uint32_t>(signal));

    _sendRawMessage(MSG_SEND_SIGNAL, payload, sizeof(payload));
}

void DaemonClient::sendPing()
{
    if (!isConnected()) return;

    // PING: [8B timestamp]
    uint8_t payload[8];
    uint64_t now = static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
    write_u64_le(payload, now);

    _sendRawMessage(MSG_PING, payload, sizeof(payload));
}

void DaemonClient::sendSetTermios(const QByteArray &uuid,
                                   uint32_t iflag, uint32_t oflag,
                                   uint32_t cflag, uint32_t lflag,
                                   uint8_t verase, bool flowControl, bool utf8)
{
    if (!isConnected()) return;
    if (uuid.size() != static_cast<int>(SESSION_ID_LEN)) return;

    // [36B session_id][4B iflag][4B oflag][4B cflag][4B lflag][1B verase][1B flow][1B utf8]
    uint8_t payload[SESSION_ID_LEN + 19];
    memcpy(payload, uuid.constData(), SESSION_ID_LEN);
    size_t p = SESSION_ID_LEN;
    write_u32_le(payload + p, iflag); p += 4;
    write_u32_le(payload + p, oflag); p += 4;
    write_u32_le(payload + p, cflag); p += 4;
    write_u32_le(payload + p, lflag); p += 4;
    payload[p++] = verase;
    payload[p++] = flowControl ? 1 : 0;
    payload[p++] = utf8 ? 1 : 0;

    _sendRawMessage(MSG_SET_TERMIOS, payload, sizeof(payload));
}

void DaemonClient::sendFgProcessQuery(const QByteArray &uuid)
{
    if (!isConnected()) return;
    if (uuid.size() != static_cast<int>(SESSION_ID_LEN)) return;
    _sendRawMessage(MSG_FG_PROCESS_QUERY,
                    reinterpret_cast<const uint8_t *>(uuid.constData()),
                    SESSION_ID_LEN);
}

void DaemonClient::sendList()
{
    if (!isConnected()) return;
    _sendRawMessage(MSG_LIST, nullptr, 0);
}

// ---------------------------------------------------------------
// Receive handling
// ---------------------------------------------------------------

void DaemonClient::_onReadyRead()
{
    if (!_socket) return;

    _recvBuf.append(_socket->readAll());

    // Parse as many complete messages as possible
    while (true) {
        if (static_cast<size_t>(_recvBuf.size()) < HEADER_SIZE)
            break;

        const uint8_t *buf = reinterpret_cast<const uint8_t *>(_recvBuf.constData());
        uint8_t type = buf[0];
        uint32_t payloadLen = read_u32_le(buf + 1);

        // Validate message size
        if (payloadLen > MAX_MESSAGE_SIZE) {
            qWarning() << "DaemonClient: message too large:" << payloadLen;
            _recvBuf.clear();
            _socket->disconnectFromServer();
            return;
        }

        // Check if we have the full message
        size_t totalSize = HEADER_SIZE + payloadLen;
        if (static_cast<size_t>(_recvBuf.size()) < totalSize)
            break;

        // Dispatch the message
        const uint8_t *payload = buf + HEADER_SIZE;
        _dispatchMessage(type, payload, payloadLen);

        // Remove consumed message
        _recvBuf.remove(0, static_cast<int>(totalSize));
    }
}

void DaemonClient::_onDisconnected()
{
    qWarning() << "DaemonClient: disconnected from daemon";
    _heartbeatTimer->stop();
    _authenticated = false;
    _capabilities = 0;
    _pendingCreatePty = nullptr;
    _pendingAttachPty = nullptr;
    _recvBuf.clear();

    emit disconnected();
}

// ---------------------------------------------------------------
// Message dispatch
// ---------------------------------------------------------------

void DaemonClient::_dispatchMessage(uint8_t type, const uint8_t *payload, uint32_t len)
{
    switch (type) {

    case MSG_HELLO_OK: {
        // Already handled synchronously in connectToDaemon(), but handle
        // gracefully if received asynchronously.
        if (len >= 9) {
            _capabilities = read_u32_le(payload + 1);
        }
        _authenticated = true;
        break;
    }

    case MSG_CREATE_OK: {
        if (len < SESSION_ID_LEN) {
            qWarning() << "DaemonClient: CREATE_OK payload too short";
            break;
        }
        QByteArray uuid(reinterpret_cast<const char *>(payload),
                        static_cast<int>(SESSION_ID_LEN));

        if (_pendingCreatePty) {
            PersistentPty *pty = _pendingCreatePty;
            _pendingCreatePty = nullptr;
    _pendingAttachPty = nullptr;
            _sessions.insert(uuid, pty);
            pty->handleCreateOk(uuid);
        } else {
            qWarning() << "DaemonClient: CREATE_OK but no pending PersistentPty";
        }
        break;
    }

    case MSG_ATTACH_OK: {
        // [36B uuid][2B rows][2B cols][4B replay_size]
        if (len < SESSION_ID_LEN + 8) {
            qWarning() << "DaemonClient: ATTACH_OK payload too short";
            break;
        }
        QByteArray uuid(reinterpret_cast<const char *>(payload),
                        static_cast<int>(SESSION_ID_LEN));
        uint16_t rows = read_u16_le(payload + SESSION_ID_LEN);
        uint16_t cols = read_u16_le(payload + SESSION_ID_LEN + 2);
        // uint32_t replaySize = read_u32_le(payload + SESSION_ID_LEN + 4);

        _pendingAttachPty = nullptr;
        PersistentPty *pty = _sessions.value(uuid, nullptr);
        if (pty)
            pty->handleAttachOk(rows, cols);
        break;
    }

    case MSG_REPLAY_DATA: {
        // [36B uuid][data...]
        if (len < SESSION_ID_LEN) {
            qWarning() << "DaemonClient: REPLAY_DATA payload too short";
            break;
        }
        QByteArray uuid(reinterpret_cast<const char *>(payload),
                        static_cast<int>(SESSION_ID_LEN));
        const uint8_t *data = payload + SESSION_ID_LEN;
        uint32_t dataLen = len - SESSION_ID_LEN;

        PersistentPty *pty = _sessions.value(uuid, nullptr);
        if (pty)
            pty->handleReplayData(data, dataLen);
        break;
    }

    case MSG_REPLAY_END: {
        // Payload may be empty or contain UUID
        if (len >= SESSION_ID_LEN) {
            QByteArray uuid(reinterpret_cast<const char *>(payload),
                            static_cast<int>(SESSION_ID_LEN));
            PersistentPty *pty = _sessions.value(uuid, nullptr);
            if (pty)
                pty->handleReplayEnd();
        } else {
            // Daemon sends REPLAY_END with empty payload — find from context
            // (the most recently ATTACHed session)
            // For now, broadcast to all sessions; PersistentPty tracks its own state
            for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
                it.value()->handleReplayEnd();
            }
        }
        break;
    }

    case MSG_OUTPUT: {
        // [36B uuid][data...]
        if (len < SESSION_ID_LEN) {
            qWarning() << "DaemonClient: OUTPUT payload too short";
            break;
        }
        QByteArray uuid(reinterpret_cast<const char *>(payload),
                        static_cast<int>(SESSION_ID_LEN));
        const uint8_t *data = payload + SESSION_ID_LEN;
        uint32_t dataLen = len - SESSION_ID_LEN;

        PersistentPty *pty = _sessions.value(uuid, nullptr);
        if (pty)
            pty->handleOutput(reinterpret_cast<const char *>(data),
                              static_cast<int>(dataLen));
        break;
    }

    case MSG_SESSION_EXITED: {
        // [36B uuid][4B exit_code]
        if (len < SESSION_ID_LEN + 4) {
            qWarning() << "DaemonClient: SESSION_EXITED payload too short";
            break;
        }
        QByteArray uuid(reinterpret_cast<const char *>(payload),
                        static_cast<int>(SESSION_ID_LEN));
        uint32_t exitCode = read_u32_le(payload + SESSION_ID_LEN);

        PersistentPty *pty = _sessions.value(uuid, nullptr);
        if (pty)
            pty->handleSessionExited(static_cast<int>(exitCode));
        break;
    }

    case MSG_DETACH_OK: {
        // Payload may be empty or contain UUID
        if (len >= SESSION_ID_LEN) {
            QByteArray uuid(reinterpret_cast<const char *>(payload),
                            static_cast<int>(SESSION_ID_LEN));
            PersistentPty *pty = _sessions.value(uuid, nullptr);
            if (pty)
                pty->handleDetachOk();
        }
        break;
    }

    case MSG_DESTROY_OK: {
        // Payload may be empty or contain UUID
        if (len >= SESSION_ID_LEN) {
            QByteArray uuid(reinterpret_cast<const char *>(payload),
                            static_cast<int>(SESSION_ID_LEN));
            PersistentPty *pty = _sessions.value(uuid, nullptr);
            if (pty)
                pty->handleDestroyOk();
        }
        break;
    }

    case MSG_FG_PROCESS_UPDATE: {
        // [36B uuid][4B pid]
        if (len < SESSION_ID_LEN + 4) {
            qWarning() << "DaemonClient: FG_PROCESS_UPDATE payload too short";
            break;
        }
        QByteArray uuid(reinterpret_cast<const char *>(payload),
                        static_cast<int>(SESSION_ID_LEN));
        uint32_t pid = read_u32_le(payload + SESSION_ID_LEN);

        PersistentPty *pty = _sessions.value(uuid, nullptr);
        if (pty)
            pty->handleFgProcessUpdate(static_cast<int>(pid));
        break;
    }

    case MSG_FG_PROCESS_INFO: {
        // [36B uuid][4B pid][2B name_len][name][2B cwd_len][cwd]
        if (len < SESSION_ID_LEN + 4) {
            qWarning() << "DaemonClient: FG_PROCESS_INFO payload too short";
            break;
        }
        QByteArray uuid(reinterpret_cast<const char *>(payload),
                        static_cast<int>(SESSION_ID_LEN));
        uint32_t pid = read_u32_le(payload + SESSION_ID_LEN);

        size_t pos = SESSION_ID_LEN + 4;
        const char *nameStr = nullptr;
        uint16_t nameLen = 0;
        size_t consumed = 0;

        QString name;
        QString cwd;

        if (read_string(payload + pos, len - pos, &nameStr, &nameLen, &consumed)) {
            name = QString::fromUtf8(nameStr, nameLen);
            pos += consumed;
        }

        const char *cwdStr = nullptr;
        uint16_t cwdLen = 0;
        if (read_string(payload + pos, len - pos, &cwdStr, &cwdLen, &consumed)) {
            cwd = QString::fromUtf8(cwdStr, cwdLen);
        }

        PersistentPty *pty = _sessions.value(uuid, nullptr);
        if (pty)
            pty->handleFgProcessInfo(static_cast<int>(pid), name, cwd);
        break;
    }

    case MSG_SIGNAL_OK: {
        // No-op
        break;
    }

    case MSG_ERROR: {
        // [1B error_code][2B msg_len][msg...]
        if (len < 1) {
            qWarning() << "DaemonClient: ERROR payload empty";
            break;
        }
        uint8_t errorCode = payload[0];
        QString errorMsg;
        if (len >= 3) {
            uint16_t msgLen = read_u16_le(payload + 1);
            if (3 + msgLen <= len)
                errorMsg = QString::fromUtf8(
                    reinterpret_cast<const char *>(payload + 3), msgLen);
        }

        qWarning() << "DaemonClient: daemon error code=" << errorCode
                    << "msg=" << errorMsg;

        // If there is a pending create or attach, notify it of failure
        if (_pendingCreatePty) {
            PersistentPty *pty = _pendingCreatePty;
            _pendingCreatePty = nullptr;
            pty->handleError(errorCode, errorMsg);
        } else if (_pendingAttachPty) {
            PersistentPty *pty = _pendingAttachPty;
            _pendingAttachPty = nullptr;
            pty->handleError(errorCode, errorMsg);
        }
        break;
    }

    case MSG_PONG: {
        // PONG received — heartbeat acknowledged, reset timer
        _heartbeatTimer->start();
        break;
    }

    case MSG_LIST_OK: {
        // [2B count] then per session:
        //   [36B id][1B alive][2B rows][2B cols][str shell][str cwd]
        //   [8B created_at][8B detached_at][1B has_client]
        if (len < 2) break;
        uint16_t count = read_u16_le(payload);
        size_t pos = 2;
        QList<DaemonSessionInfo> sessions;
        for (uint16_t i = 0; i < count; i++) {
            if (pos + SESSION_ID_LEN + 1 + 4 > len) break;
            DaemonSessionInfo info;
            info.sessionId = QByteArray(reinterpret_cast<const char *>(payload + pos),
                                         static_cast<int>(SESSION_ID_LEN));
            pos += SESSION_ID_LEN;
            info.alive = payload[pos++] != 0;
            info.rows = read_u16_le(payload + pos); pos += 2;
            info.cols = read_u16_le(payload + pos); pos += 2;
            const char *str; uint16_t slen; size_t consumed;
            if (!read_string(payload + pos, len - pos, &str, &slen, &consumed)) break;
            info.shell = QString::fromUtf8(str, slen); pos += consumed;
            if (!read_string(payload + pos, len - pos, &str, &slen, &consumed)) break;
            info.cwd = QString::fromUtf8(str, slen); pos += consumed;
            if (pos + 17 > len) break;
            info.createdAt = static_cast<qint64>(read_u64_le(payload + pos)); pos += 8;
            info.detachedAt = static_cast<qint64>(read_u64_le(payload + pos)); pos += 8;
            info.hasClient = payload[pos++] != 0;
            sessions.append(info);
        }
        emit listResult(sessions);
        break;
    }

    default:
        qWarning() << "DaemonClient: unknown message type"
                    << Qt::hex << type;
        break;
    }
}

} // namespace Konsole
