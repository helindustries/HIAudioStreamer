#include "Autostart.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSettings>

namespace
{
#ifdef Q_OS_WIN
    constexpr auto RunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr auto RunValue = "HIAudioStreamer";
#else
    constexpr auto AgentLabel = "com.helindustries.hiaudiostreamer";

    QString AgentFolder()
    {
        return QDir::homePath() + QStringLiteral("/Library/LaunchAgents");
    }

    QString AgentPath()
    {
        return AgentFolder() + QStringLiteral("/%1.plist").arg(AgentLabel);
    }

    QByteArray AgentPlist()
    {
        const auto program = QCoreApplication::applicationFilePath().toHtmlEscaped();
        return QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                              "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                              "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                              "<plist version=\"1.0\">\n<dict>\n"
                              "    <key>Label</key>\n    <string>%1</string>\n"
                              "    <key>ProgramArguments</key>\n"
                              "    <array>\n        <string>%2</string>\n    </array>\n"
                              "    <key>RunAtLoad</key>\n    <true/>\n"
                              "    <key>ProcessType</key>\n    <string>Interactive</string>\n"
                              "</dict>\n</plist>\n")
            .arg(AgentLabel, program)
            .toUtf8();
    }
#endif
} // namespace

bool HIAudioStreamer::AutostartEnabled()
{
#ifdef Q_OS_WIN
    return QSettings(RunKey, QSettings::NativeFormat).contains(RunValue);
#else
    return QFile::exists(AgentPath());
#endif
}

QString HIAudioStreamer::SetAutostart(bool enabled)
{
    auto error = QString();
#ifdef Q_OS_WIN
    auto run = QSettings(RunKey, QSettings::NativeFormat);
    if (enabled)
    {
        run.setValue(RunValue, QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(
                                   QCoreApplication::applicationFilePath())));
    }
    else run.remove(RunValue);
    run.sync();
    if (run.status() != QSettings::NoError)
        error = QStringLiteral("Could not change %1 under %2").arg(RunValue, RunKey);
#else
    if (enabled)
    {
        if (auto file = QFile(AgentPath());
            !QDir().mkpath(AgentFolder()) || !file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(AgentPlist()) < 0)
        {
            error =
                QStringLiteral("Could not write %1: %2").arg(file.fileName(), file.errorString());
        }
    }
    else if (auto file = QFile(AgentPath()); file.exists() && !file.remove())
        error = QStringLiteral("Could not remove %1: %2").arg(file.fileName(), file.errorString());
#endif
    return error;
}
