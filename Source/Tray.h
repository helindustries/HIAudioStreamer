#pragma once

#include "Bridge.h"
#include "Config.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QSystemTrayIcon>

#include <vector>

namespace AudioBridge
{
    // The whole user interface: a tray icon on Windows, a menu bar item on macOS. Every change
    // is saved at once and applied by restarting the audio streams.
    class Tray : public QObject
    {
        Q_OBJECT

    public:
        Tray();

    private:
        void Commit();
        void RefreshMenus();
        void RefreshStatus();
        void AddConnection();
        void HandleCodeNeeded(const QString& name, const QString& address);
        void HandleCodeShown(const QString& name, const QString& address, const QString& code);
        void HandlePaired(const AudioBridge::Peer& peer);
        void HandlePairingFailed(const QString& message);
        void CloseCodeBox();

        Config Current;
        Bridge Link;
        QSystemTrayIcon Icon;
        QMenu Menu;
        QTimer StatusTimer;
        std::vector<QAction*> StatusLines;
        QAction* SendAction = nullptr;
        QAction* AutostartAction = nullptr;
        QMenu* SourceMenu = nullptr;
        QMenu* OutputMenu = nullptr;
        QMenu* BufferMenu = nullptr;
        QMenu* ConnectionsMenu = nullptr;
        QPointer<QMessageBox> CodeBox;
        bool ShowingActive = false;
    };
} // namespace AudioBridge
