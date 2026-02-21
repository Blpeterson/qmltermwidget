/*
    Copyright 2007-2008 by Robert Knight <robertknight@gmail.countm>

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
#include "ProcessInfo.h"

// Unix
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/param.h>

// Qt
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QFlags>
#include <QtCore/QTextStream>
#include <QtCore/QStringList>
#include <QtNetwork/QHostInfo>

// KDE
// #include <KConfigGroup>
// #include <KSharedConfig>
#include <QDebug>

#include <sys/sysctl.h>
#include <libproc.h>

using namespace Konsole;

ProcessInfo::ProcessInfo(int aPid , bool enableEnvironmentRead)
    : _fields(ARGUMENTS | ENVIRONMENT)   // arguments and environments
    // are currently always valid,
    // they just return an empty
    // vector / map respectively
    // if no arguments
    // or environment bindings
    // have been explicitly set
    , _enableEnvironmentRead(enableEnvironmentRead)
    , _pid(aPid)
    , _parentPid(0)
    , _foregroundPid(0)
    , _userId(0)
    , _lastError(NoError)
    , _userName(QString())
    , _userHomeDir(QString())
{
}

ProcessInfo::Error ProcessInfo::error() const
{
    return _lastError;
}
void ProcessInfo::setError(Error error)
{
    _lastError = error;
}

void ProcessInfo::update()
{
    readProcessInfo(_pid, _enableEnvironmentRead);
}

QString ProcessInfo::validCurrentDir() const
{
    bool ok = false;

    // read current dir, if an error occurs try the parent as the next
    // best option
    int currentPid = parentPid(&ok);
    QString dir = currentDir(&ok);
    while (!ok && currentPid != 0) {
        ProcessInfo* current = ProcessInfo::newInstance(currentPid);
        current->update();
        currentPid = current->parentPid(&ok);
        dir = current->currentDir(&ok);
        delete current;
    }

    return dir;
}

QString ProcessInfo::format(const QString& input) const
{
    bool ok = false;

    QString output(input);

    // search for and replace known marker
    output.replace("%u", userName());
    output.replace("%h", localHost());
    output.replace("%n", name(&ok));

    QString dir = validCurrentDir();
    if (output.contains("%D")) {
        QString homeDir = userHomeDir();
        QString tempDir = dir;
        // Change User's Home Dir w/ ~ only at the beginning
        if (tempDir.startsWith(homeDir)) {
            tempDir.remove(0, homeDir.length());
            tempDir.prepend('~');
        }
        output.replace("%D", tempDir);
    }
    output.replace("%d", formatShortDir(dir));

    return output;
}

QSet<QString> ProcessInfo::_commonDirNames;

QSet<QString> ProcessInfo::commonDirNames()
{
    // static bool forTheFirstTime = true;
    //
    // if (forTheFirstTime) {
    //     const KSharedConfigPtr& config = KSharedConfig::openConfig();
    //     const KConfigGroup& configGroup = config->group("ProcessInfo");
    //     _commonDirNames = QSet<QString>::fromList(configGroup.readEntry("CommonDirNames", QStringList()));
    //
    //     forTheFirstTime = false;
    // }

    return _commonDirNames;
}

QString ProcessInfo::formatShortDir(const QString& input) const
{
    QString result;

    const QStringList& parts = input.split(QDir::separator());

    QSet<QString> dirNamesToShorten = commonDirNames();

    QListIterator<QString> iter(parts);
    iter.toBack();

    // go backwards through the list of the path's parts
    // adding abbreviations of common directory names
    // and stopping when we reach a dir name which is not
    // in the commonDirNames set
    while (iter.hasPrevious()) {
        const QString& part = iter.previous();

        if (dirNamesToShorten.contains(part)) {
            result.prepend(QString(QDir::separator()) + part[0]);
        } else {
            result.prepend(part);
            break;
        }
    }

    return result;
}

QVector<QString> ProcessInfo::arguments(bool* ok) const
{
    *ok = _fields.testFlag(ARGUMENTS);

    return _arguments;
}

QMap<QString, QString> ProcessInfo::environment(bool* ok) const
{
    *ok = _fields.testFlag(ENVIRONMENT);

    return _environment;
}

bool ProcessInfo::isValid() const
{
    return _fields.testFlag(PROCESS_ID);
}

int ProcessInfo::pid(bool* ok) const
{
    *ok = _fields.testFlag(PROCESS_ID);

    return _pid;
}

int ProcessInfo::parentPid(bool* ok) const
{
    *ok = _fields.testFlag(PARENT_PID);

    return _parentPid;
}

int ProcessInfo::foregroundPid(bool* ok) const
{
    *ok = _fields.testFlag(FOREGROUND_PID);

    return _foregroundPid;
}

QString ProcessInfo::name(bool* ok) const
{
    *ok = _fields.testFlag(NAME);

    return _name;
}

int ProcessInfo::userId(bool* ok) const
{
    *ok = _fields.testFlag(UID);

    return _userId;
}

QString ProcessInfo::userName() const
{
    return _userName;
}

QString ProcessInfo::userHomeDir() const
{
    return _userHomeDir;
}

QString ProcessInfo::localHost()
{
    return QHostInfo::localHostName();
}

void ProcessInfo::setPid(int aPid)
{
    _pid = aPid;
    _fields |= PROCESS_ID;
}

void ProcessInfo::setUserId(int uid)
{
    _userId = uid;
    _fields |= UID;
}

void ProcessInfo::setUserName(const QString& name)
{
    _userName = name;
    setUserHomeDir();
}

void ProcessInfo::setUserHomeDir()
{
    _userHomeDir = QDir::homePath();
}

void ProcessInfo::setParentPid(int aPid)
{
    _parentPid = aPid;
    _fields |= PARENT_PID;
}
void ProcessInfo::setForegroundPid(int aPid)
{
    _foregroundPid = aPid;
    _fields |= FOREGROUND_PID;
}

QString ProcessInfo::currentDir(bool* ok) const
{
    if (ok)
        *ok = _fields & CURRENT_DIR;

    return _currentDir;
}
void ProcessInfo::setCurrentDir(const QString& dir)
{
    _fields |= CURRENT_DIR;
    _currentDir = dir;
}

void ProcessInfo::setName(const QString& name)
{
    _name = name;
    _fields |= NAME;
}
void ProcessInfo::addArgument(const QString& argument)
{
    _arguments << argument;
}

void ProcessInfo::clearArguments()
{
    _arguments.clear();
}

void ProcessInfo::addEnvironmentBinding(const QString& name , const QString& value)
{
    _environment.insert(name, value);
}

void ProcessInfo::setFileError(QFile::FileError error)
{
    switch (error) {
    case QFile::PermissionsError:
        setError(ProcessInfo::PermissionsError);
        break;
    case QFile::NoError:
        setError(ProcessInfo::NoError);
        break;
    default:
        setError(ProcessInfo::UnknownError);
    }
}

//
// ProcessInfo::newInstance() is way at the bottom so it can see all of the
// implementations of the UnixProcessInfo abstract class.
//

UnixProcessInfo::UnixProcessInfo(int aPid, bool enableEnvironmentRead)
    : ProcessInfo(aPid, enableEnvironmentRead)
{
}

bool UnixProcessInfo::readProcessInfo(int aPid , bool enableEnvironmentRead)
{
    // prevent _arguments from growing longer and longer each time this
    // method is called.
    clearArguments();

    bool ok = readProcInfo(aPid);
    if (ok) {
        ok |= readArguments(aPid);
        ok |= readCurrentDir(aPid);
        if (enableEnvironmentRead) {
            ok |= readEnvironment(aPid);
        }
        // Some runtimes (Bun, Node) override the process name via
        // prctl/PR_SET_NAME so proc_name() returns e.g. a version string
        // instead of the actual executable name. Fall back to the basename
        // of argv[0] which is always the real binary name.
        bool argsOk = false;
        const auto& args = arguments(&argsOk);
        if (argsOk && !args.isEmpty()) {
            QString argv0 = QFileInfo(args.at(0)).fileName();
            if (!argv0.isEmpty())
                setName(argv0);
        }
    }
    return ok;
}

void UnixProcessInfo::readUserName()
{
    bool ok = false;
    const int uid = userId(&ok);
    if (!ok) return;

    struct passwd passwdStruct;
    struct passwd* getpwResult;
    char* getpwBuffer;
    long getpwBufferSize;
    int getpwStatus;

    getpwBufferSize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (getpwBufferSize == -1)
        getpwBufferSize = 16384;

    getpwBuffer = new char[getpwBufferSize];
    if (getpwBuffer == NULL)
        return;
    getpwStatus = getpwuid_r(uid, &passwdStruct, getpwBuffer, getpwBufferSize, &getpwResult);
    if ((getpwStatus == 0) && (getpwResult != NULL)) {
        setUserName(QString(passwdStruct.pw_name));
    } else {
        setUserName(QString());
        qWarning() << "getpwuid_r returned error : " << getpwStatus;
    }
    delete [] getpwBuffer;
}

class MacProcessInfo : public UnixProcessInfo
{
public:
    MacProcessInfo(int aPid, bool env) :
        UnixProcessInfo(aPid, env) {
    }

private:
    virtual bool readProcInfo(int aPid) {
        char name[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_name(aPid, name, sizeof(name)) > 0) {
            setName(QString(name));
        }
        setPid(aPid);
        return true;
    }

    // Read command-line arguments via sysctl KERN_PROCARGS2.
    // The returned buffer contains: argc (int), exec path, null padding, then argv strings.
    virtual bool readArguments(int aPid) {
        int mib[3] = { CTL_KERN, KERN_PROCARGS2, aPid };
        size_t size = 0;
        if (sysctl(mib, 3, NULL, &size, NULL, 0) == -1)
            return false;

        QByteArray buf(size, '\0');
        if (sysctl(mib, 3, buf.data(), &size, NULL, 0) == -1)
            return false;

        if (size < sizeof(int))
            return false;

        int argc;
        memcpy(&argc, buf.constData(), sizeof(int));

        const char *p = buf.constData() + sizeof(int);
        const char *end = buf.constData() + size;

        // Skip exec path
        while (p < end && *p != '\0') p++;
        // Skip null padding
        while (p < end && *p == '\0') p++;

        // Read argc arguments
        for (int i = 0; i < argc && p < end; i++) {
            addArgument(QString::fromUtf8(p));
            p += qstrlen(p) + 1;
        }
        return argc > 0;
    }
    virtual bool readCurrentDir(int aPid) {
        struct proc_vnodepathinfo vpi;
        const int nb = proc_pidinfo(aPid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi));
        if (nb == sizeof(vpi)) {
            setCurrentDir(QString(vpi.pvi_cdir.vip_path));
            return true;
        }
        return false;
    }
    virtual bool readEnvironment(int aPid) {
        Q_UNUSED(aPid);
        return false;
    }
};

SSHProcessInfo::SSHProcessInfo(const ProcessInfo& process)
    : _process(process)
{
    bool ok = false;

    // check that this is a SSH process
    const QString& name = _process.name(&ok);

    if (!ok || name != "ssh") {
        if (!ok)
            qWarning() << "Could not read process info";
        else
            qWarning() << "Process is not a SSH process";

        return;
    }

    // read arguments
    const QVector<QString>& args = _process.arguments(&ok);

    // SSH options
    // these are taken from the SSH manual ( accessed via 'man ssh' )

    // options which take no arguments
    static const QString noArgumentOptions("1246AaCfgKkMNnqsTtVvXxYy");
    // options which take one argument
    static const QString singleArgumentOptions("bcDeFIiJLlmOopRSWw");

    if (ok) {
        // find the username, host and command arguments
        //
        // the username/host is assumed to be the first argument
        // which is not an option
        // ( ie. does not start with a dash '-' character )
        // or an argument to a previous option.
        //
        // the command, if specified, is assumed to be the argument following
        // the username and host
        //
        // note that we skip the argument at index 0 because that is the
        // program name ( expected to be 'ssh' in this case )
        for (int i = 1 ; i < args.count() ; i++) {
            // If this one is an option ...
            // Most options together with its argument will be skipped.
            if (args[i].startsWith('-')) {
                const QChar optionChar = (args[i].length() > 1) ? args[i][1] : '\0';
                // for example: -p2222 or -p 2222 ?
                const bool optionArgumentCombined =  args[i].length() > 2;

                if (noArgumentOptions.contains(optionChar)) {
                    continue;
                } else if (singleArgumentOptions.contains(optionChar)) {
                    QString argument;
                    if (optionArgumentCombined) {
                        argument = args[i].mid(2);
                    } else {
                        // Verify correct # arguments are given
                        if ((i + 1) < args.count()) {
                            argument = args[i + 1];
                        }
                        i++;
                    }

                    // support using `-l user` to specify username.
                    if (optionChar == 'l')
                        _user = argument;
                    // support using `-p port` to specify port.
                    else if (optionChar == 'p')
                        _port = argument;

                    continue;
                }
            }

            // check whether the host has been found yet
            // if not, this must be the username/host argument
            if (_host.isEmpty()) {
                // check to see if only a hostname is specified, or whether
                // both a username and host are specified ( in which case they
                // are separated by an '@' character:  username@host )

                const int separatorPosition = args[i].indexOf('@');
                if (separatorPosition != -1) {
                    // username and host specified
                    _user = args[i].left(separatorPosition);
                    _host = args[i].mid(separatorPosition + 1);
                } else {
                    // just the host specified
                    _host = args[i];
                }
            } else {
                // host has already been found, this must be the command argument
                _command = args[i];
            }
        }
    } else {
        qWarning() << "Could not read arguments";

        return;
    }
}

QString SSHProcessInfo::userName() const
{
    return _user;
}
QString SSHProcessInfo::host() const
{
    return _host;
}
QString SSHProcessInfo::port() const
{
    return _port;
}
QString SSHProcessInfo::command() const
{
    return _command;
}
QString SSHProcessInfo::format(const QString& input) const
{
    QString output(input);

    // test whether host is an ip address
    // in which case 'short host' and 'full host'
    // markers in the input string are replaced with
    // the full address
    bool isIpAddress = false;

    struct in_addr address;
    if (inet_aton(_host.toLocal8Bit().constData(), &address) != 0)
        isIpAddress = true;
    else
        isIpAddress = false;

    // search for and replace known markers
    output.replace("%u", _user);

    if (isIpAddress)
        output.replace("%h", _host);
    else
        output.replace("%h", _host.left(_host.indexOf('.')));

    output.replace("%H", _host);
    output.replace("%c", _command);

    return output;
}

ProcessInfo* ProcessInfo::newInstance(int aPid, bool enableEnvironmentRead)
{
    return new MacProcessInfo(aPid, enableEnvironmentRead);
}
