#include "DesktopController.h"

#include <QApplication>
#include <QColor>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLockFile>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QSystemTrayIcon>
#include <cstdio>
#include <memory>
#include <unistd.h>

// The shared provider SVGs are white template assets on macOS. Tint them using the
// active Qt palette so they stay legible in both light and dark Linux appearances.
class ProviderIconProvider final : public QQuickImageProvider {
public:
    ProviderIconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override {
        const auto provider = id.section('?', 0, 0);
        if (!QRegularExpression("^[a-z0-9-]{1,80}$").match(provider).hasMatch()) return {};
        const auto color = QColor("#" + id.section("color=", 1, 1).section('&', 0, 0));
        const auto dimensions = requestedSize.isValid() ? requestedSize : QSize(36, 36);
        QImage image(dimensions, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QFile icon(":/logos/ProviderIcon-" + provider + ".svg");
        if (icon.open(QIODevice::ReadOnly)) {
            QSvgRenderer svg(icon.readAll());
            if (svg.isValid()) {
                QPainter painter(&image);
                svg.render(&painter);
                painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
                painter.fillRect(image.rect(), color.isValid() ? color : QColor(Qt::black));
                if (size) *size = dimensions;
                return image;
            }
        }
        // Not every provider ships a brand asset yet. Draw the provider initial
        // instead of leaving an empty slot in the tray and the dashboard.
        renderMonogram(image, provider, color.isValid() ? color : QColor(Qt::black));
        if (size) *size = dimensions;
        return image;
    }

private:
    static void renderMonogram(QImage &image, const QString &provider, const QColor &color) {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const auto inset = qMax(2, image.width() / 9);
        painter.drawRoundedRect(image.rect().adjusted(inset, inset, -inset, -inset),
                                image.width() / 6.0, image.width() / 6.0);
        // Keep the initial readable on light and dark accent colours.
        const auto luminance = 0.299 * color.red() + 0.587 * color.green() + 0.114 * color.blue();
        painter.setPen(luminance > 140 ? QColor(Qt::black) : QColor(Qt::white));
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(qMax(8, static_cast<int>(image.height() * 0.46)));
        painter.setFont(font);
        const auto initial = provider.left(1).toUpper();
        painter.drawText(image.rect(), Qt::AlignCenter, initial);
    }
};

// The display name of a provider, mirroring Usage.providerName in the QML layer.
static QString providerDisplayName(const QString &provider) {
    static const QHash<QString, QString> names{
        {"codex", "Codex"}, {"claude", "Claude"}, {"copilot", "GitHub Copilot"}, {"gemini", "Gemini"},
        {"cursor", "Cursor"}, {"antigravity", "Antigravity"}, {"openrouter", "OpenRouter"}, {"kiro", "Kiro"}};
    const auto name = names.value(provider);
    if (!name.isEmpty()) return name;
    if (provider.isEmpty()) return QStringLiteral("Unknown provider");
    return provider.left(1).toUpper() + provider.mid(1);
}

// One line per provider: "Codex — Session 48% used · Weekly 6% used".
static QString providerSummary(const QVariantMap &entry, const QString &quotaDisplay) {
    const auto provider = entry.value("provider").toString();
    const auto windows = entry.value("windows").toList();
    QStringList parts;
    for (const auto &item : windows) {
        const auto window = item.toMap();
        const auto remaining = qBound(0.0, window.value("remaining").toDouble(), 100.0);
        const auto value = quotaDisplay == "used" ? 100.0 - remaining : remaining;
        const auto label = window.value("label").toString();
        if (label.isEmpty()) continue;
        parts.append(QString("%1 %2% %3").arg(label).arg(value, 0, 'f', 0)
            .arg(quotaDisplay == "used" ? "used" : "left"));
    }
    if (parts.isEmpty()) {
        if (entry.value("failed").toBool()) return QString("%1 — usage unavailable").arg(providerDisplayName(provider));
        return QString("%1 — no quota windows").arg(providerDisplayName(provider));
    }
    return QString("%1 — %2").arg(providerDisplayName(provider), parts.join(" · "));
}

// The entry the tray icon should represent: the provider with the highest usage,
// or the first one, depending on the preference.
static QVariantMap trayEntry(const QVariantList &entries, const QString &source) {
    if (entries.isEmpty()) return {};
    if (source != "highest") return entries.first().toMap();
    QVariantMap best;
    double bestUsage = -1.0;
    for (const auto &item : entries) {
        const auto entry = item.toMap();
        double usage = 0.0;
        const auto windows = entry.value("windows").toList();
        for (const auto &windowItem : windows) {
            const auto window = windowItem.toMap();
            usage = qMax(usage, 100.0 - qBound(0.0, window.value("remaining").toDouble(), 100.0));
        }
        if (usage > bestUsage) {
            bestUsage = usage;
            best = entry;
        }
    }
    return best;
}

static QByteArray request(const QString &socketPath, const QJsonObject &message) {
    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(500)) return {};
    socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    if (!socket.waitForBytesWritten(1000)) return {};
    QElapsedTimer deadline; deadline.start();
    QByteArray reply;
    while (deadline.elapsed() < 3000 && reply.size() < 4 * 1024 * 1024) {
        reply += socket.readAll();
        if (reply.contains('\n')) return reply;
        if (!socket.waitForReadyRead(qMax(1, 3000 - int(deadline.elapsed())))) {
            reply += socket.readAll();
            return reply.contains('\n') ? reply : QByteArray();
        }
    }
    return {};
}

int main(int argc, char **argv) {
    // IPC clients do not load a GUI platform plugin or instantiate a second backend.
    auto application = std::make_unique<QCoreApplication>(argc, argv);
    QCoreApplication::setApplicationName("codexbar-linux");
    QCoreApplication::setApplicationVersion(QStringLiteral(CODEXBAR_DESKTOP_VERSION));
    QCommandLineParser parser;
    parser.setApplicationDescription("CodexBar desktop for Linux · Qt windows and optional tray");
    parser.addHelpOption(); parser.addVersionOption();
    for (const auto &name : {"snapshot", "refresh", "refresh-sessions", "settings", "spending", "sessions", "usage", "quick-view", "quit", "background", "no-tray"})
        parser.addOption(QCommandLineOption(name, QString("%1 the running desktop app").arg(name)));
    parser.addOption(QCommandLineOption("autostart", "Set login startup: enable, disable, status", "action"));
    parser.addOption(QCommandLineOption("configure", "Update desktop settings through local IPC", "json"));
    parser.addOption(QCommandLineOption("cli", "CodexBar CLI executable for a new instance", "path"));
    parser.addOption(QCommandLineOption("with-spending", "With --snapshot, refresh local spending when its cache is stale"));
    parser.process(*application);
    QString command = "usage";
    for (const auto &name : {"background", "usage", "quick-view", "settings", "spending", "sessions", "refresh", "refresh-sessions", "snapshot", "quit", "configure", "autostart"})
        if (parser.isSet(name)) command = name;
    const bool clientOnly = QStringList{"snapshot", "refresh", "refresh-sessions", "quit", "configure", "autostart"}.contains(command);
    const bool noTray = parser.isSet("no-tray");
    const auto cli = parser.value("cli");
    QJsonObject message{{"command", command}};
    if (command == "autostart") message["action"] = parser.value("autostart");
    if (command == "snapshot" && parser.isSet("with-spending")) message["spending"] = true;
    if (command == "configure") {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(parser.value("configure").toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            std::fputs("--configure requires a JSON object\n", stderr); return 2;
        }
        message["settings"] = document.object();
    }
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty()) runtime = QDir::tempPath() + "/codexbar-" + QString::number(getuid());
    runtime += "/codexbar-linux";
    if (!QDir().mkpath(runtime) || QFileInfo(runtime).ownerId() != getuid() || QFileInfo(runtime).isSymLink()) {
        std::fputs("Cannot create a private runtime directory\n", stderr); return 1;
    }
    QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const auto socketPath = runtime + "/desktop.sock";
    auto reply = request(socketPath, message);
    if (!reply.isEmpty()) {
        std::fwrite(reply.constData(), 1, reply.size(), stdout);
        return QJsonDocument::fromJson(reply).object().value("ok").toBool(true) ? 0 : 1;
    }
    if (clientOnly) { std::fputs("CodexBar desktop is not running\n", stderr); return 1; }
    QLockFile lock(runtime + "/desktop.lock");
    if (!lock.tryLock(0)) {
        // Another launch may still be loading QML. Never remove its live socket.
        for (int attempt = 0; attempt < 10 && reply.isEmpty(); ++attempt) {
            usleep(100000); reply = request(socketPath, message);
        }
        if (reply.isEmpty()) { std::fputs("CodexBar is already starting; retry shortly\n", stderr); return 1; }
        return 0;
    }
    application.reset();
    QApplication app(argc, argv);
    app.setApplicationName("codexbar-linux");
    app.setApplicationDisplayName("CodexBar");
    app.setDesktopFileName("com.steipete.CodexBar");
    app.setQuitOnLastWindowClosed(false);
    app.setWindowIcon(QIcon(":/icon.svg"));
    QQuickStyle::setStyle("Fusion");
    DesktopController controller(cli);
    if (!controller.listen(socketPath)) { std::fputs("Cannot start local IPC\n", stderr); return 1; }
    QPalette systemPalette = app.palette();
    bool themeApplied = false;
    auto updateTheme = [&] {
        if (!themeApplied) systemPalette = app.palette();
        QPalette palette = systemPalette;
        themeApplied = false;
        if (controller.settings().value("followOmarchyTheme").toBool()) {
            const auto state = qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + "/.local/state");
            QFile file(state + "/omarchy/current/theme/colors.toml");
            if (file.open(QIODevice::ReadOnly) && file.size() <= 65536) {
                const auto text = QString::fromUtf8(file.readAll());
                QMap<QString, QColor> colors;
                const QRegularExpression pattern("^([a-z_]+)\\s*=\\s*[\"'](#[0-9a-fA-F]{6})[\"']", QRegularExpression::MultilineOption);
                auto matches = pattern.globalMatch(text);
                while (matches.hasNext()) { const auto match = matches.next(); colors[match.captured(1)] = QColor(match.captured(2)); }
                if (colors.contains("background") && colors.contains("foreground") && colors.contains("accent")) {
                    themeApplied = true;
                    const auto background = colors["background"], foreground = colors["foreground"];
                    for (const auto role : {QPalette::Window, QPalette::Base, QPalette::Button, QPalette::ToolTipBase}) palette.setColor(role, background);
                    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::ToolTipText}) palette.setColor(role, foreground);
                    palette.setColor(QPalette::AlternateBase, colors.value("lighter_background", background.lighter(120)));
                    palette.setColor(QPalette::Highlight, colors["accent"]);
                    palette.setColor(QPalette::HighlightedText, background);
                    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
                        palette.setColor(QPalette::Disabled, role, colors.value("dark_foreground", foreground.darker(150)));
                }
            }
        }
        if (app.palette() != palette) app.setPalette(palette);
    };
    QTimer themeTimer;
    QObject::connect(&themeTimer, &QTimer::timeout, &app, updateTheme);
    QObject::connect(&controller, &DesktopController::settingsChanged, &app, updateTheme);
    themeTimer.start(10000); updateTheme();
    QQmlApplicationEngine engine;
    engine.addImageProvider("provider-icon", new ProviderIconProvider);
    QObject::connect(&engine, &QQmlApplicationEngine::quit, &app, &QCoreApplication::quit);
    engine.rootContext()->setContextProperty("desktop", &controller);
    engine.load(QUrl("qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    QSystemTrayIcon tray(QIcon(":/icon.svg"));
    QMenu menu;
    // The provider switcher mirrors the macOS menu: every enabled provider gets a
    // row with its live usage, and the icon follows the selected provider.
    QMenu *providerMenu = menu.addMenu("Providers");
    providerMenu->setObjectName("providerSwitcher");
    menu.addAction("Quick View", &controller, [&controller] { controller.showWindow("quick-view"); });
    menu.addAction("Usage & Spend…", &controller, [&controller] { controller.showWindow("usage"); });
    menu.addAction("Settings…", &controller, [&controller] { controller.showWindow("settings"); });
    menu.addSeparator();
    menu.addAction("Refresh", &controller, [&controller] { controller.refresh(); controller.refreshCosts(); });
    menu.addAction("Quit CodexBar", &app, &QCoreApplication::quit);
    tray.setContextMenu(&menu);
    QObject::connect(&tray, &QSystemTrayIcon::activated, &controller, [&controller](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            controller.showWindow(controller.settings().value("compactQuickView").toBool() ? "quick-view" : "usage");
    });
    auto updateTray = [&] {
        const auto quotaDisplay = controller.settings().value("quotaDisplay").toString();
        tray.setVisible(!noTray && controller.settings().value("showTray").toBool());
        tray.setToolTip("CodexBar · " + (controller.summary().isEmpty() ? "Usage unavailable" : controller.summary()) +
            " · " + quotaDisplay + (controller.stale() ? " · out of date" : ""));
        if (providerMenu) {
            providerMenu->clear();
            const auto entries = controller.entries();
            const auto trayLines = controller.trayLines();
            if (entries.isEmpty()) {
                providerMenu->addAction("No provider data")->setEnabled(false);
            } else {
                for (int index = 0; index < entries.size(); ++index) {
                    const auto &item = entries.at(index);
                    const auto entry = item.toMap();
                    const auto action = providerMenu->addAction(trayLines.value(index, providerSummary(entry, quotaDisplay)));
                    action->setEnabled(!entry.value("failed").toBool());
                    QObject::connect(action, &QAction::triggered, &controller, [&controller] { controller.showWindow("usage"); });
                }
            }
        }
        if (controller.settings().value("trayStyle") == "icon") { tray.setIcon(QIcon(":/icon.svg")); return; }
        QVariantList windows;
        const auto shown = trayEntry(controller.entries(), controller.settings().value("trayIconSource").toString());
        if (!shown.isEmpty()) windows = shown.value("windows").toList();
        QPixmap pixmap(64, 64); pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        for (int index = 0; index < 2; ++index) {
            const QRectF track(5, 12 + index * 25, 54, 15);
            painter.setBrush(QColor("#777777")); painter.drawRoundedRect(track, 4, 4);
            if (index >= windows.size()) continue;
            const double remaining = qBound(0.0, windows[index].toMap().value("remaining").toDouble(), 100.0);
            const bool warning = controller.settings().value("warningColors").toBool() &&
                remaining <= controller.settings().value("notifyThreshold").toInt();
            painter.setBrush(controller.stale() ? QColor("#aaaaaa") : warning ? QColor("#e59642") : QApplication::palette().highlight().color());
            const double value = controller.settings().value("quotaDisplay") == "used" ? 100 - remaining : remaining;
            painter.drawRoundedRect(QRectF(track.x(), track.y(), track.width() * value / 100, track.height()), 4, 4);
        }
        painter.end(); tray.setIcon(QIcon(pixmap));
    };
    QObject::connect(&controller, &DesktopController::changed, &tray, updateTray);
    QObject::connect(&controller, &DesktopController::settingsChanged, &tray, updateTray);
    updateTray();
    if (command != "background") QTimer::singleShot(0, &controller, [&] { controller.showWindow(command); });
    return app.exec();
}
