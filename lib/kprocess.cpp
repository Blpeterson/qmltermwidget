/*
 * This file is a part of QTerminal - http://gitorious.org/qterminal
 *
 * This file was un-linked from KDE and modified
 * by Maxim Bourmistrov <maxim@unixconn.com>
 *
 */

/*
    This file is part of the KDE libraries

    Copyright (C) 2007 Oswald Buddenhagen <ossi@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#include "kprocess.h"

#include <qfile.h>

/////////////////////////////
// public member functions //
/////////////////////////////

KProcess::KProcess(QObject *parent) :
    QProcess(parent),
    d_ptr(new KProcessPrivate(this))
{
    setOutputChannelMode(ForwardedChannels);
}

KProcess::KProcess(KProcessPrivate *d, QObject *parent) :
    QProcess(parent),
    d_ptr(d)
{
    d_ptr->q_ptr = this;
    setOutputChannelMode(ForwardedChannels);
}

KProcess::~KProcess() = default;

void KProcess::setOutputChannelMode(OutputChannelMode mode)
{
    QProcess::setProcessChannelMode(static_cast<ProcessChannelMode>(mode));
}

KProcess::OutputChannelMode KProcess::outputChannelMode() const
{
    return static_cast<OutputChannelMode>(QProcess::processChannelMode());
}

void KProcess::setNextOpenMode(QIODevice::OpenMode mode)
{
    Q_D(KProcess);

    d->openMode = mode;
}

#define DUMMYENV "_KPROCESS_DUMMY_="

void KProcess::clearEnvironment()
{
    setEnvironment(QStringList{QString::fromLatin1(DUMMYENV)});
}

void KProcess::setEnv(const QString &name, const QString &value, bool overwrite)
{
    QStringList env = environment();
    if (env.isEmpty()) {
        env = systemEnvironment();
        env.removeAll(QString::fromLatin1(DUMMYENV));
    }
    QString fname(name);
    fname.append(QLatin1Char('='));
    auto it = std::find_if(env.begin(), env.end(), [&fname](const QString &s) {
        return s.startsWith(fname);
    });
    if (it != env.end()) {
        if (overwrite) {
            *it = fname.append(value);
            setEnvironment(env);
        }
        return;
    }
    env.append(fname.append(value));
    setEnvironment(env);
}

void KProcess::unsetEnv(const QString &name)
{
    QStringList env = environment();
    if (env.isEmpty()) {
        env = systemEnvironment();
        env.removeAll(QString::fromLatin1(DUMMYENV));
    }
    QString fname(name);
    fname.append(QLatin1Char('='));
    auto it = std::find_if(env.begin(), env.end(), [&fname](const QString &s) {
        return s.startsWith(fname);
    });
    if (it != env.end()) {
        env.erase(it);
        if (env.isEmpty()) {
            env.append(QString::fromLatin1(DUMMYENV));
        }
        setEnvironment(env);
    }
}

void KProcess::setProgram(const QString &exe, const QStringList &args)
{
    Q_D(KProcess);

    d->prog = exe;
    d->args = args;
}

void KProcess::setProgram(const QStringList &argv)
{
    Q_D(KProcess);

    Q_ASSERT( !argv.isEmpty() );
    d->args = argv;
    d->prog = d->args.takeFirst();
}

KProcess &KProcess::operator<<(const QString &arg)
{
    Q_D(KProcess);

    if (d->prog.isEmpty())
        d->prog = arg;
    else
        d->args << arg;
    return *this;
}

KProcess &KProcess::operator<<(const QStringList &args)
{
    Q_D(KProcess);

    if (d->prog.isEmpty())
        setProgram(args);
    else
        d->args << args;
    return *this;
}

void KProcess::clearProgram()
{
    Q_D(KProcess);

    d->prog.clear();
    d->args.clear();
}

QStringList KProcess::program() const
{
    Q_D(const KProcess);

    QStringList argv = d->args;
    argv.prepend(d->prog);
    return argv;
}

void KProcess::start()
{
    Q_D(KProcess);

    QProcess::start(d->prog, d->args, d->openMode);
}

int KProcess::execute(int msecs)
{
    start();
    if (!waitForFinished(msecs)) {
        kill();
        waitForFinished(-1);
        return -2;
    }
    return (exitStatus() == QProcess::NormalExit) ? exitCode() : -1;
}

// static
int KProcess::execute(const QString &exe, const QStringList &args, int msecs)
{
    KProcess p;
    p.setProgram(exe, args);
    return p.execute(msecs);
}

// static
int KProcess::execute(const QStringList &argv, int msecs)
{
    KProcess p;
    p.setProgram(argv);
    return p.execute(msecs);
}

int KProcess::startDetached()
{
    Q_D(KProcess);

    qint64 pid;
    if (!QProcess::startDetached(d->prog, d->args, workingDirectory(), &pid))
        return 0;
    return static_cast<int>(pid);
}

// static
int KProcess::startDetached(const QString &exe, const QStringList &args)
{
    qint64 pid;
    if (!QProcess::startDetached(exe, args, QString(), &pid))
        return 0;
    return static_cast<int>(pid);
}

// static
int KProcess::startDetached(const QStringList &argv)
{
    QStringList args = argv;
    QString prog = args.takeFirst();
    return startDetached(prog, args);
}
