/*
 * This file is a part of QTerminal - http://gitorious.org/qterminal
 *
 * This file was un-linked from KDE and modified
 * by Maxim Bourmistrov <maxim@unixconn.com>
 *
 */

/*
    This file is part of Konsole, KDE's terminal emulator.

    Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>
    Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA.
*/

#ifndef PTY_H
#define PTY_H

// Qt
#include <QStringList>
#include <QVector>
#include <QList>
#include <QSize>

// Own
#include "PtyInterface.h"

class KPtyProcess;

namespace Konsole {

/**
 * The Pty class is used to start the terminal process,
 * send data to it, receive data from it and manipulate
 * various properties of the pseudo-teletype interface
 * used to communicate with the process.
 *
 * To use this class, construct an instance and connect
 * to the sendData slot and receivedData signal to
 * send data to or receive data from the process.
 *
 * To start the terminal process, call the start() method
 * with the program name and appropriate arguments.
 */
class Pty: public PtyInterface
{
Q_OBJECT

  public:

    /**
     * Constructs a new Pty.
     *
     * Connect to the sendData() slot and receivedData() signal to prepare
     * for sending and receiving data from the terminal process.
     *
     * To start the terminal process, call the run() method with the
     * name of the program to start and appropriate arguments.
     */
    explicit Pty(QObject* parent = nullptr);

    /**
     * Construct a process using an open pty master.
     * See KPtyProcess::KPtyProcess()
     */
    explicit Pty(int ptyMasterFd, QObject* parent = nullptr);

    ~Pty() override;

    /**
     * Starts the terminal process.
     *
     * Returns 0 if the process was started successfully or non-zero
     * otherwise.
     *
     * @param program Path to the program to start
     * @param arguments Arguments to pass to the program being started
     * @param environment A list of key=value pairs which will be added
     * to the environment for the new process.  At the very least this
     * should include an assignment for the TERM environment variable.
     * @param workingDir Working directory for the new process.
     * @param winid Specifies the value of the WINDOWID environment variable
     * in the process's environment.
     * @param addToUtmp Specifies whether a utmp entry should be created for
     * the pty used.  See K3Process::setUsePty()
     */
    int start( const QString& program,
               const QStringList& arguments,
               const QStringList& environment,
               const QString& workingDir,
               ulong winid,
               bool addToUtmp
             ) override;

    void setEmptyPTYProperties() override;
    void setWriteable(bool writeable) override;
    void setFlowControlEnabled(bool on) override;
    bool flowControlEnabled() const override;
    void setWindowSize(int lines, int cols) override;
    QSize windowSize() const override;
    void setErase(char erase) override;
    char erase() const override;
    int foregroundProcessGroup() const override;
    void closePty() override;
    void requestClose() override;
    void kill() override;

    qint64 processId() const override;
    bool isRunning() const override;
    int slaveFd() const override;
    bool sendSignal(int signal) override;
    bool waitForFinished(int msecs = -1) override;

  public slots:
    void setUtf8Mode(bool on) override;
    void lockPty(bool lock) override;
    void sendData(const char* buffer, int length) override;

  private slots:
    // called when data is received from the terminal process
    void dataReceived();

  private:
      void init();

    // takes a list of key=value pairs and adds them
    // to the environment for the process
    void addEnvironmentVariables(const QStringList& environment);

    KPtyProcess *_process;
    int  _windowColumns;
    int  _windowLines;
    char _eraseChar;
    bool _xonXoff;
    bool _utf8;
};

}

#endif // PTY_H
