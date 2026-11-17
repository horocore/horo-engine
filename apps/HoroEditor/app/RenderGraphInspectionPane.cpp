#include "RenderGraphInspectionPane.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace Horo::Editor {
    namespace {
        constexpr GuiLocalizationScope GraphTextScope{"render_graph"};
        constexpr std::size_t PageSize = 128;
        constexpr std::array SectionKeys{"passes", "resources", "lifetimes", "barriers", "queues", "culling", "timing"};

        /** @brief Resolves declared Horo vocabularies into localized presentation without backend-name branching. */
        template <typename Enum, std::size_t N>
        const std::string &EnumLabel(const Enum value, const std::array<const char *, N> &keys, const ILocalizationService &localization) {
            const auto index = static_cast<std::size_t>(value);
            return GraphTextScope.Text(localization, index < N ? keys[index] : "unavailable");
        }

        /** @brief Projects one immutable authored pass into localized cells. */
        Ui::TableRow PassRow(const Render::RenderGraphPass &pass, const ILocalizationService &localization) {
            return {{{std::to_string(pass.reference.id.value)},
                     {EnumLabel(pass.kind, std::array{"graphics", "compute", "copy"}, localization)},
                     {EnumLabel(pass.queue, std::array{"graphics", "compute", "transfer"}, localization)},
                     {EnumLabel(pass.cullPolicy, std::array{"keep", "allow_cull"}, localization)}}};
        }

        /** @brief Projects one immutable resource identity into localized cells. */
        Ui::TableRow ResourceRow(const Render::RenderGraphResource &resource, const ILocalizationService &localization) {
            std::string binding = GraphTextScope.Text(localization, "none");
            std::visit([&]<typename Handle>(const Handle &handle) {
                if constexpr (!std::is_same_v<Handle, std::monostate>)
                    binding = std::format("{}:{}:{}", handle.owner.value, handle.slot, handle.generation);
            }, resource.binding);
            return {{{std::to_string(resource.id.value)},
                     {EnumLabel(resource.kind, std::array{"buffer", "texture"}, localization)},
                     {EnumLabel(resource.resourceClass, std::array{"external", "persistent", "transient", "history"}, localization)},
                     {std::move(binding)}}};
        }

        /** @brief Projects one immutable compiled lifetime into localized cells. */
        Ui::TableRow LifetimeRow(const Render::RenderGraphResourceLifetime &lifetime, const ILocalizationService &localization) {
            const bool used = lifetime.disposition == Render::RenderGraphLifetimeDisposition::Used;
            return {{{std::to_string(lifetime.resource.value)},
                     {used ? std::to_string(lifetime.firstPass.id.value) : GraphTextScope.Text(localization, "unused")},
                     {used ? std::to_string(lifetime.lastPass.id.value) : GraphTextScope.Text(localization, "unused")},
                     {used ? std::format("{} → {}", lifetime.firstUseIndex, lifetime.lastUseIndex)
                           : GraphTextScope.Text(localization, "unused")}}};
        }

        /** @brief Projects one immutable logical transition into localized cells. */
        Ui::TableRow TransitionRow(const Render::RenderGraphTransition &transition, const ILocalizationService &localization) {
            return {{{std::to_string(transition.resource.value)},
                     {std::format("{} → {}", transition.before.id.value, transition.after.id.value)},
                     {std::format("{} → {}",
                                  EnumLabel(transition.oldState.access, std::array{"none", "read", "write", "read_write"}, localization),
                                  EnumLabel(transition.newState.access, std::array{"none", "read", "write", "read_write"}, localization))},
                     {std::format("{} → {}", transition.oldState.queue.value, transition.newState.queue.value)}}};
        }

        /** @brief Projects one immutable effective queue into localized cells. */
        Ui::TableRow QueueRow(const Render::RenderGraphInspectionExecutionPass &pass, const std::size_t index,
                              const ILocalizationService &localization) {
            return {{{std::to_string(pass.pass.id.value)},
                     {std::to_string(pass.queue.value)},
                     {std::to_string(index)},
                     {GraphTextScope.Text(localization, "planned")}}};
        }

        /** @brief Projects one immutable culling decision into localized cells. */
        Ui::TableRow CullingRow(const Render::RenderGraphPassDisposition &pass, const ILocalizationService &localization) {
            return {{{std::to_string(pass.pass.id.value)},
                     {EnumLabel(pass.disposition, std::array{"retained", "culled"}, localization)},
                     {EnumLabel(pass.reason,
                                std::array{"reason.policy", "reason.export", "reason.import", "reason.external", "reason.dependency",
                                           "reason.producer", "reason.unused", "reason.culled"},
                                localization)},
                     {GraphTextScope.Text(localization, "planned")}}};
        }

        /** @brief Draws source availability using the shared body-text contract. */
        void DrawStatus(const GlobalDockPaneDrawContext &context, const char *key, const ImVec4 color) {
            Ui::WrappedText(context.contentWidth, GraphTextScope.Text(context.gui.localization, key), color, context.gui.theme.fonts);
        }

        /** @brief Read-only optional pane: query/format work occurs only when the dock host draws this pane. */
        class GraphPane final : public IGlobalDockPane {
        public:
            static constexpr GuiLocalizationScope kTextScope = GraphTextScope;

            explicit GraphPane(RenderGraphInspectionQuery query) : IGlobalDockPane(kTextScope), query_(std::move(query)) {}

            std::string_view Id() const noexcept override {
                return "horo.global_dock.render_graph";
            }

            std::string_view LabelKey() const noexcept override {
                return "render_graph.title";
            }

            void Detach() override {
                query_ = {};
                snapshot_.reset();
                rows_.clear();
            }

            void Draw(const GlobalDockPaneDrawContext &context) override;

        private:
            std::size_t Count() const noexcept;
            Ui::TableRow Row(std::size_t index, const ILocalizationService &localization) const;
            void RefreshRows(const ILocalizationService &localization);
            bool ReadPublication(const GlobalDockPaneDrawContext &context);
            void DrawNavigation(const GlobalDockPaneDrawContext &context);
            RenderGraphInspectionQuery query_;
            std::shared_ptr<const Render::RenderGraphInspectionSnapshot> snapshot_;
            std::vector<Ui::TableRow> rows_;
            std::array<Ui::TableColumn, 4> columns_;
            std::size_t page_{};
            int section_{};
            std::string languageLabel_;
            std::string sourceLabel_;
            bool dirty_{true};
        };

        /** @brief Returns the full immutable dataset count, independent of the bounded visible page. */
        std::size_t GraphPane::Count() const noexcept {
            if (!snapshot_)
                return 0;
            switch (section_) {
                case 0:
                    return snapshot_->Passes().size();
                case 1:
                    return snapshot_->Resources().size();
                case 2:
                    return snapshot_->Lifetimes().size();
                case 3:
                    return snapshot_->Transitions().size();
                case 4:
                    return snapshot_->Execution().size();
                case 5:
                    return snapshot_->Dispositions().size();
                case 6:
                    return 1;
                default:
                    return 0;
            }
        }

        /** @brief Selects a typed projection for one record in the current immutable page. */
        Ui::TableRow GraphPane::Row(const std::size_t index, const ILocalizationService &localization) const {
            switch (section_) {
                case 0:
                    return PassRow(snapshot_->Passes()[index], localization);
                case 1:
                    return ResourceRow(snapshot_->Resources()[index], localization);
                case 2:
                    return LifetimeRow(snapshot_->Lifetimes()[index], localization);
                case 3:
                    return TransitionRow(snapshot_->Transitions()[index], localization);
                case 4:
                    return QueueRow(snapshot_->Execution()[index], index, localization);
                case 5:
                    return CullingRow(snapshot_->Dispositions()[index], localization);
                default:
                    return {{{GraphTextScope.Text(localization, "timing")},
                             {GraphTextScope.Text(localization, "unavailable")},
                             {GraphTextScope.Text(localization, "unavailable")},
                             {GraphTextScope.Text(localization, "planned")}}};
            }
        }

        /** @brief Rebuilds only one finite page when source, language, section or page changes. */
        void GraphPane::RefreshRows(const ILocalizationService &localization) {
            constexpr std::array Headers{std::array{"id", "kind", "queue_role", "policy"},
                                         std::array{"id", "kind", "class", "binding"},
                                         std::array{"id", "first_pass", "last_pass", "use_interval"},
                                         std::array{"id", "pass_edge", "access", "queue_edge"},
                                         std::array{"id", "queue", "order", "coverage"},
                                         std::array{"id", "disposition", "reason", "coverage"},
                                         std::array{"kind", "cpu", "gpu", "coverage"}};
            for (std::size_t column = 0; column < columns_.size(); ++column)
                columns_[column] = {std::to_string(column),
                                    GraphTextScope.Text(localization, Headers[static_cast<std::size_t>(section_)][column])};
            rows_.clear();
            const std::size_t end = std::min(Count(), page_ * PageSize + PageSize);
            rows_.reserve(end - page_ * PageSize);
            for (std::size_t index = page_ * PageSize; index < end; ++index)
                rows_.push_back(Row(index, localization));
            const auto &source = snapshot_->Context();
            sourceLabel_ = std::format("{}: {}\n{}: {}\n{}: {}", GraphTextScope.Text(localization, "renderer"), source.renderer.value,
                                       GraphTextScope.Text(localization, "frame"), source.frame.value,
                                       GraphTextScope.Text(localization, "revision"), source.revision);
            dirty_ = false;
        }

        /** @brief Reads only the composed cached publication and handles unavailable/failed source state. */
        bool GraphPane::ReadPublication(const GlobalDockPaneDrawContext &context) {
            if (!query_) {
                DrawStatus(context, "no_capture", Theme::Muted());
                return false;
            }
            const auto queried = query_();
            if (queried.HasError()) {
                DrawStatus(context, "query_failed", Theme::Err());
                return false;
            }
            if (snapshot_ != queried.Value()) {
                snapshot_ = queried.Value();
                page_ = 0;
                dirty_ = true;
            }
            if (!snapshot_) {
                DrawStatus(context, "no_capture", Theme::Muted());
                return false;
            }
            return true;
        }

        /** @brief Keeps page/section work bounded and invalidates presentation only when it changes. */
        void GraphPane::DrawNavigation(const GlobalDockPaneDrawContext &context) {
            const auto &localization = context.gui.localization;
            const auto &fonts = context.gui.theme.fonts;
            const auto text = [&](const char *key) -> const std::string & {
                return GraphTextScope.Text(localization, key);
            };
            std::array<const char *, SectionKeys.size()> labels{};
            for (std::size_t index = 0; index < labels.size(); ++index)
                labels[index] = text(SectionKeys[index]).c_str();
            ImGui::SetNextItemWidth(std::max(1.0F, context.contentWidth));
            if (Ui::ComboControl("##RenderGraphSection", &section_, labels.data(), static_cast<int>(labels.size()), fonts)) {
                page_ = 0;
                dirty_ = true;
            }
            if (languageLabel_ != labels[0]) {
                languageLabel_ = labels[0];
                dirty_ = true;
            }
            const std::size_t pages = std::max(std::size_t{1}, (Count() + PageSize - 1) / PageSize);
            if (Ui::Button(
                    {.label = text("previous").c_str(), .size = {context.contentWidth, 0}, .enabled = page_ > 0, .font = fonts.sans})) {
                --page_;
                dirty_ = true;
            }
            if (Ui::Button(
                    {.label = text("next").c_str(), .size = {context.contentWidth, 0}, .enabled = page_ + 1 < pages, .font = fonts.sans})) {
                ++page_;
                dirty_ = true;
            }
        }

        /** @brief Draws explicit source status and bounded selectable tables using shared localized controls. */
        void GraphPane::Draw(const GlobalDockPaneDrawContext &context) {
            ImGui::SetCursorScreenPos(context.contentOrigin);
            if (!ReadPublication(context))
                return;
            const auto &localization = context.gui.localization;
            const auto &fonts = context.gui.theme.fonts;
            DrawNavigation(context);
            if (dirty_)
                RefreshRows(localization);
            Ui::WrappedText(context.contentWidth, GraphTextScope.Text(localization, "planned"), Theme::Warn(), fonts);
            Ui::WrappedText(context.contentWidth, sourceLabel_, Theme::Text(), fonts);
            for (auto &row : rows_)
                for (auto &cell : row.cells)
                    cell.color = Theme::Text();
            static_cast<void>(
                Ui::DrawTable({.id = "##RenderGraphRecords", .componentSize = Ui::ComponentSize::Medium}, columns_, rows_, fonts));
        }
    }  // namespace

    /** @copydoc MakeRenderGraphInspectionPane */
    std::unique_ptr<IGlobalDockPane> MakeRenderGraphInspectionPane(RenderGraphInspectionQuery query) {
        return std::make_unique<GraphPane>(std::move(query));
    }
}  // namespace Horo::Editor
