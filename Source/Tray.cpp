#include "Tray.h"

#include "AudioBackend.h"
#include "Autostart.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QStandardPaths>
#include <QtGui/QActionGroup>
#include <QtGui/QCursor>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtWidgets/QApplication>
#include <QtWidgets/QInputDialog>

#include <array>

namespace
{
    constexpr auto StatusRefreshMs = 1000;
    constexpr auto BufferChoicesMs = std::array{20, 40, 80, 150};
    constexpr auto IconSize = 64;
    constexpr auto MessageMs = 5000;
    constexpr auto Title = "AudioBridge";
    constexpr auto DiagnosticsEveryTicks = 10;
    constexpr auto MaxLogBytes = 1024 * 1024;

    // A speaker, with sound waves while anything is being sent or played. Drawn rather than
    // shipped as a file; on macOS it is a template image so the menu bar can tint it.
    QIcon MakeIcon(bool active)
    {
        auto pixmap = QPixmap(IconSize, IconSize);
        pixmap.fill(Qt::transparent);
        auto painter = QPainter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
#ifdef Q_OS_MACOS
        const auto colour = QColor(Qt::black);
#else
        const auto colour = active ? QColor(70, 190, 110) : QColor(170, 170, 170);
#endif
        painter.setPen(QPen(colour, 6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(colour);
        painter.drawPolygon(QPolygonF({QPointF(6, 24), QPointF(18, 24), QPointF(32, 10),
            QPointF(32, 54), QPointF(18, 40), QPointF(6, 40)}));
        painter.setBrush(Qt::NoBrush);
        if (active)
        {
            painter.drawArc(QRectF(26, 20, 16, 24), -55 * 16, 110 * 16);
            painter.drawArc(QRectF(28, 8, 30, 48), -55 * 16, 110 * 16);
        }
        painter.end();
        auto icon = QIcon(pixmap);
#ifdef Q_OS_MACOS
        icon.setIsMask(true);
#endif
        return icon;
    }

    // An accessory app on macOS opens its dialogs behind whatever is in front unless asked.
    void BringForward(QWidget& widget)
    {
        widget.show();
        widget.raise();
        widget.activateWindow();
    }

    QString AskText(const QString& label, const QString& initial, bool& accepted)
    {
        auto dialog = QInputDialog();
        dialog.setWindowTitle(QString::fromUtf8(Title));
        dialog.setLabelText(label);
        dialog.setTextValue(initial);
        BringForward(dialog);
        accepted = dialog.exec() == QDialog::Accepted;
        return dialog.textValue().trimmed();
    }
} // namespace

AudioBridge::Tray::Tray()
    : Current(LoadConfig())
{
    SaveConfig(Current);
    for (auto i = 0; i < 1 + MaxPeers + 2; ++i)
    {
        auto* line = Menu.addAction(QString());
        line->setEnabled(false);
        StatusLines.push_back(line);
    }
    Menu.addSeparator();
    SendAction = Menu.addAction(QStringLiteral("Send"));
    SendAction->setCheckable(true);
    SourceMenu = Menu.addMenu(QStringLiteral("Source"));
    OutputMenu = Menu.addMenu(QStringLiteral("Output"));
    BufferMenu = Menu.addMenu(QStringLiteral("Buffer"));
    Menu.addSeparator();
    ConnectionsMenu = Menu.addMenu(QStringLiteral("Connections"));
    AutostartAction = Menu.addAction(QStringLiteral("Start at Login"));
    AutostartAction->setCheckable(true);
    Menu.addSeparator();
    auto* quit = Menu.addAction(QStringLiteral("Quit AudioBridge"));

    connect(SendAction, &QAction::triggered, this, [this](bool on) {
        Current.Send = on;
        Commit();
    });
    connect(AutostartAction, &QAction::triggered, this, [](bool on) { SetAutostart(on); });
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);
    // A logoff or shutdown may end the process without running destructors, so the local
    // output is given back while there is still time.
    connect(qApp, &QGuiApplication::commitDataRequest, this, [this] { Link.ReleaseLocalMute(); });
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { Link.ReleaseLocalMute(); });
    connect(&Menu, &QMenu::aboutToShow, this, &Tray::RefreshMenus);
    connect(&StatusTimer, &QTimer::timeout, this, &Tray::RefreshStatus);
    connect(&Link, &Bridge::PairingCodeNeeded, this, &Tray::HandleCodeNeeded);
    connect(&Link, &Bridge::PairingCodeShown, this, &Tray::HandleCodeShown);
    connect(&Link, &Bridge::Paired, this, &Tray::HandlePaired);
    connect(&Link, &Bridge::PairingFailed, this, &Tray::HandlePairingFailed);
    connect(&Link, &Bridge::Failed, this, [this](const QString& message) {
        WriteDiagnostics(QStringLiteral("error: ") + message);
        Icon.showMessage(QString::fromUtf8(Title), message, QSystemTrayIcon::Warning, MessageMs);
    });
#ifndef Q_OS_MACOS
    // Windows only opens the context menu on a right click; a left click should do the same.
    connect(&Icon, &QSystemTrayIcon::activated, this, [this](auto reason) {
        if (reason == QSystemTrayIcon::Trigger) Menu.popup(QCursor::pos());
    });
#endif

    Icon.setContextMenu(&Menu);
    Icon.setIcon(MakeIcon(false));
    Icon.show();
    RefreshMenus();
    DefaultOutput = DefaultOutputId();
    Link.Apply(Current);
    WriteDiagnostics(QStringLiteral("started, default output %1").arg(DefaultOutput));
    RefreshStatus();
    StatusTimer.start(StatusRefreshMs);
    if (Current.Peers.isEmpty())
        Icon.showMessage(QString::fromUtf8(Title),
            QStringLiteral("Not connected yet. Use Connections > Add Connection."),
            QSystemTrayIcon::Information, MessageMs);
}

void AudioBridge::Tray::Commit()
{
    SaveConfig(Current);
    Link.Apply(Current);
    RefreshMenus();
    RefreshStatus();
}

// Neither OS moves a running stream when the default output changes, so "System Default" has
// to be followed by hand. On macOS the system tap is clocked by the default output as well,
// which is why both streams restart rather than only the playback.
void AudioBridge::Tray::FollowDefaultOutput()
{
    const auto current = DefaultOutputId();
    if (current != DefaultOutput && !current.isEmpty())
    {
        WriteDiagnostics(QStringLiteral("default output changed to %1").arg(current));
        DefaultOutput = current;
        if (Current.Output.isEmpty()) Link.Apply(Current);
    }
}

void AudioBridge::Tray::WriteDiagnostics(const QString& line)
{
    const auto folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(folder);
    auto file = QFile(folder + QStringLiteral("/AudioBridge.log"));
    const auto mode = file.size() > MaxLogBytes ? QIODevice::WriteOnly | QIODevice::Truncate
                                                : QIODevice::WriteOnly | QIODevice::Append;
    if (file.open(mode))
        file.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QLatin1Char(' ') + line
                    + QLatin1Char('\n'))
                       .toUtf8());
}

void AudioBridge::Tray::RefreshStatus()
{
    FollowDefaultOutput();
    Link.UpdateLocalMute();
    if (++TicksSinceDiagnostics >= DiagnosticsEveryTicks)
    {
        TicksSinceDiagnostics = 0;
        WriteDiagnostics(Link.Diagnostics());
    }
    const auto lines = Link.Status().split(QLatin1Char('\n'));
    for (auto i = std::size_t{0}; i < StatusLines.size(); ++i)
    {
        const auto shown = static_cast<qsizetype>(i) < lines.size();
        StatusLines[i]->setVisible(shown);
        if (shown) StatusLines[i]->setText(lines[i]);
    }
    Icon.setToolTip(lines.join(QLatin1Char('\n')));
    const auto active = Link.Active();
    if (active != ShowingActive)
    {
        ShowingActive = active;
        Icon.setIcon(MakeIcon(active));
    }
}

void AudioBridge::Tray::RefreshMenus()
{
    SendAction->setChecked(Current.Send);
    AutostartAction->setChecked(AutostartEnabled());

    // Devices come and go, so the lists are rebuilt each time the menu opens.
    SourceMenu->clear();
    auto* sources = new QActionGroup(SourceMenu);
    auto addSource = [this, sources](const QString& name, const QString& id) {
        auto* action = SourceMenu->addAction(name);
        action->setCheckable(true);
        action->setChecked(Current.Source == id);
        sources->addAction(action);
        connect(action, &QAction::triggered, this, [this, id] {
            Current.Source = id;
            Commit();
        });
    };
    addSource(QStringLiteral("System Audio"), QString());
    SourceMenu->addSeparator();
    for (const auto& device : InputDevices())
        addSource(device.Name, device.Id);

    OutputMenu->clear();
    auto* outputs = new QActionGroup(OutputMenu);
    auto addOutput = [this, outputs](const QString& name, const QString& id) {
        auto* action = OutputMenu->addAction(name);
        action->setCheckable(true);
        action->setChecked(Current.Output == id);
        outputs->addAction(action);
        connect(action, &QAction::triggered, this, [this, id] {
            Current.Output = id;
            Commit();
        });
    };
    addOutput(QStringLiteral("System Default"), QString());
    OutputMenu->addSeparator();
    for (const auto& device : OutputDevices())
        addOutput(device.Name, device.Id);

    BufferMenu->clear();
    auto* buffers = new QActionGroup(BufferMenu);
    for (const auto ms : BufferChoicesMs)
    {
        auto* action = BufferMenu->addAction(QStringLiteral("%1 ms").arg(ms));
        action->setCheckable(true);
        action->setChecked(Current.BufferMs == ms);
        buffers->addAction(action);
        connect(action, &QAction::triggered, this, [this, ms] {
            Current.BufferMs = ms;
            Commit();
        });
    }

    ConnectionsMenu->clear();
    for (auto i = 0; i < Current.Peers.size(); ++i)
    {
        const auto& peer = Current.Peers[i];
        auto* entry = ConnectionsMenu->addMenu(QStringLiteral("%1 (%2)").arg(peer.Name, peer.Address));
        auto* send = entry->addAction(QStringLiteral("Send to %1").arg(peer.Name));
        send->setCheckable(true);
        send->setChecked(peer.Send);
        connect(send, &QAction::triggered, this, [this, i](bool on) {
            Current.Peers[i].Send = on;
            Commit();
        });
        auto* remove = entry->addAction(QStringLiteral("Remove"));
        connect(remove, &QAction::triggered, this, [this, i] {
            Current.Peers.removeAt(i);
            Commit();
        });
    }
    if (!Current.Peers.isEmpty()) ConnectionsMenu->addSeparator();
    auto* add = ConnectionsMenu->addAction(QStringLiteral("Add Connection…"));
    add->setEnabled(Current.Peers.size() < MaxPeers);
    connect(add, &QAction::triggered, this, &Tray::AddConnection);
}

void AudioBridge::Tray::AddConnection()
{
    auto accepted = false;
    const auto address = AskText(QStringLiteral("IP address of the other machine, optionally "
                                                "with :port.\nAudioBridge must be running there."),
        QString(), accepted);
    if (accepted && !address.isEmpty())
    {
        Link.StartPairing(address);
        Icon.showMessage(QString::fromUtf8(Title),
            QStringLiteral("Asking %1 to pair…").arg(address), QSystemTrayIcon::Information,
            MessageMs);
    }
}

void AudioBridge::Tray::HandleCodeNeeded(const QString& name, const QString& address)
{
    auto accepted = false;
    const auto code = AskText(QStringLiteral("Enter the 4-digit code that %1 (%2) is showing.")
                                  .arg(name, address),
        QString(), accepted);
    if (accepted)
        Link.ConfirmPairing(code);
    else
        Link.CancelPairing();
}

void AudioBridge::Tray::HandleCodeShown(const QString& name, const QString& address,
    const QString& code)
{
    CloseCodeBox();
    CodeBox = new QMessageBox(QMessageBox::Information, QString::fromUtf8(Title),
        QStringLiteral("%1 (%2) wants to connect.\n\nEnter this code there:\n\n%3")
            .arg(name, address, code),
        QMessageBox::Cancel);
    CodeBox->setAttribute(Qt::WA_DeleteOnClose);
    connect(CodeBox, &QMessageBox::rejected, this, [this] { Link.CancelPairing(); });
    BringForward(*CodeBox);
}

void AudioBridge::Tray::CloseCodeBox()
{
    if (CodeBox != nullptr)
    {
        CodeBox->disconnect(this);
        CodeBox->close();
    }
}

void AudioBridge::Tray::HandlePaired(const AudioBridge::Peer& peer)
{
    CloseCodeBox();
    // Pairing again with a known machine replaces it rather than listing it twice.
    auto replaced = false;
    for (auto& existing : Current.Peers)
        if (!replaced && (existing.PublicKey == peer.PublicKey || existing.Address == peer.Address))
        {
            existing = peer;
            replaced = true;
        }
    if (!replaced) Current.Peers.append(peer);
    Commit();
    Icon.showMessage(QString::fromUtf8(Title), QStringLiteral("Connected to %1").arg(peer.Name),
        QSystemTrayIcon::Information, MessageMs);
}

void AudioBridge::Tray::HandlePairingFailed(const QString& message)
{
    CloseCodeBox();
    Icon.showMessage(QString::fromUtf8(Title), message, QSystemTrayIcon::Warning, MessageMs);
}
