// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <QDialog>
#include <QPointer>
#include "common/param_package.h"
#include "core/frontend/applets/controller.h"

struct DeviceEntry {
    std::string raw_name;
    std::string display_name;
};

class MainWindow;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QGroupBox;
class QLabel;
class QTimer;

class InputProfiles;
class StickWidget;

namespace InputCommon {
class InputSubsystem;
}

namespace Ui {
class QtControllerSelectorDialog;
}

namespace Core {
class System;
}

namespace Core::HID {
class HIDCore;
enum class NpadStyleIndex : u8;
} // namespace Core::HID

class ControllerNavigation;

class QtControllerSelectorDialog final : public QDialog {
    Q_OBJECT

public:
    explicit QtControllerSelectorDialog(QWidget* parent,
                                        Core::Frontend::ControllerParameters parameters_,
                                        InputCommon::InputSubsystem* input_subsystem_,
                                        Core::System& system_);
    ~QtControllerSelectorDialog() override;

    int exec() override;

    void keyPressEvent(QKeyEvent* evt) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

    // When set, exec() will always show the dialog even if parameters are already met.
    void SetForceShow() { force_show = true; }

private:
    // Applies the current configuration.
    void ApplyConfiguration();

    // Loads the current input configuration into the frontend applet.
    void LoadConfiguration();

    // Initializes the "Configure Vibration" Dialog.
    void CallConfigureVibrationDialog();

    // Initializes the "Configure Motion / Touch" Dialog.
    void CallConfigureMotionTouchDialog();

    // Initializes the "Create Input Profile" Dialog.
    void CallConfigureInputProfileDialog();

    // Checks the current configuration against the given parameters.
    // This sets and returns the value of parameters_met.
    bool CheckIfParametersMet();

    // Sets the controller icons for "Supported Controller Types".
    void SetSupportedControllers();

    // Sets the emulated controllers per player.
    void SetEmulatedControllers(std::size_t player_index);

    // Gets the Controller Type for a given controller combobox index per player.
    Core::HID::NpadStyleIndex GetControllerTypeFromIndex(int index, std::size_t player_index) const;

    // Gets the controller combobox index for a given Controller Type per player.
    int GetIndexFromControllerType(Core::HID::NpadStyleIndex type, std::size_t player_index) const;

    // Updates the controller icons per player.
    void UpdateControllerIcon(std::size_t player_index);

    // Updates the controller state (type and connection status) per player.
    void UpdateControllerState(std::size_t player_index);

    // Updates the LED pattern per player.
    void UpdateLEDPattern(std::size_t player_index);

    // Updates the border color per player.
    void UpdateBorderColor(std::size_t player_index);

    // Sets the "Explain Text" per player.
    void SetExplainText(std::size_t player_index);

    // Updates the console mode.
    void UpdateDockedState(bool is_handheld);

    // Disables and disconnects unsupported players based on the given parameters.
    void DisableUnsupportedPlayers();

    // Moves the gamepad focus highlight to the given player slot.
    void SetFocusedPlayer(std::size_t index);

    // Moves the gamepad focus to the OK button.
    enum class FocusedButton { None, OK };
    void SetFocusedButton(FocusedButton btn);

    // Rebuilds the stylesheet for a player groupbox to reflect current focus + game border state.
    void RefreshPlayerSlotStyle(std::size_t player_index);

    // Applies the selected physical input device mappings to a player's controller.
    void ApplyInputDevice(std::size_t player_index);

    // Picks a free input device for a newly-connected player slot if none is assigned.
    // Priority: free gamepad → keyboard → keyboard+mouse.
    void AutoAssignInputDevice(std::size_t player_index);

    // Syncs combo boxes for connected slots with their EmulatedController button params,
    // then assigns button params to disconnected slots for any unrouted physical devices
    // so their A/B presses reach HID callbacks even before they claim a slot.
    // Called at open, after every connect, and after every disconnect.
    void RefreshPrePopulate();

    // Builds the GUID+port→{raw,display} name map from cached_input_devices.
    // Always rebuilt fresh each session; removes any stale on-disk file.
    void BuildDeviceNameMap();

    // Called when A is pressed by a device not yet in cached_input_devices.
    // Assigns a unique display name, appends to cached lists and all combo boxes.
    int RegisterUnknownDevice(const std::string& guid, const std::string& port,
                               const std::string& raw_name);

    // Called on the Qt thread when player N's physical controller presses A (connect) or B (disconnect).
    void OnPlayerButtonA(std::size_t player_index);
    void OnPlayerButtonB(std::size_t player_index);

    std::unique_ptr<Ui::QtControllerSelectorDialog> ui;

    // Parameters sent in from the backend HLE applet.
    Core::Frontend::ControllerParameters parameters;

    InputCommon::InputSubsystem* input_subsystem;

    std::unique_ptr<InputProfiles> input_profiles;

    Core::System& system;

    ControllerNavigation* controller_navigation = nullptr;

    // This is true if and only if all parameters are met. Otherwise, this is false.
    // This determines whether the "OK" button can be clicked to exit the applet.
    bool parameters_met{false};

    // When true, exec() shows the dialog even when parameters are already met.
    bool force_show{false};

    // Index of the player slot currently highlighted by gamepad navigation.
    // Initialised to NUM_PLAYERS (sentinel for "none") until SetFocusedPlayer is called.
    std::size_t focused_player_index{NUM_PLAYERS};

    // Which dialog button (OK/Cancel) currently has gamepad focus, if any.
    FocusedButton focused_button{FocusedButton::None};

    // Last player slot that had focus; used to restore column position when returning from buttons.
    std::size_t last_focused_player{0};

    static constexpr std::size_t NUM_PLAYERS = 8;

    // Widgets encapsulating the groupboxes and comboboxes per player.
    std::array<QWidget*, NUM_PLAYERS> player_widgets;

    // Groupboxes encapsulating the controller icons and LED patterns per player.
    std::array<QGroupBox*, NUM_PLAYERS> player_groupboxes;

    // Icons for currently connected controllers/players.
    std::array<QWidget*, NUM_PLAYERS> connected_controller_icons;

    // Labels that represent the player numbers in place of the controller icons.
    std::array<QLabel*, NUM_PLAYERS> player_labels;

    // LED patterns for currently connected controllers/players.
    std::array<std::array<QCheckBox*, 4>, NUM_PLAYERS> led_patterns_boxes;

    // Labels representing additional information known as "Explain Text" per player.
    std::array<QLabel*, NUM_PLAYERS> explain_text_labels;

    // Comboboxes with a list of emulated controllers per player.
    std::array<QComboBox*, NUM_PLAYERS> emulated_controllers;

    /// Pairs of emulated controller index and Controller Type enum per player.
    std::array<std::vector<std::pair<int, Core::HID::NpadStyleIndex>>, NUM_PLAYERS>
        index_controller_type_pairs;

    // Labels representing the number of connected controllers
    // above the "Connected Controllers" checkboxes.
    std::array<QLabel*, NUM_PLAYERS> connected_controller_labels;

    // Checkboxes representing the "Connected Controllers".
    std::array<QCheckBox*, NUM_PLAYERS> connected_controller_checkboxes;

    // Left-stick position indicators, one per player slot.
    std::array<StickWidget*, NUM_PLAYERS> stick_indicators{};

    // Input device (gamepad) selectors, one per player slot.
    std::array<QComboBox*, NUM_PLAYERS> input_device_combos{};

    // Cached list of physical gamepads (IsController() == true) from the input subsystem.
    std::vector<Common::ParamPackage> cached_input_devices;

    // Deduplicated display labels for cached_input_devices (same index).
    std::vector<std::string> cached_device_labels;

    // Persistent map of GUID → {raw_name, display_name}.
    // Loaded from {CacheDir}/controller_applet/device_names.json on open,
    // saved on OK (not cancel).
    std::map<std::string, DeviceEntry> device_name_map;

    // Callback keys for per-player A/B claim detection (index NUM_PLAYERS = Handheld → slot 0).
    std::array<int, NUM_PLAYERS> player_claim_callback_keys{};
    int handheld_claim_callback_key{-1};

    // Rising-edge state for A/B per slot (NUM_PLAYERS+1 entries; last = Handheld).
    std::mutex claim_state_mutex;
    std::array<bool, NUM_PLAYERS + 1> prev_a_pressed{};
    std::array<bool, NUM_PLAYERS + 1> prev_b_pressed{};

    QTimer* stick_poll_timer = nullptr;
};

class QtControllerSelector final : public QObject, public Core::Frontend::ControllerApplet {
    Q_OBJECT

public:
    explicit QtControllerSelector(MainWindow& parent);
    ~QtControllerSelector() override;

    void Close() const override;
    void ReconfigureControllers(
        ReconfigureCallback callback_,
        const Core::Frontend::ControllerParameters& parameters) const override;

signals:
    void MainWindowReconfigureControllers(
        const Core::Frontend::ControllerParameters& parameters) const;
    void MainWindowRequestExit() const;

private:
    void MainWindowReconfigureFinished(bool is_success);

    mutable ReconfigureCallback callback;
};
