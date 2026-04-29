// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <map>
#include <thread>

#include <QIcon>
#include <QMetaObject>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>

#include "common/assert.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/hle/service/sm/sm.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#include "hid_core/resources/npad/npad.h"
#include "qt_common/qt_compat.h"
#include "ui_qt_controller.h"
#include "yuzu/applets/qt_controller.h"
#include "input_common/main.h"
#include "yuzu/configuration/configure_input.h"
#include "yuzu/configuration/configure_input_profile_dialog.h"
#include "yuzu/configuration/configure_motion_touch.h"
#include "yuzu/configuration/configure_vibration.h"
#include "yuzu/configuration/input_profiles.h"
#include "yuzu/main_window.h"
#include "yuzu/util/controller_navigation.h"

// Small analog stick indicator matching the style of PlayerControlPreview::DrawJoystickDot.
// Dotted ring = range boundary; filled dot = current left-stick position.
class StickWidget : public QWidget {
public:
    explicit StickWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(28);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setPosition(float x, float y) {
        if (pos_x != x || pos_y != y) {
            pos_x = x;
            pos_y = y;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const QString theme = QIcon::themeName();
        const bool dark = theme.contains(QStringLiteral("dark")) ||
                          theme.contains(QStringLiteral("midnight"));
        const QColor ring_color = dark ? QColor(160, 160, 160) : QColor(145, 145, 145);
        const QColor dot_color  = dark ? QColor(170, 238, 255) : QColor(0, 0, 200);

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const float cx = width() / 2.0f;
        const float cy = height() / 2.0f;
        const float r = std::min(cx, cy) - 2.0f;

        // Dotted range ring (matches DrawJoystickProperties)
        QPen pen(ring_color, 1, Qt::DotLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(cx, cy), r, r);

        // Stick dot (matches DrawJoystickDot, radius 2 scaled proportionally)
        const float dot_r = std::max(2.0f, r * 0.15f);
        p.setPen(Qt::NoPen);
        p.setBrush(dot_color);
        p.drawEllipse(QPointF(cx + pos_x * (r - dot_r),
                              cy - pos_y * (r - dot_r)),  // flip Y for screen coords
                      dot_r, dot_r);
    }

private:
    float pos_x = 0.0f;
    float pos_y = 0.0f;
};

namespace {

void UpdateController(Core::HID::EmulatedController* controller,
                      Core::HID::NpadStyleIndex controller_type, bool connected) {
    if (controller->IsConnected(true)) {
        controller->Disconnect();
    }
    controller->SetNpadStyleIndex(controller_type);
    if (connected) {
        controller->Connect(true);
    }
}

// Returns true if the given controller type is compatible with the given parameters.
bool IsControllerCompatible(Core::HID::NpadStyleIndex controller_type,
                            Core::Frontend::ControllerParameters parameters) {
    switch (controller_type) {
    case Core::HID::NpadStyleIndex::Fullkey:
        return parameters.allow_pro_controller;
    case Core::HID::NpadStyleIndex::JoyconDual:
        return parameters.allow_dual_joycons;
    case Core::HID::NpadStyleIndex::JoyconLeft:
        return parameters.allow_left_joycon;
    case Core::HID::NpadStyleIndex::JoyconRight:
        return parameters.allow_right_joycon;
    case Core::HID::NpadStyleIndex::Handheld:
        return parameters.enable_single_mode && parameters.allow_handheld;
    case Core::HID::NpadStyleIndex::GameCube:
        return parameters.allow_gamecube_controller;
    default:
        return false;
    }
}

} // namespace

QtControllerSelectorDialog::QtControllerSelectorDialog(
    QWidget* parent, Core::Frontend::ControllerParameters parameters_,
    InputCommon::InputSubsystem* input_subsystem_, Core::System& system_)
    : QDialog(parent), ui(std::make_unique<Ui::QtControllerSelectorDialog>()),
      parameters(std::move(parameters_)), input_subsystem{input_subsystem_},
      input_profiles(std::make_unique<InputProfiles>()), system{system_} {
    ui->setupUi(this);

    player_widgets = {
        ui->widgetPlayer1, ui->widgetPlayer2, ui->widgetPlayer3, ui->widgetPlayer4,
        ui->widgetPlayer5, ui->widgetPlayer6, ui->widgetPlayer7, ui->widgetPlayer8,
    };

    player_groupboxes = {
        ui->groupPlayer1Connected, ui->groupPlayer2Connected, ui->groupPlayer3Connected,
        ui->groupPlayer4Connected, ui->groupPlayer5Connected, ui->groupPlayer6Connected,
        ui->groupPlayer7Connected, ui->groupPlayer8Connected,
    };

    connected_controller_icons = {
        ui->controllerPlayer1, ui->controllerPlayer2, ui->controllerPlayer3, ui->controllerPlayer4,
        ui->controllerPlayer5, ui->controllerPlayer6, ui->controllerPlayer7, ui->controllerPlayer8,
    };

    led_patterns_boxes = {{
        {ui->checkboxPlayer1LED1, ui->checkboxPlayer1LED2, ui->checkboxPlayer1LED3,
         ui->checkboxPlayer1LED4},
        {ui->checkboxPlayer2LED1, ui->checkboxPlayer2LED2, ui->checkboxPlayer2LED3,
         ui->checkboxPlayer2LED4},
        {ui->checkboxPlayer3LED1, ui->checkboxPlayer3LED2, ui->checkboxPlayer3LED3,
         ui->checkboxPlayer3LED4},
        {ui->checkboxPlayer4LED1, ui->checkboxPlayer4LED2, ui->checkboxPlayer4LED3,
         ui->checkboxPlayer4LED4},
        {ui->checkboxPlayer5LED1, ui->checkboxPlayer5LED2, ui->checkboxPlayer5LED3,
         ui->checkboxPlayer5LED4},
        {ui->checkboxPlayer6LED1, ui->checkboxPlayer6LED2, ui->checkboxPlayer6LED3,
         ui->checkboxPlayer6LED4},
        {ui->checkboxPlayer7LED1, ui->checkboxPlayer7LED2, ui->checkboxPlayer7LED3,
         ui->checkboxPlayer7LED4},
        {ui->checkboxPlayer8LED1, ui->checkboxPlayer8LED2, ui->checkboxPlayer8LED3,
         ui->checkboxPlayer8LED4},
    }};

    explain_text_labels = {
        ui->labelPlayer1Explain, ui->labelPlayer2Explain, ui->labelPlayer3Explain,
        ui->labelPlayer4Explain, ui->labelPlayer5Explain, ui->labelPlayer6Explain,
        ui->labelPlayer7Explain, ui->labelPlayer8Explain,
    };

    emulated_controllers = {
        ui->comboPlayer1Emulated, ui->comboPlayer2Emulated, ui->comboPlayer3Emulated,
        ui->comboPlayer4Emulated, ui->comboPlayer5Emulated, ui->comboPlayer6Emulated,
        ui->comboPlayer7Emulated, ui->comboPlayer8Emulated,
    };

    player_labels = {
        ui->labelPlayer1, ui->labelPlayer2, ui->labelPlayer3, ui->labelPlayer4,
        ui->labelPlayer5, ui->labelPlayer6, ui->labelPlayer7, ui->labelPlayer8,
    };

    connected_controller_labels = {
        ui->labelConnectedPlayer1, ui->labelConnectedPlayer2, ui->labelConnectedPlayer3,
        ui->labelConnectedPlayer4, ui->labelConnectedPlayer5, ui->labelConnectedPlayer6,
        ui->labelConnectedPlayer7, ui->labelConnectedPlayer8,
    };

    connected_controller_checkboxes = {
        ui->checkboxPlayer1Connected, ui->checkboxPlayer2Connected, ui->checkboxPlayer3Connected,
        ui->checkboxPlayer4Connected, ui->checkboxPlayer5Connected, ui->checkboxPlayer6Connected,
        ui->checkboxPlayer7Connected, ui->checkboxPlayer8Connected,
    };

    ui->labelError->setStyleSheet(QStringLiteral("QLabel { color: gray; }"));
    ui->labelError->setText(tr("A: Connect  ●  Y: Controller Type  ●  Start: OK  ●  B: Cancel"));

    // Setup/load everything prior to setting up connections.
    // This avoids unintentionally changing the states of elements while loading them in.
    SetSupportedControllers();
    DisableUnsupportedPlayers();

    for (std::size_t player_index = 0; player_index < NUM_PLAYERS; ++player_index) {
        SetEmulatedControllers(player_index);
    }

    LoadConfiguration();

    controller_navigation = new ControllerNavigation(system.HIDCore(), this);

    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        SetExplainText(i);
        UpdateControllerIcon(i);
        UpdateLEDPattern(i);
        UpdateBorderColor(i);

        connect(player_groupboxes[i], &QGroupBox::toggled, [this, i](bool checked) {
            // Reconnect current controller if it was the last one checked
            // (player number was reduced by more than one)
            const bool reconnect_first = !checked && i < player_groupboxes.size() - 1 &&
                                         player_groupboxes[i + 1]->isChecked();

            // Ensures that connecting a controller changes the number of players
            if (connected_controller_checkboxes[i]->isChecked() != checked) {
                // Ensures that the players are always connected in sequential order
                PropagatePlayerNumberChanged(i, checked, reconnect_first);
            }
        });
        connect(connected_controller_checkboxes[i], &QCheckBox::clicked, [this, i](bool checked) {
            // Reconnect current controller if it was the last one checked
            // (player number was reduced by more than one)
            const bool reconnect_first = !checked &&
                                         i < connected_controller_checkboxes.size() - 1 &&
                                         connected_controller_checkboxes[i + 1]->isChecked();

            // Ensures that the players are always connected in sequential order
            PropagatePlayerNumberChanged(i, checked, reconnect_first);
        });

        connect(emulated_controllers[i], qOverload<int>(&QComboBox::currentIndexChanged),
                [this, i](int) {
                    UpdateControllerIcon(i);
                    UpdateControllerState(i);
                    UpdateLEDPattern(i);
                    CheckIfParametersMet();
                });

        connect(connected_controller_checkboxes[i], &QCheckBox::STATE_CHANGED,
                [this, i](int state) {
                    player_groupboxes[i]->setChecked(state == Qt::Checked);
                    UpdateControllerIcon(i);
                    UpdateControllerState(i);
                    UpdateLEDPattern(i);
                    UpdateBorderColor(i);
                    CheckIfParametersMet();
                });

        if (i == 0) {
            connect(emulated_controllers[i], qOverload<int>(&QComboBox::currentIndexChanged),
                    [this, i](int index) {
                        UpdateDockedState(GetControllerTypeFromIndex(index, i) ==
                                          Core::HID::NpadStyleIndex::Handheld);
                    });
        }
    }

    connect(ui->vibrationButton, &QPushButton::clicked, this,
            &QtControllerSelectorDialog::CallConfigureVibrationDialog);

    connect(ui->motionButton, &QPushButton::clicked, this,
            &QtControllerSelectorDialog::CallConfigureMotionTouchDialog);

    connect(ui->inputConfigButton, &QPushButton::clicked, this,
            &QtControllerSelectorDialog::CallConfigureInputProfileDialog);

    connect(ui->buttonBox, &QDialogButtonBox::accepted, this,
            &QtControllerSelectorDialog::ApplyConfiguration);

    connect(controller_navigation, &ControllerNavigation::TriggerKeyboardEvent,
            [this](Qt::Key key) {
                QKeyEvent* event = new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier);
                QCoreApplication::postEvent(this, event);
            });

    // Register per-player A/B callbacks so every physical controller can claim its own slot.
    // Fires on the HID thread; we detect rising edges and marshal to Qt thread via QueuedConnection.
    {
        QPointer<QtControllerSelectorDialog> self(this);
        auto register_cb = [this, &self](Core::HID::EmulatedController* controller,
                                         std::size_t slot) -> int {
            Core::HID::ControllerUpdateCallback cb{
                .on_change = [this, self, controller, slot](Core::HID::ControllerTriggerType type) {
                    if (type != Core::HID::ControllerTriggerType::Button)
                        return;
                    const auto buttons = controller->GetButtonsValues();
                    const bool a_now = buttons[Settings::NativeButton::A].value;
                    const bool b_now = buttons[Settings::NativeButton::B].value;
                    bool fire_a = false, fire_b = false;
                    {
                        std::scoped_lock lock{claim_state_mutex};
                        if (a_now && !prev_a_pressed[slot]) fire_a = true;
                        if (b_now && !prev_b_pressed[slot]) fire_b = true;
                        prev_a_pressed[slot] = a_now;
                        prev_b_pressed[slot] = b_now;
                    }
                    if (fire_a)
                        QMetaObject::invokeMethod(this, [self, slot]() {
                            if (self) self->OnPlayerButtonA(slot);
                        }, Qt::QueuedConnection);
                    if (fire_b)
                        QMetaObject::invokeMethod(this, [self, slot]() {
                            if (self) self->OnPlayerButtonB(slot);
                        }, Qt::QueuedConnection);
                },
                .is_npad_service = false,
            };
            return controller->SetCallback(std::move(cb));
        };

        for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
            player_claim_callback_keys[i] =
                register_cb(system.HIDCore().GetEmulatedControllerByIndex(i), i);
        }
        handheld_claim_callback_key = register_cb(
            system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Handheld), 0);
    }

    // Prevent child widgets from drawing native focus rings (blue OS highlight).
    // All navigation is handled via keyPressEvent on the dialog itself.
    for (auto* gb : player_groupboxes) {
        gb->setFocusPolicy(Qt::NoFocus);
    }

    // Route all key events from comboboxes back to the dialog so gamepad nav
    // is never consumed by the combobox's own keyboard handling.
    for (auto* combo : emulated_controllers) {
        combo->installEventFilter(this);
    }

    if (auto* ok = ui->buttonBox->button(QDialogButtonBox::Ok)) {
        ok->setText(tr("(+)  OK"));
    }
    if (auto* cancel = ui->buttonBox->button(QDialogButtonBox::Cancel)) {
        cancel->setText(tr("(B)  Cancel"));
    }

    // Per-slot button hints: (A) on the groupbox checkbox title, (Y) to the left
    // of the controller type combobox.
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        player_groupboxes[i]->setTitle(QStringLiteral("(A)"));

        auto* outer = qobject_cast<QVBoxLayout*>(player_widgets[i]->layout());
        if (outer) {
            const int combo_idx = outer->indexOf(emulated_controllers[i]);
            if (combo_idx >= 0) {
                delete outer->takeAt(combo_idx); // removes item, not the widget
                auto* row = new QWidget(player_widgets[i]);
                auto* hbox = new QHBoxLayout(row);
                hbox->setContentsMargins(0, 0, 0, 0);
                hbox->setSpacing(4);
                auto* hint = new QLabel(QStringLiteral("(Y)"), row);
                hint->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
                hint->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
                hint->setStyleSheet(QStringLiteral("color: gray;"));
                hbox->addWidget(hint);
                hbox->addWidget(emulated_controllers[i], 1);
                outer->insertWidget(combo_idx, row);
            }
        }
    }

    // Add a stick indicator below the LEDs in each player slot.
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        stick_indicators[i] = new StickWidget(player_groupboxes[i]);
        if (auto* layout = qobject_cast<QVBoxLayout*>(player_groupboxes[i]->layout())) {
            layout->addWidget(stick_indicators[i]);
        }
    }

    // Build the cached gamepad list (physical controllers only, no keyboard/mouse).
    // Deduplicate display names by appending (2), (3)... when the same name appears more than once.
    {
        const auto all_devices = input_subsystem->GetInputDevices();
        for (const auto& dev : all_devices) {
            if (input_subsystem->IsController(dev)) {
                cached_input_devices.push_back(dev);
            }
        }
    }
    cached_device_labels.clear();
    {
        std::map<std::string, int> name_counts;
        for (const auto& dev : cached_input_devices) {
            name_counts[dev.Get("display", "Unknown")]++;
        }
        std::map<std::string, int> name_seen;
        for (const auto& dev : cached_input_devices) {
            const std::string base = dev.Get("display", "Unknown");
            if (name_counts[base] > 1) {
                cached_device_labels.push_back(
                    base + " (" + std::to_string(++name_seen[base]) + ")");
            } else {
                cached_device_labels.push_back(base);
            }
        }
    }

    // Add an input device (gamepad) selector row below each controller-type row.
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        input_device_combos[i] = new QComboBox(player_widgets[i]);
        input_device_combos[i]->installEventFilter(this);

        input_device_combos[i]->blockSignals(true);
        if (cached_input_devices.empty()) {
            input_device_combos[i]->addItem(tr("No gamepad detected"));
            input_device_combos[i]->setEnabled(false);
        } else {
            for (const auto& label : cached_device_labels) {
                input_device_combos[i]->addItem(QString::fromStdString(label));
            }
        }
        input_device_combos[i]->blockSignals(false);

        auto* outer = qobject_cast<QVBoxLayout*>(player_widgets[i]->layout());
        if (outer) {
            auto* row = new QWidget(player_widgets[i]);
            auto* hbox = new QHBoxLayout(row);
            hbox->setContentsMargins(0, 0, 0, 0);
            hbox->setSpacing(4);
            auto* hint = new QLabel(QStringLiteral("(X)"), row);
            hint->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
            hint->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            hint->setStyleSheet(QStringLiteral("color: gray;"));
            hbox->addWidget(hint);
            hbox->addWidget(input_device_combos[i], 1);
            outer->addWidget(row);
        }

        connect(input_device_combos[i], qOverload<int>(&QComboBox::currentIndexChanged),
                [this, i](int) { ApplyInputDevice(i); });
    }

    // Poll left-stick positions at 20 Hz to update the indicators.
    stick_poll_timer = new QTimer(this);
    connect(stick_poll_timer, &QTimer::timeout, this, [this] {
        for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
            if (player_widgets[i]->isHidden())
                continue;
            const auto* controller = system.HIDCore().GetEmulatedControllerByIndex(i);
            // GetSticksValues() reads the config-mode state that is active while the applet
            // is open (EnableAllControllerConfiguration is called in LoadConfiguration).
            const auto sticks = controller->GetSticksValues();
            stick_indicators[i]->setPosition(
                sticks[Settings::NativeAnalog::LStick].x.value,
                sticks[Settings::NativeAnalog::LStick].y.value);
        }
    });
    stick_poll_timer->start(50);
    if (auto* ok = ui->buttonBox->button(QDialogButtonBox::Ok)) {
        ok->setFocusPolicy(Qt::NoFocus);
    }

    // Defer initial gamepad focus until after the dialog is shown.
    // isVisible() is always false during the constructor; setStyleSheet() applied before
    // show() may not paint. QTimer::singleShot(0) fires after exec() starts the event loop.
    {
        std::size_t default_focus = NUM_PLAYERS;
        for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
            if (!player_widgets[i]->isHidden() && !player_groupboxes[i]->isChecked()) {
                default_focus = i;
                break;
            }
        }
        if (default_focus == NUM_PLAYERS) {
            // All connected: prefer P2, fall back to P1
            if (!player_widgets[1]->isHidden()) {
                default_focus = 1;
            } else if (!player_widgets[0]->isHidden()) {
                default_focus = 0;
            }
        }
        if (default_focus < NUM_PLAYERS) {
            QTimer::singleShot(0, this, [this, default_focus]() {
                // Force layout recalculation now that the dialog is visible.
                // Without this, programmatically inserted widgets (Y/X hint rows,
                // stick indicators) can render at wrong positions until focus moves.
                for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
                    if (player_widgets[i]->isHidden())
                        continue;
                    if (auto* l = player_widgets[i]->layout())
                        l->activate();
                    if (auto* l = player_groupboxes[i]->layout())
                        l->activate();
                }
                SetFocusedPlayer(default_focus);
            });
        }
    }

    // Enhancement: Check if the parameters have already been met before disconnecting controllers.
    // If all the parameters are met AND only allows a single player,
    // stop the constructor here as we do not need to continue.
    if (CheckIfParametersMet() && parameters.enable_single_mode) {
        return;
    }

    // If keep_controllers_connected is false, forcefully disconnect all controllers
    if (!parameters.keep_controllers_connected) {
        for (auto player : player_groupboxes) {
            player->setChecked(false);
        }
    }

    resize(0, 0);
}

QtControllerSelectorDialog::~QtControllerSelectorDialog() {
    controller_navigation->UnloadController();
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        system.HIDCore().GetEmulatedControllerByIndex(i)->DeleteCallback(
            player_claim_callback_keys[i]);
    }
    system.HIDCore()
        .GetEmulatedController(Core::HID::NpadIdType::Handheld)
        ->DeleteCallback(handheld_claim_callback_key);
    system.HIDCore().DisableAllControllerConfiguration();
}

int QtControllerSelectorDialog::exec() {
    if (parameters_met && !force_show) {
        return QDialog::Accepted;
    }
    return QDialog::exec();
}

void QtControllerSelectorDialog::ApplyConfiguration() {
    const bool pre_docked_mode = Settings::IsDockedMode();
    const bool docked_mode_selected = ui->radioDocked->isChecked();
    Settings::values.use_docked_mode.SetValue(
        docked_mode_selected ? Settings::ConsoleMode::Docked : Settings::ConsoleMode::Handheld);
    OnDockedModeChanged(pre_docked_mode, docked_mode_selected, system);

    Settings::values.vibration_enabled.SetValue(ui->vibrationGroup->isChecked());
    Settings::values.motion_enabled.SetValue(ui->motionGroup->isChecked());
}

void QtControllerSelectorDialog::LoadConfiguration() {
    system.HIDCore().EnableAllControllerConfiguration();

    const auto* handheld = system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Handheld);
    for (std::size_t index = 0; index < NUM_PLAYERS; ++index) {
        const auto* controller = system.HIDCore().GetEmulatedControllerByIndex(index);
        const auto connected =
            controller->IsConnected(true) || (index == 0 && handheld->IsConnected(true));
        player_groupboxes[index]->setChecked(connected);
        connected_controller_checkboxes[index]->setChecked(connected);
        emulated_controllers[index]->setCurrentIndex(
            GetIndexFromControllerType(controller->GetNpadStyleIndex(true), index));
    }

    UpdateDockedState(handheld->IsConnected(true));

    ui->vibrationGroup->setChecked(Settings::values.vibration_enabled.GetValue());
    ui->motionGroup->setChecked(Settings::values.motion_enabled.GetValue());
}

void QtControllerSelectorDialog::CallConfigureVibrationDialog() {
    ConfigureVibration dialog(this, system.HIDCore());

    dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                          Qt::WindowSystemMenuHint);
    dialog.setWindowModality(Qt::WindowModal);

    if (dialog.exec() == QDialog::Accepted) {
        dialog.ApplyConfiguration();
    }
}

void QtControllerSelectorDialog::CallConfigureMotionTouchDialog() {
    ConfigureMotionTouch dialog(this, input_subsystem);

    dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                          Qt::WindowSystemMenuHint);
    dialog.setWindowModality(Qt::WindowModal);

    if (dialog.exec() == QDialog::Accepted) {
        dialog.ApplyConfiguration();
    }
}

void QtControllerSelectorDialog::CallConfigureInputProfileDialog() {
    ConfigureInputProfileDialog dialog(this, input_subsystem, input_profiles.get(), system);

    dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                          Qt::WindowSystemMenuHint);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.exec();
}

void QtControllerSelectorDialog::keyPressEvent(QKeyEvent* evt) {
    // Returns the column (0-3) of the first visible slot found starting at start_col and
    // stepping by dir (+1 or -1). Returns -1 if none found.
    const auto find_col = [this](int grid_row, int start_col, int dir) -> int {
        for (int c = start_col; c >= 0 && c < 4; c += dir) {
            const std::size_t idx = static_cast<std::size_t>(grid_row * 4 + c);
            if (idx < NUM_PLAYERS && player_widgets[idx]->isVisible())
                return c;
        }
        return -1;
    };

    const auto is_row_visible = [this](int grid_row) -> bool {
        for (int c = 0; c < 4; ++c) {
            const std::size_t idx = static_cast<std::size_t>(grid_row * 4 + c);
            if (idx < NUM_PLAYERS && player_widgets[idx]->isVisible())
                return true;
        }
        return false;
    };

    const auto show_error = [this] {
        ui->labelError->setStyleSheet(QStringLiteral("QLabel { color: red; }"));
        ui->labelError->setText(tr("Not enough controllers"));
    };

    switch (evt->key()) {

    case Qt::Key_Return: // Start/Plus → always confirm
        if (parameters_met) {
            ApplyConfiguration();
            accept();
        } else {
            show_error();
        }
        return;

    case Qt::Key_Enter: { // A button → confirm if OK button focused
        if (focused_button == FocusedButton::OK) {
            if (parameters_met) {
                ApplyConfiguration();
                accept();
            } else {
                show_error();
            }
        }
        // Connection toggling is handled per-controller by OnPlayerButtonA/B.
        return;
    }

    case Qt::Key_Escape: // B button → cancel, close without applying configuration.
        reject();
        return;

    case Qt::Key_Y: // Y → cycle controller type for focused player
        if (focused_player_index < NUM_PLAYERS &&
            player_widgets[focused_player_index]->isVisible()) {
            auto* combo = emulated_controllers[focused_player_index];
            const int count = combo->count();
            if (count > 1)
                combo->setCurrentIndex((combo->currentIndex() + 1) % count);
        }
        return;

    case Qt::Key_X: // X → cycle input device (gamepad) for focused player
        if (focused_player_index < NUM_PLAYERS &&
            player_widgets[focused_player_index]->isVisible() &&
            !cached_input_devices.empty()) {
            auto* combo = input_device_combos[focused_player_index];
            const int count = combo->count();
            if (count > 0)
                combo->setCurrentIndex((combo->currentIndex() + 1) % count);
        }
        return;

    case Qt::Key_Left: {
        if (focused_button == FocusedButton::None && focused_player_index < NUM_PLAYERS) {
            const int row = static_cast<int>(focused_player_index) / 4;
            const int col = static_cast<int>(focused_player_index) % 4;
            const int found = find_col(row, col - 1, -1);
            if (found >= 0)
                SetFocusedPlayer(static_cast<std::size_t>(row * 4 + found));
        }
        return;
    }

    case Qt::Key_Right: {
        if (focused_button == FocusedButton::None && focused_player_index < NUM_PLAYERS) {
            const int row = static_cast<int>(focused_player_index) / 4;
            const int col = static_cast<int>(focused_player_index) % 4;
            const int found = find_col(row, col + 1, +1);
            if (found >= 0)
                SetFocusedPlayer(static_cast<std::size_t>(row * 4 + found));
        }
        return;
    }

    case Qt::Key_Up: {
        if (focused_button != FocusedButton::None) {
            // Button row → try same column in row 2 (P5-P8), else row 1 (P1-P4)
            const int col = static_cast<int>(last_focused_player) % 4;
            if (is_row_visible(1)) {
                int found = find_col(1, col, -1); // same col or leftward
                if (found < 0)
                    found = find_col(1, 0, +1); // leftmost fallback
                if (found >= 0) {
                    SetFocusedPlayer(static_cast<std::size_t>(4 + found));
                    return;
                }
            }
            {
                int found = find_col(0, col, -1);
                if (found < 0)
                    found = find_col(0, 0, +1);
                if (found >= 0)
                    SetFocusedPlayer(static_cast<std::size_t>(found));
            }
        } else if (focused_player_index >= 4) {
            // Row 2 → row 1, same column
            const int col = static_cast<int>(focused_player_index) % 4;
            int found = find_col(0, col, -1);
            if (found < 0)
                found = find_col(0, 0, +1);
            if (found >= 0)
                SetFocusedPlayer(static_cast<std::size_t>(found));
        }
        // Row 1 → no-op (already topmost)
        return;
    }

    case Qt::Key_Down: {
        if (focused_button != FocusedButton::None) {
            // Already at bottom: no-op
        } else if (focused_player_index < NUM_PLAYERS) {
            const int row = static_cast<int>(focused_player_index) / 4;
            const int col = static_cast<int>(focused_player_index) % 4;
            if (row == 0) {
                // Row 1 → try row 2 (same col or leftward), else button row
                if (is_row_visible(1)) {
                    int found = find_col(1, col, -1);
                    if (found < 0)
                        found = find_col(1, 0, +1);
                    if (found >= 0) {
                        SetFocusedPlayer(static_cast<std::size_t>(4 + found));
                        return;
                    }
                }
                SetFocusedButton(FocusedButton::OK);
            } else {
                // Row 2 → button row
                SetFocusedButton(FocusedButton::OK);
            }
        }
        return;
    }

    default:
        break;
    }

    QDialog::keyPressEvent(evt);
}

bool QtControllerSelectorDialog::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        keyPressEvent(static_cast<QKeyEvent*>(event));
        return true;
    }
    return QDialog::eventFilter(obj, event);
}

void QtControllerSelectorDialog::RefreshPlayerSlotStyle(std::size_t player_index) {
    const bool is_focused = (player_index == focused_player_index);
    QString style;

    if (is_focused) {
        const QColor hl = player_groupboxes[player_index]->palette().color(QPalette::Highlight);
        QColor bg = hl;
        bg.setAlpha(70);
        style = QStringLiteral("QGroupBox#groupPlayer%1Connected { "
                               "background-color: rgba(%2,%3,%4,%5); }")
                    .arg(player_index + 1)
                    .arg(bg.red())
                    .arg(bg.green())
                    .arg(bg.blue())
                    .arg(bg.alpha());
    }

    player_groupboxes[player_index]->setStyleSheet(style);
}

void QtControllerSelectorDialog::SetFocusedPlayer(std::size_t index) {
    // Clear button focus visuals
    if (focused_button == FocusedButton::OK) {
        if (auto* b = ui->buttonBox->button(QDialogButtonBox::Ok))
            b->setStyleSheet({});
        focused_button = FocusedButton::None;
    }

    const std::size_t prev = focused_player_index;
    focused_player_index = index;

    if (prev < NUM_PLAYERS)
        RefreshPlayerSlotStyle(prev);

    if (index < NUM_PLAYERS && !player_widgets[index]->isHidden()) {
        last_focused_player = index;
        RefreshPlayerSlotStyle(index);
        emulated_controllers[index]->setFocus(Qt::OtherFocusReason);
    }
}

void QtControllerSelectorDialog::SetFocusedButton(FocusedButton btn) {
    // Clear player focus visuals
    const std::size_t prev_player = focused_player_index;
    focused_player_index = NUM_PLAYERS;
    if (prev_player < NUM_PLAYERS)
        RefreshPlayerSlotStyle(prev_player);

    // Clear old OK focus visual
    if (focused_button == FocusedButton::OK) {
        if (auto* b = ui->buttonBox->button(QDialogButtonBox::Ok))
            b->setStyleSheet({});
    }

    focused_button = btn;

    if (btn == FocusedButton::OK) {
        const QColor hl = ui->buttonBox->palette().color(QPalette::Highlight);
        const QColor hl_text = ui->buttonBox->palette().color(QPalette::HighlightedText);
        const QString hl_style =
            QStringLiteral("QPushButton { border: 2px solid %1; background-color: %2; color: %3; }")
                .arg(hl.name())
                .arg(hl.name())
                .arg(hl_text.name());
        if (auto* b = ui->buttonBox->button(QDialogButtonBox::Ok))
            b->setStyleSheet(hl_style);
    }
}

bool QtControllerSelectorDialog::CheckIfParametersMet() {
    const auto num_connected_players = static_cast<int>(
        std::count_if(player_groupboxes.begin(), player_groupboxes.end(),
                      [](const QGroupBox* player) { return player->isChecked(); }));

    const auto min_supported_players = parameters.enable_single_mode ? 1 : parameters.min_players;
    const auto max_supported_players = parameters.enable_single_mode ? 1 : parameters.max_players;

    if (num_connected_players < min_supported_players ||
        num_connected_players > max_supported_players) {
        parameters_met = false;
    } else {
        parameters_met = [this] {
            for (std::size_t index = 0; index < NUM_PLAYERS; ++index) {
                if (!player_groupboxes[index]->isChecked() ||
                    !player_groupboxes[index]->isEnabled()) {
                    continue;
                }
                if (!IsControllerCompatible(
                        GetControllerTypeFromIndex(
                            emulated_controllers[index]->currentIndex(), index),
                        parameters)) {
                    return false;
                }
            }
            return true;
        }();
    }

    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(parameters_met);

    if (parameters_met) {
        ui->labelError->setStyleSheet(QStringLiteral("QLabel { color: #0097DB; }"));
        ui->labelError->setText(tr("Requirements met  ●  Start: OK"));
    } else {
        ui->labelError->setStyleSheet(QStringLiteral("QLabel { color: gray; }"));
        ui->labelError->setText(tr("Requirements not met"));
    }

    return parameters_met;
}

void QtControllerSelectorDialog::SetSupportedControllers() {
    const QString theme = [] {
        if (QIcon::themeName().contains(QStringLiteral("dark"))) {
            return QStringLiteral("_dark");
        } else if (QIcon::themeName().contains(QStringLiteral("midnight"))) {
            return QStringLiteral("_midnight");
        } else {
            return QString{};
        }
    }();

    if (parameters.enable_single_mode && parameters.allow_handheld) {
        ui->controllerSupported1->setStyleSheet(
            QStringLiteral("image: url(:/controller/applet_handheld%0); ").arg(theme));
    } else {
        ui->controllerSupported1->setStyleSheet(
            QStringLiteral("image: url(:/controller/applet_handheld%0_disabled); ").arg(theme));
    }

    if (parameters.allow_dual_joycons) {
        ui->controllerSupported2->setStyleSheet(
            QStringLiteral("image: url(:/controller/applet_dual_joycon%0); ").arg(theme));
    } else {
        ui->controllerSupported2->setStyleSheet(
            QStringLiteral("image: url(:/controller/applet_dual_joycon%0_disabled); ").arg(theme));
    }

    if (parameters.allow_left_joycon) {
        ui->controllerSupported3->setStyleSheet(
            QStringLiteral("image: url(:/controller/applet_joycon_left%0); ").arg(theme));
    } else {
        ui->controllerSupported3->setStyleSheet(
            QStringLiteral("image: url(:/controller/applet_joycon_left%0_disabled); ").arg(theme));
    }

    if (parameters.allow_right_joycon) {
        ui->controllerSupported4->setStyleSheet(
            QStringLiteral("image: url(:/controller/applet_joycon_right%0); ").arg(theme));
    } else {
        ui->controllerSupported4->setStyleSheet(
            QStringLiteral("image: url(:/controller/applet_joycon_right%0_disabled); ").arg(theme));
    }

    if (parameters.allow_pro_controller || parameters.allow_gamecube_controller) {
        ui->controllerSupported5->setStyleSheet(
            QStringLiteral("image: url(:/controller/applet_pro_controller%0); ").arg(theme));
    } else {
        ui->controllerSupported5->setStyleSheet(
            QStringLiteral("image: url(:/controller/applet_pro_controller%0_disabled); ")
                .arg(theme));
    }

    // enable_single_mode overrides min_players and max_players.
    if (parameters.enable_single_mode) {
        ui->numberSupportedLabel->setText(QStringLiteral("1"));
        return;
    }

    if (parameters.min_players == parameters.max_players) {
        ui->numberSupportedLabel->setText(QStringLiteral("%1").arg(parameters.max_players));
    } else {
        ui->numberSupportedLabel->setText(
            QStringLiteral("%1 - %2").arg(parameters.min_players).arg(parameters.max_players));
    }
}

void QtControllerSelectorDialog::SetEmulatedControllers(std::size_t player_index) {
    const auto npad_style_set = system.HIDCore().GetSupportedStyleTag();
    auto& pairs = index_controller_type_pairs[player_index];

    pairs.clear();
    emulated_controllers[player_index]->clear();

    const auto add_item = [&](Core::HID::NpadStyleIndex controller_type,
                              const QString& controller_name) {
        pairs.emplace_back(emulated_controllers[player_index]->count(), controller_type);
        emulated_controllers[player_index]->addItem(controller_name);
    };

    if (npad_style_set.fullkey == 1) {
        add_item(Core::HID::NpadStyleIndex::Fullkey, tr("Pro Controller"));
    }

    if (npad_style_set.joycon_dual == 1) {
        add_item(Core::HID::NpadStyleIndex::JoyconDual, tr("Dual Joycons"));
    }

    if (npad_style_set.joycon_left == 1) {
        add_item(Core::HID::NpadStyleIndex::JoyconLeft, tr("Left Joycon"));
    }

    if (npad_style_set.joycon_right == 1) {
        add_item(Core::HID::NpadStyleIndex::JoyconRight, tr("Right Joycon"));
    }

    if (player_index == 0 && npad_style_set.handheld == 1) {
        add_item(Core::HID::NpadStyleIndex::Handheld, tr("Handheld"));
    }

    if (npad_style_set.gamecube == 1) {
        add_item(Core::HID::NpadStyleIndex::GameCube, tr("GameCube Controller"));
    }

    // Disable all unsupported controllers
    if (!Settings::values.enable_all_controllers) {
        return;
    }

    if (npad_style_set.palma == 1) {
        add_item(Core::HID::NpadStyleIndex::Pokeball, tr("Poke Ball Plus"));
    }

    if (npad_style_set.lark == 1) {
        add_item(Core::HID::NpadStyleIndex::NES, tr("NES Controller"));
    }

    if (npad_style_set.lucia == 1) {
        add_item(Core::HID::NpadStyleIndex::SNES, tr("SNES Controller"));
    }

    if (npad_style_set.lagoon == 1) {
        add_item(Core::HID::NpadStyleIndex::N64, tr("N64 Controller"));
    }

    if (npad_style_set.lager == 1) {
        add_item(Core::HID::NpadStyleIndex::SegaGenesis, tr("Sega Genesis"));
    }
}

Core::HID::NpadStyleIndex QtControllerSelectorDialog::GetControllerTypeFromIndex(
    int index, std::size_t player_index) const {
    const auto& pairs = index_controller_type_pairs[player_index];

    const auto it = std::find_if(pairs.begin(), pairs.end(),
                                 [index](const auto& pair) { return pair.first == index; });

    if (it == pairs.end()) {
        return Core::HID::NpadStyleIndex::Fullkey;
    }

    return it->second;
}

int QtControllerSelectorDialog::GetIndexFromControllerType(Core::HID::NpadStyleIndex type,
                                                           std::size_t player_index) const {
    const auto& pairs = index_controller_type_pairs[player_index];

    const auto it = std::find_if(pairs.begin(), pairs.end(),
                                 [type](const auto& pair) { return pair.second == type; });

    if (it == pairs.end()) {
        return 0;
    }

    return it->first;
}

void QtControllerSelectorDialog::UpdateControllerIcon(std::size_t player_index) {
    if (!player_groupboxes[player_index]->isChecked()) {
        connected_controller_icons[player_index]->setStyleSheet(QString{});
        player_labels[player_index]->show();
        return;
    }

    const QString stylesheet = [this, player_index] {
        switch (GetControllerTypeFromIndex(emulated_controllers[player_index]->currentIndex(),
                                           player_index)) {
        case Core::HID::NpadStyleIndex::Fullkey:
        case Core::HID::NpadStyleIndex::GameCube:
            return QStringLiteral("image: url(:/controller/applet_pro_controller%0); ");
        case Core::HID::NpadStyleIndex::JoyconDual:
            return QStringLiteral("image: url(:/controller/applet_dual_joycon%0); ");
        case Core::HID::NpadStyleIndex::JoyconLeft:
            return QStringLiteral("image: url(:/controller/applet_joycon_left%0); ");
        case Core::HID::NpadStyleIndex::JoyconRight:
            return QStringLiteral("image: url(:/controller/applet_joycon_right%0); ");
        case Core::HID::NpadStyleIndex::Handheld:
            return QStringLiteral("image: url(:/controller/applet_handheld%0); ");
        default:
            return QString{};
        }
    }();

    if (stylesheet.isEmpty()) {
        connected_controller_icons[player_index]->setStyleSheet(QString{});
        player_labels[player_index]->show();
        return;
    }

    const QString theme = [] {
        if (QIcon::themeName().contains(QStringLiteral("dark"))) {
            return QStringLiteral("_dark");
        } else if (QIcon::themeName().contains(QStringLiteral("midnight"))) {
            return QStringLiteral("_midnight");
        } else {
            return QString{};
        }
    }();

    connected_controller_icons[player_index]->setStyleSheet(stylesheet.arg(theme));
    player_labels[player_index]->hide();
}

void QtControllerSelectorDialog::UpdateControllerState(std::size_t player_index) {
    auto* controller = system.HIDCore().GetEmulatedControllerByIndex(player_index);

    const auto controller_type = GetControllerTypeFromIndex(
        emulated_controllers[player_index]->currentIndex(), player_index);
    const auto player_connected = player_groupboxes[player_index]->isChecked() &&
                                  controller_type != Core::HID::NpadStyleIndex::Handheld;

    if (controller->GetNpadStyleIndex(true) == controller_type &&
        controller->IsConnected(true) == player_connected) {
        return;
    }

    // Disconnect the controller first.
    UpdateController(controller, controller_type, false);

    // Handheld
    if (player_index == 0) {
        if (controller_type == Core::HID::NpadStyleIndex::Handheld) {
            auto* handheld =
                system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Handheld);
            UpdateController(handheld, Core::HID::NpadStyleIndex::Handheld,
                             player_groupboxes[player_index]->isChecked());
        }
    }

    UpdateController(controller, controller_type, player_connected);
}

void QtControllerSelectorDialog::UpdateLEDPattern(std::size_t player_index) {
    if (!player_groupboxes[player_index]->isChecked() ||
        GetControllerTypeFromIndex(emulated_controllers[player_index]->currentIndex(),
                                   player_index) == Core::HID::NpadStyleIndex::Handheld) {
        led_patterns_boxes[player_index][0]->setChecked(false);
        led_patterns_boxes[player_index][1]->setChecked(false);
        led_patterns_boxes[player_index][2]->setChecked(false);
        led_patterns_boxes[player_index][3]->setChecked(false);
        return;
    }

    const auto* controller = system.HIDCore().GetEmulatedControllerByIndex(player_index);
    const auto led_pattern = controller->GetLedPattern();
    led_patterns_boxes[player_index][0]->setChecked(led_pattern.position1);
    led_patterns_boxes[player_index][1]->setChecked(led_pattern.position2);
    led_patterns_boxes[player_index][2]->setChecked(led_pattern.position3);
    led_patterns_boxes[player_index][3]->setChecked(led_pattern.position4);
}

void QtControllerSelectorDialog::UpdateBorderColor(std::size_t player_index) {
    RefreshPlayerSlotStyle(player_index);
}

void QtControllerSelectorDialog::SetExplainText(std::size_t player_index) {
    if (!parameters.enable_explain_text ||
        player_index >= static_cast<std::size_t>(parameters.max_players)) {
        return;
    }

    explain_text_labels[player_index]->setText(QString::fromStdString(
        Common::StringFromFixedZeroTerminatedBuffer(parameters.explain_text[player_index].data(),
                                                    parameters.explain_text[player_index].size())));
}

void QtControllerSelectorDialog::UpdateDockedState(bool is_handheld) {
    // Disallow changing the console mode if the controller type is handheld.
    ui->radioDocked->setEnabled(!is_handheld);
    ui->radioUndocked->setEnabled(!is_handheld);

    ui->radioDocked->setChecked(Settings::IsDockedMode());
    ui->radioUndocked->setChecked(!Settings::IsDockedMode());

    // Also force into undocked mode if the controller type is handheld.
    if (is_handheld) {
        ui->radioUndocked->setChecked(true);
    }
}

void QtControllerSelectorDialog::PropagatePlayerNumberChanged(size_t player_index, bool checked,
                                                              bool reconnect_current) {
    connected_controller_checkboxes[player_index]->setChecked(checked);

    if (checked) {
        // Check all previous buttons when checked
        if (player_index > 0) {
            PropagatePlayerNumberChanged(player_index - 1, checked);
        }
    } else {
        // Unchecked all following buttons when unchecked
        if (player_index < connected_controller_checkboxes.size() - 1) {
            PropagatePlayerNumberChanged(player_index + 1, checked);
        }
    }

    if (reconnect_current) {
        connected_controller_checkboxes[player_index]->setCheckState(Qt::Checked);
    }

    // Ensure the requirements label always reflects the final settled state.
    CheckIfParametersMet();
}

void QtControllerSelectorDialog::OnPlayerButtonA(std::size_t player_index) {
    if (player_index >= NUM_PLAYERS) return;
    if (player_widgets[player_index]->isHidden()) return;
    if (!player_groupboxes[player_index]->isEnabled()) return;
    if (player_groupboxes[player_index]->isChecked()) return; // already connected

    PropagatePlayerNumberChanged(player_index, true);
}

void QtControllerSelectorDialog::OnPlayerButtonB(std::size_t player_index) {
    if (player_index >= NUM_PLAYERS) return;
    if (player_widgets[player_index]->isHidden()) return;
    if (!player_groupboxes[player_index]->isChecked()) return; // already disconnected

    PropagatePlayerNumberChanged(player_index, false);
}

void QtControllerSelectorDialog::ApplyInputDevice(std::size_t player_index) {
    const int idx = input_device_combos[player_index]->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(cached_input_devices.size()))
        return;

    const auto& device = cached_input_devices[idx];
    auto* controller = system.HIDCore().GetEmulatedControllerByIndex(player_index);

    const auto button_mapping = input_subsystem->GetButtonMappingForDevice(device);
    const auto analog_mapping = input_subsystem->GetAnalogMappingForDevice(device);
    const auto motion_mapping = input_subsystem->GetMotionMappingForDevice(device);

    for (const auto& [btn, param] : button_mapping) {
        controller->SetButtonParam(static_cast<std::size_t>(btn), param);
    }
    for (const auto& [axis, param] : analog_mapping) {
        controller->SetStickParam(static_cast<std::size_t>(axis), param);
    }
    for (const auto& [motion, param] : motion_mapping) {
        controller->SetMotionParam(static_cast<std::size_t>(motion), param);
    }

    // Persist to Settings::values so the configure dialog and future sessions see this mapping.
    controller->SaveCurrentConfig();

    // Write through to disk so the assignment survives restart.
    if (auto* mw = qobject_cast<MainWindow*>(parent()))
        mw->OnSaveConfig();
}

void QtControllerSelectorDialog::DisableUnsupportedPlayers() {
    const auto max_supported_players = parameters.enable_single_mode ? 1 : parameters.max_players;

    switch (max_supported_players) {
    case 0:
    default:
        ASSERT(false);
        return;
    case 1:
        ui->widgetSpacer->hide();
        ui->widgetSpacer2->hide();
        ui->widgetSpacer3->hide();
        ui->widgetSpacer4->hide();
        break;
    case 2:
        ui->widgetSpacer->hide();
        ui->widgetSpacer2->hide();
        ui->widgetSpacer3->hide();
        break;
    case 3:
        ui->widgetSpacer->hide();
        ui->widgetSpacer2->hide();
        break;
    case 4:
        ui->widgetSpacer->hide();
        break;
    case 5:
    case 6:
    case 7:
    case 8:
        break;
    }

    for (std::size_t index = max_supported_players; index < NUM_PLAYERS; ++index) {
        auto* controller = system.HIDCore().GetEmulatedControllerByIndex(index);
        // Disconnect any unsupported players here and disable or hide them if applicable.
        UpdateController(controller, controller->GetNpadStyleIndex(true), false);
        // Hide the player widgets when max_supported_controllers is less than or equal to 4.
        if (max_supported_players <= 4) {
            player_widgets[index]->hide();
        }

        // Disable and hide the following to prevent these from interaction.
        player_widgets[index]->setDisabled(true);
        connected_controller_checkboxes[index]->setDisabled(true);
        connected_controller_labels[index]->hide();
        connected_controller_checkboxes[index]->hide();
    }
}

QtControllerSelector::QtControllerSelector(MainWindow& parent) {
    connect(this, &QtControllerSelector::MainWindowReconfigureControllers, &parent,
            &MainWindow::ControllerSelectorReconfigureControllers, Qt::QueuedConnection);
    connect(this, &QtControllerSelector::MainWindowRequestExit, &parent,
            &MainWindow::ControllerSelectorRequestExit, Qt::QueuedConnection);
    connect(&parent, &MainWindow::ControllerSelectorReconfigureFinished, this,
            &QtControllerSelector::MainWindowReconfigureFinished, Qt::QueuedConnection);
}

QtControllerSelector::~QtControllerSelector() = default;

void QtControllerSelector::Close() const {
    callback = {};
    emit MainWindowRequestExit();
}

void QtControllerSelector::ReconfigureControllers(
    ReconfigureCallback callback_, const Core::Frontend::ControllerParameters& parameters) const {
    callback = std::move(callback_);
    emit MainWindowReconfigureControllers(parameters);
}

void QtControllerSelector::MainWindowReconfigureFinished(bool is_success) {
    if (callback) {
        callback(is_success);
    }
}
