#include "Config.h"
#include "Tray.h"

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

    auto result = 1;
    if (QSystemTrayIcon::isSystemTrayAvailable())
    {
        auto tray = AudioBridge::Tray();
        result = QApplication::exec();
    }
    else
        QMessageBox::critical(nullptr, QStringLiteral("AudioBridge"),
            QStringLiteral("There is no system tray to put AudioBridge in."));
    return result;
}
