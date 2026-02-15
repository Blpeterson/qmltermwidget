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

// Singleton client for communication with the crt-sessiond daemon over a
// Unix domain socket.  All PersistentPty instances share this connection.

#ifndef DAEMONCLIENT_H
#define DAEMONCLIENT_H

#include <QObject>
#include <QLocalSocket>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QTimer>

#include "protocol.h"

namespace Konsole {

class PersistentPty;  // forward decl

// Snapshot of a daemon session returned by MSG_LIST_OK.
struct DaemonSessionInfo {
    QByteArray sessionId;   // 36-byte UUID
    bool alive;             // Shell process still running
    uint16_t rows, cols;    // Current terminal dimensions
    QString shell;          // Shell binary path
    QString cwd;            // Initial working directory
    qint64 createdAt;       // Unix timestamp (seconds)
    qint64 detachedAt;      // Unix timestamp, 0 if attached
    bool hasClient;         // Currently attached to a client
};

class DaemonClient : public QObject {
    Q_OBJECT
public:
    static DaemonClient *instance();

    bool connectToDaemon();
    void disconnectFromDaemon();
    bool isConnected() const;
    uint32_t negotiatedCapabilities() const;

    // Session management
    void registerSession(const QByteArray &uuid, PersistentPty *pty);
    void unregisterSession(const QByteArray &uuid);

    // Protocol messages (async)
    void sendCreate(PersistentPty *pty, const QString &shell, const QStringList &args,
                    const QStringList &env, const QString &cwd,
                    uint16_t rows, uint16_t cols);
    void sendAttach(const QByteArray &uuid);
    void sendDetach(const QByteArray &uuid);
    void sendDestroy(const QByteArray &uuid);
    void sendInput(const QByteArray &uuid, const char *data, int len);
    void sendResize(const QByteArray &uuid, uint16_t rows, uint16_t cols);
    void sendSignal(const QByteArray &uuid, int signal);
    void sendPing();
    void sendSetTermios(const QByteArray &uuid,
                        uint32_t iflag, uint32_t oflag,
                        uint32_t cflag, uint32_t lflag,
                        uint8_t verase, bool flowControl, bool utf8);
    void sendFgProcessQuery(const QByteArray &uuid);
    void sendList();

signals:
    void connected();
    void disconnected();
    void connectionError(const QString &msg);
    void listResult(const QList<DaemonSessionInfo> &sessions);

private:
    explicit DaemonClient(QObject *parent = nullptr);
    ~DaemonClient() override;

    static QString _socketPath();

    QLocalSocket *_socket;
    QByteArray _recvBuf;
    QHash<QByteArray, PersistentPty *> _sessions;
    // Pending CREATE: the PersistentPty that sent CREATE, waiting for CREATE_OK with UUID
    PersistentPty *_pendingCreatePty;
    // Pending ATTACH: the PersistentPty that sent ATTACH, waiting for ATTACH_OK
    PersistentPty *_pendingAttachPty;
    QTimer *_heartbeatTimer;
    uint32_t _capabilities;
    bool _authenticated;

    void _onReadyRead();
    void _onDisconnected();
    void _dispatchMessage(uint8_t type, const uint8_t *payload, uint32_t len);
    void _sendRawMessage(uint8_t type, const uint8_t *payload, uint32_t len);
};

} // namespace Konsole

Q_DECLARE_METATYPE(Konsole::DaemonSessionInfo)

#endif // DAEMONCLIENT_H
