#include "AudioBackend.h"
#include "Config.h"
#include "Tray.h"

#include <QtCore/QDir>
#include <QtCore/QLockFile>
#include <QtCore/QStandardPaths>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QSystemTrayIcon>

namespace
{
    constexpr auto LockFile = "HIAudioStreamer.lock";
    // Run by the installer after it had to stop a running copy, which had no chance to unmute.
    constexpr auto RestoreOutputOption = "--restore-output";
} // namespace

int main(int argc, char* argv[])
{
    auto application = QApplication(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Hel Industries"));
    QApplication::setOrganizationDomain(QStringLiteral("helindustries.com"));
    QApplication::setApplicationName(QStringLiteral("HIAudioStreamer"));
    QApplication::setQuitOnLastWindowClosed(false);
    qRegisterMetaType<HIAudioStreamer::Peer>();
    qRegisterMetaType<HIAudioStreamer::Config>();

    // One instance per user: a second would fight the first for the port and for the mute.
    // QLockFile notices a lock left by a crashed run and takes it over.
    auto result = 0;
    if (auto lock = QLockFile(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                  .filePath(QString::fromUtf8(LockFile)));
        lock.tryLock(0))
    {
        if (QApplication::arguments().contains(QString::fromUtf8(RestoreOutputOption)))
            HIAudioStreamer::RestoreLocalOutput();
        else if (QSystemTrayIcon::isSystemTrayAvailable())
        {
            HIAudioStreamer::RestoreLocalOutput();
            auto tray = HIAudioStreamer::Tray();
            result = QApplication::exec();
        }
        else
        {
            QMessageBox::critical(nullptr, QStringLiteral("HIAudioStreamer"),
                                  QStringLiteral(
                                      "There is no system tray to put HIAudioStreamer in."));
            result = 1;
        }
    }
    else qInfo("HIAudioStreamer is already running for this user; this instance ends.");
    return result;
}
