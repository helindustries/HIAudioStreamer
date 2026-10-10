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

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>

namespace
{
    constexpr auto StatusRefreshMs = 1000;
    constexpr auto BufferChoicesMs = std::array{20, 40, 80, 150};
    constexpr auto IconSize = 64;
    constexpr auto MessageMs = 5000;
    constexpr auto Title = "HIAudioStreamer";
    constexpr auto DiagnosticsEveryTicks = 10;
    constexpr auto MaxLogBytes = 1024 * 1024;
    constexpr auto LogFile = "HIAudioStreamer.log";
    // Sending, one per connection, the local mute, and room for an error.
    constexpr auto StatusLineCount = 1 + HIAudioStreamer::MaxPeers + 2;

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

    // Nothing when the dialog was cancelled.
    std::optional<QString> AskText(const QString& label)
    {
        auto dialog = QInputDialog();
        dialog.setWindowTitle(QString::fromUtf8(Title));
        dialog.setLabelText(label);
        BringForward(dialog);
        return dialog.exec() == QDialog::Accepted ? std::optional(dialog.textValue().trimmed())
                                                  : std::nullopt;
    }
} // namespace

HIAudioStreamer::Tray::Tray()
    : Current(LoadConfig())
{
    SaveConfig(Current);
    std::ranges::generate_n(std::back_inserter(StatusLines), StatusLineCount, [this] {
        auto* line = Menu.addAction(QString());
        line->setEnabled(false);
        return line;
    });
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
    auto* quit = Menu.addAction(QStringLiteral("Quit HIAudioStreamer"));

    connect(SendAction, &QAction::triggered, this, [this](bool on) {
        Current.Send = on;
        Commit();
    });
    connect(AutostartAction, &QAction::triggered, this, [this](bool on) {
        if (const auto error = SetAutostart(on); !error.isEmpty())
        {
            Icon.showMessage(QString::fromUtf8(Title), error, QSystemTrayIcon::Warning, MessageMs);
            AutostartAction->setChecked(AutostartEnabled());
        }
    });
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
    {
        Icon.showMessage(QString::fromUtf8(Title),
                         QStringLiteral("Not connected yet. Use Connections > Add Connection."),
                         QSystemTrayIcon::Information, MessageMs);
    }
}

void HIAudioStreamer::Tray::Commit()
{
    SaveConfig(Current);
    Link.Apply(Current);
    RefreshMenus();
    RefreshStatus();
}

// Neither OS moves a running stream when the default output changes, so "System Default" has
// to be followed by hand. On macOS the system tap is clocked by the default output as well,
// which is why both streams restart rather than only the playback.
void HIAudioStreamer::Tray::FollowDefaultOutput()
{
    if (const auto current = DefaultOutputId(); current != DefaultOutput && !current.isEmpty())
    {
        WriteDiagnostics(QStringLiteral("default output changed to %1").arg(current));
        DefaultOutput = current;
        if (Current.Output.isEmpty()) Link.Apply(Current);
    }
}

void HIAudioStreamer::Tray::WriteDiagnostics(const QString& line)
{
    const auto folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (auto file = QFile(QDir(folder).filePath(QString::fromUtf8(LogFile)));
        QDir().mkpath(folder)
        && file.open(QIODevice::WriteOnly
                     | (file.size() > MaxLogBytes ? QIODevice::Truncate : QIODevice::Append)))
    {
        file.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QLatin1Char(' ') + line
                    + QLatin1Char('\n'))
                       .toUtf8());
    }
    else
    {
        qWarning("Cannot write the log %s: %s", qUtf8Printable(file.fileName()),
                 qUtf8Printable(file.errorString()));
    }
}

void HIAudioStreamer::Tray::RefreshStatus()
{
    FollowDefaultOutput();
    Link.UpdateLocalMute();
    if (++TicksSinceDiagnostics >= DiagnosticsEveryTicks)
    {
        TicksSinceDiagnostics = 0;
        WriteDiagnostics(Link.Diagnostics());
    }
    const auto lines = Link.Status().split(QLatin1Char('\n'));
    for (auto i = qsizetype{0}; auto* statusLine : StatusLines)
    {
        const auto shown = i < lines.size();
        statusLine->setVisible(shown);
        if (shown) statusLine->setText(lines[i]);
        ++i;
    }
    Icon.setToolTip(lines.join(QLatin1Char('\n')));
    if (const auto active = Link.Active(); active != ShowingActive)
    {
        ShowingActive = active;
        Icon.setIcon(MakeIcon(active));
    }
}

void HIAudioStreamer::Tray::RefreshMenus()
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
    for (const auto& [id, name] : InputDevices())
        addSource(name, id);

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
    for (const auto& [id, name] : OutputDevices())
        addOutput(name, id);

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
    for (auto i = 0; const auto& peer : Current.Peers)
    {
        auto* entry =
            ConnectionsMenu->addMenu(QStringLiteral("%1 (%2)").arg(peer.Name, peer.Address));
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
        ++i;
    }
    if (!Current.Peers.isEmpty()) ConnectionsMenu->addSeparator();
    auto* add = ConnectionsMenu->addAction(QStringLiteral("Add Connection…"));
    add->setEnabled(Current.Peers.size() < MaxPeers);
    connect(add, &QAction::triggered, this, &Tray::AddConnection);
}

void HIAudioStreamer::Tray::AddConnection()
{
    if (const auto address = AskText(QStringLiteral("IP address of the other machine, optionally "
                                                    "with :port.\nHIAudioStreamer must be "
                                                    "running there."));
        address && !address->isEmpty())
    {
        Link.StartPairing(*address);
        Icon.showMessage(QString::fromUtf8(Title),
                         QStringLiteral("Asking %1 to pair…").arg(*address),
                         QSystemTrayIcon::Information, MessageMs);
    }
}

void HIAudioStreamer::Tray::HandleCodeNeeded(const QString& name, const QString& address)
{
    if (const auto code = AskText(
            QStringLiteral("Enter the 4-digit code that %1 (%2) is showing.").arg(name, address)))
        Link.ConfirmPairing(*code);
    else Link.CancelPairing();
}

void HIAudioStreamer::Tray::HandleCodeShown(const QString& name, const QString& address,
                                            const QString& code)
{
    CloseCodeBox();
    CodeBox =
        new QMessageBox(QMessageBox::Information, QString::fromUtf8(Title),
                        QStringLiteral("%1 (%2) wants to connect.\n\nEnter this code there:\n\n%3")
                            .arg(name, address, code),
                        QMessageBox::Cancel);
    CodeBox->setAttribute(Qt::WA_DeleteOnClose);
    connect(CodeBox, &QMessageBox::rejected, this, [this] { Link.CancelPairing(); });
    BringForward(*CodeBox);
}

void HIAudioStreamer::Tray::CloseCodeBox()
{
    if (CodeBox != nullptr)
    {
        CodeBox->disconnect(this);
        CodeBox->close();
    }
}

void HIAudioStreamer::Tray::HandlePaired(const HIAudioStreamer::Peer& peer)
{
    CloseCodeBox();
    // Pairing again with a known machine replaces it rather than listing it twice.
    if (auto existing = std::ranges::find_if(Current.Peers,
                                             [&peer](const auto& known) {
                                                 return known.PublicKey == peer.PublicKey
                                                        || known.Address == peer.Address;
                                             });
        existing != Current.Peers.end())
        *existing = peer;
    else Current.Peers.append(peer);
    Commit();
    Icon.showMessage(QString::fromUtf8(Title), QStringLiteral("Connected to %1").arg(peer.Name),
                     QSystemTrayIcon::Information, MessageMs);
}

void HIAudioStreamer::Tray::HandlePairingFailed(const QString& message)
{
    CloseCodeBox();
    Icon.showMessage(QString::fromUtf8(Title), message, QSystemTrayIcon::Warning, MessageMs);
}
