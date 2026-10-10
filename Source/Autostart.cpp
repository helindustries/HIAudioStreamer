#include "Autostart.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSettings>

namespace
{
#ifdef Q_OS_WIN
    constexpr auto RunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr auto RunValue = "AudioBridge";
#else
    constexpr auto AgentLabel = "com.helindustries.audiobridge";

    QString AgentPath()
    {
        return QDir::homePath() + QStringLiteral("/Library/LaunchAgents/%1.plist").arg(AgentLabel);
    }

    QByteArray AgentPlist()
    {
        const auto program = QCoreApplication::applicationFilePath().toHtmlEscaped();
        return QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n<dict>\n"
            "    <key>Label</key>\n    <string>%1</string>\n"
            "    <key>ProgramArguments</key>\n    <array>\n        <string>%2</string>\n    </array>\n"
            "    <key>RunAtLoad</key>\n    <true/>\n"
            "    <key>ProcessType</key>\n    <string>Interactive</string>\n"
            "</dict>\n</plist>\n")
            .arg(AgentLabel, program)
            .toUtf8();
    }
#endif
} // namespace

bool AudioBridge::AutostartEnabled()
{
#ifdef Q_OS_WIN
    return QSettings(RunKey, QSettings::NativeFormat).contains(RunValue);
#else
    return QFile::exists(AgentPath());
#endif
}

void AudioBridge::SetAutostart(bool enabled)
{
#ifdef Q_OS_WIN
    auto run = QSettings(RunKey, QSettings::NativeFormat);
    if (enabled)
        run.setValue(RunValue, QStringLiteral("\"%1\"").arg(
            QDir::toNativeSeparators(QCoreApplication::applicationFilePath())));
    else
        run.remove(RunValue);
#else
    if (enabled)
    {
        QDir().mkpath(QDir::homePath() + QStringLiteral("/Library/LaunchAgents"));
        auto file = QFile(AgentPath());
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(AgentPlist());
    }
    else
        QFile::remove(AgentPath());
#endif
}
