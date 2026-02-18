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

#ifndef PTYINTERFACE_H
#define PTYINTERFACE_H

#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>

namespace Konsole {

enum class PtyExitStatus { NormalExit, CrashExit };

/**
 * Abstract interface for terminal PTY backends.
 *
 * Pty implements this using a local KPtyProcess.
 * PersistentPty implements this via the session daemon.
 */
class PtyInterface : public QObject {
    Q_OBJECT

public:
    explicit PtyInterface(QObject *parent = nullptr) : QObject(parent) {}

    // Lifecycle
    virtual int start(const QString &program, const QStringList &args,
                      const QStringList &env, const QString &workingDir,
                      ulong winid, bool addToUtmp) = 0;
    virtual void closePty() = 0;
    virtual void requestClose() = 0;
    virtual void kill() = 0;

    // Terminal properties
    virtual void setWindowSize(int lines, int cols) = 0;
    virtual QSize windowSize() const = 0;
    virtual void setFlowControlEnabled(bool on) = 0;
    virtual bool flowControlEnabled() const = 0;
    virtual void setErase(char erase) = 0;
    virtual char erase() const = 0;
    virtual void setWriteable(bool writeable) = 0;
    virtual void setEmptyPTYProperties() = 0;

    // Process info
    virtual qint64 processId() const = 0;
    virtual int foregroundProcessGroup() const = 0;
    virtual bool isRunning() const = 0;
    virtual int slaveFd() const = 0;

    // Signal/wait
    virtual bool sendSignal(int signal) = 0;
    virtual bool waitForFinished(int msecs = -1) = 0;

public slots:
    virtual void setUtf8Mode(bool on) = 0;
    virtual void lockPty(bool lock) = 0;
    virtual void sendData(const char *buffer, int length) = 0;

signals:
    void receivedData(const char *buffer, int length);
    void finished(int exitCode, PtyExitStatus exitStatus);
};

} // namespace Konsole

#endif // PTYINTERFACE_H
