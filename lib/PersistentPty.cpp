/*
    Copyright (c) 2026 Alex Fabri
    https://crtplus.fromhelloworld.com
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

#include "PersistentPty.h"
#include "DaemonClient.h"

#include <QTimer>
#include <QtDebug>
#include <termios.h>

using namespace Konsole;

// Default termios flags matching openpty() defaults.  The daemon applies
// these via tcsetattr(); only VERASE, flow_control, and utf8 vary per call.
static constexpr uint32_t kDefaultIflag = ICRNL | IXON | IXOFF | IMAXBEL;
static constexpr uint32_t kDefaultOflag = OPOST | ONLCR;
static constexpr uint32_t kDefaultCflag = CS8 | CREAD | CLOCAL;
static constexpr uint32_t kDefaultLflag = ISIG | ICANON | ECHO | ECHOE | ECHOK
                                        | ECHOKE | ECHOCTL | IEXTEN;

PersistentPty::PersistentPty(QObject *parent)
    : PtyInterface(parent)
{
}

PersistentPty::~PersistentPty()
{
    if (_attached && !_sessionId.isEmpty()) {
        DaemonClient::instance()->sendDetach(_sessionId);
    }
    if (!_sessionId.isEmpty()) {
        DaemonClient::instance()->unregisterSession(_sessionId);
    }
}

// ---------------------------------------------------------------------------
// PtyInterface overrides — Lifecycle
// ---------------------------------------------------------------------------

int PersistentPty::start(const QString &program, const QStringList &args,
                         const QStringList &env, const QString &workingDir,
                         ulong winid, bool addToUtmp)
{
    Q_UNUSED(winid)
    Q_UNUSED(addToUtmp)

    DaemonClient *client = DaemonClient::instance();

    if (!client->isConnected()) {
        if (!client->connectToDaemon()) {
            qWarning() << "PersistentPty::start: failed to connect to daemon";
            return -1;
        }
    }

    uint16_t rows = static_cast<uint16_t>(_windowLines);
    uint16_t cols = static_cast<uint16_t>(_windowCols);

    client->sendCreate(this, program, args, env, workingDir, rows, cols);

    // Spin event loop until handleCreateOk() sets _sessionId, or timeout
    QEventLoop loop;
    _waitLoop = &loop;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    _waitLoop = nullptr;

    if (_sessionId.isEmpty()) {
        qWarning() << "PersistentPty::start: CREATE timed out or failed";
        return -1;
    }

    _attached = true;
    _sendTermiosIfAttached();
    return 0;
}

void PersistentPty::closePty()
{
    if (_attached && !_sessionId.isEmpty()) {
        DaemonClient::instance()->sendDetach(_sessionId);
    }
    _attached = false;
}

void PersistentPty::requestClose()
{
    if (!_sessionId.isEmpty()) {
        DaemonClient::instance()->sendDestroy(_sessionId);
    }
    _attached = false;
    _sessionExited = true;
}

void PersistentPty::kill()
{
    requestClose();
}

// ---------------------------------------------------------------------------
// PtyInterface overrides — Terminal properties
// ---------------------------------------------------------------------------

void PersistentPty::setWindowSize(int lines, int cols)
{
    if (_windowLines == lines && _windowCols == cols)
        return;

    _windowLines = lines;
    _windowCols = cols;

    if (_attached && !_sessionId.isEmpty()) {
        DaemonClient::instance()->sendResize(
            _sessionId,
            static_cast<uint16_t>(lines),
            static_cast<uint16_t>(cols));
    }
}

QSize PersistentPty::windowSize() const
{
    return {_windowCols, _windowLines};
}

void PersistentPty::setFlowControlEnabled(bool on)
{
    _flowControl = on;
    _sendTermiosIfAttached();
}

bool PersistentPty::flowControlEnabled() const
{
    return _flowControl;
}

void PersistentPty::setErase(char erase)
{
    _eraseChar = erase;
    _sendTermiosIfAttached();
}

char PersistentPty::erase() const
{
    return _eraseChar;
}

void PersistentPty::setWriteable(bool writeable)
{
    Q_UNUSED(writeable)
    // No-op: PTY slave is owned by the daemon
}

void PersistentPty::setEmptyPTYProperties()
{
    // No-op: not meaningful for daemon-backed PTY
}

// ---------------------------------------------------------------------------
// PtyInterface overrides — Process info
// ---------------------------------------------------------------------------

qint64 PersistentPty::processId() const
{
    return _shellPid;
}

int PersistentPty::foregroundProcessGroup() const
{
    return _fgProcessGroup;
}

bool PersistentPty::isRunning() const
{
    return _attached && !_sessionExited;
}

int PersistentPty::slaveFd() const
{
    return -1;  // PTY slave is owned by the daemon
}

// ---------------------------------------------------------------------------
// PtyInterface overrides — Signal / wait
// ---------------------------------------------------------------------------

bool PersistentPty::sendSignal(int signal)
{
    if (_sessionId.isEmpty())
        return false;

    DaemonClient::instance()->sendSignal(_sessionId, signal);
    return true;
}

bool PersistentPty::waitForFinished(int msecs)
{
    if (_sessionExited)
        return true;

    QEventLoop loop;
    _waitLoop = &loop;
    if (msecs >= 0) {
        QTimer::singleShot(msecs, &loop, &QEventLoop::quit);
    }
    loop.exec();
    _waitLoop = nullptr;

    return _sessionExited;
}

// ---------------------------------------------------------------------------
// PtyInterface overrides — Slots
// ---------------------------------------------------------------------------

void PersistentPty::setUtf8Mode(bool on)
{
    _utf8 = on;
    _sendTermiosIfAttached();
}

void PersistentPty::lockPty(bool lock)
{
    Q_UNUSED(lock)
    // No-op
}

void PersistentPty::sendData(const char *buffer, int length)
{
    if (!_attached || _sessionId.isEmpty())
        return;

    DaemonClient::instance()->sendInput(_sessionId, buffer, length);
}

// ---------------------------------------------------------------------------
// Attach to existing daemon session
// ---------------------------------------------------------------------------

int PersistentPty::attachToSession(const QByteArray &sessionId)
{
    DaemonClient *client = DaemonClient::instance();

    if (!client->isConnected()) {
        if (!client->connectToDaemon()) {
            qWarning() << "PersistentPty::attachToSession: failed to connect to daemon";
            return -1;
        }
    }

    _sessionId = sessionId;
    client->registerSession(_sessionId, this);
    client->sendAttach(_sessionId);

    // Spin event loop until handleAttachOk() sets _attached, or timeout
    QEventLoop loop;
    _waitLoop = &loop;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    _waitLoop = nullptr;

    if (!_attached) {
        qWarning() << "PersistentPty::attachToSession: ATTACH timed out or failed"
                    << sessionId;
        client->unregisterSession(_sessionId);
        _sessionId.clear();
        return -1;
    }

    // Don't send SET_TERMIOS here — the daemon already restored the saved
    // termios from before detach.  Sending default flags would overwrite
    // raw mode set by TUI apps (e.g. Claude CLI, vim).

    // SIGWINCH is deferred to handleReplayEnd() so TUI apps redraw after
    // the scrollback replay is fully processed by the terminal emulator.

    return 0;
}

// ---------------------------------------------------------------------------
// Termios sync
// ---------------------------------------------------------------------------

void PersistentPty::_sendTermiosIfAttached()
{
    if (!_attached || _sessionId.isEmpty()) return;
    DaemonClient::instance()->sendSetTermios(
        _sessionId, kDefaultIflag, kDefaultOflag, kDefaultCflag, kDefaultLflag,
        static_cast<uint8_t>(_eraseChar), _flowControl, _utf8);
}

// ---------------------------------------------------------------------------
// Handlers called by DaemonClient
// ---------------------------------------------------------------------------

void PersistentPty::handleCreateOk(const QByteArray &uuid)
{
    _sessionId = uuid;
    DaemonClient::instance()->registerSession(_sessionId, this);

    if (_waitLoop && _waitLoop->isRunning())
        _waitLoop->quit();
}

void PersistentPty::handleAttachOk(uint16_t rows, uint16_t cols)
{
    _windowLines = rows;
    _windowCols = cols;
    _attached = true;
    _replaying = true;  // REPLAY_DATA messages follow

    if (_waitLoop && _waitLoop->isRunning())
        _waitLoop->quit();
}

void PersistentPty::handleReplayData(const uint8_t *data, uint32_t len)
{
    emit receivedData(reinterpret_cast<const char *>(data),
                      static_cast<int>(len));
}

void PersistentPty::handleReplayEnd()
{
    _replaying = false;

    // Force a resize so the daemon sends SIGWINCH to the process group.
    // TUI apps (nano, vim, etc.) need this to fully redraw after reattach.
    // Deferred to after replay so the redraw isn't overwritten by scrollback data.
    if (_attached && !_sessionId.isEmpty()) {
        DaemonClient::instance()->sendResize(
            _sessionId,
            static_cast<uint16_t>(_windowLines),
            static_cast<uint16_t>(_windowCols));
    }
}

void PersistentPty::handleOutput(const char *data, int len)
{
    emit receivedData(data, len);
}

void PersistentPty::handleSessionExited(int exitCode)
{
    _sessionExited = true;
    _exitCode = exitCode;
    _attached = false;

    emit finished(exitCode,
                  exitCode == 0 ? PtyExitStatus::NormalExit
                                : PtyExitStatus::CrashExit);

    if (_waitLoop && _waitLoop->isRunning())
        _waitLoop->quit();
}

void PersistentPty::handleDetachOk()
{
    _attached = false;
}

void PersistentPty::handleDestroyOk()
{
    _attached = false;
    _sessionExited = true;
}

void PersistentPty::handleFgProcessUpdate(int pid)
{
    _fgProcessGroup = pid;
}

void PersistentPty::handleFgProcessInfo(int pid, const QString &name,
                                        const QString &cwd)
{
    Q_UNUSED(name)
    Q_UNUSED(cwd)
    _fgProcessGroup = pid;
}

void PersistentPty::handleError(uint8_t code, const QString &msg)
{
    qWarning() << "PersistentPty: daemon error" << code << msg;

    if (_waitLoop && _waitLoop->isRunning())
        _waitLoop->quit();
}
