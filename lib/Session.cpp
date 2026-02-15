/*
    This file is part of Konsole

    Copyright (C) 2006-2007 by Robert Knight <robertknight@gmail.com>
    Copyright (C) 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

    Rewritten for QT4 by e_k <e_k at users.sourceforge.net>, Copyright (C)2008

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

// Own
#include "Session.h"

// Standard
#include <cstdlib>

// Qt
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QFile>
#include <QtDebug>
#include <QRegularExpression>

#include "Pty.h"
#include "PersistentPty.h"
//#include "kptyprocess.h"
#include "TerminalDisplay.h"
#include "ShellCommand.h"
#include "Vt102Emulation.h"

// QMLTermWidget
#include <QQuickWindow>

using namespace Konsole;
using namespace Qt::Literals::StringLiterals;

int Session::lastSessionId = 0;

Session::Session(bool usePersistentPty, QObject* parent) :
    QObject(parent),
        _shellProcess(nullptr)
        , _emulation(nullptr)
        , _monitorActivity(false)
        , _monitorSilence(false)
        , _notifiedActivity(false)
        , _autoClose(true)
        , _wantedClose(false)
        , _silenceSeconds(10)
        , _isTitleChanged(false)
        , _addToUtmp(false)  // disabled by default because of a bug encountered on certain systems
        // which caused Konsole to hang when closing a tab and then opening a new
        // one.  A 'QProcess destroyed while still running' warning was being
        // printed to the terminal.  Likely a problem in KPty::logout()
        // or KPty::login() which uses a QProcess to start /usr/bin/utempter
        , _flowControl(true)
        , _fullScripting(false)
        , _sessionId(0)
//   , _zmodemBusy(false)
//   , _zmodemProc(0)
//   , _zmodemProgress(0)
        , _hasDarkBackground(false)
        , _foregroundProcessInfo(NULL)
        , _foregroundPid(0)
        , _usePersistentPty(usePersistentPty)
        , _waitingForPrompt(false)
        , _scrollPendingCount(0)
{
    //prepare DBus communication
//    new SessionAdaptor(this);
    _sessionId = ++lastSessionId;
//    QDBusConnection::sessionBus().registerObject(QLatin1String("/Sessions/")+QString::number(_sessionId), this);

    //create teletype for I/O with shell process
    if (_usePersistentPty) {
        _shellProcess = new PersistentPty(this);
    } else {
        _shellProcess = new Pty();
    }
    ptySlaveFd = _shellProcess->slaveFd();

    //create emulation backend
    _emulation = new Vt102Emulation();

    connect( _emulation, SIGNAL( titleChanged( int, const QString & ) ),
             this, SLOT( setUserTitle( int, const QString & ) ) );
    connect( _emulation, SIGNAL( stateSet(int) ),
             this, SLOT( activityStateSet(int) ) );
//    connect( _emulation, SIGNAL( zmodemDetected() ), this ,
//            SLOT( fireZModemDetected() ) );
    connect( _emulation, SIGNAL( changeTabTextColorRequest( int ) ),
             this, SIGNAL( changeTabTextColorRequest( int ) ) );
    connect( _emulation, SIGNAL(profileChangeCommandReceived(const QString &)),
             this, SIGNAL( profileChangeCommandReceived(const QString &)) );

    connect(_emulation, SIGNAL(imageResizeRequest(QSize)),
            this, SLOT(onEmulationSizeChange(QSize)));
    connect(_emulation, SIGNAL(imageSizeChanged(int, int)),
            this, SLOT(onViewSizeChange(int, int)));
    connect(_emulation, &Vt102Emulation::cursorChanged,
            this, &Session::cursorChanged);

    //connect teletype to emulation backend
    _shellProcess->setUtf8Mode(true);

    _connectPtySignals();

    //setup timer for monitoring session activity
    _monitorTimer = new QTimer(this);
    _monitorTimer->setSingleShot(true);
    connect(_monitorTimer, SIGNAL(timeout()), this, SLOT(monitorTimerDone()));

    // Coalesce rapid size changes (e.g. during tab switch when StackLayout
    // toggles visibility and Qt resizes the terminal in multiple steps).
    // Only the final size after the sequence settles triggers SIGWINCH.
    _resizeTimer = new QTimer(this);
    _resizeTimer->setSingleShot(true);
    _resizeTimer->setInterval(50);
    connect(_resizeTimer, &QTimer::timeout, this, &Session::updateTerminalSize);
}

void Session::_connectPtySignals()
{
    connect(_shellProcess, &PtyInterface::receivedData, this, &Session::onReceiveBlock);
    connect(_emulation, &Emulation::sendData, _shellProcess, &PtyInterface::sendData);
    // When Ctrl+C (0x03) is sent while waiting for a prompt (e.g. during
    // SSH password auth in "Open in New Pane"), kill the shell process so
    // the pane auto-closes. Views are hidden first to prevent a flash of
    // error output. _wantedClose ensures done() emits finished().
    connect(_emulation, &Emulation::sendData, this, [this](const char *data, int len) {
        if (_waitingForPrompt) {
            for (int i = 0; i < len; i++) {
                if (data[i] == '\x03') {
                    _waitingForPrompt = false;
                    _promptBuffer.clear();
                    _pendingReadyText.clear();
                    _scrollPendingCount = 0;
                    _wantedClose = true;
                    for (auto *view : _views)
                        view->setVisible(false);
                    _shellProcess->kill();
                    return;
                }
            }
        }
    });
    connect(_emulation, &Emulation::lockPtyRequest, _shellProcess, &PtyInterface::lockPty);
    connect(_emulation, &Emulation::useUtf8Request, _shellProcess, &PtyInterface::setUtf8Mode);
    connect(_shellProcess, &PtyInterface::finished, this, &Session::done);
}

WId Session::windowId() const
{
    // On Qt5, requesting window IDs breaks QQuickWidget and the likes,
    // for example, see the following bug reports:
    // https://bugreports.qt.io/browse/QTBUG-40765
    // https://codereview.qt-project.org/#/c/94880/
    return 0;
}

void Session::setDarkBackground(bool darkBackground)
{
    _hasDarkBackground = darkBackground;
}
bool Session::hasDarkBackground() const
{
    return _hasDarkBackground;
}
bool Session::isRunning() const
{
    return (_shellProcess != nullptr && _shellProcess->isRunning());
}

void Session::setProgram(const QString & program)
{
    _program = ShellCommand::expand(program);
}
void Session::setInitialWorkingDirectory(const QString & dir)
{
    _initialWorkingDir = ShellCommand::expand(dir);
}
void Session::setArguments(const QStringList & arguments)
{
    _arguments = ShellCommand::expand(arguments);
}

QList<TerminalDisplay *> Session::views() const
{
    return _views;
}

void Session::addView(TerminalDisplay * widget)
{
    Q_ASSERT( !_views.contains(widget) );

    _views.append(widget);

    if ( _emulation != nullptr ) {
        // connect emulation - view signals and slots
        connect( widget , &TerminalDisplay::keyPressedSignal, _emulation ,
                 &Emulation::sendKeyEvent);
        connect( widget , SIGNAL(mouseSignal(int,int,int,int)) , _emulation ,
                 SLOT(sendMouseEvent(int,int,int,int)) );
        connect( widget , SIGNAL(sendStringToEmu(const char *)) , _emulation ,
                 SLOT(sendString(const char *)) );

        // allow emulation to notify view when the foreground process
        // indicates whether or not it is interested in mouse signals
        connect( _emulation , SIGNAL(programUsesMouseChanged(bool)) , widget ,
                 SLOT(setUsesMouse(bool)) );

        widget->setUsesMouse( _emulation->programUsesMouse() );

        connect( _emulation , SIGNAL(programBracketedPasteModeChanged(bool)) ,
                 widget , SLOT(setBracketedPasteMode(bool)) );

        widget->setBracketedPasteMode(_emulation->programBracketedPasteMode());

        widget->setScreenWindow(_emulation->createWindow());
    }

    //connect view signals and slots
    QObject::connect( widget ,SIGNAL(changedContentSizeSignal(int,int)),this,
                      SLOT(onViewSizeChange(int,int)));

    QObject::connect( widget ,SIGNAL(destroyed(QObject *)) , this ,
                      SLOT(viewDestroyed(QObject *)) );
//slot for close
    //QObject::connect(this, SIGNAL(finished()), widget, SLOT(close()));

}

void Session::viewDestroyed(QObject * view)
{
    TerminalDisplay * display = (TerminalDisplay *)view;

    Q_ASSERT( _views.contains(display) );

    removeView(display);
}

void Session::removeView(TerminalDisplay * widget)
{
    _views.removeAll(widget);

    disconnect(widget,nullptr,this,nullptr);

    if ( _emulation != nullptr ) {
        // disconnect
        //  - key presses signals from widget
        //  - mouse activity signals from widget
        //  - string sending signals from widget
        //
        //  ... and any other signals connected in addView()
        disconnect( widget, nullptr, _emulation, nullptr);

        // disconnect state change signals emitted by emulation
        disconnect( _emulation , nullptr , widget , nullptr);
    }

    // For PersistentPty sessions, do NOT auto-close when views are removed.
    // During app shutdown, views are destroyed before sessions, and calling
    // close() here would send DESTROY, killing the daemon session.
    // For local Pty sessions, the session will be cleaned up by the destructor.
    if ( _views.count() == 0 && !_usePersistentPty ) {
        close();
    }
}

void Session::run()
{
    // Upon a KPty error, there is no description on what that error was...
    // Check to see if the given program is executable.

    /* ok I'm not exactly sure where _program comes from - however it was set to /bin/bash on my system
     * That's bad for BSD as its /usr/local/bin/bash there - its also bad for arch as its /usr/bin/bash there too!
     * So i added a check to see if /bin/bash exists - if no then we use $SHELL - if that does not exist either, we fall back to /bin/sh
     * As far as i know /bin/sh exists on every unix system.. You could also just put some ifdef __FREEBSD__ here but i think these 2 filechecks are worth
     * their computing time on any system - especially with the problem on arch linux being there too.
     */
    QString exec = QString::fromLocal8Bit(QFile::encodeName(_program));
    // if 'exec' is not specified, fall back to default shell.  if that
    // is not set then fall back to /bin/sh

    // here we expect full path. If there is no fullpath let's expect it's
    // a custom shell (eg. python, etc.) available in the PATH.
    if (exec.startsWith(QLatin1Char('/')) || exec.isEmpty())
    {
        const QString defaultShell{QLatin1String("/bin/sh")};

        QFile excheck(exec);
        if ( exec.isEmpty() || !excheck.exists() ) {
            exec = QString::fromLocal8Bit(qgetenv("SHELL"));
        }
        excheck.setFileName(exec);

        if ( exec.isEmpty() || !excheck.exists() ) {
            qWarning() << "Neither default shell nor $SHELL is set to a correct path. Fallback to" << defaultShell;
            exec = defaultShell;
        }
    }

    // _arguments sometimes contain ("") so isEmpty()
    // or count() does not work as expected...
    QString argsTmp(_arguments.join(QLatin1Char(' ')).trimmed());
    QStringList arguments;
    arguments << exec;
    if (argsTmp.length())
        arguments << _arguments;

    QString workDir = _initialWorkingDir.isEmpty() ? QDir::currentPath() : _initialWorkingDir;

    _shellProcess->setFlowControlEnabled(_flowControl);
    _shellProcess->setErase(_emulation->eraseChar());

    // this is not strictly accurate use of the COLORFGBG variable.  This does not
    // tell the terminal exactly which colors are being used, but instead approximates
    // the color scheme as "black on white" or "white on black" depending on whether
    // the background color is deemed dark or not
    QString backgroundColorHint = _hasDarkBackground ? QLatin1String("COLORFGBG=15;0") : QLatin1String("COLORFGBG=0;15");

    /* if we do all the checking if this shell exists then we use it ;)
     * Dont know about the arguments though.. maybe youll need some more checking im not sure
     * However this works on Arch and FreeBSD now.
     */
    int result = _shellProcess->start(exec,
                                      arguments,
                                      _environment << backgroundColorHint,
                                      workDir,
                                      windowId(),
                                      _addToUtmp);

    // Graceful degradation: if PersistentPty fails (daemon unreachable),
    // fall back to a local Pty so the terminal still works.
    if (result < 0 && dynamic_cast<PersistentPty *>(_shellProcess)) {
        qWarning() << "PersistentPty failed, falling back to local Pty";
        _shellProcess->disconnect(this);
        _emulation->disconnect(_shellProcess);
        delete _shellProcess;

        _shellProcess = new Pty();
        ptySlaveFd = _shellProcess->slaveFd();
        _shellProcess->setUtf8Mode(true);
        _connectPtySignals();

        _shellProcess->setFlowControlEnabled(_flowControl);
        _shellProcess->setErase(_emulation->eraseChar());

        result = _shellProcess->start(exec, arguments,
                                      _environment << backgroundColorHint,
                                      workDir, windowId(), _addToUtmp);
    }

    if (result < 0) {
        qDebug() << "CRASHED! result: " << result;
        return;
    }

    _shellProcess->setWriteable(false);  // We are reachable via kwrited.
    emit started();
}

void Session::runEmptyPTY()
{
    _shellProcess->setFlowControlEnabled(_flowControl);
    _shellProcess->setErase(_emulation->eraseChar());
    _shellProcess->setWriteable(false);

    // disconnect send data from emulator to internal terminal process
    disconnect( _emulation,SIGNAL(sendData(const char *,int)),
                _shellProcess, SLOT(sendData(const char *,int)) );

    _shellProcess->setEmptyPTYProperties();
    emit started();
}

void Session::setUserTitle( int what, const QString & caption )
{
    //set to true if anything is actually changed (eg. old _nameTitle != new _nameTitle )
    bool modified = false;

    // (btw: what=0 changes _userTitle and icon, what=1 only icon, what=2 only _nameTitle
    if ((what == 0) || (what == 2)) {
        _isTitleChanged = true;
        if ( _userTitle != caption ) {
            _userTitle = caption;
            modified = true;
        }
    }

    if ((what == 0) || (what == 1)) {
        _isTitleChanged = true;
        if ( _iconText != caption ) {
            _iconText = caption;
            modified = true;
        }
    }

    if (what == 11) {
        QString colorString = caption.section(QLatin1Char(';'),0,0);
        //qDebug() << __FILE__ << __LINE__ << ": setting background colour to " << colorString;
        QColor backColor = QColor(colorString);
        if (backColor.isValid()) { // change color via \033]11;Color\007
            if (backColor != _modifiedBackground) {
                _modifiedBackground = backColor;

                // bail out here until the code to connect the terminal display
                // to the changeBackgroundColor() signal has been written
                // and tested - just so we don't forget to do this.
                Q_ASSERT( 0 );

                emit changeBackgroundColorRequest(backColor);
            }
        }
    }

    if (what == 30) {
        _isTitleChanged = true;
        if ( _nameTitle != caption ) {
            setTitle(Session::NameRole,caption);
            return;
        }
    }

    if (what == 31) {
        QString cwd=caption;
        static const QRegularExpression homeRegExp{"^~"_L1};
        cwd = cwd.replace(homeRegExp, QDir::homePath());
        emit openUrlRequest(cwd);
    }

    // change icon via \033]32;Icon\007
    if (what == 32) {
        _isTitleChanged = true;
        if ( _iconName != caption ) {
            _iconName = caption;

            modified = true;
        }
    }

    if (what == 50) {
        emit profileChangeCommandReceived(caption);
        return;
    }

    if ( modified ) {
        emit titleChanged();
    }
}

QString Session::userTitle() const
{
    return _userTitle;
}
void Session::setTabTitleFormat(TabTitleContext context , const QString & format)
{
    if ( context == LocalTabTitle ) {
        _localTabTitleFormat = format;
    } else if ( context == RemoteTabTitle ) {
        _remoteTabTitleFormat = format;
    }
}
QString Session::tabTitleFormat(TabTitleContext context) const
{
    if ( context == LocalTabTitle ) {
        return _localTabTitleFormat;
    } else if ( context == RemoteTabTitle ) {
        return _remoteTabTitleFormat;
    }

    return QString();
}

void Session::monitorTimerDone()
{
    //FIXME: The idea here is that the notification popup will appear to tell the user than output from
    //the terminal has stopped and the popup will disappear when the user activates the session.
    //
    //This breaks with the addition of multiple views of a session.  The popup should disappear
    //when any of the views of the session becomes active


    //FIXME: Make message text for this notification and the activity notification more descriptive.
    if (_monitorSilence) {
        emit silence();
        emit stateChanged(NOTIFYSILENCE);
    } else {
        emit stateChanged(NOTIFYNORMAL);
    }

    _notifiedActivity=false;
}

void Session::activityStateSet(int state)
{
    if (state==NOTIFYBELL) {
        emit bellRequest(tr("Bell in session '%1'").arg(_nameTitle));
    } else if (state==NOTIFYACTIVITY) {
        if (_monitorSilence) {
            _monitorTimer->start(_silenceSeconds*1000);
        }

        if ( _monitorActivity ) {
            //FIXME:  See comments in Session::monitorTimerDone()
            if (!_notifiedActivity) {
                _notifiedActivity=true;
                emit activity();
            }
        }
    }

    if ( state==NOTIFYACTIVITY && !_monitorActivity ) {
        state = NOTIFYNORMAL;
    }
    if ( state==NOTIFYSILENCE && !_monitorSilence ) {
        state = NOTIFYNORMAL;
    }

    emit stateChanged(state);
}

void Session::onViewSizeChange(int /*height*/, int /*width*/)
{
    // Restart the coalesce timer so rapid consecutive size changes
    // (e.g. StackLayout visibility toggle) only produce one SIGWINCH.
    _resizeTimer->start();
}
void Session::onEmulationSizeChange(QSize size)
{
    setSize(size);
}

void Session::updateTerminalSize()
{
    QListIterator<TerminalDisplay *> viewIter(_views);

    int minLines = -1;
    int minColumns = -1;

    // minimum number of lines and columns that views require for
    // their size to be taken into consideration ( to avoid problems
    // with new view widgets which haven't yet been set to their correct size )
    const int VIEW_LINES_THRESHOLD = 2;
    const int VIEW_COLUMNS_THRESHOLD = 2;

    // Select largest number of lines and columns that will fit in all visible views.
    // Hidden views (e.g. background tabs) are skipped so they don't receive SIGWINCH
    // while offscreen — they get resized when they become visible via itemChange().
    while ( viewIter.hasNext() ) {
        TerminalDisplay * view = viewIter.next();
        if ( view->isVisible() &&
                view->lines() >= VIEW_LINES_THRESHOLD &&
                view->columns() >= VIEW_COLUMNS_THRESHOLD ) {
            minLines = (minLines == -1) ? view->lines() : qMin( minLines , view->lines() );
            minColumns = (minColumns == -1) ? view->columns() : qMin( minColumns , view->columns() );
        }
    }

    // backend emulation must have a _terminal of at least 1 column x 1 line in size
    if ( minLines > 0 && minColumns > 0 ) {
        _emulation->setImageSize( minLines , minColumns );
        _shellProcess->setWindowSize( minLines , minColumns );
    }
}

void Session::refresh()
{
    // attempt to get the shell process to redraw the display
    //
    // this requires the program running in the shell
    // to cooperate by sending an update in response to
    // a window size change
    //
    // the window size is changed twice, first made slightly larger and then
    // resized back to its normal size so that there is actually a change
    // in the window size (some shells do nothing if the
    // new and old sizes are the same)
    //
    // if there is a more 'correct' way to do this, please
    // send an email with method or patches to konsole-devel@kde.org

    const QSize existingSize = _shellProcess->windowSize();
    _shellProcess->setWindowSize(existingSize.height(),existingSize.width()+1);
    _shellProcess->setWindowSize(existingSize.height(),existingSize.width());
}

bool Session::sendSignal(int signal)
{
    return _shellProcess->sendSignal(signal);
}

void Session::close()
{
    _autoClose = true;
    _wantedClose = true;

    if (isRunning())
    {
        _shellProcess->requestClose();
    }
    else
    {
        // terminal process has finished, just close the session
        QTimer::singleShot(1, this, SIGNAL(finished()));
    }
}

void Session::sendText(const QString & text) const
{
    _emulation->sendText(text);
}

void Session::sendKeyEvent(QKeyEvent* e) const
{
    _emulation->sendKeyEvent(e, false);
}

Session::~Session()
{
    if (_shellProcess) {
        if (_usePersistentPty) {
            // PersistentPty: skip close() — destructor sends DETACH, allowing
            // daemon sessions to survive for reattachment.
            _shellProcess->disconnect();
        } else {
            // Regular Pty: must close() to terminate the process before
            // QProcess::~QProcess() runs (avoids signal-during-destruction crash).
            close();
        }
    }
    delete _emulation;
    delete _shellProcess;
}

void Session::setProfileKey(const QString & key)
{
    _profileKey = key;
    emit profileChanged(key);
}
QString Session::profileKey() const
{
    return _profileKey;
}

void Session::done(int exitCode, PtyExitStatus exitStatus)
{
    if (!_autoClose) {
        _userTitle = QString::fromLatin1("This session is done. Finished");
        emit titleChanged();
        return;
    }

    // message is not being used. But in the original kpty.cpp file
    // (https://cgit.kde.org/kpty.git/) it's part of a notification.
    // So, we make it translatable, hoping that in the future it will
    // be used in some kind of notification.
    QString message;
    if (!_wantedClose || exitCode != 0) {

        if (exitStatus == PtyExitStatus::NormalExit) {
            message = tr("Session '%1' exited with code %2.").arg(_nameTitle).arg(exitCode);
        } else {
            message = tr("Session '%1' crashed.").arg(_nameTitle);
        }
    }

    if ( !_wantedClose && exitStatus != PtyExitStatus::NormalExit )
        message = tr("Session '%1' exited unexpectedly.").arg(_nameTitle);
    else
        emit finished();
}

Emulation * Session::emulation() const
{
    return _emulation;
}

QString Session::keyBindings() const
{
    return _emulation->keyBindings();
}

QStringList Session::environment() const
{
    return _environment;
}

void Session::setEnvironment(const QStringList & environment)
{
    _environment = environment;
}

int Session::sessionId() const
{
    return _sessionId;
}

void Session::setKeyBindings(const QString & id)
{
    _emulation->setKeyBindings(id);
}

void Session::setTitle(TitleRole role , const QString & newTitle)
{
    if ( title(role) != newTitle ) {
        if ( role == NameRole ) {
            _nameTitle = newTitle;
        } else if ( role == DisplayedTitleRole ) {
            _displayTitle = newTitle;
        }

        emit titleChanged();
    }
}

QString Session::title(TitleRole role) const
{
    if ( role == NameRole ) {
        return _nameTitle;
    } else if ( role == DisplayedTitleRole ) {
        return _displayTitle;
    } else {
        return QString();
    }
}

void Session::setIconName(const QString & iconName)
{
    if ( iconName != _iconName ) {
        _iconName = iconName;
        emit titleChanged();
    }
}

void Session::setIconText(const QString & iconText)
{
    _iconText = iconText;
    //kDebug(1211)<<"Session setIconText " <<  _iconText;
}

QString Session::iconName() const
{
    return _iconName;
}

QString Session::iconText() const
{
    return _iconText;
}

bool Session::isTitleChanged() const
{
    return _isTitleChanged;
}

void Session::setHistoryType(const HistoryType & hType)
{
    _emulation->setHistory(hType);
}

const HistoryType & Session::historyType() const
{
    return _emulation->history();
}

void Session::clearHistory()
{
    _emulation->clearHistory();
}

QStringList Session::arguments() const
{
    return _arguments;
}

QString Session::program() const
{
    return _program;
}

// unused currently
bool Session::isMonitorActivity() const
{
    return _monitorActivity;
}
// unused currently
bool Session::isMonitorSilence()  const
{
    return _monitorSilence;
}

void Session::setMonitorActivity(bool _monitor)
{
    _monitorActivity=_monitor;
    _notifiedActivity=false;

    activityStateSet(NOTIFYNORMAL);
}

void Session::setMonitorSilence(bool _monitor)
{
    if (_monitorSilence==_monitor) {
        return;
    }

    _monitorSilence=_monitor;
    if (_monitorSilence) {
        _monitorTimer->start(_silenceSeconds*1000);
    } else {
        _monitorTimer->stop();
    }

    activityStateSet(NOTIFYNORMAL);
}

void Session::setMonitorSilenceSeconds(int seconds)
{
    _silenceSeconds=seconds;
    if (_monitorSilence) {
        _monitorTimer->start(_silenceSeconds*1000);
    }
}

void Session::setAddToUtmp(bool set)
{
    _addToUtmp = set;
}

void Session::setFlowControlEnabled(bool enabled)
{
    if (_flowControl == enabled) {
        return;
    }

    _flowControl = enabled;

    if (_shellProcess) {
        _shellProcess->setFlowControlEnabled(_flowControl);
    }

    emit flowControlEnabledChanged(enabled);
}
bool Session::flowControlEnabled() const
{
    return _flowControl;
}

void Session::onReceiveBlock( const char * buf, int len )
{
    _checkForPrompt(buf, len);
    _emulation->receiveData( buf, len );
    emit receivedData( QString::fromLatin1( buf, len ) );

    // After sending deferred text (e.g. cd command after SSH login), scroll
    // to bottom for several receive cycles so the response is visible.
    if (_scrollPendingCount > 0) {
        _scrollPendingCount--;
        for (auto *view : _views)
            view->scrollToEnd();
    }
}

void Session::sendTextOnceReady(const QString &text, const QString &promptChars)
{
    _pendingReadyText = text;

    // Parse comma-separated prompt tokens (e.g. "$, #, %, >, ]").
    // Each entry is trimmed, so "$,#,%,>" and "$, #, %, >" are equivalent.
    // Supports multi-character tokens (e.g. "→, ❯, myhost)").
    _promptTokens.clear();
    QString src = promptChars.isEmpty() ? QStringLiteral("$, #, %, >") : promptChars;
    const auto parts = src.split(QLatin1Char(','));
    for (const auto &part : parts) {
        QByteArray token = part.trimmed().toLatin1();
        if (!token.isEmpty())
            _promptTokens.append(token);
    }

    _waitingForPrompt = true;
    _promptBuffer.clear();
}

// Scans incoming PTY data for shell prompt tokens (comma-separated, configurable
// via sendTextOnceReady's promptChars parameter, default "$, #, %, >"). Checks
// if the trimmed line starts or ends with any token to handle various prompt
// styles. When a prompt is detected, sends the queued text and arms scroll.
void Session::_checkForPrompt(const char *buf, int len)
{
    if (!_waitingForPrompt) return;

    // Append new data, keeping only the last line
    _promptBuffer.append(buf, len);
    int lastNl = _promptBuffer.lastIndexOf('\n');
    if (lastNl >= 0)
        _promptBuffer = _promptBuffer.mid(lastNl + 1);

    // Strip ANSI escape sequences: ESC [ ... final_byte
    QByteArray clean;
    clean.reserve(_promptBuffer.size());
    int i = 0;
    while (i < _promptBuffer.size()) {
        char c = _promptBuffer.at(i);
        if (c == '\033') {
            i++;
            if (i < _promptBuffer.size() && _promptBuffer.at(i) == '[') {
                i++;
                while (i < _promptBuffer.size()) {
                    char p = _promptBuffer.at(i);
                    if (p >= 0x40 && p <= 0x7E) { i++; break; }
                    i++;
                }
            }
        } else if (c == '\r') {
            // Skip carriage returns
            i++;
        } else {
            clean.append(c);
            i++;
        }
    }

    // Trim trailing whitespace
    QByteArray trimmed = clean.trimmed();
    if (trimmed.isEmpty()) return;

    // Check if the cleaned line starts or ends with any configured prompt token
    bool matched = false;
    for (const auto &token : _promptTokens) {
        if (trimmed.startsWith(token) || trimmed.endsWith(token)) {
            matched = true;
            break;
        }
    }
    if (matched) {
        _waitingForPrompt = false;
        _promptBuffer.clear();
        sendText(_pendingReadyText);
        _pendingReadyText.clear();
        _scrollPendingCount = 5;
    }
}

QSize Session::size()
{
    return _emulation->imageSize();
}

void Session::setSize(const QSize & size)
{
    if ((size.width() <= 1) || (size.height() <= 1)) {
        return;
    }

    emit resizeRequest(size);
}
int Session::foregroundProcessId() const
{
    return _shellProcess->foregroundProcessGroup();
}

QString Session::foregroundProcessName()
{
    QString name;

    if (updateForegroundProcessInfo()) {
        bool ok = false;
        name = _foregroundProcessInfo->name(&ok);
        if (!ok)
            name.clear();
    }

    return name;
}

// Returns a display label for the foreground process.
// For SSH, parses command-line arguments to return "user@host".
// For other processes, returns the plain process name.
QString Session::foregroundProcessLabel()
{
    if (updateForegroundProcessInfo()) {
        bool ok = false;
        QString name = _foregroundProcessInfo->name(&ok);
        if (ok && name == QLatin1String("ssh")) {
            SSHProcessInfo sshInfo(*_foregroundProcessInfo);
            QString host = sshInfo.host();
            if (!host.isEmpty()) {
                QString user = sshInfo.userName();
                if (!user.isEmpty())
                    return user + QLatin1Char('@') + host;
                return host;
            }
        }
        if (ok)
            return name;
    }
    return QString();
}

QString Session::currentDir()
{
    QString path;
    if (updateForegroundProcessInfo()) {
        bool ok = false;
        path= _foregroundProcessInfo->currentDir(&ok);
        if (!ok)
            path.clear();
    }
    return path;
}

QVariantMap Session::sshConnectionInfo()
{
    QVariantMap result;
    if (updateForegroundProcessInfo()) {
        bool ok = false;
        QString name = _foregroundProcessInfo->name(&ok);
        if (ok && name == QLatin1String("ssh")) {
            SSHProcessInfo sshInfo(*_foregroundProcessInfo);
            result["host"] = sshInfo.host();
            result["user"] = sshInfo.userName();
            result["port"] = sshInfo.port();
        }
    }
    return result;
}

bool Session::updateForegroundProcessInfo()
{
    Q_ASSERT(_shellProcess);

    const int foregroundPid = _shellProcess->foregroundProcessGroup();
    if (foregroundPid != _foregroundPid) {
        delete _foregroundProcessInfo;
        _foregroundProcessInfo = ProcessInfo::newInstance(foregroundPid);
        _foregroundPid = foregroundPid;
    }

    if (_foregroundProcessInfo) {
        _foregroundProcessInfo->update();
        return _foregroundProcessInfo->isValid();
    } else {
        return false;
    }
}


int Session::processId() const
{
    return static_cast<int>(_shellProcess->processId());
}
int Session::getPtySlaveFd() const
{
    return ptySlaveFd;
}

void Session::setUsePersistentPty(bool persistent)
{
    if (_usePersistentPty == persistent)
        return;
    if (isRunning())
        return;

    _usePersistentPty = persistent;

    // Disconnect old Pty signals and destroy it
    _shellProcess->disconnect(this);
    _emulation->disconnect(_shellProcess);
    delete _shellProcess;

    // Create new Pty of the requested type
    if (persistent) {
        _shellProcess = new PersistentPty(this);
    } else {
        _shellProcess = new Pty();
    }
    ptySlaveFd = _shellProcess->slaveFd();
    _shellProcess->setUtf8Mode(true);
    _connectPtySignals();
}

QString Session::daemonSessionId() const
{
    auto *ppty = dynamic_cast<PersistentPty *>(_shellProcess);
    if (ppty)
        return QString::fromLatin1(ppty->sessionId());
    return QString();
}

int Session::attachToSession(const QString &sessionId)
{
    auto *ppty = dynamic_cast<PersistentPty *>(_shellProcess);
    if (!ppty)
        return -1;

    int result = ppty->attachToSession(sessionId.toLatin1());
    if (result < 0)
        return result;

    // Sync terminal size to match current emulation dimensions
    if (_emulation) {
        QSize sz = ppty->windowSize();
        _emulation->setImageSize(sz.height(), sz.width());
    }

    emit started();
    return 0;
}

SessionGroup::SessionGroup()
        : _masterMode(0)
{
}
SessionGroup::~SessionGroup()
{
    // disconnect all
    connectAll(false);
}
int SessionGroup::masterMode() const
{
    return _masterMode;
}
QList<Session *> SessionGroup::sessions() const
{
    return _sessions.keys();
}
bool SessionGroup::masterStatus(Session * session) const
{
    return _sessions[session];
}

void SessionGroup::addSession(Session * session)
{
    _sessions.insert(session,false);

    QListIterator<Session *> masterIter(masters());

    while ( masterIter.hasNext() ) {
        connectPair(masterIter.next(),session);
    }
}
void SessionGroup::removeSession(Session * session)
{
    setMasterStatus(session,false);

    QListIterator<Session *> masterIter(masters());

    while ( masterIter.hasNext() ) {
        disconnectPair(masterIter.next(),session);
    }

    _sessions.remove(session);
}
void SessionGroup::setMasterMode(int mode)
{
    _masterMode = mode;

    connectAll(false);
    connectAll(true);
}
QList<Session *> SessionGroup::masters() const
{
    return _sessions.keys(true);
}
void SessionGroup::connectAll(bool connect)
{
    QListIterator<Session *> masterIter(masters());

    while ( masterIter.hasNext() ) {
        Session * master = masterIter.next();

        QListIterator<Session *> otherIter(_sessions.keys());
        while ( otherIter.hasNext() ) {
            Session * other = otherIter.next();

            if ( other != master ) {
                if ( connect ) {
                    connectPair(master,other);
                } else {
                    disconnectPair(master,other);
                }
            }
        }
    }
}
void SessionGroup::setMasterStatus(Session * session, bool master)
{
    bool wasMaster = _sessions[session];
    _sessions[session] = master;

    if (wasMaster == master) {
        return;
    }

    QListIterator<Session *> iter(_sessions.keys());
    while (iter.hasNext()) {
        Session * other = iter.next();

        if (other != session) {
            if (master) {
                connectPair(session, other);
            } else {
                disconnectPair(session, other);
            }
        }
    }
}

void SessionGroup::connectPair(Session * master , Session * other) const
{
//    qDebug() << k_funcinfo;

    if ( _masterMode & CopyInputToAll ) {
        qDebug() << "Connection session " << master->nameTitle() << "to" << other->nameTitle();

        connect( master->emulation() , SIGNAL(sendData(const char *,int)) , other->emulation() ,
                 SLOT(sendString(const char *,int)) );
    }
}
void SessionGroup::disconnectPair(Session * master , Session * other) const
{
//    qDebug() << k_funcinfo;

    if ( _masterMode & CopyInputToAll ) {
        qDebug() << "Disconnecting session " << master->nameTitle() << "from" << other->nameTitle();

        disconnect( master->emulation() , SIGNAL(sendData(const char *,int)) , other->emulation() ,
                    SLOT(sendString(const char *,int)) );
    }
}

//#include "moc_Session.cpp"
