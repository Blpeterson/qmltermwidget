/*
    This file is part of Konsole QML plugin,
    which is a terminal emulator from KDE.

    Copyright 2013      by Dmitry Zagnoyko <hiroshidi@gmail.com>

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

#ifndef KSESSION_H
#define KSESSION_H

#include <QObject>

// Konsole
#include "Session.h"

namespace Konsole {
class TerminalDisplay;
class Session;
}

class KSession : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString  kbScheme  READ  getKeyBindings WRITE setKeyBindings NOTIFY changedKeyBindings)
    Q_PROPERTY(QString  initialWorkingDirectory READ getInitialWorkingDirectory WRITE setInitialWorkingDirectory NOTIFY initialWorkingDirectoryChanged)
    Q_PROPERTY(QString  title READ getTitle WRITE setTitle NOTIFY titleChanged)
    Q_PROPERTY(QString  shellProgram READ getShellProgram WRITE setShellProgram)
    Q_PROPERTY(QStringList  shellProgramArgs READ getShellProgramArgs WRITE setArgs)
    Q_PROPERTY(QString  history READ getHistory)
    Q_PROPERTY(bool hasActiveProcess READ hasActiveProcess)
    Q_PROPERTY(QString foregroundProcessName READ foregroundProcessName)
    Q_PROPERTY(QString foregroundProcessLabel READ foregroundProcessLabel)
    Q_PROPERTY(QString currentDir READ currentDir)
    Q_PROPERTY(QString sessionId READ sessionId NOTIFY sessionIdChanged)
    Q_PROPERTY(bool persistentSession READ persistentSession WRITE setPersistentSession)

public:
    KSession(QObject *parent = 0);
    ~KSession();

    QString sessionId() const;
    bool persistentSession() const;
    void setPersistentSession(bool persistent);

public:
    //bool setup();
    void addView(Konsole::TerminalDisplay *display);
    void removeView(Konsole::TerminalDisplay *display);

    int getRandomSeed();
    QString getKeyBindings();

    //look-n-feel, if you don`t like defaults

    //environment
    void setEnvironment(const QStringList & environment);

    //Initial working directory
    void setInitialWorkingDirectory(const QString & dir);
    QString getInitialWorkingDirectory();

    QString getShellProgram() const;
    QStringList getShellProgramArgs() const;

    // History size for scrolling
    void setHistorySize(int lines); //infinite if lines < 0
    int historySize() const;

    QString getHistory() const;

    // Sets whether flow control is enabled
    void setFlowControlEnabled(bool enabled);

    // Returns whether flow control is enabled
    bool flowControlEnabled(void);

    /**
     * Sets whether the flow control warning box should be shown
     * when the flow control stop key (Ctrl+S) is pressed.
     */
    //void setFlowControlWarningEnabled(bool enabled);

    /*! Get all available keyboard bindings
     */
    static QStringList availableKeyBindings();

    //! Return current key bindings
    QString keyBindings();

    QString getTitle();

    /**
     * Returns \c true if the session has an active subprocess running in it
     * spawned from the initial shell.
     */
    bool hasActiveProcess() const;

    /**
     * Returns the name of the terminal's foreground process.
     */
    QString foregroundProcessName();

    /**
     * Returns a display label for the foreground process.
     * For SSH, returns "user@host"; otherwise returns the process name.
     */
    QString foregroundProcessLabel();

    /**
     * Returns the current working directory of the process.
     */
    QString currentDir();

    /**
     * Returns SSH connection info (host, user, port) if the foreground
     * process is ssh. Returns an empty map otherwise.
     */
    Q_INVOKABLE QVariantMap sshConnectionInfo();

    /**
     * Explicitly closes (DESTROY) the daemon session.
     * Used for user-initiated close actions (Cmd+W) where the shell should die.
     */
    Q_INVOKABLE void closeSession();

    /**
     * Attaches to an existing daemon session by UUID.
     * Returns 0 on success, -1 on failure.
     */
    Q_INVOKABLE int attachToSession(const QString &uuid);

    /**
     * Queues text to be sent after a shell prompt is detected.
     * Used for sending commands (like cd) after an SSH connection is established.
     * @param promptChars Characters to match as prompt endings (default: "$#%>").
     */
    Q_INVOKABLE void sendTextOnceReady(const QString &text, const QString &promptChars = QString());

signals:
    void started();
    void finished();
    void copyAvailable(bool);

    void termGetFocus();
    void termLostFocus();

    void termKeyPressed(QKeyEvent *, bool);

    void changedKeyBindings(QString kb);

    void titleChanged();

    void historySizeChanged();

    void initialWorkingDirectoryChanged();

    void sessionIdChanged();

    void matchFound(int startColumn, int startLine, int endColumn, int endLine);
    void noMatchFound();

    void bellRequest(const QString &message);
    void activity();

public slots:
    /*! Set named key binding for given widget
     */
    void setKeyBindings(const QString & kb);
    void setTitle(QString name);

    void startShellProgram();

    bool sendSignal(int signal);

    //  Shell program, default is /bin/bash
    void setShellProgram(const QString & progname);

    // Shell program args, default is none
    void setArgs(const QStringList &args);

    int getShellPID();
    void changeDir(const QString & dir);

    // Send some text to terminal
    void sendText(QString text);
    // Send some text to terminal
    void sendKey(int rep, int key, int mod) const;

    void clearScreen();

    // Search history
    void search(const QString &regexp, int startLine = 0, int startColumn = 0, bool forwards = true );

protected slots:
    void sessionFinished();
    void selectionChanged(bool textSelected);

private slots:
    Konsole::Session* createSession(QString name);
    void onStateChanged(int state);

private:
    //Konsole::KTerminalDisplay *m_terminalDisplay;
    QString _initialWorkingDirectory;
    QString m_shellProgram;
    QStringList m_shellArgs;
    Konsole::Session *m_session;
    bool _persistentSession = false;

};

#endif // KSESSION_H
