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

#ifndef PERSISTENTPTY_H
#define PERSISTENTPTY_H

#include "PtyInterface.h"
#include <QByteArray>
#include <QEventLoop>

namespace Konsole {

class DaemonClient;  // forward decl

class PersistentPty : public PtyInterface {
    Q_OBJECT
public:
    explicit PersistentPty(QObject *parent = nullptr);
    ~PersistentPty() override;

    // PtyInterface overrides
    int start(const QString &program, const QStringList &args,
              const QStringList &env, const QString &workingDir,
              ulong winid, bool addToUtmp) override;
    void closePty() override;       // DETACH
    void requestClose() override;   // DESTROY
    void kill() override;           // DESTROY (fire-and-forget)
    void setWindowSize(int lines, int cols) override;
    QSize windowSize() const override;
    void setFlowControlEnabled(bool on) override;
    bool flowControlEnabled() const override;
    void setErase(char erase) override;
    char erase() const override;
    void setWriteable(bool writeable) override;
    void setEmptyPTYProperties() override;
    qint64 processId() const override;
    int foregroundProcessGroup() const override;
    bool isRunning() const override;
    int slaveFd() const override;
    bool sendSignal(int signal) override;
    bool waitForFinished(int msecs = -1) override;

    void setUtf8Mode(bool on) override;
    void lockPty(bool lock) override;
    void sendData(const char *buffer, int length) override;

    // Attach to existing daemon session
    int attachToSession(const QByteArray &sessionId);
    QByteArray sessionId() const { return _sessionId; }

    // Called by DaemonClient
    void handleCreateOk(const QByteArray &uuid);
    void handleAttachOk(uint16_t rows, uint16_t cols);
    void handleReplayData(const uint8_t *data, uint32_t len);
    void handleReplayEnd();
    void handleOutput(const char *data, int len);
    void handleSessionExited(int exitCode);
    void handleDetachOk();
    void handleDestroyOk();
    void handleFgProcessUpdate(int pid);
    void handleFgProcessInfo(int pid, const QString &name, const QString &cwd);
    void handleError(uint8_t code, const QString &msg);

private:
    void _sendTermiosIfAttached();

    QByteArray _sessionId;
    int _windowLines = 0, _windowCols = 0;
    bool _attached = false;
    bool _sessionExited = false;
    int _exitCode = 0;
    int _fgProcessGroup = 0;
    qint64 _shellPid = 0;
    bool _flowControl = true;
    char _eraseChar = 0;
    bool _utf8 = true;
    bool _replaying = false;
    QEventLoop *_waitLoop = nullptr;  // For synchronous start()/waitForFinished()
};

} // namespace Konsole

#endif // PERSISTENTPTY_H
