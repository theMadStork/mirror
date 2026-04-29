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
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_NoSystemBackground);
    }

    QSize minimumSizeHint() const override {
        // Floor is 2× font height so the ring is always legible; never less than 24px.
        const int fh = fontMetrics().height();
        const int h = std::max(24, fh * 2);
        return QSize(h, h);
    }
    QSize sizeHint() const override {
        const int fh = fontMetrics().height();
        const int h = std::max(48, fh * 3);
        return QSize(h, h);
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
        // Cap at 22px so the ring always fits inside the widget with padding.
        const float r = std::min({cx - 5.0f, cy - 5.0f, 22.0f});

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

// Strips " Controller" substring (case-sensitive) from name if present.
// Used by RegisterUnknownDevice for the hot-plug edge case.
std::string StripControllerWord(std::string name) {
    const auto pos = name.find(" Controller");
    if (pos != std::string::npos)
        name.erase(pos, 11);
    return name;
}

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
    // Player number labels (P1/P2/...) are redundant — the LED sequence already
    // identifies the slot — and they overlap the stick indicator in disconnected slots.
    for (auto* lbl : player_labels) {
        lbl->hide();
    }

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

    // The .ui file has per-slot profile comboboxes that have no backend in this implementation.
    // Hide them so they don't appear as non-functional "Use Current Config" dropdowns.
    for (auto* combo : std::array<QComboBox*, NUM_PLAYERS>{
             ui->comboPlayer1Profile, ui->comboPlayer2Profile,
             ui->comboPlayer3Profile, ui->comboPlayer4Profile,
             ui->comboPlayer5Profile, ui->comboPlayer6Profile,
             ui->comboPlayer7Profile, ui->comboPlayer8Profile}) {
        combo->hide();
    }

    ui->labelError->setStyleSheet(QStringLiteral("QLabel { color: gray; }"));
    ui->labelError->setText(tr("A: Connect  ●  B: Disconnect  ●  Y: Controller type  ●  X: Input device  ●  +: OK"));

    // Setup/load everything prior to setting up connections.
    // This avoids unintentionally changing the states of elements while loading them in.
    SetSupportedControllers();
    DisableUnsupportedPlayers();

    for (std::size_t player_index = 0; player_index < NUM_PLAYERS; ++player_index) {
        SetEmulatedControllers(player_index);
    }

    LoadConfiguration();
    RefreshPrePopulate();

    controller_navigation = new ControllerNavigation(system.HIDCore(), this);

    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        SetExplainText(i);
        UpdateControllerIcon(i);
        UpdateLEDPattern(i);
        UpdateBorderColor(i);

        // Groupbox is the single HID-sync path — no cascade.
        connect(player_groupboxes[i], &QGroupBox::toggled, [this, i](bool checked) {
            {
                QSignalBlocker blocker(connected_controller_checkboxes[i]);
                connected_controller_checkboxes[i]->setChecked(checked);
            }

            UpdateControllerIcon(i);
            UpdateControllerState(i);
            UpdateLEDPattern(i);
            UpdateBorderColor(i);

            if (!checked) {
                // Clear device assignment so the device is free for other slots.
                if (input_device_combos[i]) {
                    QSignalBlocker blocker(input_device_combos[i]);
                    input_device_combos[i]->setCurrentIndex(-1);
                }
                // P1 is the navigator. When P1 disconnects, clear ALL navigation focus
                // unconditionally — regardless of which player currently has the highlight —
                // because no one can navigate until a new P1 claims slot 0.
                if (i == 0) {
                    const std::size_t prev = focused_player_index;
                    focused_player_index = NUM_PLAYERS;
                    if (prev < NUM_PLAYERS)
                        RefreshPlayerSlotStyle(prev);
                }
                // Re-route all unassigned physical devices so their button presses
                // remain detectable after this slot frees up.
                RefreshPrePopulate();
            }

            CheckIfParametersMet();
        });

        // Checkbox click just drives the groupbox (which fires toggled).
        connect(connected_controller_checkboxes[i], &QCheckBox::clicked, [this, i](bool checked) {
            player_groupboxes[i]->setChecked(checked);
        });

        connect(emulated_controllers[i], qOverload<int>(&QComboBox::currentIndexChanged),
                [this, i](int) {
                    UpdateControllerIcon(i);
                    UpdateControllerState(i);
                    UpdateLEDPattern(i);
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
                    if (fire_b && slot != 0) // P1/Handheld B routes through ControllerNavigation → Key_Escape
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
        cancel->setText(tr("Cancel"));
    }

    // Per-slot button hints: (Y) to the left of the controller type combobox.
    // The (A)/(B) hints are shown once in the status label, not repeated per slot.
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        player_groupboxes[i]->setTitle(QString{});

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

    // Redesign each player slot groupbox layout:
    //   Before: [controller icon (stretch 1)] [LEDs] [stick widget]
    //   After:  [LEDs (top)] [controller icon (stretch 1, center)] [stick widget (bottom, transparent)]
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        auto* gb_layout = qobject_cast<QVBoxLayout*>(player_groupboxes[i]->layout());
        if (!gb_layout) continue;

        QWidget* icon_widget = connected_controller_icons[i];
        // LED container is the parent of the first LED checkbox
        QWidget* led_widget = led_patterns_boxes[i][0]->parentWidget();

        gb_layout->removeWidget(icon_widget);
        gb_layout->removeWidget(led_widget);

        // LEDs at the very top, aligned center
        gb_layout->insertWidget(0, led_widget, 0, Qt::AlignHCenter | Qt::AlignTop);
        // Controller icon fills remaining space
        gb_layout->insertWidget(1, icon_widget, 1);

        // Remove the 16px top margin that was only needed when the icon was the first item.
        if (auto* icon_layout = qobject_cast<QVBoxLayout*>(icon_widget->layout())) {
            icon_layout->setContentsMargins(0, 0, 0, 0);
        }

        // No hardcoded minimum — the groupbox itself has a 100×100 floor from the .ui file.
        // Let the icon widget fill whatever space remains after the LED row.
        icon_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        // Transparent background on the groupbox so the stick widget below
        // (and any undrawn areas) show the dialog background cleanly.
        player_groupboxes[i]->setStyleSheet(
            QStringLiteral("QGroupBox#groupPlayer%1Connected { background-color: transparent; }")
                .arg(i + 1));
    }

    // Stick indicator: lives in the outer player widget layout, directly below the groupbox.
    // Parenting to the groupbox caused clipping: the groupbox is AlignHCenter in the outer
    // layout so it sizes to sizeHint (small when label hidden), cutting off the stick ring.
    // Placing it as a sibling in the outer layout gives it its own guaranteed slot.
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        stick_indicators[i] = new StickWidget(player_widgets[i]);
        auto* outer = qobject_cast<QVBoxLayout*>(player_widgets[i]->layout());
        if (outer) {
            // Reduce spacing between all items in this column so the stick sits
            // tightly between the groupbox and the explain label below it.
            outer->setSpacing(2);
            const int gb_idx = outer->indexOf(player_groupboxes[i]);
            outer->insertWidget(gb_idx + 1, stick_indicators[i], 0);
        }
    }

    // Build the full device list. Deduplicate by GUID+port so the same physical
    // device is not listed twice (e.g., once via SDL and once via XInput/HID).
    // When there is a tie, prefer the SDL entry so button mappings are consistent.
    {
        std::map<std::string, std::size_t> seen; // GUID+port → index in cached_input_devices
        for (const auto& dev : input_subsystem->GetInputDevices()) {
            const std::string engine = dev.Get("engine", "");
            if (engine == "any") continue;
            const std::string guid = dev.Get("guid", "");
            const std::string port = dev.Get("port", "");
            if (guid.empty()) {
                // Keyboard / mouse — no GUID; excluded from the gamepad selector.
                continue;
            }
            const std::string key = guid + ":" + port;
            auto it = seen.find(key);
            if (it == seen.end()) {
                seen[key] = cached_input_devices.size();
                cached_input_devices.push_back(dev);
            } else if (engine == "sdl" &&
                       cached_input_devices[it->second].Get("engine", "") != "sdl") {
                // Replace non-SDL duplicate with the SDL version.
                cached_input_devices[it->second] = dev;
            }
        }
    }
    // Build display-name map and cached_device_labels.
    BuildDeviceNameMap();

    // Add an input device (gamepad) selector row below each controller-type row.
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        input_device_combos[i] = new QComboBox(player_widgets[i]);
        input_device_combos[i]->installEventFilter(this);

        input_device_combos[i]->blockSignals(true);
        input_device_combos[i]->setPlaceholderText(tr("—"));
        for (const auto& label : cached_device_labels) {
            input_device_combos[i]->addItem(QString::fromStdString(label));
        }
        input_device_combos[i]->setCurrentIndex(-1);  // unassigned by default; pre-selection sets it
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
                [this, i](int idx) {
                    if (idx >= 0) {
                        // Reject if this device is already assigned to another connected slot.
                        for (std::size_t j = 0; j < NUM_PLAYERS; ++j) {
                            if (j == i) continue;
                            if (player_groupboxes[j]->isChecked() && input_device_combos[j] &&
                                input_device_combos[j]->currentIndex() == idx) {
                                QSignalBlocker blocker(input_device_combos[i]);
                                input_device_combos[i]->setCurrentIndex(-1);
                                ui->labelError->setStyleSheet(
                                    QStringLiteral("QLabel { color: orange; }"));
                                ui->labelError->setText(tr("Input device already assigned"));
                                QTimer::singleShot(1500, this, [this] { CheckIfParametersMet(); });
                                return;
                            }
                        }
                    }
                    ApplyInputDevice(i);
                });
    }

    // Pre-select the physical device mapped to each connected slot.
    // GUID+port is unique per session — no two players share the same port.
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        if (!player_groupboxes[i]->isChecked()) continue;
        const auto* ctrl = system.HIDCore().GetEmulatedControllerByIndex(i);
        const auto param = ctrl->GetButtonParam(static_cast<std::size_t>(Settings::NativeButton::A));
        const std::string guid = param.Get("guid", "");
        const std::string port = param.Get("port", "");
        if (guid.empty()) continue; // keyboard/mouse — no GUID, skip
        for (int j = 0; j < static_cast<int>(cached_input_devices.size()); ++j) {
            if (cached_input_devices[j].Get("guid", "") == guid &&
                cached_input_devices[j].Get("port", "") == port) {
                input_device_combos[i]->blockSignals(true);
                input_device_combos[i]->setCurrentIndex(j);
                input_device_combos[i]->blockSignals(false);
                break;
            }
        }
    }

    // If a connected player's previously-saved device is no longer present
    // (unplugged since last session), auto-assign a replacement so no connected
    // slot ever opens with a blank device.
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        if (player_groupboxes[i]->isChecked() && input_device_combos[i]->currentIndex() < 0) {
            AutoAssignInputDevice(i);
        }
    }

    // Poll left-stick positions at 20 Hz to update the indicators.
    stick_poll_timer = new QTimer(this);
    connect(stick_poll_timer, &QTimer::timeout, this, [this] {
        for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
            if (player_widgets[i]->isHidden() || !player_groupboxes[i]->isChecked())
                continue;
            const auto* controller = system.HIDCore().GetEmulatedControllerByIndex(i);
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
                // Force a full layout pass now that the dialog is visible.
                // Dynamic widgets (Y/X rows, stick indicator) invalidate the layout cache;
                // activate() + updateGeometry() + adjustSize() ensures everything is drawn
                // correctly on first show rather than snapping into place on first interaction.
                for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
                    if (player_widgets[i]->isHidden())
                        continue;
                    if (auto* l = player_groupboxes[i]->layout())
                        l->activate();
                    if (auto* l = player_widgets[i]->layout())
                        l->activate();
                    player_groupboxes[i]->updateGeometry();
                    player_widgets[i]->updateGeometry();
                }
                adjustSize();
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

    switch (evt->key()) {

    case Qt::Key_Return: // Start/Plus → always confirm
        ApplyConfiguration();
        accept();
        return;

    case Qt::Key_Enter: { // A button → connect focused player, or confirm if OK focused
        if (focused_button == FocusedButton::OK) {
            ApplyConfiguration();
            accept();
        } else if (focused_player_index < NUM_PLAYERS) {
            OnPlayerButtonA(focused_player_index);
        }
        return;
    }

    case Qt::Key_Escape: // B → disconnect focused connected slot; fallback to P1 self-disconnect
        if (focused_player_index < NUM_PLAYERS &&
            player_groupboxes[focused_player_index]->isChecked()) {
            OnPlayerButtonB(focused_player_index);
        } else if (focused_player_index >= NUM_PLAYERS && player_groupboxes[0]->isChecked()) {
            OnPlayerButtonB(0);
        }
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

    case Qt::Key_X: // X → cycle to next unassigned gamepad for focused player (P2+ only)
        if (focused_player_index > 0 && focused_player_index < NUM_PLAYERS &&
            player_widgets[focused_player_index]->isVisible()) {
            auto* combo = input_device_combos[focused_player_index];
            const int count = combo->count();
            if (count > 0) {
                const int start = combo->currentIndex(); // may be -1 (no device selected)
                int next = -1;
                // step <= count so we can visit all items even when start is -1.
                // The (+ count) % count ensures no negative remainder on step 1 with start=-1.
                for (int step = 1; step <= count; ++step) {
                    const int candidate = ((start + step) % count + count) % count;
                    if (candidate == start) continue; // wrapped back to start
                    // Only skip devices held by a connected player.
                    bool in_use = false;
                    for (std::size_t j = 0; j < NUM_PLAYERS; ++j) {
                        if (j != focused_player_index &&
                            input_device_combos[j] != nullptr &&
                            player_groupboxes[j]->isChecked() &&
                            input_device_combos[j]->currentIndex() == candidate) {
                            in_use = true;
                            break;
                        }
                    }
                    if (!in_use) { next = candidate; break; }
                }
                if (next >= 0) {
                    combo->setCurrentIndex(next);
                } else {
                    ui->labelError->setStyleSheet(QStringLiteral("QLabel { color: orange; }"));
                    ui->labelError->setText(tr("No free pad"));
                    QTimer::singleShot(1500, this, [this] { CheckIfParametersMet(); });
                }
            }
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
    // Always keep background transparent so child widgets (stick indicator etc.) are never
    // obscured by a Qt-default fill from the stylesheet engine.
    QString style =
        QStringLiteral("QGroupBox#groupPlayer%1Connected { background-color: transparent; }")
            .arg(player_index + 1);

    if (is_focused) {
        const QColor hl = player_groupboxes[player_index]->palette().color(QPalette::Highlight);
        style = QStringLiteral("QGroupBox#groupPlayer%1Connected { "
                               "background-color: transparent; border: 2px solid %2; }")
                    .arg(player_index + 1)
                    .arg(hl.name());
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

    // OK is always enabled — player decides whether requirements are satisfied.
    if (auto* ok = ui->buttonBox->button(QDialogButtonBox::Ok))
        ok->setEnabled(true);

    if (parameters_met) {
        ui->labelError->setStyleSheet(QStringLiteral("QLabel { color: #0097DB; }"));
        ui->labelError->setText(tr("Requirements met  ●  Start: OK"));
    } else {
        ui->labelError->setStyleSheet(QStringLiteral("QLabel { color: gray; }"));
        ui->labelError->setText(tr("A: Connect  ●  B: Disconnect  ●  Y: Type  ●  X: Device"));
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

void QtControllerSelectorDialog::OnPlayerButtonA(std::size_t physical_slot) {
    if (physical_slot >= NUM_PLAYERS) return;

    // Get the identity of the physical controller that pressed A.
    const auto* phys_ctrl = system.HIDCore().GetEmulatedControllerByIndex(physical_slot);
    const auto btn_param = phys_ctrl->GetButtonParam(
        static_cast<std::size_t>(Settings::NativeButton::A));
    const std::string pressed_guid   = btn_param.Get("guid",   "");
    const std::string pressed_port   = btn_param.Get("port",   "");
    const std::string pressed_engine = btn_param.Get("engine", "");

    // Find this device in cached_input_devices.
    // Try exact GUID+port match first, then GUID-only fallback for keyboards/mice.
    int device_idx = -1;
    for (int d = 0; d < static_cast<int>(cached_input_devices.size()); ++d) {
        if (cached_input_devices[d].Get("guid", "") == pressed_guid &&
            cached_input_devices[d].Get("port", "") == pressed_port) {
            device_idx = d;
            break;
        }
    }
    if (device_idx < 0 && !pressed_guid.empty()) {
        // GUID-only fallback (e.g. keyboard, devices without a port field)
        for (int d = 0; d < static_cast<int>(cached_input_devices.size()); ++d) {
            if (cached_input_devices[d].Get("guid", "") == pressed_guid) {
                device_idx = d;
                break;
            }
        }
    }

    // Guard: if this device is already assigned to a connected UI slot, do nothing.
    if (device_idx >= 0) {
        for (std::size_t j = 0; j < NUM_PLAYERS; ++j) {
            if (player_groupboxes[j]->isChecked() && input_device_combos[j] &&
                input_device_combos[j]->currentIndex() == device_idx) {
                return;
            }
        }
    }

    // Find first empty (unchecked, enabled, visible) UI slot.
    std::size_t target_slot = NUM_PLAYERS;
    for (std::size_t j = 0; j < NUM_PLAYERS; ++j) {
        if (!player_widgets[j]->isHidden() && player_widgets[j]->isEnabled() &&
            !player_groupboxes[j]->isChecked()) {
            target_slot = j;
            break;
        }
    }
    if (target_slot >= NUM_PLAYERS) return; // no empty slot available

    // Device-not-found edge case: register it so it gets a unique display name.
    if (device_idx < 0 && !pressed_guid.empty()) {
        const std::string raw = btn_param.Get("display", pressed_engine);
        device_idx = RegisterUnknownDevice(pressed_guid, pressed_port, raw);
    }

    // Connect the target slot (fires toggled which handles HID + UI updates).
    player_groupboxes[target_slot]->setChecked(true);

    // Assign the specific physical device to this slot.
    if (device_idx >= 0 && input_device_combos[target_slot]) {
        input_device_combos[target_slot]->setCurrentIndex(device_idx);
    } else {
        AutoAssignInputDevice(target_slot);
    }

    // P1 takes over gamepad focus.
    if (target_slot == 0) {
        SetFocusedPlayer(0);
    }

    // Re-sync all combo boxes and re-route any device displaced by this connection.
    RefreshPrePopulate();
}

void QtControllerSelectorDialog::OnPlayerButtonB(std::size_t player_index) {
    if (player_index >= NUM_PLAYERS) return;
    if (!player_groupboxes[player_index]->isChecked()) return;
    player_groupboxes[player_index]->setChecked(false);
}

void QtControllerSelectorDialog::ApplyInputDevice(std::size_t player_index) {
    if (player_index >= NUM_PLAYERS) return;
    if (!player_groupboxes[player_index]->isChecked()) return; // never write config for disconnected player
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

void QtControllerSelectorDialog::AutoAssignInputDevice(std::size_t player_index) {
    if (player_index >= NUM_PLAYERS) return;
    auto* combo = input_device_combos[player_index];
    if (!combo || combo->currentIndex() >= 0) return; // already has a device

    const auto try_assign = [&](auto predicate) -> bool {
        for (int i = 0; i < static_cast<int>(cached_input_devices.size()); ++i) {
            if (!predicate(cached_input_devices[i])) continue;
            bool in_use = false;
            for (std::size_t j = 0; j < NUM_PLAYERS; ++j) {
                if (j != player_index && input_device_combos[j] &&
                    player_groupboxes[j]->isChecked() &&
                    input_device_combos[j]->currentIndex() == i) {
                    in_use = true;
                    break;
                }
            }
            if (!in_use) {
                combo->setCurrentIndex(i);
                return true;
            }
        }
        return false;
    };

    // Assign the first free gamepad (keyboard/mouse excluded from cached_input_devices).
    try_assign([](const Common::ParamPackage&) { return true; });
}

void QtControllerSelectorDialog::RefreshPrePopulate() {
    const int n_devices = static_cast<int>(cached_input_devices.size());

    // Pass 1 — connected slots: sync each combo to match the slot's EmulatedController
    // button params, and record which device indices are already owned.
    std::vector<bool> routed(n_devices, false);

    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        if (!player_groupboxes[i]->isChecked()) continue;

        const auto* controller = system.HIDCore().GetEmulatedControllerByIndex(i);
        const auto btn_param = controller->GetButtonParam(
            static_cast<std::size_t>(Settings::NativeButton::A));
        const std::string guid = btn_param.Get("guid", "");
        const std::string port = btn_param.Get("port", "");
        if (guid.empty()) continue;

        for (int d = 0; d < n_devices; ++d) {
            if (cached_input_devices[d].Get("guid", "") == guid &&
                cached_input_devices[d].Get("port", "") == port) {
                routed[d] = true;
                if (input_device_combos[i] && input_device_combos[i]->currentIndex() != d) {
                    QSignalBlocker blocker(input_device_combos[i]);
                    input_device_combos[i]->setCurrentIndex(d);
                }
                break;
            }
        }
    }

    // Pass 2 — disconnected slots: assign button params of unrouted devices so their
    // button events reach HID callbacks before they formally claim a slot.
    std::size_t next_slot = 0;
    for (int d = 0; d < n_devices; ++d) {
        if (routed[d]) continue;

        // Advance past connected slots.
        while (next_slot < NUM_PLAYERS && player_groupboxes[next_slot]->isChecked())
            ++next_slot;
        if (next_slot >= NUM_PLAYERS) break;

        auto* controller = system.HIDCore().GetEmulatedControllerByIndex(next_slot);
        const auto button_mapping =
            input_subsystem->GetButtonMappingForDevice(cached_input_devices[d]);
        for (const auto& [btn, param] : button_mapping)
            controller->SetButtonParam(static_cast<std::size_t>(btn), param);

        ++next_slot;
    }
}

void QtControllerSelectorDialog::BuildDeviceNameMap() {
    // Rebuilt fresh every session — no disk file.
    // SDL's GetInputDevices() now assigns display names with a global counter per base name,
    // so the "display" field is already unique program-wide. We just index it here.
    device_name_map.clear();
    cached_device_labels.clear();

    for (const auto& dev : cached_input_devices) {
        const std::string key     = dev.Get("guid", "") + ":" + dev.Get("port", "");
        const std::string raw     = dev.Get("raw", dev.Get("display", "Unknown"));
        const std::string display = dev.Get("display", "Unknown");
        device_name_map[key] = {raw, display};
        cached_device_labels.push_back(display);
    }
}

int QtControllerSelectorDialog::RegisterUnknownDevice(const std::string& guid,
                                                       const std::string& port,
                                                       const std::string& raw_name) {
    const std::string key  = guid + ":" + port;
    const std::string base = StripControllerWord(raw_name);
    // Count existing entries with the same base to assign the next unique index,
    // consistent with BuildDeviceNameMap's global counter.
    int n = 0;
    for (const auto& [k, e] : device_name_map) {
        if (StripControllerWord(e.raw_name) == base) ++n;
    }
    const std::string display = base + " " + std::to_string(n);
    device_name_map[key] = {raw_name, display};

    // Append to cached lists so existing combo indices remain valid.
    Common::ParamPackage pkg;
    pkg.Set("guid", guid);
    pkg.Set("port", port);
    pkg.Set("display", display);
    pkg.Set("raw", raw_name);
    pkg.Set("engine", "sdl");
    const int new_idx = static_cast<int>(cached_input_devices.size());
    cached_input_devices.push_back(pkg);
    cached_device_labels.push_back(display);

    // Add to every combo box.
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        if (!input_device_combos[i]) continue;
        QSignalBlocker blocker(input_device_combos[i]);
        input_device_combos[i]->addItem(QString::fromStdString(display));
    }
    return new_idx;
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
