/****************************************
 *
 *   INSERT-PROJECT-NAME-HERE - INSERT-GENERIC-NAME-HERE
 *   Copyright (C) 2020 Victor Tran
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * *************************************/
#include "mountstate.h"

#include <driveobjectmanager.h>
#include <DriveObjects/diskobject.h>
#include <DriveObjects/blockinterface.h>
#include "installerdata.h"

struct MountStatePrivate {
    QTemporaryDir mountDir;
};

MountState::MountState(QState* parent) : QStateMachine(parent) {
    d = new MountStatePrivate();
    d->mountDir.setAutoRemove(false);
}

MountState::~MountState() {
    delete d;
}

void MountState::onEntry(QEvent* event) {
    //Add states
    QList<QPair<QString, QString>> mounts = InstallerData::valueTemp("mounts").value<QList<QPair<QString, QString>>>();
    InstallerData::insertTemp("systemRoot", d->mountDir.path());

    QState* previousState = nullptr;
    for (QPair<QString, QString> mount : mounts) {
        QState* state = new QState();
        connect(state, &QState::entered, this, [ = ] {
            DiskObject* mountDrive = DriveObjectManager::diskForPath(QDBusObjectPath(mount.second));
            QString mountPath = d->mountDir.path() + "/" + mount.first;

            QDir::root().mkpath(mountPath);

            //Mount the drive
            QProcess* proc = new QProcess();
            proc->setProcessChannelMode(QProcess::ForwardedChannels);
            proc->start("mount", {mountDrive->interface<BlockInterface>()->blockName(), mountPath});
            connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [ = ](int exitCode, QProcess::ExitStatus exitStatus) {
                if (exitCode == 0) {
                    emit mountNext();
                } else {
                    qDebug() << "Error";
                }
                proc->deleteLater();
            });
        });
        this->addState(state);

        if (previousState) {
            previousState->addTransition(this, &MountState::mountNext, state);
        } else {
            this->setInitialState(state);
        }

        previousState = state;
    }

    QFinalState* finalState = new QFinalState();
    this->addState(finalState);
    if (previousState) {
        previousState->addTransition(this, &MountState::mountNext, finalState);
    } else {
        this->setInitialState(finalState);
    }

    QStateMachine::onEntry(event);
}