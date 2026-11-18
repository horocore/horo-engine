#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Assets/AssetCookInputSnapshot.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "WorkspacePathText.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <format>
#include <random>
#include <set>

namespace Horo::Editor {
    namespace {
        using AI::EnvironmentQuerySourceCodec;

        /** @brief Captures a registered query and its ordinary sidecar through the existing bounded source reader. */
        Result<Assets::AssetCookInputSnapshot> CaptureQuerySource(const std::filesystem::path &root,
                                                                  const Assets::AssetRegistrySnapshot &registry, Assets::AssetId source) {
            const Assets::AssetCookLimits limits{.maximumSourceBytes = EnvironmentQuerySourceCodec::MaximumBytes, .maximumAssets = 1};
            return Assets::AssetCookInputSnapshot::CaptureSelected(root, registry, std::span{&source, 1}, limits,
                                                                   EnvironmentQuerySourceCodec::MaximumBytes);
        }

        /** @brief Creates a new semantic QueryId while retaining the selected source's exact stage and result contracts. */
        Result<std::vector<std::uint8_t>> PrepareQueryCopy(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                                           Assets::AssetId selected) {
            using Copy = Result<std::vector<std::uint8_t>>;
            std::vector<Assets::AssetId> queries;
            const Assets::AssetCookLimits limits{.maximumSourceBytes = EnvironmentQuerySourceCodec::MaximumBytes};
            for (const auto &record : registry.Records()) {
                if (record.type.Value() != AI::EnvironmentQuerySourceAssetType)
                    continue;
                if (queries.size() == limits.maximumAssets)
                    return Copy::Failure(MakeError(Assets::AssetSourceErrors::Limit));
                queries.push_back(record.id);
            }
            constexpr std::uint64_t MaximumInventoryBytes = 64ULL * 1024ULL * 1024ULL;
            const auto inventory = Assets::AssetCookInputSnapshot::CaptureSelected(root, registry, queries, limits, MaximumInventoryBytes);
            if (inventory.HasError())
                return Copy::Failure(inventory.ErrorValue());

            std::set<AI::QueryId> known;
            std::optional<AI::EnvironmentQueryAsset> original;
            for (const auto &file : inventory.Value().Sources()) {
                auto query = EnvironmentQuerySourceCodec::Decode(file.bytes);
                if (query.HasError())
                    return Copy::Failure(query.ErrorValue());
                if (!known.insert(query.Value().Id()).second)
                    return Copy::Failure(MakeError(AI::AIErrors::EnvironmentQueryIdentityConflict));
                if (file.record.id == selected)
                    original = std::move(query).Value();
            }
            if (!original)
                return Copy::Failure(MakeError(AI::AIErrors::EnvironmentQuerySchemaInvalid));
            if (const auto verified = inventory.Value().VerifyUnchanged(); verified.HasError())
                return Copy::Failure(verified.ErrorValue());

            std::random_device entropy;
            std::uniform_int_distribution<std::uint64_t> ids;
            for (unsigned attempt = 0; attempt < 32; ++attempt) {
                const auto id = AI::QueryId::Create(ids(entropy));
                if (id.HasError() || known.contains(id.Value()))
                    continue;
                const auto stages = original->Stages();
                AI::QueryAssetSource source{.id = id.Value(),
                                            .displayName = original->DisplayName(),
                                            .result = original->ResultSchema(),
                                            .stages = {stages.begin(), stages.end()}};
                const auto query = AI::EnvironmentQueryAsset::Capture(source);
                if (query.HasError())
                    return Copy::Failure(query.ErrorValue());
                return EnvironmentQuerySourceCodec::Encode(query.Value());
            }
            return Copy::Failure(MakeError(AI::AIErrors::EnvironmentQueryIdentityConflict));
        }

        /** @brief Keeps unknown contributed stages visible and distinguishable from executable stage families. */
        std::string_view StageLabel(AI::QueryStageKind kind) {
            using enum AI::QueryStageKind;
            switch (kind) {
                case Generator:
                    return "workspace.environment_query.generator";
                case Test:
                    return "workspace.environment_query.test";
                case Unknown:
                case Count:
                    return "workspace.environment_query.unknown";
            }
            return "workspace.environment_query.unknown";
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::OpenEmbeddedEnvironmentQuerySource */
    void EditorWorkspaceController::OpenEmbeddedEnvironmentQuerySource(const SourceOpenResult &result) {
        const auto &opened = *result.document;
        const auto fail = [&](std::string_view key) {
            if (opened.disposition == DocumentOpenDisposition::Opened)
                static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            m_viewModel.contentBrowserOperationError = key;
        };
        const auto *record = m_assetRegistry.FindByPath(opened.identity.key.source.Value());
        if (!record || record->type.Value() != AI::EnvironmentQuerySourceAssetType) {
            fail("workspace.environment_query.invalid");
            return;
        }
        auto &documents = m_viewModel.environmentQueryDocuments;
        constexpr std::size_t MaximumDocuments = 16;
        if (documents.empty())
            documents.reserve(MaximumDocuments);
        const auto existing = std::ranges::find_if(documents, [&](const auto &document) {
            return document.identity.instance == opened.identity.instance || document.asset == record->id;
        });
        if (existing == documents.end() && documents.size() >= MaximumDocuments) {
            fail("workspace.environment_query.limit");
            return;
        }
        const auto captured = CaptureQuerySource(ProjectRoot(), m_assetRegistry, record->id);
        if (captured.HasError()) {
            fail("workspace.environment_query.invalid");
            return;
        }
        const auto &source = captured.Value().Sources().front();
        if (existing != documents.end() && existing->identity == opened.identity && existing->asset == record->id &&
            existing->source == source.sourceDigest) {
            static_cast<void>(m_viewModel.workspacePanelHost.FocusDocument(opened.identity.instance));
            return;
        }
        auto query = EnvironmentQuerySourceCodec::Decode(source.bytes);
        if (query.HasError() || captured.Value().VerifyUnchanged().HasError()) {
            fail("workspace.environment_query.invalid");
            return;
        }
        EnvironmentQueryWorkspaceDocument candidate{opened.identity, record->id, source.sourceDigest,
                                                    std::make_shared<const AI::EnvironmentQueryAsset>(std::move(query).Value())};
        auto &host = m_viewModel.workspacePanelHost;
        if (host.OpenDocument(opened.identity.key).HasError()) {
            fail("workspace.source_open.unavailable");
            return;
        }
        if (existing == documents.end()) {
            documents.push_back(std::move(candidate));
            return;
        }
        // A source move preserves the durable AssetId. The new path owns a fresh tab key.
        if (existing->identity.instance != opened.identity.instance)
            static_cast<void>(host.CloseDocument(existing->identity.instance));
        *existing = std::move(candidate);
    }

    /** @copydoc EditorWorkspaceController::CloseEnvironmentQuerySource */
    bool EditorWorkspaceController::CloseEnvironmentQuerySource(DocumentInstanceId instance) {
        auto &documents = m_viewModel.environmentQueryDocuments;
        const auto opened = std::ranges::find_if(documents, [instance](const auto &document) {
            return document.identity.instance == instance;
        });
        if (opened == documents.end())
            return false;
        if (m_viewModel.workspacePanelHost.CloseDocument(instance).HasValue())
            documents.erase(opened);
        return true;
    }

    /** @copydoc EditorWorkspaceController::CopyEnvironmentQuerySource */
    bool EditorWorkspaceController::CopyEnvironmentQuerySource(const ContentBrowserAssetTransferPlan &plan, Assets::AssetId newId) {
        const auto fail = [&](std::string_view key) {
            m_viewModel.contentBrowserOperationError = key;
            return false;
        };
        if (plan.companions.size() != 2 || std::ranges::find(plan.companions, plan.sourceSidecar) == plan.companions.end())
            return fail("workspace.content_browser.operation.companion_invalid");
        if (!newId.IsValid())
            return fail("workspace.content_browser.operation.copy_failed");
        const auto relative = WorkspacePortablePathText(plan.destination.lexically_relative(plan.projectRoot));
        const auto path = ProjectPath::Parse(relative);
        const auto type = Assets::AssetTypeId::Parse(AI::EnvironmentQuerySourceAssetType);
        if (path.HasError() || type.HasError())
            return fail("workspace.content_browser.operation.invalid_target");
        try {
            const auto registry = m_mutableAssetRegistry->Snapshot();
            const auto *current = registry.Find(plan.assetId);
            if (!current || current->sourcePath.String() != WorkspacePortablePathText(plan.source.lexically_relative(plan.projectRoot)))
                return fail("workspace.environment_query.copy_failed");
            const auto prepared = PrepareQueryCopy(plan.projectRoot, registry, plan.assetId);
            if (prepared.HasError() || m_mutableAssetRegistry->Snapshot().Revision() != registry.Revision())
                return fail("workspace.environment_query.copy_failed");
            const auto bytes = std::as_bytes(std::span{prepared.Value()});
            Application::PreparedAuthoredProjectAssetBatch batch{newId.ToString(),
                                                                 plan.projectRoot,
                                                                 {{newId, type.Value(), path.Value(), {bytes.begin(), bytes.end()}}}};
            Application::ProjectAssetImportCommitter writer{*m_mutations, *m_durableFiles, m_mutableAssetRegistry};
            const auto written = writer.CommitAuthored(batch);
            if (writer.LastReceipt().registryPublished) {
                m_assetRegistry = m_mutableAssetRegistry->Snapshot();
                m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
                if (m_collisionInventory)
                    *m_collisionInventory = {};
                if (m_collisionSettings)
                    static_cast<void>(m_collisionSettings->ObserveInventory({}));
                RebuildContentBrowserProjection(plan.projectRoot, WorkspaceNativePath(m_viewModel.contentBrowser.absoluteCurrentPath));
            }
            return written.HasValue() || fail(writer.LastReceipt().status == Application::ProjectAssetImportStatus::ReconciliationRequired
                                                  ? "workspace.content_browser.operation.rollback_failed"
                                                  : "workspace.environment_query.copy_failed");
        } catch (...) {
            return fail("workspace.environment_query.copy_failed");
        }
    }

    /** @copydoc EditorWorkspaceView::DrawEnvironmentQueryDocument */
    bool EditorWorkspaceView::DrawEnvironmentQueryDocument(const EditorWorkspaceViewModel &viewModel) const {
        const auto opened = std::ranges::find_if(viewModel.environmentQueryDocuments, [&](const auto &document) {
            return document.identity.instance == viewModel.workspacePanelHost.ActiveDocument();
        });
        if (opened == viewModel.environmentQueryDocuments.end())
            return false;
        const auto text = [&](std::string_view key) {
            return m_context.localization.Get("editor", key);
        };
        const auto label = [&](std::string_view value, const auto &color) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, color, m_context.theme.fonts);
        };
        const auto &query = *opened->query;
        if (!query.DisplayName().empty())
            label(query.DisplayName(), Theme::Text());
        label(text("workspace.environment_query.read_only"), Theme::Muted());
        for (const auto &stage : query.Stages()) {
            const auto stageType = text(StageLabel(stage.kind));
            label(std::format("{} / {} ({})", stageType, stage.displayName, stage.id.Value()), Theme::Text());
        }
        return true;
    }
}  // namespace Horo::Editor
