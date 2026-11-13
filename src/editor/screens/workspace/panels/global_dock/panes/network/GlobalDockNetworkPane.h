#pragma once

#include "Horo/Editor/Localization/LocalizedGui.h"
/** @file GlobalDockNetworkPane.h
 * @brief Presentation of immutable bounded runtime network diagnostics.
 */
#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Editor/EditorUiComponents.h"

#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>
struct ImVec2;

namespace Horo::Editor {
    struct EditorGuiContext;

    /** @brief Narrow query/command client; owns only display selection and copied diagnostic evidence. */
    class GlobalDockNetworkPane final : public LocalizedGui {
    public:
        /** @brief Catalog scope for this embedded dock surface. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.global_dock.network"};

        /** @brief Creates a pane with its required localization scope. */
        GlobalDockNetworkPane() : LocalizedGui(kTextScope) {}

        /** @brief Borrows host-owned application capabilities through panel detachment.
         * @param query Optional source query. @param control Optional typed capture command capability. */
        void Attach(const Application::INetworkDebuggerQuery *query, Application::INetworkDebuggerControl *control) noexcept;
        /** @brief Releases borrowed capabilities and removes live provenance. */
        void Detach() noexcept;
        /** @brief Draws a responsive view of one coherent publication.
         * @param origin Content origin. @param width Available width. @param context Shared theme and localization. */
        void Draw(const ImVec2 &origin, float width, const EditorGuiContext &context);

        /** @brief Returns the last drawn immutable projection. @return Copied evidence and assessed provenance. */
        [[nodiscard]] const Application::NetworkDebuggerProjection &Projection() const noexcept {
            return projection_;
        }

        /** @brief Returns the localization key for explicit source provenance. @param state Assessed state. @return Stable key. */
        [[nodiscard]] static const char *StateKey(Application::NetworkDebuggerState state) noexcept;

    private:
        enum class View : std::uint8_t {
            Connections,
            Replication,
            Rpc,
            Prediction,
            Interest,
            Capture,
            Count
        };
        void DrawToolbar(float width, const EditorGuiContext &context);
        /** @brief Copied typed evidence retained independently of a producer ring slot. */
        using Record = std::variant<Network::NetworkConnectionRecord, Network::NetworkReplicationRecord, Network::NetworkRpcRecord,
                                    Network::NetworkPredictionRecord, Network::NetworkInterestRecord, Network::NetworkCaptureRecord>;

        /** @brief Pins one record to its exact source and publication revision. */
        struct Selection {
            Network::NetworkDiagnosticSource source;
            std::uint64_t revision{};
            std::uint64_t ordinal{};
            Record record;
        };

        void DrawEvidence(float width, const EditorGuiContext &context);
        void DrawDetails(float width, const EditorGuiContext &context);
        void DrawEvidenceRow(float width, const EditorGuiContext &context, const std::string &label, const std::string &value) const;
        [[nodiscard]] bool DrawHistory(float width, const EditorGuiContext &context);
        void Request(Network::NetworkCaptureAction action);
        const Application::INetworkDebuggerQuery *query_{};
        Application::INetworkDebuggerControl *control_{};
        Application::NetworkDebuggerProjection projection_;
        std::array<char, 256> search_{};
        View view_{View::Capture};
        std::optional<Selection> selection_;
        std::vector<Ui::TableRow> rows_;
        std::size_t rowsUsed_{};
        std::vector<Record> records_;
        std::vector<std::uint64_t> ordinals_;
        std::array<Ui::TableColumn, 4> columns_;
        std::string languageLabel_;
        bool rowsDirty_{true};
    };
}  // namespace Horo::Editor
