#include "DesktopController.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <csignal>
#include <memory>
#include <unistd.h>

namespace {
QJSValue module(QJSEngine &engine, const QString &path, const QString &exports) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("Missing bundled model");
    auto result = engine.evaluate("(function(){\n" + QString::fromUtf8(file.readAll()) +
        "\nreturn {" + exports + "};})()", path);
    if (result.isError()) qFatal("Unable to load bundled model");
    return result;
}
void stop(QProcess &process) {
    if (process.state() == QProcess::NotRunning) return;
    const auto pid = process.processId();
    if (pid > 0) ::kill(-pid, SIGKILL);
    process.kill();
    process.waitForFinished(1000);
}
}

DesktopController::DesktopController(const QString &cliOverride, QObject *parent) : QObject(parent) {
    m_usageModel = module(m_engine, ":/Shared/Usage.js", "rows:rows, costs:costs, command:command, summary:summary, barSegments:barSegments, resetText:resetText, providerLines:providerLines");
    m_noticeModel = module(m_engine, ":/Shared/Notifications.js", "transition:transition, summary:summary");
    m_noticeState = m_engine.newObject();
    loadSettings(cliOverride);
    loadProviders();
    connect(&m_poll, &QTimer::timeout, this, &DesktopController::refresh);
    m_poll.start(m_settings.value("refreshSeconds").toInt() * 1000);
    connect(&m_clock, &QTimer::timeout, this, &DesktopController::changed);
    m_clock.start(30000);
    QTimer::singleShot(0, this, &DesktopController::refresh);
}

DesktopController::~DesktopController() {
    m_usage.disconnect(this); m_cost.disconnect(this);
    // The sessions process is stopped while the controller is still alive: its
    // completion handler would otherwise write into the already destroyed list.
    m_sessionProcess.disconnect(this);
    stop(m_usage); stop(m_cost); stop(m_catalog); stop(m_sessionProcess);
}

QJSValue DesktopController::call(const QJSValue &model, const QString &function, const QJSValueList &args) const {
    return model.property(function).call(args);
}

bool DesktopController::validate(QVariantMap &values) {
    if (!QRegularExpression("^[a-z0-9-]{1,80}$").match(values.value("provider").toString()).hasMatch()) {
        m_configError = "Enter a provider ID such as codex, claude, both, or enabled."; return false;
    }
    if (!QStringList{"auto", "oauth", "cli", "api", "web"}.contains(values.value("source").toString())) {
        m_configError = "Unsupported provider source."; return false;
    }
    if (values.value("executable").toString().trimmed().isEmpty()) {
        m_configError = "Specify the CodexBar CLI executable."; return false;
    }
    for (const auto &key : {"refreshSeconds", "accountIndex", "notifyThreshold", "barProviders"}) {
        bool ok = false;
        const int number = values.value(key).toInt(&ok);
        const int min = QString(key) == "refreshSeconds" ? 60 : QString(key) == "notifyThreshold" ? 1 : 0;
        const int max = QString(key) == "refreshSeconds" ? 3600 : QString(key) == "notifyThreshold" ? 99
            : QString(key) == "barProviders" ? 80 : 999;
        if (!ok || number < min || number > max) { m_configError = QString("Invalid %1.").arg(key); return false; }
        values[key] = number;
    }
    const auto order = values.value("providerOrder").toStringList();
    if (order.size() > 80 || (values.value("provider") == "custom" && order.isEmpty())) {
        m_configError = "Choose at least one provider (maximum 80)."; return false;
    }
    QStringList unique;
    for (const auto &id : order) {
        if (!QRegularExpression("^[a-z0-9-]{1,80}$").match(id).hasMatch() ||
            QStringList{"all", "both", "enabled", "custom"}.contains(id) || unique.contains(id)) {
            m_configError = "The provider list contains an invalid or duplicate ID."; return false;
        }
        unique.append(id);
    }
    values["providerOrder"] = unique;
    if (!QStringList{"remaining", "used"}.contains(values.value("quotaDisplay").toString()) ||
        !QStringList{"countdown", "absolute", "both"}.contains(values.value("resetDisplay").toString()) ||
        !QStringList{"meters", "icon"}.contains(values.value("trayStyle").toString()) ||
        !QStringList{"highest", "first"}.contains(values.value("trayIconSource").toString())) {
        m_configError = "Unsupported display preference."; return false;
    }
    for (const auto &key : {"allAccounts", "showIdentity", "showCosts", "showStatus", "notifications", "showTray", "compactQuickView", "refreshOnOpen", "showPace", "showBarDetail", "showScopedCaps", "warningColors", "followOmarchyTheme"})
        values[key] = values.value(key).toBool();
    return true;
}

void DesktopController::loadSettings(const QString &cliOverride) {
    m_settings = {{"executable", "codexbar"}, {"provider", "codex"}, {"source", "auto"},
        {"refreshSeconds", 300}, {"accountIndex", 0}, {"notifyThreshold", 10}, {"allAccounts", false},
        {"showIdentity", false}, {"showCosts", true}, {"showStatus", true}, {"notifications", false}, {"showTray", true}, {"compactQuickView", false}, {"refreshOnOpen", false}, {"providerOrder", QStringList{}},
        {"quotaDisplay", "remaining"}, {"resetDisplay", "countdown"}, {"showPace", true},
        {"showBarDetail", false}, {"showScopedCaps", false}, {"barProviders", 2},
        {"warningColors", true}, {"trayStyle", "meters"}, {"trayIconSource", "first"}, {"followOmarchyTheme", false}};
    m_configPath = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/codexbar/linux.json";
    QFile file(m_configPath);
    QVariantMap candidate = m_settings;
    if (file.open(QIODevice::ReadOnly)) {
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) {
            m_configBlocked = true;
            m_configError = "Invalid linux.json. Fix or remove it and restart CodexBar before saving.";
        } else {
            const auto loaded = document.object().toVariantMap();
            for (auto it = loaded.begin(); it != loaded.end(); ++it) if (candidate.contains(it.key())) candidate[it.key()] = it.value();
            if (validate(candidate)) m_settings = candidate;
            else m_configBlocked = true;
        }
    }
    if (file.exists() && !file.isOpen()) {
        m_configBlocked = true;
        m_configError = "Cannot read linux.json. Check permissions and restart CodexBar.";
    }
    if (!cliOverride.isEmpty()) m_settings["executable"] = cliOverride;
}

bool DesktopController::saveSettings(const QVariantMap &changes) {
    if (m_configBlocked) { emit settingsChanged(); return false; }
    QVariantMap next = m_settings;
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        if (!next.contains(it.key())) { m_configError = "Unknown setting: " + it.key(); emit settingsChanged(); return false; }
        next[it.key()] = it.value();
    }
    if (!validate(next)) { emit settingsChanged(); return false; }
    QDir().mkpath(QFileInfo(m_configPath).absolutePath());
    QSaveFile file(m_configPath);
    if (!file.open(QIODevice::WriteOnly)) { m_configError = "Cannot write application settings."; emit settingsChanged(); return false; }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto encoded = QJsonDocument::fromVariant(next).toJson(QJsonDocument::Indented);
    if (file.write(encoded) != encoded.size() || !file.commit()) {
        m_configError = "Cannot save application settings."; emit settingsChanged(); return false;
    }
    bool queryChanged = false;
    for (const auto &key : {"provider", "providerOrder", "source", "executable", "accountIndex", "allAccounts", "showIdentity", "showStatus", "showCosts"})
        queryChanged = queryChanged || next.value(key) != m_settings.value(key);
    m_settings = next; m_configError.clear();
    if (queryChanged) {
        ++m_generation;
        if (m_usage.state() == QProcess::NotRunning) { m_usageBatch = false; m_pendingProviders.clear(); }
        m_entries.clear(); m_spending.clear(); m_updated = 0; m_costUpdated = 0;
    }
    updateLabels();
    m_noticeState = m_engine.newObject();
    m_poll.start(m_settings.value("refreshSeconds").toInt() * 1000);
    emit settingsChanged(); emit changed(); if (queryChanged) refresh();
    return true;
}

bool DesktopController::stale() const {
    return m_updated > 0 && (!m_error.isEmpty() || QDateTime::currentMSecsSinceEpoch() - m_updated >
        qMax(600000, m_settings.value("refreshSeconds").toInt() * 2000));
}

QString DesktopController::updated() const {
    return m_updated ? QDateTime::fromMSecsSinceEpoch(m_updated).toLocalTime().toString("HH:mm") : QString();
}

void DesktopController::refresh() {
    if (busy()) return;
    m_usageBatch = true; m_batchFailed = false; m_batchEntries.clear();
    m_pendingProviders = m_settings.value("provider") == "custom" ? m_settings.value("providerOrder").toStringList() :
        QStringList{m_settings.value("provider").toString()};
    nextProvider();
}

void DesktopController::nextProvider() {
    if (m_usage.state() != QProcess::NotRunning) return;
    if (m_pendingProviders.isEmpty()) return;
    m_currentProvider = m_pendingProviders.takeFirst();
    auto selection = m_settings;
    selection["provider"] = m_currentProvider;
    if (m_settings.value("provider") == "custom") { selection["accountIndex"] = 0; selection["allAccounts"] = false; }
    auto args = call(m_usageModel, "command", {m_engine.toScriptValue(selection)}).toVariant().toStringList();
    // Process deadlines and groups are managed here, without a coreutils dependency.
    args = args.mid(3);
    probe(m_usage, args, false);
}

void DesktopController::refreshCosts() {
    if (costBusy() || !m_settings.value("showCosts").toBool()) return;
    probe(m_cost, {m_settings.value("executable").toString(), "cost", "--provider", "both", "--format", "json", "--days", "30"}, true);
}

void DesktopController::refreshSessions(bool force) {
    if (m_sessionProcess.state() != QProcess::NotRunning) return;
    // A finished scan keeps its handlers installed; drop them so the next scan
    // does not read into the completed buffer.
    m_sessionProcess.disconnect(this);
    const auto command = QStringList{m_settings.value("executable").toString(), "sessions", "--json-v2"};
    auto output = std::make_shared<QByteArray>();
    auto complete = std::make_shared<bool>(false);
    const int generation = m_generation;
    auto *deadline = new QTimer(&m_sessionProcess);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, &m_sessionProcess, [this] { stop(m_sessionProcess); });
    auto finish = [this, output, complete, generation, deadline](bool failed) {
        if (*complete) return;
        *complete = true; deadline->stop(); deadline->deleteLater();
        if (generation != m_generation) return;
        output->append(m_sessionProcess.readAllStandardOutput());
        QVariantList sessions;
        bool parsed = false;
        if (!failed && output->size() <= 4 * 1024 * 1024) {
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(*output, &parse);
            if (parse.error == QJsonParseError::NoError && document.isArray()) {
                parsed = true;
                for (const auto &item : document.array()) {
                    if (!item.isObject()) continue;
                    const auto session = item.toObject();
                    sessions.append(QVariantMap{
                        {"id", session.value("id").toString()},
                        {"provider", session.value("provider").toString()},
                        {"state", session.value("state").toString()},
                        {"project", session.value("projectName").toString()},
                        {"cwd", session.value("cwd").toString()},
                        {"pid", session.value("pid").toInt()},
                        {"lastActivity", session.value("lastActivityAt").toString()}});
                }
            }
        }
        if (parsed) {
            // A valid empty list is a real answer; a failed scan keeps the last
            // known list and reports why it could not be refreshed.
            m_sessions = sessions;
            m_sessionsError.clear();
            m_sessionsUpdated = QDateTime::currentMSecsSinceEpoch();
        } else if (m_sessionsError.isEmpty()) {
            m_sessionsError = "Agent sessions could not be refreshed. The last known list is shown.";
        }
        emit changed();
    };
    connect(&m_sessionProcess, &QProcess::readyReadStandardOutput, this, [this, output] {
        output->append(m_sessionProcess.readAllStandardOutput());
        if (output->size() > 4 * 1024 * 1024) stop(m_sessionProcess);
    });
    connect(&m_sessionProcess, &QProcess::readyReadStandardError, this, [this] { m_sessionProcess.readAllStandardError(); });
    connect(&m_sessionProcess, &QProcess::finished, this, [finish](int, QProcess::ExitStatus status) { finish(status != QProcess::NormalExit); });
    connect(&m_sessionProcess, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(true);
    });
    m_sessionProcess.setChildProcessModifier([] { ::setsid(); });
    m_sessionProcess.start(command.first(), command.mid(1));
    deadline->start(30000);
}


// How much of the window is used when it resets, assuming the current rate.
// Returns a negative value when the window cannot be projected.
static double projectedUsage(const QVariantMap &window) {
    const auto minutes = window.value("minutes").toInt();
    if (minutes <= 0) return -1;
    const auto resetsAt = window.value("resetsAt").toString();
    const auto reset = QDateTime::fromString(resetsAt, Qt::ISODate);
    if (!reset.isValid()) return -1;
    const auto now = QDateTime::currentDateTimeUtc();
    if (now >= reset) return -1;
    const auto start = reset.addSecs(-minutes * 60);
    const auto elapsed = start.secsTo(now);
    // The window has to have started before it can be projected.
    if (elapsed <= 0) return -1;
    const auto used = 100.0 - window.value("remaining").toDouble();
    const auto left = now.secsTo(reset);
    if (left <= 0) return -1;
    const auto projected = used + (used / elapsed) * left;
    return qBound(0.0, projected, 200.0);
}

// How far through the working days a weekly window is, in percent. Only the
// working days count towards the expected burn.
static double workdayProgress(const QVariantMap &window, int workDays) {
    if (workDays <= 0) return -1;
    const auto minutes = window.value("minutes").toInt();
    if (minutes < 7 * 24 * 60) return -1;
    const auto resetsAt = window.value("resetsAt").toString();
    const auto reset = QDateTime::fromString(resetsAt, Qt::ISODate);
    if (!reset.isValid()) return -1;
    const auto start = reset.addSecs(-minutes * 60);
    const auto now = QDateTime::currentDateTimeUtc();
    double total = 0;
    double elapsed = 0;
    for (auto day = start; day < reset; day = day.addDays(1)) {
        if (static_cast<int>(day.date().dayOfWeek()) > workDays) continue;
        total += 1;
        if (day <= now) elapsed += 1;
    }
    if (total <= 0) return -1;
    return qBound(0.0, elapsed / total * 100.0, 100.0);
}

void DesktopController::probe(QProcess &process, const QStringList &command, bool cost) {
    process.disconnect(this);
    auto output = std::make_shared<QByteArray>();
    auto complete = std::make_shared<bool>(false);
    const int generation = m_generation;
    auto *deadline = new QTimer(&process);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, &process, [&process] { stop(process); });
    auto finish = [this, &process, output, complete, generation, cost, deadline](bool failed) {
        if (*complete) return;
        *complete = true; deadline->stop(); deadline->deleteLater();
        if (generation != m_generation) {
            if (!cost) { m_usageBatch = false; m_pendingProviders.clear(); }
            QTimer::singleShot(0, this, cost ? &DesktopController::refreshCosts : &DesktopController::refresh);
            return;
        }
        output->append(process.readAllStandardOutput());
        QJSValueList arguments{QString::fromUtf8(*output)};
        if (!cost) arguments.append(m_settings.value("showIdentity").toBool());
        const auto parsed = call(m_usageModel, cost ? "costs" : "rows", arguments);
        const bool invalid = failed || parsed.isError() || !parsed.isArray() || output->size() > 8 * 1024 * 1024;
        if (cost) {
            if (invalid) m_costError = "Local spending could not be refreshed. Previous data may be stale.";
            else { m_spending = parsed.toVariant().toList(); m_costError.clear(); m_costUpdated = QDateTime::currentMSecsSinceEpoch(); }
        } else {
            const bool custom = m_settings.value("provider") == "custom";
            auto rows = invalid ? QVariantList{} : parsed.toVariant().toList();
            // A CLI that does not recognise the requested ID may answer for other providers; never
            // show their usage under this one.
            if (custom)
                rows.erase(std::remove_if(rows.begin(), rows.end(), [this](const QVariant &row) {
                    return row.toMap().value("provider") != m_currentProvider; }), rows.end());
            if (invalid || (custom && rows.isEmpty())) {
                m_batchFailed = true;
                for (const auto &entry : m_entries) {
                    if (!custom || entry.toMap().value("provider") == m_currentProvider)
                        m_batchEntries.append(entry);
                }
                if (custom && !std::any_of(m_batchEntries.begin(), m_batchEntries.end(),
                    [this](const QVariant &entry) { return entry.toMap().value("provider") == m_currentProvider; }))
                    m_batchEntries.append(QVariantMap{{"provider", m_currentProvider}, {"windows", QVariantList{}},
                        {"error", "Usage unavailable"}, {"failed", true}});
            } else m_batchEntries.append(rows);
            if (!m_pendingProviders.isEmpty()) {
                QTimer::singleShot(0, this, &DesktopController::nextProvider);
                return;
            }
            m_usageBatch = false;
            m_entries = m_batchEntries;
            updateLabels();
            if (m_batchFailed) {
                m_error = "Some usage could not be refreshed. Previous results may be out of date.";
                m_noticeState = m_engine.newObject();
            } else {
                m_error.clear(); m_updated = QDateTime::currentMSecsSinceEpoch(); updateNotifications(m_entries);
            }
        }
        emit changed();
    };
    connect(&process, &QProcess::readyReadStandardOutput, this, [&process, output] {
        output->append(process.readAllStandardOutput());
        if (output->size() > 8 * 1024 * 1024) stop(process);
    });
    connect(&process, &QProcess::readyReadStandardError, this, [&process] { process.readAllStandardError(); });
    connect(&process, &QProcess::finished, this, [finish](int, QProcess::ExitStatus status) { finish(status != QProcess::NormalExit); });
    connect(&process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(true);
    });
    process.setChildProcessModifier([] { ::setsid(); });
    process.start(command.first(), command.mid(1));
    deadline->start(60000);
    emit changed();
}

QVariantMap DesktopController::overview() {
    // The dashboard summary: how much is tracked, what is closest to its limit
    // and when the next window resets.
    QVariantMap summary{{"providers", 0}, {"highest", 0.0}, {"credits", QVariant()},
        {"nextReset", ""}, {"nextLabel", ""}, {"spend", QVariant()}, {"tokens", 0}};
    qint64 nextReset = 0;
    double credits = 0;
    bool hasCredits = false;
    double highest = 0;
    int providers = 0;
    for (const auto &item : m_entries) {
        const auto entry = item.toMap();
        if (entry.value("failed").toBool()) continue;
        providers += 1;
        const auto creditsValue = entry.value("credits");
        if (creditsValue.isValid() && !creditsValue.isNull()) {
            credits += creditsValue.toDouble();
            hasCredits = true;
        }
        for (const auto &windowItem : entry.value("windows").toList()) {
            const auto window = windowItem.toMap();
            highest = qMax(highest, 100.0 - qBound(0.0, window.value("remaining").toDouble(), 100.0));
            const auto resetsAt = window.value("resetsAt").toString();
            const auto reset = QDateTime::fromString(resetsAt, Qt::ISODate);
            if (!resetsAt.isEmpty() && reset.isValid() && (nextReset == 0 || reset.toMSecsSinceEpoch() < nextReset)) {
                nextReset = reset.toMSecsSinceEpoch();
                // Keep the original string so the reset helper can parse it again.
                summary["nextReset"] = resetsAt;
                summary["nextLabel"] = window.value("label").toString();
            }
        }
    }
    double spend = 0;
    bool hasSpend = false;
    double tokens = 0;
    for (const auto &item : m_spending) {
        const auto row = item.toMap();
        const auto month = row.value("month");
        if (month.isValid() && !month.isNull()) {
            spend += month.toDouble();
            hasSpend = true;
        }
        const auto rowTokens = row.value("tokens");
        if (rowTokens.isValid() && !rowTokens.isNull()) tokens += rowTokens.toDouble();
    }
    summary["providers"] = providers;
    summary["highest"] = highest;
    summary["credits"] = hasCredits ? QVariant(credits) : QVariant();
    summary["spend"] = hasSpend ? QVariant(spend) : QVariant();
    summary["tokens"] = tokens;
    return summary;
}

QStringList DesktopController::trayLines() {
    QStringList lines;
    const auto value = call(m_usageModel, "providerLines",
        {m_engine.toScriptValue(m_entries), m_settings.value("quotaDisplay").toString()});
    if (value.isError() || !value.isArray()) return lines;
    for (const auto &line : value.toVariant().toList())
        lines.append(line.toString());
    return lines;
}

void DesktopController::updateNotifications(const QVariantList &entries) {
    const auto result = call(m_noticeModel, "transition", {m_noticeState, m_engine.toScriptValue(entries),
        m_settings.value("notifyThreshold").toInt()});
    if (result.isError()) return;
    m_noticeState = result.property("state");
    if (!m_settings.value("notifications").toBool()) return;
    for (const auto &item : result.property("events").toVariant().toList()) {
        auto event = item.toMap();
        auto message = QDBusMessage::createMethodCall("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications", "Notify");
        message.setArguments({"CodexBar", uint(0), "codexbar", "CodexBar · " + event.value("provider").toString(),
            event.value("message").toString(), QStringList(), QVariantMap(), 8000});
        QDBusConnection::sessionBus().asyncCall(message);
    }
}

void DesktopController::copySummary() {
    QGuiApplication::clipboard()->setText(call(m_noticeModel, "summary", {m_engine.toScriptValue(m_entries)}).toString());
}

void DesktopController::showWindow(const QString &page) {
    if (page == "spending" && QDateTime::currentMSecsSinceEpoch() - m_costUpdated > 300000) refreshCosts();
    // Agent sessions are opt-in: they are only listed when the tab is opened.
    if (page == "sessions" && QDateTime::currentMSecsSinceEpoch() - m_sessionsUpdated > 60000) refreshSessions(true);
    if ((page == "usage" || page == "quick-view") && m_settings.value("refreshOnOpen").toBool()) refresh();
    emit windowRequested(page);
}

void DesktopController::updateLabels() {
    const auto rows = m_engine.toScriptValue(m_entries);
    const auto mode = m_settings.value("quotaDisplay").toString();
    m_summary = call(m_usageModel, "summary", {rows, mode}).toString();
    // The bar keeps its single leading percentage until these are switched on, so an upgrade
    // changes nothing; the limit is display only and never stops a provider being polled.
    auto options = m_engine.newObject();
    options.setProperty("detail", m_settings.value("showBarDetail").toBool());
    options.setProperty("pace", m_settings.value("showPace").toBool());
    options.setProperty("scopedCaps", m_settings.value("showScopedCaps").toBool());
    options.setProperty("maxProviders", m_settings.value("barProviders").toInt());
    m_barEntries = QJsonArray::fromVariantList(
        call(m_usageModel, "barSegments", {rows, mode, options}).toVariant().toList());
}

QJsonObject DesktopController::snapshot() {
    static const auto extraKeySalt = QUuid::createUuid().toRfc4122();
    QJsonArray compact;
    for (const auto &entry : m_entries) {
        auto row = entry.toMap();
        QJsonArray windows;
        for (const auto &item : row.value("windows").toList()) {
            auto window = item.toMap();
            const auto key = window.value("key").toString();
            if (key.startsWith("extra:")) {
                // Keep provider-controlled IDs opaque over IPC and stable within this backend process.
                const auto identity = row.value("provider").toString() + "/" + key;
                window["key"] = "extra:" + QString::fromLatin1(
                    QCryptographicHash::hash(extraKeySalt + identity.toUtf8(), QCryptographicHash::Sha256).toHex());
            }
            const auto remaining = window.value("remaining").toDouble();
            window["displayValue"] = m_settings.value("quotaDisplay") == "used" ? 100 - remaining : remaining;
            window["displaySuffix"] = m_settings.value("quotaDisplay") == "used" ? "used" : "left";
            window["warning"] = m_settings.value("warningColors").toBool() && remaining <= m_settings.value("notifyThreshold").toInt();
            // The macOS app projects the usage at reset and marks the working-day
            // progress of a weekly window; both are derived from the window itself.
            const auto projection = projectedUsage(window);
            window["projected"] = projection;
            window["workdayPercent"] = workdayProgress(window, m_settings.value("weeklyProgressWorkDays").toInt());
            // This model contains no identity; display fields keep adapters consistent with native windows.
            window["resetText"] = call(m_usageModel, "resetText",
                {window.value("resetsAt").toString(), double(QDateTime::currentMSecsSinceEpoch()), m_settings.value("resetDisplay").toString()}).toString();
            windows.append(QJsonObject::fromVariantMap(window));
        }
        // Plan, status and balances carry no account identity; accountLabel stays out of IPC.
        compact.append(QJsonObject{{"provider", row.value("provider").toString()},
            {"windows", windows},
            {"error", row.value("error").toString()},
            {"plan", row.value("plan").toString()},
            {"status", row.value("status").toString()},
            {"statusLevel", row.value("statusLevel").toString()},
            {"updatedAt", row.value("updatedAt").toString()},
            {"credits", QJsonValue::fromVariant(row.value("credits"))},
            {"extraUsage", QJsonValue::fromVariant(row.value("extraUsage"))},
            {"resetCredits", QJsonValue::fromVariant(row.value("resetCredits"))}});
    }
    QJsonArray sessions;
    for (const auto &item : m_sessions) {
        const auto session = item.toMap();
        // Session rows carry project names and paths, never account identity.
        sessions.append(QJsonObject{{"id", session.value("id").toString()},
            {"provider", session.value("provider").toString()},
            {"state", session.value("state").toString()},
            {"project", session.value("project").toString()},
            {"cwd", session.value("cwd").toString()},
            {"pid", session.value("pid").toInt()},
            {"lastActivity", session.value("lastActivity").toString()}});
    }
    QJsonArray spending;
    for (const auto &item : m_spending) {
        const auto row = item.toMap();
        spending.append(QJsonObject{{"provider", row.value("provider").toString()},
            {"today", QJsonValue::fromVariant(row.value("today"))},
            {"month", QJsonValue::fromVariant(row.value("month"))},
            {"tokens", QJsonValue::fromVariant(row.value("tokens"))},
            {"provenance", row.value("provenance").toString()},
            {"error", row.value("error").toString()}});
    }
    return {{"schemaVersion", 1}, {"pid", QCoreApplication::applicationPid()}, {"summary", m_summary}, {"barEntries", m_barEntries},
        {"entries", compact}, {"sessions", sessions}, {"sessionsError", m_sessionsError},
        {"trayLines", QJsonArray::fromStringList(trayLines())}, {"overview", QJsonObject::fromVariantMap(overview())},
        {"trayIconSource", m_settings.value("trayIconSource").toString()}, {"overview", QJsonObject::fromVariantMap(overview())},
        {"trayIconSource", m_settings.value("trayIconSource").toString()}, {"quotaDisplay", m_settings.value("quotaDisplay").toString()}, {"busy", busy()}, {"stale", stale()}, {"error", m_error},
        {"updated", updated()}, {"costBusy", costBusy()}, {"costProviders", m_spending.size()}, {"costError", m_costError}, {"spending", spending}};
}

bool DesktopController::listen(const QString &socketPath) {
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(socketPath); // Caller holds the exclusive application lock.
    if (!m_server.listen(socketPath)) return false;
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (auto *socket = m_server.nextPendingConnection()) {
            socket->setReadBufferSize(65537);
            auto input = std::make_shared<QByteArray>();
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(3000, socket, [socket] { socket->disconnectFromServer(); });
            connect(socket, &QLocalSocket::readyRead, this, [this, socket, input] {
                input->append(socket->readAll());
                if (input->size() > 65536) { socket->disconnectFromServer(); return; }
                if (!input->contains('\n')) return;
                const auto request = QJsonDocument::fromJson(input->left(input->indexOf('\n'))).object();
                const auto command = request.value("command").toString();
                QJsonObject response{{"ok", true}};
                if (command == "snapshot" || command == "background") {
                    // Panel adapters may opt in to spending without opening the window; reuse its five-minute
                    // cache, and after a failed scan retry at most every few seconds so polling stays bounded.
                    const auto now = QDateTime::currentMSecsSinceEpoch();
                    if (request.value("spending").toBool() && !costBusy() && now - m_costUpdated > 300000 &&
                        (m_costError.isEmpty() || now - m_costRequested > 5000)) {
                        m_costRequested = now;
                        refreshCosts();
                    }
                    response = snapshot();
                }
                else if (command == "autostart") {
                    const auto action = request.value("action").toString();
                    const bool ok = action == "status" || ((action == "enable" || action == "disable") && setLaunchAtLogin(action == "enable"));
                    response = {{"ok", ok}, {"enabled", launchAtLogin()}};
                }
                else if (command == "refresh") { refresh(); refreshCosts(); }
                else if (command == "refresh-sessions") { refreshSessions(true); response = {{"ok", true}}; }
                else if (command == "refresh-sessions") { refreshSessions(true); response = {{"ok", true}}; }
                else if (command == "settings" || command == "usage" || command == "spending" || command == "sessions" || command == "quick-view") showWindow(command);
                else if (command == "configure" && request.value("settings").isObject()) {
                    const bool ok = saveSettings(request.value("settings").toObject().toVariantMap());
                    response = {{"ok", ok}, {"error", m_configError}};
                } else if (command == "quit") QTimer::singleShot(100, qApp, &QCoreApplication::quit);
                else response = {{"ok", false}, {"error", "Unknown command"}};
                socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
                socket->disconnectFromServer();
            });
        }
    });
    return true;
}

void DesktopController::loadProviders() {
    for (const auto &id : {"codex", "claude", "cursor", "gemini", "copilot", "antigravity"})
        m_providers.append(QVariantMap{{"provider", id}, {"displayName", QString(id)}});
    auto output = std::make_shared<QByteArray>();
    auto *deadline = new QTimer(&m_catalog);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, &m_catalog, [this] { stop(m_catalog); });
    connect(&m_catalog, &QProcess::readyReadStandardOutput, this, [this, output] {
        output->append(m_catalog.readAllStandardOutput());
        if (output->size() > 1024 * 1024) stop(m_catalog);
    });
    connect(&m_catalog, &QProcess::readyReadStandardError, this, [this] { m_catalog.readAllStandardError(); });
    connect(&m_catalog, &QProcess::finished, this, [this, output, deadline](int code, QProcess::ExitStatus status) {
        deadline->stop();
        if (code || status != QProcess::NormalExit || output->size() > 1024 * 1024) return;
        const auto document = QJsonDocument::fromJson(*output);
        if (!document.isArray()) return;
        QVariantList providers;
        for (const auto &item : document.array()) {
            const auto row = item.toObject();
            const auto id = row.value("provider").toString();
            const auto name = row.value("displayName").toString();
            if (QRegularExpression("^[a-z0-9-]{1,80}$").match(id).hasMatch() && !name.isEmpty())
                providers.append(QVariantMap{{"provider", id}, {"displayName", name.left(100)}});
        }
        if (!providers.isEmpty()) { m_providers = providers; emit settingsChanged(); }
    });
    m_catalog.setChildProcessModifier([] { ::setsid(); });
    m_catalog.start(m_settings.value("executable").toString(), {"config", "providers", "--format", "json"});
    deadline->start(10000);
}

bool DesktopController::launchAtLogin() const {
    QFile file(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
        "/autostart/com.steipete.CodexBar.desktop");
    if (!file.open(QIODevice::ReadOnly)) return false;
    return !QString::fromUtf8(file.readAll()).contains(QRegularExpression("(^|\n)Hidden\\s*=\\s*true(\n|$)"));
}

bool DesktopController::setLaunchAtLogin(bool enabled) {
    const auto path = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
        "/autostart/com.steipete.CodexBar.desktop";
    QDir().mkpath(QFileInfo(path).absolutePath());
    QString executable = QCoreApplication::applicationFilePath();
    if (executable.contains('\n') || executable.contains('\r')) return false;
    executable.replace("%", "%%");
    for (const auto &character : {QString("\\"), QString("\""), QString("`"), QString("$")})
        executable.replace(character, "\\" + character);
    const auto content = QString("[Desktop Entry]\nType=Application\nName=CodexBar\nExec=\"%1\" --background\nIcon=codexbar\nTerminal=false\nHidden=%2\n")
        .arg(executable, enabled ? "false" : "true").toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
        m_configError = "Cannot update login startup."; emit settingsChanged(); return false;
    }
    m_configError.clear(); emit settingsChanged(); return true;
}

bool DesktopController::accountAction(const QString &provider, const QString &action) {
    if (!QStringList{"codex", "claude"}.contains(provider) || !QStringList{"login", "logout"}.contains(action)) return false;
    const auto terminal = QStandardPaths::findExecutable("xdg-terminal-exec");
    const auto cli = QStandardPaths::findExecutable(provider);
    if (terminal.isEmpty() || cli.isEmpty()) {
        m_configError = "Install xdg-terminal-exec and the provider CLI to manage sign-in.";
        emit settingsChanged(); return false;
    }
    QStringList arguments{"--hold", "--", cli};
    if (provider == "claude") arguments.append("auth");
    arguments.append(action);
    if (!QProcess::startDetached(terminal, arguments)) {
        m_configError = "Could not open the sign-in terminal."; emit settingsChanged(); return false;
    }
    m_configError.clear(); emit settingsChanged(); return true;
}
