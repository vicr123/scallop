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
#include "diskmanagementstate.h"

#include <driveobjectmanager.h>
#include <DriveObjects/diskobject.h>
#include <DriveObjects/blockinterface.h>
#include <DriveObjects/partitiontableinterface.h>
#include <DriveObjects/partitioninterface.h>
#include <DriveObjects/encryptedinterface.h>
#include <QDebug>
#include "installerdata.h"

struct DiskManagementStatePrivate {
    QJsonObject diskDetails;
    QMap<QString, DiskObject*> disks;
    QList<QPair<QString, QString>> mounts;
};

DiskManagementState::DiskManagementState(QState* parent) : QStateMachine(parent) {
    d = new DiskManagementStatePrivate();

    //Determine what to do
    d->diskDetails = InstallerData::value("disk").toObject();

    //TODO: Disk encryption?

    QFinalState* finishedState = new QFinalState();
    connect(finishedState, &QFinalState::entered, this, [ = ] {
        InstallerData::insertTemp("mounts", QVariant::fromValue(d->mounts));
    });

    //Determine what to do
    if (d->diskDetails.value("type").toString() == QStringLiteral("whole-disk")) {
        //Erase the disk
        QState* partitionTableState = new QState();
        connect(partitionTableState, &QState::entered, this, [ = ] {
            QTextStream(stdout) << tr("Erasing disk %1").arg(d->diskDetails.value("block").toString()) << "\n";
            DiskObject* disk = DriveObjectManager::diskByBlockName(InstallerData::value("disk").toObject().value("block").toString());
            disk->interface<BlockInterface>()->format("gpt", {})->then([ = ] {
                d->disks.insert("disk", disk);
                emit nextState();
            })->error([ = ](QString error) {
                QTextStream(stderr) << tr("Failed to erase the disk") << "\n";
                QTextStream(stderr) << error << "\n";
                emit failure();
            });
        });


        QState* bootPartition = new QState();
        connect(bootPartition, &QState::entered, this, [ = ] {
            QTextStream(stdout) << tr("Creating Boot Partition") << "\n";

            DiskObject* disk = d->disks.value("disk");
            PartitionTableInterface* partitionTable = disk->interface<PartitionTableInterface>();

            if (InstallerData::isEfi()) {
                //Create an EFI System Partition
                partitionTable->createPartitionAndFormat(0, 536870912 /* 512 MB */, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" /* EFI System */, "efi", {}, "vfat", {})->then([ = ](QDBusObjectPath object) {
                    DiskObject* bootObject = DriveObjectManager::diskForPath(object);
                    d->disks.insert("firmwareboot", bootObject);
                    d->mounts.append({"/boot", object.path()});

                    emit nextState();
                })->error([ = ](QString message) {
                    //TODO: Enter a failure state
                    emit failure();

                    QTextStream(stderr) << tr("Failed to create the boot partition") << "\n";
                    QTextStream(stderr) << message << "\n";
                });
            } else {
                //Create a BIOS Boot partition
                partitionTable->createPartition(0, 1048576 /* 1 MB */, "21686148-6449-6E6F-744E-656564454649" /* BIOS Boot */, "biosboot", {})->then([ = ](QDBusObjectPath object) {
                    DiskObject* bootObject = DriveObjectManager::diskForPath(object);
                    d->disks.insert("firmwareboot", bootObject);

                    emit nextState();
                })->error([ = ](QString message) {
                    //TODO: Enter a failure state
                    emit failure();

                    QTextStream(stderr) << tr("Failed to create the boot partition") << "\n";
                    QTextStream(stderr) << message << "\n";
                });
            }
        });

        QState* rootPartition = new QState();
        connect(rootPartition, &QState::entered, this, [ = ] {
            DiskObject* disk = d->disks.value("disk");
            PartitionTableInterface* partitionTable = disk->interface<PartitionTableInterface>();
            DiskObject* bootObject = d->disks.value("firmwareboot");
            quint64 newOffset = bootObject->interface<PartitionInterface>()->offset() + bootObject->interface<PartitionInterface>()->size();

            QVariantMap formatOptions;
            QJsonObject luksConfig = InstallerData::value("luks").toObject();
            if (luksConfig.contains("password")) {
                //Encrypt this partition
                formatOptions.insert("encrypt.passphrase", luksConfig.value("password").toString());
                formatOptions.insert("encrypt.type", "luks1");
            }

            QTextStream(stdout) << tr("Creating Root Partition") << "\n";
            partitionTable->createPartitionAndFormat(newOffset, 0, "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709" /* Linux Root */, "root", {}, "ext4", formatOptions)->then([ = ](QDBusObjectPath object) {

                DiskObject* rootObject = DriveObjectManager::diskForPath(object);
                EncryptedInterface* encrypted = rootObject->interface<EncryptedInterface>();
                if (encrypted) {
                    rootObject = encrypted->cleartextDevice();
                }
                d->mounts.append({"/", rootObject->path().path()});
                d->disks.insert("root", rootObject);

                emit nextState();
            })->error([ = ](QString message) {
                //TODO: Enter a failure state
                emit failure();

                QTextStream(stderr) << tr("Failed to create the root partition") << "\n";
                QTextStream(stderr) << message << "\n";
            });
        });

        this->addState(partitionTableState);
        this->addState(bootPartition);
        this->addState(rootPartition);
        this->addState(finishedState);

        this->setInitialState(partitionTableState);
        partitionTableState->addTransition(this, &DiskManagementState::nextState, bootPartition);
        bootPartition->addTransition(this, &DiskManagementState::nextState, rootPartition);
        rootPartition->addTransition(this, &DiskManagementState::nextState, finishedState);
    } else if (d->diskDetails.value("type").toString() == QStringLiteral("mount-list")) {
        //Figure out the mounts
        QJsonArray mounts = d->diskDetails.value("mounts").toArray();
        for (QJsonValue mount : mounts) {
            QJsonObject mountObject = mount.toObject();
            d->mounts.append({mountObject.value("mountPoint").toString(), mountObject.value("block").toString()});
        }

        this->addState(finishedState);
        this->setInitialState(finishedState);
    }
}

DiskManagementState::~DiskManagementState() {
    delete d;
}
