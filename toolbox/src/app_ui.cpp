#include "app_ui.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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

namespace toolbox {

namespace {

constexpr float PagePadding = 24.0F;
constexpr float MainLogPanelHeight = 160.0F;
constexpr float ScenarioLogPanelHeight = 170.0F;
constexpr std::size_t MainLogPanelLines = 12;
constexpr float OffsetTolerance = 1.0F;
constexpr float SettingsActionGap = 18.0F;
constexpr float SettingsSectionGap = 20.0F;
constexpr float ToolboxSidebarWidth = 250.0F;
constexpr float LogFontSize = 14.0F;
constexpr char ProjectUrl[] = APP_PROJECT_URL;
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
    brls::SelectorCell* scenario_selector{};
    brls::SelectorCell* profile_selector{};
    brls::Label* active_cfg_label{};
    brls::Label* default_cfg_label{};
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
    const RuntimeProfile* const profile = ActiveRuntimeProfile(config);
    if (profile == nullptr) {
        return "No active profile";
    }
    const std::string destination =
        config.scenario == RuntimeScenario::BsdSystemUdp ? profile->bsd_destination_ipv4 : profile->tunnel_destination_ipv4;
    return "dest " + destination + ":" + std::to_string(profile->udp_destination_port) + " | payload " +
           std::to_string(config.udp.payload_bytes) + " B | datagrams " + std::to_string(config.udp.datagram_count) + " | flows " +
           std::to_string(config.udp.concurrent_flows) + " | next ID " + std::to_string(config.next_workload_id);
}

std::vector<std::string> RuntimeScenarioNames() {
    return {
        RuntimeScenarioName(RuntimeScenario::DirectTunnelUdp),
        RuntimeScenarioName(RuntimeScenario::BsdSystemUdp),
        RuntimeScenarioName(RuntimeScenario::TunnelContractValidation),
    };
}

int RuntimeScenarioIndex(RuntimeScenario scenario) {
    switch (scenario) {
    case RuntimeScenario::DirectTunnelUdp:
        return 0;
    case RuntimeScenario::BsdSystemUdp:
        return 1;
    case RuntimeScenario::TunnelContractValidation:
        return 2;
    }
    return 0;
}

RuntimeScenario RuntimeScenarioAt(int index) {
    switch (index) {
    case 1:
        return RuntimeScenario::BsdSystemUdp;
    case 2:
        return RuntimeScenario::TunnelContractValidation;
    default:
        return RuntimeScenario::DirectTunnelUdp;
    }
}

std::vector<std::string> RuntimeProfileNames(const RuntimeConfig& config) {
    std::vector<std::string> names;
    names.reserve(config.profiles.size());
    for (const RuntimeProfile& profile : config.profiles) {
        names.push_back(profile.name.empty() ? "Unnamed profile" : profile.name);
    }
    return names;
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
    return rendered.empty() ? "No toolbox activity yet." : rendered;
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
    if (model->main.scenario_selector != nullptr) {
        model->main.scenario_selector->setSelection(RuntimeScenarioIndex(model->config.scenario), true);
    }
    if (model->main.profile_selector != nullptr) {
        model->main.profile_selector->setData(RuntimeProfileNames(model->config));
        model->main.profile_selector->setSelection(static_cast<int>(model->config.active_profile), true);
    }
    if (model->main.active_cfg_label != nullptr) {
        model->main.active_cfg_label->setText(FormatRuntimeConfig(model->config));
    }
    if (model->main.default_cfg_label != nullptr) {
        model->main.default_cfg_label->setText(FormatRuntimeConfig(model->defaults));
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

bool SaveConfiguration(const std::shared_ptr<UiModel>& model) {
    std::string error;
    if (!SaveRuntimeConfig(model->config, &error)) {
        logger::Status(*model->context, "configuration save failed: %s", error.c_str());
        brls::Application::notify("Configuration was not saved");
        return false;
    }
    logger::Status(*model->context, "configuration saved path=%s", RuntimeConfigPath);
    brls::Application::notify("Configuration saved");
    RefreshMainPage(model);
    return true;
}

class SettingsPage final : public brls::ScrollingFrame {
  public:
    explicit SettingsPage(std::shared_ptr<UiModel> model) : model_(std::move(model)) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(PagePadding);
        content->setWidthPercentage(100.0F);
        setContentView(content);

        AddHeader(content, "Run Behavior", "The next ID is reserved and advanced when a run starts.");
        next_workload_id_ = AddU32(
            content,
            "Next run ID",
            &model_->config.next_workload_id,
            std::numeric_limits<std::uint32_t>::max() - 1,
            "run.next_workload_id"
        );

        AddHeader(content, "UDP Behavior", "Configure the UDP scenarios shared by the selected profile.", SettingsSectionGap);
        payload_bytes_ = AddSize(
            content,
            "Payload bytes",
            &model_->config.udp.payload_bytes,
            wgnx::tunnel::MaximumUdpPayloadStorageBytes,
            "udp.payload_bytes"
        );
        datagram_count_ = AddU32(content, "Datagram count", &model_->config.udp.datagram_count, 4096, "udp.datagram_count");
        pacing_ms_ = AddU32(content, "Pacing milliseconds", &model_->config.udp.pacing_ms, 60000, "udp.pacing_ms");
        concurrent_flows_ = AddU32(
            content,
            "Concurrent flows",
            &model_->config.udp.concurrent_flows,
            wgnx::tunnel::MaximumFlowsPerClient,
            "udp.concurrent_flows"
        );
        receive_deadline_ms_ =
            AddU32(content, "Receive deadline milliseconds", &model_->config.udp.receive_deadline_ms, 60000, "udp.receive_deadline_ms");
        payload_seed_ = AddU32(
            content,
            "Payload seed",
            &model_->config.udp.payload_seed,
            std::numeric_limits<std::uint32_t>::max(),
            "udp.payload_seed"
        );
        echo_replies_ = AddBoolean(content, "Echo replies", &model_->config.udp.echo_replies);

        AddHeader(content, "BSD System Behavior", "Checks used by the BSD system scenario.", SettingsSectionGap);
        verify_bsd_post_route_rejection_ =
            AddBoolean(content, "Verify tunneled socket option rejection", &model_->config.bsd_system_udp.verify_post_route_rejection);
        expected_bsd_outcome_ = new brls::SelectorCell();
        expected_bsd_outcome_->init(
            "Expected outcome",
            {"Normal workload", "No-reply timeout", "Terminal closure"},
            BsdExpectedOutcomeIndex(model_->config.bsd_system_udp.expected_outcome),
            [model = model_](int next) {
                model->config.bsd_system_udp.expected_outcome =
                    next == BsdExpectedNoReplyTimeoutIndex
                        ? BsdSystemUdpExpectedOutcome::NoReplyTimeout
                        : (next == BsdExpectedTerminalClosureIndex ? BsdSystemUdpExpectedOutcome::TerminalClosure
                                                                   : BsdSystemUdpExpectedOutcome::EchoReply);
            }
        );
        content->addView(expected_bsd_outcome_);
        require_bsd_writable_recovery_ =
            AddBoolean(content, "Require writable recovery after queue pressure", &model_->config.bsd_system_udp.require_writable_recovery);

        AddHeader(content, "Tunnel Contract Validation", "Clone lifetime and mixed-batch API coverage.", SettingsSectionGap);
        verify_cloned_session_lifetime_ =
            AddBoolean(content, "Verify cloned session lifetime", &model_->config.tunnel_contract.verify_cloned_session_lifetime);
        verify_mixed_batch_ = AddBoolean(content, "Verify mixed batch dispositions", &model_->config.tunnel_contract.verify_mixed_batch);

        AddActions(content);
    }

  private:
    void AddHeader(brls::Box* content, const char* title, const char* subtitle, float margin_top = 0.0F) {
        auto* header = new brls::Header();
        header->setTitle(title);
        header->setSubtitle(subtitle);
        header->setMarginTop(margin_top);
        content->addView(header);
    }

    brls::BooleanCell* AddBoolean(brls::Box* content, const char* title, bool* value) {
        auto* cell = new brls::BooleanCell();
        cell->init(title, *value, [value](bool next) { *value = next; });
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

    void AddActions(brls::Box* content) {
        auto* save = new brls::Button();
        save->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        save->setText("Save configuration");
        save->registerClickAction([model = model_](brls::View*) { return SaveConfiguration(model); });
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
        reset->setGrow(1.0F);
        reset->setMarginLeft(SettingsActionGap);
        actions->addView(save);
        actions->addView(reset);
        content->addView(actions);
    }

    void RefreshFields() {
        next_workload_id_->setValue(static_cast<long>(model_->config.next_workload_id));
        payload_bytes_->setValue(static_cast<long>(model_->config.udp.payload_bytes));
        datagram_count_->setValue(static_cast<long>(model_->config.udp.datagram_count));
        pacing_ms_->setValue(static_cast<long>(model_->config.udp.pacing_ms));
        concurrent_flows_->setValue(static_cast<long>(model_->config.udp.concurrent_flows));
        receive_deadline_ms_->setValue(static_cast<long>(model_->config.udp.receive_deadline_ms));
        payload_seed_->setValue(static_cast<long>(model_->config.udp.payload_seed));
        echo_replies_->setOn(model_->config.udp.echo_replies, false);
        verify_bsd_post_route_rejection_->setOn(model_->config.bsd_system_udp.verify_post_route_rejection, false);
        expected_bsd_outcome_->setSelection(BsdExpectedOutcomeIndex(model_->config.bsd_system_udp.expected_outcome), true);
        require_bsd_writable_recovery_->setOn(model_->config.bsd_system_udp.require_writable_recovery, false);
        verify_cloned_session_lifetime_->setOn(model_->config.tunnel_contract.verify_cloned_session_lifetime, false);
        verify_mixed_batch_->setOn(model_->config.tunnel_contract.verify_mixed_batch, false);
    }

    std::shared_ptr<UiModel> model_;
    brls::InputNumericCell* next_workload_id_{};
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
    brls::BooleanCell* verify_cloned_session_lifetime_{};
    brls::BooleanCell* verify_mixed_batch_{};
};

class ProfilesPage final : public brls::ScrollingFrame {
  public:
    explicit ProfilesPage(std::shared_ptr<UiModel> model) : model_(std::move(model)) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(PagePadding);
        content->setWidthPercentage(100.0F);
        setContentView(content);

        auto* header = new brls::Header();
        header->setTitle("Profiles");
        header->setSubtitle("Select a target profile and configure its destinations.");
        content->addView(header);

        selector_ = new brls::SelectorCell();
        selector_->init(
            "Active profile",
            RuntimeProfileNames(model_->config),
            static_cast<int>(model_->config.active_profile),
            [this](int next) {
                model_->config.active_profile = static_cast<std::size_t>(next);
                RefreshFields();
                RefreshMainPage(model_);
            }
        );
        content->addView(selector_);
        name_ = AddText(content, "Profile name", "Profile name", 32, [this](std::string next) {
            if (RuntimeProfile* profile = ActiveRuntimeProfile(&model_->config)) {
                profile->name = std::move(next);
                selector_->setData(RuntimeProfileNames(model_->config));
                RefreshMainPage(model_);
            }
        });
        tunnel_destination_ = AddText(content, "Tunnel destination IPv4", "IPv4 address", 15, [this](std::string next) {
            if (RuntimeProfile* profile = ActiveRuntimeProfile(&model_->config)) {
                profile->tunnel_destination_ipv4 = std::move(next);
                RefreshMainPage(model_);
            }
        });
        bsd_destination_ = AddText(content, "BSD destination IPv4", "IPv4 address", 15, [this](std::string next) {
            if (RuntimeProfile* profile = ActiveRuntimeProfile(&model_->config)) {
                profile->bsd_destination_ipv4 = std::move(next);
                RefreshMainPage(model_);
            }
        });
        port_ = new brls::InputNumericCell();
        port_->init(
            "UDP destination port",
            0,
            [this](long next) {
                if (RuntimeProfile* profile = ActiveRuntimeProfile(&model_->config)) {
                    AssignUnsigned(
                        next,
                        &profile->udp_destination_port,
                        std::numeric_limits<std::uint16_t>::max(),
                        "profile.udp_destination_port",
                        *model_->context
                    );
                    RefreshMainPage(model_);
                }
            },
            "Unsigned integer",
            5
        );
        content->addView(port_);

        auto* actions = new brls::Box(brls::Axis::ROW);
        actions->setWidthPercentage(100.0F);
        actions->setMarginTop(18.0F);
        auto* add = new brls::Button();
        add->setText("Add profile");
        add->registerClickAction([this](brls::View*) {
            if (model_->config.profiles.size() == 8) {
                brls::Application::notify("Profile limit reached");
                return true;
            }
            RuntimeProfile profile = *ActiveRuntimeProfile(model_->config);
            profile.name = "Profile " + std::to_string(model_->config.profiles.size() + 1);
            model_->config.profiles.push_back(std::move(profile));
            model_->config.active_profile = model_->config.profiles.size() - 1;
            RefreshFields();
            RefreshMainPage(model_);
            return true;
        });
        auto* save = new brls::Button();
        save->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        save->setText("Save configuration");
        save->setMarginLeft(SettingsActionGap);
        save->registerClickAction([model = model_](brls::View*) { return SaveConfiguration(model); });
        actions->addView(add);
        actions->addView(save);
        content->addView(actions);
        RefreshFields();
    }

  private:
    brls::InputCell* AddText(
        brls::Box* content, const char* title, const char* hint, std::size_t maximum_length, std::function<void(std::string)> on_change
    ) {
        auto* cell = new brls::InputCell();
        cell->init(title, "", std::move(on_change), hint, hint, maximum_length);
        content->addView(cell);
        return cell;
    }

    void RefreshFields() {
        const RuntimeProfile* const profile = ActiveRuntimeProfile(model_->config);
        if (profile == nullptr) {
            return;
        }
        selector_->setData(RuntimeProfileNames(model_->config));
        selector_->setSelection(static_cast<int>(model_->config.active_profile), true);
        name_->setValue(profile->name);
        tunnel_destination_->setValue(profile->tunnel_destination_ipv4);
        bsd_destination_->setValue(profile->bsd_destination_ipv4);
        port_->setValue(profile->udp_destination_port);
    }

    std::shared_ptr<UiModel> model_;
    brls::SelectorCell* selector_{};
    brls::InputCell* name_{};
    brls::InputCell* tunnel_destination_{};
    brls::InputCell* bsd_destination_{};
    brls::InputNumericCell* port_{};
};

class MainPage final : public brls::Box {
  public:
    explicit MainPage(std::shared_ptr<UiModel> model) : brls::Box(brls::Axis::COLUMN), model_(std::move(model)) {
        setPadding(PagePadding);
        setShrink(1.0F);
        auto* header = new brls::Header();
        header->setTitle("Run");
        header->setSubtitle("Select a scenario and target profile.");
        addView(header);
        auto* selectors = new brls::Box(brls::Axis::ROW);
        selectors->setWidthPercentage(100.0F);
        model_->main.scenario_selector = new brls::SelectorCell();
        model_->main.scenario_selector
            ->init("Scenario", RuntimeScenarioNames(), RuntimeScenarioIndex(model_->config.scenario), [model = model_](int next) {
                model->config.scenario = RuntimeScenarioAt(next);
                RefreshMainPage(model);
            });
        model_->main.scenario_selector->setGrow(1.0F);
        model_->main.scenario_selector->setShrink(1.0F);
        model_->main.profile_selector = new brls::SelectorCell();
        model_->main.profile_selector->init(
            "Profile",
            RuntimeProfileNames(model_->config),
            static_cast<int>(model_->config.active_profile),
            [model = model_](int next) {
                model->config.active_profile = static_cast<std::size_t>(next);
                RefreshMainPage(model);
            }
        );
        model_->main.profile_selector->setGrow(1.0F);
        model_->main.profile_selector->setShrink(1.0F);
        model_->main.profile_selector->setMarginLeft(SettingsActionGap);
        selectors->addView(model_->main.scenario_selector);
        selectors->addView(model_->main.profile_selector);
        addView(selectors);

        auto* active_cfg_header = new brls::Header();
        active_cfg_header->setTitle("Active Configuration");
        active_cfg_header->setSubtitle("Values used by the next run.");
        active_cfg_header->setMarginTop(SettingsSectionGap);
        addView(active_cfg_header);

        model_->main.active_cfg_label = new brls::Label();
        model_->main.active_cfg_label->setSingleLine(false);
        model_->main.active_cfg_label->setAutoAnimate(false);
        model_->main.active_cfg_label->setMarginTop(12.0F);
        model_->main.active_cfg_label->setFontSize(16.0F);
        addView(model_->main.active_cfg_label);

        auto* log_header = new brls::Header();
        log_header->setTitle("Recent Activity");
        log_header->setSubtitle("Latest toolbox events.");
        log_header->setMarginTop(SettingsSectionGap);
        addView(log_header);

        model_->main.log_panel = new LogPanel(MainLogPanelLines);
        model_->main.log_panel->setWidthPercentage(100.0F);
        model_->main.log_panel->setMarginTop(8.0F);
        model_->main.log_panel->setHeight(MainLogPanelHeight);
        addView(model_->main.log_panel);

        auto* footer = new brls::Box(brls::Axis::ROW);
        footer->setWidthPercentage(100.0F);
        footer->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        footer->setMarginTop(SettingsSectionGap);
        footer->setHeight(40.0F);

        model_->main.status_label = new brls::Label();
        model_->main.status_label->setHeight(40.0F);

        auto* commands = new brls::Box(brls::Axis::ROW);

        auto* clear = new brls::Button();
        clear->setText("Clear log");
        clear->setWidth(190.0F);
        clear->registerClickAction([model = model_](brls::View*) {
            model->main.log_panel->Clear();
            return true;
        });
        model_->main.run_button = new brls::Button();
        model_->main.run_button->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        model_->main.run_button->setText("Run scenario");
        model_->main.run_button->setWidth(190.0F);
        model_->main.run_button->setMarginLeft(SettingsActionGap);
        model_->main.run_button->registerClickAction([model = model_](brls::View*) {
            StartRun(model);
            return true;
        });
        commands->addView(clear);
        commands->addView(model_->main.run_button);
        footer->addView(model_->main.status_label);
        footer->addView(commands);
        addView(footer);
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
        if (model->config.next_workload_id == std::numeric_limits<std::uint32_t>::max()) {
            logger::Status(*model->context, "run rejected: workload ID space is exhausted");
            model->run_status = "Configuration error: workload ID space is exhausted";
            RefreshMainPage(model);
            brls::Application::notify("Choose a new workload ID before running");
            return;
        }
        const std::uint32_t workload_id = model->config.next_workload_id;
        const RuntimeConfig previous_config = model->config;
        ++model->config.next_workload_id;
        if (!SaveConfiguration(model)) {
            model->config = previous_config;
            RefreshMainPage(model);
            return;
        }
        const RuntimeConfig runtime_config = model->config;
        const RuntimeProfile* const profile = ActiveRuntimeProfile(runtime_config);
        if (profile == nullptr) {
            return;
        }
        const TunnelUdpWorkloadConfig workload = BuildTunnelUdpWorkload(runtime_config, *profile, workload_id);
        model->run_active = true;
        model->run_status = "Running " + std::string(RuntimeScenarioName(runtime_config.scenario)) + " with " + profile->name + " (ID " +
                            std::to_string(workload_id) + ")";
        model->main.log_panel->ShowRecent();
        RefreshMainPage(model);
        AppContext* const context = model->context;
        logger::Status(
            *context,
            "runtime scenario requested scenario=%s profile=%s workload=%u",
            RuntimeScenarioName(runtime_config.scenario),
            profile->name.c_str(),
            workload_id
        );
        brls::async([model, context, runtime_config, workload] {
            ScenarioResult result;
            switch (runtime_config.scenario) {
            case RuntimeScenario::DirectTunnelUdp:
                result = RunWgnxTunnelUdpWorkload(*context, workload);
                break;
            case RuntimeScenario::BsdSystemUdp:
                result = RunBsdSystemUdpWorkload(*context, workload, runtime_config.bsd_system_udp);
                break;
            case RuntimeScenario::TunnelContractValidation:
                result = RunWgnxTunnelContractValidation(*context, workload, runtime_config.tunnel_contract);
                break;
            }
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
            brls::sync([model, result = std::move(result)] {
                if (!model->active) {
                    return;
                }
                model->run_active = false;
                model->run_status = result.name + ": " + (result.skipped ? "skipped" : (result.success ? "completed" : "failed"));
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

        auto* default_cfg_header = new brls::Header();
        default_cfg_header->setTitle("Compiled Defaults");
        default_cfg_header->setSubtitle("Read from config.hpp");
        default_cfg_header->setMarginTop(12.0F);
        addView(default_cfg_header);

        defaults_ = new brls::Label();
        defaults_->setMarginTop(8.0F);
        defaults_->setFontSize(16.0F);
        defaults_->setSingleLine(false);
        defaults_->setAutoAnimate(false);
        addView(defaults_);

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
        footer->setMarginTop(SettingsSectionGap);
        footer->setHeight(40.0F);

        model_->scenario.status_label = new brls::Label();
        model_->scenario.status_label->setHeight(40.0F);

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
        header->setTitle("NX Reversing Toolbox");
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
        protocol_header->setSubtitle("Versions compiled into this toolbox build.");
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

class AppTabs final : public brls::TabFrame {
  public:
    explicit AppTabs(std::shared_ptr<UiModel> model) : model_(std::move(model)) {
        getAppletFrameItem()->title = APP_TITLE;
        getAppletFrameItem()->setIconFromFile("romfs:/img/toolbox.png");
        addTab("Main", [model = model_] { return new MainPage(model); });
        addTab("Logs", [model = model_] { return new LogsPage(model); });
        addTab("Settings", [model = model_] { return new SettingsPage(model); });
        addTab("Profiles", [model = model_] { return new ProfilesPage(model); });
        addSeparator();
        addTab("Scenarios", [model = model_] { return new ScenariosPage(model); });
        addSeparator();
        addTab("About", [model = model_] { return new AboutPage(model); });
        focusTab(0);
    }

  private:
    std::shared_ptr<UiModel> model_;
};

class AppActivity final : public brls::Activity {
  public:
    explicit AppActivity(std::shared_ptr<UiModel> model) : model_(std::move(model)) {}

    ~AppActivity() override {
        model_->active = false;
    }

    brls::View* createContentView() override {
        auto* tabs = new AppTabs(model_);
        return new brls::AppletFrame(tabs);
    }

    void onContentAvailable() override {
        registerAction("Bump tunnel binding", brls::BUTTON_RT, [model = model_](brls::View*) {
            RequestUdpBindBump(model);
            return true;
        });
    }

  private:
    std::shared_ptr<UiModel> model_;
};

} // namespace

int RunAppUi(AppContext& context, RuntimeConfig defaults, ConfigLoadReport loaded_config) {
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
    brls::getStyle().addMetric("brls/tab_frame/sidebar_width", ToolboxSidebarWidth);
    brls::getStyle().addMetric("brls/sidebar/padding_left", 52.0F);
    brls::getStyle().addMetric("brls/sidebar/padding_right", 24.0F);
    brls::Application::createWindow(APP_TITLE);
    brls::Application::setGlobalQuit(true);
    brls::Application::pushActivity(new AppActivity(model));

    while (brls::Application::mainLoop()) {
    }

    model->active = false;
    logger::SetUiSink({});
    return 0;
}

} // namespace toolbox
