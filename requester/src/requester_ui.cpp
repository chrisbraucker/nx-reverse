#include "requester_ui.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <borealis.hpp>
#include <borealis/core/touch/scroll_gesture.hpp>

#include "bsd_system_udp_scenario.hpp"
#include "logger.hpp"
#include "nxrv/build_info.hpp"
#include "runtime_config.hpp"
#include "scenarios.hpp"
#include "wgnx/client.hpp"
#include "wgnx/protocol.hpp"
#include "wgnx/tunnel_protocol.hpp"
#include "wgnx_tunnel_scenario.hpp"

namespace requester {

namespace {

constexpr float PagePadding = 24.0F;
constexpr float MainLogPanelHeight = 220.0F;
constexpr float ScenarioLogPanelHeight = 170.0F;
constexpr std::size_t MainLogPanelLines = 12;
constexpr float OffsetTolerance = 1.0F;
constexpr float SettingsActionGap = 18.0F;
constexpr float SettingsSectionGap = 24.0F;
constexpr float RequesterSidebarWidth = 250.0F;
constexpr float LogFontSize = 14.0F;
constexpr char ProjectUrl[] = APP_PROJECT_URL;
constexpr int TunnelDataPathIndex = 0;
constexpr int BsdSystemDataPathIndex = 1;
constexpr int BsdExpectedNormalIndex = 0;
constexpr int BsdExpectedNoReplyTimeoutIndex = 1;
constexpr int BsdExpectedTerminalClosureIndex = 2;

int BsdExpectedOutcomeIndex(const BsdSystemUdpExpectedOutcome expected_outcome) {
    switch (expected_outcome) {
    case BsdSystemUdpExpectedOutcome::NoReplyTimeout:
        return BsdExpectedNoReplyTimeoutIndex;
    case BsdSystemUdpExpectedOutcome::TerminalClosure:
        return BsdExpectedTerminalClosureIndex;
    case BsdSystemUdpExpectedOutcome::EchoReply:
        return BsdExpectedNormalIndex;
    }
    return BsdExpectedNormalIndex;
}

class LogPanel;

struct MainPageBindings {
    brls::Label* scenario_label{};
    brls::Label* status_label{};
    brls::Button* run_button{};
    LogPanel* log_panel{};
};

struct ScenarioPageBindings {
    brls::Button* run_button{};
    brls::Label* status_label{};
    LogPanel* log_panel{};
};

struct UiModel {
    AppContext* context{};
    RuntimeConfig defaults;
    RuntimeConfig config;
    std::string configuration_source;
    MainPageBindings main;
    ScenarioPageBindings scenario;
    LogPanel* logs_panel{};
    std::string run_status{"Ready to run the configured workload."};
    bool active{true};
    bool run_active{false};
};

std::string FormatRuntimeConfig(const RuntimeConfig& config) {
    const auto& tunnel = config.tunnel_udp;
    const char* const data_path = config.bsd_system_udp.enabled ? "bsd:s" : "Tunnel";
    return "destination " + tunnel.destination_ipv4 + ":" + std::to_string(tunnel.destination_port) + " | payload " +
           std::to_string(tunnel.payload_bytes) + " B | datagrams " + std::to_string(tunnel.datagram_count) + " | flows " +
           std::to_string(tunnel.concurrent_flows) + " | data path " + data_path +
           (config.bsd_system_udp.enabled
                ? " | BSD outcome " + std::string(BsdSystemUdpExpectedOutcomeName(config.bsd_system_udp.expected_outcome))
                : "") +
           " | contract " + (config.tunnel_contract.enabled ? "enabled" : "disabled");
}

std::string FormatRecentLines(std::size_t maximum_line_count = 0) {
    const std::vector<std::string> lines = logger::RecentLines(maximum_line_count);
    std::string rendered;
    for (const std::string& line : lines) {
        if (!rendered.empty()) {
            rendered += '\n';
        }
        rendered += line;
    }
    return rendered.empty() ? "No requester activity yet." : rendered;
}

class LogPanel final : public brls::ScrollingFrame {
  public:
    explicit LogPanel(std::size_t maximum_line_count = 0) : maximum_line_count_(maximum_line_count) {
        label_ = new brls::Label();
        label_->setWidthPercentage(100.0F);
        label_->setShrink(.8F);
        label_->setFontSize(LogFontSize);
        label_->setSingleLine(false);
        label_->setAutoAnimate(false);
        label_->setText(FormatRecentLines(maximum_line_count_));
        setContentView(label_);
        addGestureRecognizer(new brls::ScrollGestureRecognizer(
            [this](brls::PanGestureStatus state, brls::Sound*) {
                if (state.state != brls::GestureState::FAILED && state.state != brls::GestureState::UNSURE &&
                    state.state != brls::GestureState::INTERRUPTED) {
                    auto_scroll_ = false;
                }
            },
            brls::PanAxis::VERTICAL
        ));
    }

    void willAppear(bool reset_state) override {
        brls::ScrollingFrame::willAppear(reset_state);
        Refresh(true);
    }

    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* current_view) override {
        if (direction == brls::FocusDirection::UP || direction == brls::FocusDirection::DOWN) {
            auto_scroll_ = false;
        }
        return brls::ScrollingFrame::getNextFocus(direction, current_view);
    }

    void Refresh(bool reset_auto_scroll = false) {
        if (cleared_) {
            return;
        }
        const float current_offset = getContentOffsetY();
        if (reset_auto_scroll) {
            auto_scroll_ = true;
        } else if (auto_scroll_ && std::fabs(current_offset - last_automatic_offset_) > OffsetTolerance) {
            auto_scroll_ = false;
        }

        label_->setText(FormatRecentLines(maximum_line_count_));
        if (auto_scroll_) {
            setContentOffsetY(std::numeric_limits<float>::max(), false);
            last_automatic_offset_ = getContentOffsetY();
        }
    }

    void Clear() {
        cleared_ = true;
        label_->setText("Log output cleared.");
    }

    void ShowRecent() {
        cleared_ = false;
        Refresh(true);
    }

  private:
    brls::Label* label_{};
    std::size_t maximum_line_count_{};
    float last_automatic_offset_{};
    bool auto_scroll_{true};
    bool cleared_{false};
};

void RefreshLogViews(const std::shared_ptr<UiModel>& model) {
    if (!model->active) {
        return;
    }
    if (model->main.log_panel != nullptr) {
        model->main.log_panel->Refresh();
    }
    if (model->logs_panel != nullptr) {
        model->logs_panel->Refresh();
    }
    if (model->scenario.log_panel != nullptr) {
        model->scenario.log_panel->Refresh();
    }
}

void RefreshScenarioPage(const std::shared_ptr<UiModel>& model) {
    if (!model->active) {
        return;
    }
    if (model->scenario.status_label != nullptr) {
        model->scenario.status_label->setText(model->run_status);
    }
    if (model->scenario.run_button != nullptr) {
        model->scenario.run_button->setState(model->run_active ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
    }
}

void RefreshMainPage(const std::shared_ptr<UiModel>& model) {
    if (!model->active) {
        return;
    }
    if (model->main.scenario_label != nullptr) {
        model->main.scenario_label->setText(FormatRuntimeConfig(model->config));
    }
    if (model->main.status_label != nullptr) {
        model->main.status_label->setText(model->run_status);
    }
    if (model->main.run_button != nullptr) {
        model->main.run_button->setState(model->run_active ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
    }
}

void RequestUdpBindBump(const std::shared_ptr<UiModel>& model) {
    AppContext& context = *model->context;
    if (!wgnx::client::IsServiceRunning()) {
        logger::Status(context, "UDP bind bump rejected: wgnx:ctl is not running");
        brls::Application::notify("WireGuard sysmodule is not running");
        RefreshLogViews(model);
        return;
    }

    logger::Status(context, "UDP bind bump requested via ZR");
    const Result rc = wgnx::client::BumpUdpBinding();
    if (R_FAILED(rc)) {
        logger::Status(context, "UDP bind bump failed rc=%s", FormatResult(rc).c_str());
        brls::Application::notify("UDP bind bump failed");
    } else {
        logger::Status(context, "UDP bind bump queued");
        brls::Application::notify("UDP bind bump queued");
    }
    RefreshLogViews(model);
}

template <typename T> bool AssignUnsigned(long input, T* target, T maximum, const char* name, AppContext& context) {
    if (input < 0 || static_cast<unsigned long long>(input) > static_cast<unsigned long long>(maximum)) {
        logger::Status(context, "configuration edit rejected field=%s value=%ld", name, input);
        brls::Application::notify("Invalid value for " + std::string(name));
        return false;
    }
    *target = static_cast<T>(input);
    return true;
}

class SettingsPage final : public brls::ScrollingFrame {
  public:
    explicit SettingsPage(std::shared_ptr<UiModel> model) : model_(std::move(model)) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(PagePadding);
        content->setWidthPercentage(100.0F);
        setContentView(content);

        AddHeader(content, "UDP Workload", "Configure shared traffic and choose the data path.");
        data_path_ = AddDataPathSelector(content);
        datagram_count_ = AddU32(content, "Datagram count", &model_->config.tunnel_udp.datagram_count, 4096, "tunnel_udp.datagram_count");
        pacing_ms_ = AddU32(content, "Pacing milliseconds", &model_->config.tunnel_udp.pacing_ms, 60000, "tunnel_udp.pacing_ms");
        concurrent_flows_ = AddU32(
            content,
            "Concurrent flows",
            &model_->config.tunnel_udp.concurrent_flows,
            wgnx::tunnel::MaximumFlowsPerClient,
            "tunnel_udp.concurrent_flows"
        );
        echo_replies_ = AddBoolean(content, "Echo replies", &model_->config.tunnel_udp.echo_replies);

        AddHeader(content, "BSD:S Contract Validation", "Extra checks for the requester-only transparent UDP path.", SettingsSectionGap);
        verify_bsd_post_route_rejection_ =
            AddBoolean(content, "Verify tunneled socket option rejection", &model_->config.bsd_system_udp.verify_post_route_rejection);
        expected_bsd_outcome_ = new brls::SelectorCell();
        expected_bsd_outcome_->init(
            "Expected BSD:S outcome",
            {"Normal workload", "No-reply timeout", "Terminal closure"},
            BsdExpectedOutcomeIndex(model_->config.bsd_system_udp.expected_outcome),
            [model = model_](int next) {
                switch (next) {
                case BsdExpectedNoReplyTimeoutIndex:
                    model->config.bsd_system_udp.expected_outcome = BsdSystemUdpExpectedOutcome::NoReplyTimeout;
                    break;
                case BsdExpectedTerminalClosureIndex:
                    model->config.bsd_system_udp.expected_outcome = BsdSystemUdpExpectedOutcome::TerminalClosure;
                    break;
                default:
                    model->config.bsd_system_udp.expected_outcome = BsdSystemUdpExpectedOutcome::EchoReply;
                    break;
                }
            }
        );
        content->addView(expected_bsd_outcome_);
        require_bsd_writable_recovery_ =
            AddBoolean(content, "Require writable recovery after queue pressure", &model_->config.bsd_system_udp.require_writable_recovery);

        AddHeader(content, "Tunnel Contract Validation", "Clone lifetime and mixed-batch API coverage.", SettingsSectionGap);
        contract_enabled_ = new brls::BooleanCell();
        contract_enabled_->init("Run contract validation", model_->config.tunnel_contract.enabled, [this](bool next) {
            model_->config.tunnel_contract.enabled = next;
            UpdateContractValidationAppearance();
        });
        content->addView(contract_enabled_);
        verify_cloned_session_lifetime_ =
            AddBoolean(content, "Verify cloned session lifetime", &model_->config.tunnel_contract.verify_cloned_session_lifetime);
        verify_mixed_batch_ = AddBoolean(content, "Verify mixed batch dispositions", &model_->config.tunnel_contract.verify_mixed_batch);
        UpdateContractValidationAppearance();

        AddHeader(content, "Common Options", "", SettingsSectionGap);

        auto* shared_settings = new brls::Box(brls::Axis::COLUMN);
        shared_settings->setWidthPercentage(100.0F);
        content->addView(shared_settings);
        destination_ipv4_ = AddText(shared_settings, "Shared destination IPv4", &model_->config.tunnel_udp.destination_ipv4);
        destination_port_ =
            AddU16(shared_settings, "Shared destination port", &model_->config.tunnel_udp.destination_port, "tunnel_udp.destination_port");
        workload_id_ = AddU32(
            shared_settings,
            "Shared workload ID",
            &model_->config.tunnel_udp.workload_id,
            std::numeric_limits<std::uint32_t>::max(),
            "tunnel_udp.workload_id"
        );
        payload_bytes_ = AddSize(
            shared_settings,
            "Shared payload bytes",
            &model_->config.tunnel_udp.payload_bytes,
            wgnx::tunnel::MaximumUdpPayloadStorageBytes,
            "tunnel_udp.payload_bytes"
        );
        receive_deadline_ms_ = AddU32(
            shared_settings,
            "Shared receive deadline milliseconds",
            &model_->config.tunnel_udp.receive_deadline_ms,
            60000,
            "tunnel_udp.receive_deadline_ms"
        );
        payload_seed_ = AddU32(
            shared_settings,
            "Shared payload seed",
            &model_->config.tunnel_udp.payload_seed,
            std::numeric_limits<std::uint32_t>::max(),
            "tunnel_udp.payload_seed"
        );

        auto* save = new brls::Button();
        save->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        save->setText("Save configuration");
        save->registerClickAction([model = model_](brls::View*) {
            std::string error;
            if (!SaveRuntimeConfig(model->config, &error)) {
                logger::Status(*model->context, "configuration save failed: %s", error.c_str());
                brls::Application::notify("Configuration was not saved");
                return true;
            }
            logger::Status(*model->context, "configuration saved path=%s", RuntimeConfigPath);
            brls::Application::notify("Configuration saved");
            RefreshMainPage(model);
            return true;
        });
        auto* reset = new brls::Button();
        reset->setText("Reset to compiled defaults");
        reset->registerClickAction([this](brls::View*) {
            model_->config = model_->defaults;
            RefreshFields();
            RefreshMainPage(model_);
            logger::Status(*model_->context, "configuration reset to compiled defaults");
            brls::Application::notify("Compiled defaults restored");
            return true;
        });

        auto* actions = new brls::Box(brls::Axis::ROW);
        actions->setWidthPercentage(100.0F);
        actions->setMarginTop(18.0F);
        save->setGrow(1.0F);
        save->setShrink(1.0F);
        reset->setGrow(1.0F);
        reset->setShrink(1.0F);
        reset->setMarginLeft(SettingsActionGap);
        actions->addView(save);
        actions->addView(reset);
        content->addView(actions);
    }

  private:
    void AddHeader(brls::Box* content, const char* title, const char* subtitle, float margin_top = 0.0F) {
        auto* header = new brls::Header();
        header->setTitle(title);
        if (subtitle[0] != '\0') {
            header->setSubtitle(subtitle);
        }
        header->setMarginTop(margin_top);
        content->addView(header);
    }

    brls::BooleanCell* AddBoolean(brls::Box* content, const char* title, bool* value) {
        auto* cell = new brls::BooleanCell();
        cell->init(title, *value, [value](bool next) { *value = next; });
        content->addView(cell);
        return cell;
    }

    brls::SelectorCell* AddDataPathSelector(brls::Box* content) {
        auto* cell = new brls::SelectorCell();
        const int selected = model_->config.bsd_system_udp.enabled ? BsdSystemDataPathIndex : TunnelDataPathIndex;
        cell->init("Data path", {"Tunnel", "bsd:s"}, selected, [model = model_](int next) {
            model->config.tunnel_udp.enabled = next == TunnelDataPathIndex;
            model->config.bsd_system_udp.enabled = next == BsdSystemDataPathIndex;
        });
        content->addView(cell);
        return cell;
    }

    brls::InputCell* AddText(brls::Box* content, const char* title, std::string* value) {
        auto* cell = new brls::InputCell();
        cell->init(title, *value, [value](std::string next) { *value = std::move(next); }, "IPv4 address", "IPv4 dotted-quad address", 15);
        content->addView(cell);
        return cell;
    }

    brls::InputNumericCell* AddU16(brls::Box* content, const char* title, std::uint16_t* value, const char* name) {
        auto* cell = new brls::InputNumericCell();
        cell->init(
            title,
            *value,
            [this, value, name](long next) {
                AssignUnsigned(next, value, std::numeric_limits<std::uint16_t>::max(), name, *model_->context);
            },
            "Unsigned integer",
            5
        );
        content->addView(cell);
        return cell;
    }

    brls::InputNumericCell* AddU32(brls::Box* content, const char* title, std::uint32_t* value, std::uint32_t maximum, const char* name) {
        auto* cell = new brls::InputNumericCell();
        cell->init(
            title,
            static_cast<long>(*value),
            [this, value, maximum, name](long next) { AssignUnsigned(next, value, maximum, name, *model_->context); },
            "Unsigned integer",
            10
        );
        content->addView(cell);
        return cell;
    }

    brls::InputNumericCell* AddSize(brls::Box* content, const char* title, std::size_t* value, std::size_t maximum, const char* name) {
        auto* cell = new brls::InputNumericCell();
        cell->init(
            title,
            static_cast<long>(*value),
            [this, value, maximum, name](long next) { AssignUnsigned(next, value, maximum, name, *model_->context); },
            "Unsigned integer",
            5
        );
        content->addView(cell);
        return cell;
    }

    void UpdateContractValidationAppearance() {
        brls::Theme theme = brls::Application::getTheme();
        const auto color = model_->config.tunnel_contract.enabled ? theme["brls/text"] : theme["brls/text_disabled"];
        verify_cloned_session_lifetime_->setTextColor(color);
        verify_mixed_batch_->setTextColor(color);
    }

    void RefreshFields() {
        const auto& tunnel = model_->config.tunnel_udp;
        data_path_->setSelection(model_->config.bsd_system_udp.enabled ? BsdSystemDataPathIndex : TunnelDataPathIndex, true);
        destination_ipv4_->setValue(tunnel.destination_ipv4);
        destination_port_->setValue(tunnel.destination_port);
        workload_id_->setValue(static_cast<long>(tunnel.workload_id));
        payload_bytes_->setValue(static_cast<long>(tunnel.payload_bytes));
        datagram_count_->setValue(static_cast<long>(tunnel.datagram_count));
        pacing_ms_->setValue(static_cast<long>(tunnel.pacing_ms));
        concurrent_flows_->setValue(static_cast<long>(tunnel.concurrent_flows));
        receive_deadline_ms_->setValue(static_cast<long>(tunnel.receive_deadline_ms));
        payload_seed_->setValue(static_cast<long>(tunnel.payload_seed));
        echo_replies_->setOn(tunnel.echo_replies, false);
        verify_bsd_post_route_rejection_->setOn(model_->config.bsd_system_udp.verify_post_route_rejection, false);
        expected_bsd_outcome_->setSelection(BsdExpectedOutcomeIndex(model_->config.bsd_system_udp.expected_outcome), true);
        require_bsd_writable_recovery_->setOn(model_->config.bsd_system_udp.require_writable_recovery, false);
        const auto& contract = model_->config.tunnel_contract;
        contract_enabled_->setOn(contract.enabled, false);
        verify_cloned_session_lifetime_->setOn(contract.verify_cloned_session_lifetime, false);
        verify_mixed_batch_->setOn(contract.verify_mixed_batch, false);
        UpdateContractValidationAppearance();
    }

    std::shared_ptr<UiModel> model_;
    brls::SelectorCell* data_path_{};
    brls::InputCell* destination_ipv4_{};
    brls::InputNumericCell* destination_port_{};
    brls::InputNumericCell* workload_id_{};
    brls::InputNumericCell* payload_bytes_{};
    brls::InputNumericCell* datagram_count_{};
    brls::InputNumericCell* pacing_ms_{};
    brls::InputNumericCell* concurrent_flows_{};
    brls::InputNumericCell* receive_deadline_ms_{};
    brls::InputNumericCell* payload_seed_{};
    brls::BooleanCell* echo_replies_{};
    brls::BooleanCell* verify_bsd_post_route_rejection_{};
    brls::SelectorCell* expected_bsd_outcome_{};
    brls::BooleanCell* require_bsd_writable_recovery_{};
    brls::BooleanCell* contract_enabled_{};
    brls::BooleanCell* verify_cloned_session_lifetime_{};
    brls::BooleanCell* verify_mixed_batch_{};
};

class MainPage final : public brls::Box {
  public:
    explicit MainPage(std::shared_ptr<UiModel> model) : brls::Box(brls::Axis::COLUMN), model_(std::move(model)) {
        setPadding(PagePadding);
        setShrink(1.0F);

        auto* scenario_header = new brls::Header();
        scenario_header->setTitle("Current Scenario");
        scenario_header->setSubtitle("Configured UDP workload.");
        addView(scenario_header);

        model_->main.scenario_label = new brls::Label();
        model_->main.scenario_label->setHeight(42.0F);
        addView(model_->main.scenario_label);

        model_->main.status_label = new brls::Label();
        model_->main.status_label->setHeight(46.0F);
        addView(model_->main.status_label);

        auto* log_header = new brls::Header();
        log_header->setTitle("Recent Activity");
        log_header->setSubtitle("Latest requester events for the selected run.");
        addView(log_header);

        model_->main.log_panel = new LogPanel(MainLogPanelLines);
        model_->main.log_panel->setWidthPercentage(100.0F);
        model_->main.log_panel->setHeight(MainLogPanelHeight);
        addView(model_->main.log_panel);

        auto* commands = new brls::Box(brls::Axis::ROW);
        commands->setWidthPercentage(100.0F);
        commands->setJustifyContent(brls::JustifyContent::FLEX_END);
        commands->setMarginTop(18.0F);
        model_->main.run_button = new brls::Button();
        model_->main.run_button->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        model_->main.run_button->setText("Run");
        model_->main.run_button->setWidth(190.0F);
        model_->main.run_button->registerClickAction([model = model_](brls::View*) {
            StartRun(model);
            return true;
        });
        commands->addView(model_->main.run_button);
        addView(commands);

        RefreshMainPage(model_);
    }

    ~MainPage() override {
        if (model_->main.log_panel != nullptr) {
            model_->main = {};
        }
    }

  private:
    static void StartRun(const std::shared_ptr<UiModel>& model) {
        if (model->run_active) {
            return;
        }
        std::string validation_error;
        if (!ValidateRuntimeConfig(model->config, &validation_error)) {
            logger::Status(*model->context, "run rejected: %s", validation_error.c_str());
            model->run_status = "Configuration error: " + validation_error;
            RefreshMainPage(model);
            brls::Application::notify("Fix the configuration before running");
            return;
        }

        model->run_active = true;
        model->run_status = "Running " + FormatRuntimeConfig(model->config);
        RefreshMainPage(model);
        const RuntimeConfig runtime_config = model->config;
        AppContext* const context = model->context;
        logger::Status(*context, "requester workload requested: %s", FormatRuntimeConfig(model->config).c_str());

        brls::async([model, context, runtime_config] {
            std::vector<ScenarioResult> results;
            if (runtime_config.tunnel_udp.enabled) {
                results.push_back(RunWgnxTunnelUdpWorkload(*context, runtime_config.tunnel_udp));
            }
            if (runtime_config.bsd_system_udp.enabled) {
                results.push_back(RunBsdSystemUdpWorkload(*context, runtime_config.tunnel_udp, runtime_config.bsd_system_udp));
            }
            if (runtime_config.tunnel_contract.enabled) {
                results.push_back(RunWgnxTunnelContractValidation(*context, runtime_config.tunnel_udp, runtime_config.tunnel_contract));
            }
            if (results.empty()) {
                results.push_back({.name = "requester", .skipped = true, .detail = "no runtime scenario is enabled"});
            }
            bool all_success = true;
            std::string summary;
            for (const ScenarioResult& result : results) {
                const char* state = result.skipped ? "SKIP" : (result.success ? "OK" : "FAIL");
                logger::Status(
                    *context,
                    "[%s] %s | sent=%zu recv=%zu | %s",
                    state,
                    result.name.c_str(),
                    result.bytes_sent,
                    result.bytes_received,
                    result.detail.c_str()
                );
                all_success = all_success && (result.success || result.skipped);
                if (!summary.empty()) {
                    summary += " | ";
                }
                summary += result.name + ": " + (result.skipped ? "skipped" : (result.success ? "completed" : "failed"));
            }
            brls::sync([model, all_success, summary = std::move(summary)] {
                if (!model->active) {
                    return;
                }
                model->run_active = false;
                model->run_status = std::string(all_success ? "Completed: " : "Failed: ") + summary;
                RefreshMainPage(model);
                RefreshLogViews(model);
            });
        });
    }

    std::shared_ptr<UiModel> model_;
};

class LogsPage final : public brls::Box {
  public:
    explicit LogsPage(std::shared_ptr<UiModel> model) : brls::Box(brls::Axis::COLUMN), model_(std::move(model)) {
        setPadding(PagePadding);
        setShrink(1.0F);

        auto* header = new brls::Header();
        header->setTitle("Logs");
        header->setSubtitle("Scroll to inspect earlier events without losing position.");
        addView(header);

        model_->logs_panel = new LogPanel();
        model_->logs_panel->setWidthPercentage(100.0F);
        model_->logs_panel->setGrow(1.0F);
        addView(model_->logs_panel);
    }

    ~LogsPage() override {
        model_->logs_panel = nullptr;
    }

  private:
    std::shared_ptr<UiModel> model_;
};

class ScenariosPage final : public brls::Box {
  public:
    explicit ScenariosPage(std::shared_ptr<UiModel> model) : brls::Box(brls::Axis::COLUMN), model_(std::move(model)) {
        setPadding(PagePadding);
        setShrink(1.0F);

        const auto scenarios = AvailableScenarios();
        scenario_names_.reserve(scenarios.size());
        for (const ScenarioDescriptor& scenario : scenarios) {
            scenario_names_.emplace_back(scenario.name);
        }

        auto* header = new brls::Header();
        header->setTitle("Scenarios");
        header->setSubtitle("Run one compiled diagnostic scenario at a time.");
        addView(header);

        selector_ = new brls::SelectorCell();
        selector_->init("Scenario", scenario_names_, 0, [this](int index) {
            selected_index_ = static_cast<std::size_t>(index);
            UpdateDetails();
        });
        addView(selector_);

        description_ = new brls::Label();
        description_->setSingleLine(false);
        description_->setAutoAnimate(false);
        description_->setMarginTop(12.0F);
        addView(description_);

        auto* defaults_header = new brls::Header();
        defaults_header->setTitle("Compiled Defaults");
        defaults_header->setSubtitle("Read from config.hpp");
        defaults_header->setMarginTop(12.0F);
        addView(defaults_header);

        defaults_ = new brls::Label();
        defaults_->setMarginTop(8.0F);
        defaults_->setFontSize(16.0F);
        defaults_->setSingleLine(false);
        defaults_->setAutoAnimate(false);
        addView(defaults_);

        model_->scenario.status_label = new brls::Label();
        model_->scenario.status_label->setHeight(40.0F);

        auto* log_header = new brls::Header();
        log_header->setTitle("Scenario Log");
        log_header->setSubtitle("Recent output is shown while the selected scenario runs.");
        log_header->setMarginTop(12.0F);
        addView(log_header);

        model_->scenario.log_panel = new LogPanel(MainLogPanelLines);
        model_->scenario.log_panel->setWidthPercentage(100.0F);
        model_->scenario.log_panel->setHeight(ScenarioLogPanelHeight);
        addView(model_->scenario.log_panel);

        auto* footer = new brls::Box(brls::Axis::ROW);
        footer->setWidthPercentage(100.0F);
        footer->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        footer->setMarginTop(20.0F);
        footer->setHeight(40.0F);

        auto* commands = new brls::Box(brls::Axis::ROW);

        auto* clear = new brls::Button();
        clear->setText("Clear log");
        clear->setWidth(190.0F);
        clear->registerClickAction([model = model_](brls::View*) {
            model->scenario.log_panel->Clear();
            return true;
        });
        model_->scenario.run_button = new brls::Button();
        model_->scenario.run_button->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        model_->scenario.run_button->setText("Run scenario");
        model_->scenario.run_button->setWidth(190.0F);
        model_->scenario.run_button->setMarginLeft(SettingsActionGap);
        model_->scenario.run_button->registerClickAction([this](brls::View*) {
            StartRun();
            return true;
        });
        commands->addView(clear);
        commands->addView(model_->scenario.run_button);
        footer->addView(model_->scenario.status_label);
        footer->addView(commands);
        addView(footer);

        UpdateDetails();
        RefreshScenarioPage(model_);
    }

    ~ScenariosPage() override {
        if (model_->scenario.log_panel != nullptr) {
            model_->scenario = {};
        }
    }

  private:
    void UpdateDetails() {
        const ScenarioDescriptor& scenario = AvailableScenarios()[selected_index_];
        description_->setText(std::string(scenario.description));
        defaults_->setText(std::string(scenario.compiled_defaults));
    }

    void StartRun() {
        if (model_->run_active) {
            return;
        }

        const std::string name = scenario_names_[selected_index_];
        model_->run_active = true;
        model_->run_status = "Running " + name;
        model_->scenario.log_panel->ShowRecent();
        RefreshMainPage(model_);
        RefreshScenarioPage(model_);
        AppContext* const context = model_->context;
        logger::Status(*context, "scenario requested: %s", name.c_str());

        brls::async([model = model_, context, name] {
            const ScenarioResult result = RunScenario(*context, name);
            const char* const state = result.skipped ? "SKIP" : (result.success ? "OK" : "FAIL");
            logger::Status(
                *context,
                "[%s] %s | sent=%zu recv=%zu | %s",
                state,
                result.name.c_str(),
                result.bytes_sent,
                result.bytes_received,
                result.detail.c_str()
            );
            brls::sync([model, result] {
                if (!model->active) {
                    return;
                }
                model->run_active = false;
                model->run_status = result.name + ": " + (result.skipped ? "skipped" : (result.success ? "completed" : "failed"));
                RefreshMainPage(model);
                RefreshScenarioPage(model);
                RefreshLogViews(model);
            });
        });
    }

    std::shared_ptr<UiModel> model_;
    brls::SelectorCell* selector_{};
    brls::Label* description_{};
    brls::Label* defaults_{};
    std::vector<std::string> scenario_names_;
    std::size_t selected_index_{};
};

class AboutPage final : public brls::ScrollingFrame {
  public:
    explicit AboutPage(const std::shared_ptr<UiModel>& model) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(PagePadding);
        content->setWidthPercentage(100.0F);
        setContentView(content);

        auto* header = new brls::Header();
        header->setTitle("NX Reversing Requester");
        header->setSubtitle("Controlled Horizon network traffic harness.");
        content->addView(header);

        AddDetail(content, "Version", nxrv::build_info::Version);
        AddDetail(content, "Build", nxrv::build_info::BuildId);
        AddDetail(content, "Author", APP_AUTHOR);
        AddDetail(content, "Configuration", model->configuration_source);
        AddDetail(content, "Horizon OS", FormatHosVersion());

        auto* project = new brls::DetailCell();
        project->setText("Project");
        project->setDetailText(ProjectUrl);
        project->registerClickAction([](brls::View*) {
            brls::Application::getPlatform()->openBrowser(ProjectUrl);
            return true;
        });
        content->addView(project);

        auto* protocol_header = new brls::Header();
        protocol_header->setTitle("IPC Protocols");
        protocol_header->setSubtitle("Versions compiled into this requester build.");
        content->addView(protocol_header);
        AddDetail(content, "wgnx:ctl", "API " + std::to_string(wgnx::IpcApiVersion));
        AddDetail(content, "wgnx:tun", "API " + std::to_string(wgnx::tunnel::TunApiVersion));
    }

  private:
    static void AddDetail(brls::Box* content, const std::string& title, const std::string& detail) {
        auto* cell = new brls::DetailCell();
        cell->setText(title);
        cell->setDetailText(detail);
        content->addView(cell);
    }
};

class RequesterTabs final : public brls::TabFrame {
  public:
    explicit RequesterTabs(std::shared_ptr<UiModel> model) : model_(std::move(model)) {
        getAppletFrameItem()->title = APP_TITLE;
        getAppletFrameItem()->setIconFromFile("romfs:/img/requester.png");
        addTab("Main", [model = model_] { return new MainPage(model); });
        addTab("Logs", [model = model_] { return new LogsPage(model); });
        addTab("Settings", [model = model_] { return new SettingsPage(model); });
        addSeparator();
        addTab("Scenarios", [model = model_] { return new ScenariosPage(model); });
        addSeparator();
        addTab("About", [model = model_] { return new AboutPage(model); });
        focusTab(0);
    }

  private:
    std::shared_ptr<UiModel> model_;
};

class RequesterActivity final : public brls::Activity {
  public:
    explicit RequesterActivity(std::shared_ptr<UiModel> model) : model_(std::move(model)) {}

    ~RequesterActivity() override {
        model_->active = false;
    }

    brls::View* createContentView() override {
        auto* tabs = new RequesterTabs(model_);
        return new brls::AppletFrame(tabs);
    }

    void onContentAvailable() override {
        registerAction("Bump UDP bind", brls::BUTTON_RT, [model = model_](brls::View*) {
            RequestUdpBindBump(model);
            return true;
        });
    }

  private:
    std::shared_ptr<UiModel> model_;
};

} // namespace

int RunRequesterUi(AppContext& context, RuntimeConfig defaults, ConfigLoadReport loaded_config) {
    auto model = std::make_shared<UiModel>();
    model->context = &context;
    model->defaults = std::move(defaults);
    model->configuration_source = loaded_config.loaded_from_file ? "sdmc config" : "compiled defaults";
    model->config = std::move(loaded_config.config);

    logger::SetUiSink([model](const std::string&) { brls::sync([model] { RefreshLogViews(model); }); });

    if (!brls::Application::init()) {
        logger::Log(context, "Borealis initialization failed");
        logger::SetUiSink({});
        return 1;
    }
    brls::getStyle().addMetric("brls/tab_frame/sidebar_width", RequesterSidebarWidth);
    brls::getStyle().addMetric("brls/sidebar/padding_left", 52.0F);
    brls::getStyle().addMetric("brls/sidebar/padding_right", 24.0F);
    brls::Application::createWindow(APP_TITLE);
    brls::Application::setGlobalQuit(true);
    brls::Application::pushActivity(new RequesterActivity(model));

    while (brls::Application::mainLoop()) {
    }

    model->active = false;
    logger::SetUiSink({});
    return 0;
}

} // namespace requester
