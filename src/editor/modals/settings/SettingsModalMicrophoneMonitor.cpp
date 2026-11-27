#include "SettingsModalInternal.h"

namespace Horo::Editor::SettingsModalInternal {
    namespace {
        using namespace Ui;
        using Status = EditorMicrophoneMonitorStatus;

        /** @brief Localizes one owner-lane state without polling native devices. */
        const char *StatusKey(const Status status) noexcept {
            using enum Status;
            switch (status) {
                case Idle:
                case Ready:
                    return "audio.microphone.monitor.description";
                case Scanning:
                    return "audio.microphone.monitor.scanning";
                case Starting:
                    return "audio.microphone.monitor.starting";
                case Listening:
                    return "audio.microphone.monitor.listening";
                case Stopping:
                    return "audio.microphone.monitor.stopping";
                case PermissionDenied:
                    return "audio.microphone.monitor.permission_denied";
                case Failed:
                    return "audio.microphone.monitor.failed";
            }
            return "audio.microphone.monitor.failed";
        }

        /** @brief Tests whether a transient selection belongs to the current Settings choices. */
        bool Selected(const int index, const std::span<const EditorMicrophoneMonitorOption> options) noexcept {
            return index >= 0 && static_cast<std::size_t>(index) < options.size();
        }

        /** @brief Draws one device picker using the shared keyboard-accessible control. */
        bool DrawDevice(const EditorGuiContext &ctx, const char *key, const char *id, int &selection,
                        const std::span<const EditorMicrophoneMonitorOption> options) {
            const auto label = SettingsModal::kTextScope.Text(ctx.localization, key);
            const ComboItemSource source{.label = [options](const int index) { return options[static_cast<std::size_t>(index)].label.c_str(); }};
            bool changed{};
            SettingRow(label.c_str(), "", ctx.theme.fonts, [&] {
                changed = ComboControl(id, &selection, static_cast<int>(options.size()), source, ctx.theme.fonts);
            }, id, SettingRowLayout::Stacked);
            return changed;
        }

        /** @brief Keeps private listening confirmation bound to the currently selected output. */
        void DrawSelection(SettingsAudioTab &audio, const EditorGuiContext &ctx, const IEditorMicrophoneMonitor &monitor) {
            static_cast<void>(DrawDevice(ctx, "audio.microphone.input", "##monitor-input", audio.monitorInput, monitor.InputOptions()));
            if (DrawDevice(ctx, "audio.microphone.monitor.output", "##monitor-output", audio.monitorOutput, monitor.OutputOptions()))
                audio.monitorPrivateListening = false;
            if (Selected(audio.monitorOutput, monitor.OutputOptions())) {
                const auto confirmation = SettingsModal::kTextScope.Text(ctx.localization, "audio.microphone.monitor.confirm_headphones");
                SettingRow(confirmation.c_str(), "", ctx.theme.fonts, [&] {
                    static_cast<void>(ToggleControl("##monitor-headphones", &audio.monitorPrivateListening, ctx.theme.fonts));
                }, "monitor-headphones", SettingRowLayout::Stacked);
            }
        }

        /** @brief Clears a projection whenever its discovery is replaced or withdrawn. */
        void ResetSelection(SettingsAudioTab &audio) noexcept {
            audio.monitorInput = -1;
            audio.monitorOutput = -1;
            audio.monitorPrivateListening = false;
        }

        /** @brief Queues a fresh scan or the explicitly confirmed current pair. */
        void DrawStart(SettingsAudioTab &audio, const EditorGuiContext &ctx, IEditorMicrophoneMonitor &monitor, const bool ready) {
            const auto scan = SettingsModal::kTextScope.Text(ctx.localization, "audio.microphone.monitor.scan");
            const auto start = SettingsModal::kTextScope.Text(ctx.localization, "audio.microphone.monitor.start");
            const bool available = monitor.CanConfigure();
            if (Button({.label = scan.c_str(), .variant = ButtonVariant::Secondary, .enabled = available})) {
                ResetSelection(audio);
                monitor.Scan();
            }
            const bool allowed = ready && available && audio.monitorPrivateListening && Selected(audio.monitorInput, monitor.InputOptions()) &&
                                 Selected(audio.monitorOutput, monitor.OutputOptions());
            if (Button({.label = start.c_str(), .variant = ButtonVariant::Primary, .enabled = allowed})) {
                monitor.StartListening(static_cast<std::size_t>(audio.monitorInput), static_cast<std::size_t>(audio.monitorOutput), true);
                ResetSelection(audio);
            }
        }

        /** @brief Keeps Stop available while the session opens or plays. */
        void DrawActions(SettingsAudioTab &audio, const EditorGuiContext &ctx, IEditorMicrophoneMonitor &monitor, const Status status,
                         const bool granted) {
            using enum Status;
            if (status == Starting || status == Listening) {
                const auto stop = SettingsModal::kTextScope.Text(ctx.localization, "audio.microphone.monitor.stop");
                if (Button({.label = stop.c_str(), .variant = ButtonVariant::Secondary}))
                    monitor.StopListening();
            } else if (granted && status != Scanning && status != Stopping) {
                DrawStart(audio, ctx, monitor, status == Ready);
            }
        }
    }  // namespace

    /** @copydoc DrawMicrophoneMonitor */
    void DrawMicrophoneMonitor(SettingsAudioTab &audio, const EditorGuiContext &ctx) {
        auto *monitor = ctx.microphoneMonitor;
        if (!monitor || !ctx.microphonePermission)
            return;
        using enum Status;
        const auto status = monitor->MonitorStatus();
        const bool ready = status == Ready;
        const bool granted = ctx.microphonePermission->Status() == EditorMicrophonePermissionStatus::Granted;
        if (!ready || !granted)
            ResetSelection(audio);
        if (status == Idle && !granted)
            return;
        if (ready && granted)
            DrawSelection(audio, ctx, *monitor);
        const auto label = SettingsModal::kTextScope.Text(ctx.localization, "audio.microphone.monitor.title");
        const auto description = SettingsModal::kTextScope.Text(ctx.localization, StatusKey(status));
        SettingRow(label.c_str(), description.c_str(), ctx.theme.fonts, [&] { DrawActions(audio, ctx, *monitor, status, granted); },
                   "microphone-monitor", SettingRowLayout::Stacked);
    }
}  // namespace Horo::Editor::SettingsModalInternal
