#include "AudioBackend.h"
#include "Config.h"
#include "Tray.h"

#include <QtCore/QDir>
#include <QtCore/QLockFile>
#include <QtCore/QStandardPaths>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QSystemTrayIcon>

int main(int argc, char* argv[])
{
    auto application = QApplication(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Hel Industries"));
    QApplication::setOrganizationDomain(QStringLiteral("helindustries.com"));
    QApplication::setApplicationName(QStringLiteral("AudioBridge"));
    QApplication::setQuitOnLastWindowClosed(false);
    qRegisterMetaType<AudioBridge::Peer>();
    qRegisterMetaType<AudioBridge::Config>();

    // One instance per user: a second would fight the first for the port and for the mute.
    // QLockFile notices a lock left by a crashed run and takes it over.
    auto lock = QLockFile(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                              .filePath(QStringLiteral("AudioBridge.lock")));
    // A second instance simply ends: the first one already sits in the tray.
    auto result = 0;
    if (lock.tryLock(0))
    {
        if (QSystemTrayIcon::isSystemTrayAvailable())
        {
            AudioBridge::RestoreLocalOutput();
            auto tray = AudioBridge::Tray();
            result = QApplication::exec();
        }
        else
        {
            QMessageBox::critical(nullptr, QStringLiteral("AudioBridge"),
                QStringLiteral("There is no system tray to put AudioBridge in."));
            result = 1;
        }
    }
    return result;
}
