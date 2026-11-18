#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Assets/AssetCookInputSnapshot.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
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

        /** @brief Exact query and result namespaces from current pinned source pairs. */
        struct QueryNamespace final {
            std::set<AI::QueryId> queries;
            std::set<AI::QueryResultSchemaId> results;
            std::optional<AI::EnvironmentQueryAsset> selected;
        };

        /** @brief Captures all registered query IDs and one optional duplication source, with bounded exact inputs. */
        Result<QueryNamespace> CaptureQueryNamespace(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                                     std::optional<Assets::AssetId> selected = {}) {
            using Namespace = Result<QueryNamespace>;
            std::vector<Assets::AssetId> queries;
            const Assets::AssetCookLimits limits{.maximumSourceBytes = EnvironmentQuerySourceCodec::MaximumBytes};
            for (const auto &record : registry.Records()) {
                if (record.type.Value() != AI::EnvironmentQuerySourceAssetType)
                    continue;
                if (queries.size() == limits.maximumAssets)
                    return Namespace::Failure(MakeError(Assets::AssetSourceErrors::Limit));
                queries.push_back(record.id);
            }
            constexpr std::uint64_t MaximumInventoryBytes = 64ULL * 1024ULL * 1024ULL;
            const auto inventory = Assets::AssetCookInputSnapshot::CaptureSelected(root, registry, queries, limits, MaximumInventoryBytes);
            if (inventory.HasError())
                return Namespace::Failure(inventory.ErrorValue());

            QueryNamespace names;
            for (const auto &file : inventory.Value().Sources()) {
                auto query = EnvironmentQuerySourceCodec::Decode(file.bytes);
                if (query.HasError())
                    return Namespace::Failure(query.ErrorValue());
                if (!names.queries.insert(query.Value().Id()).second)
                    return Namespace::Failure(MakeError(AI::AIErrors::EnvironmentQueryIdentityConflict));
                names.results.insert(query.Value().ResultSchema().id);
                if (file.record.id == selected)
                    names.selected = std::move(query).Value();
            }
            if (selected && !names.selected)
                return Namespace::Failure(MakeError(AI::AIErrors::EnvironmentQuerySchemaInvalid));
            if (const auto verified = inventory.Value().VerifyUnchanged(); verified.HasError())
                return Namespace::Failure(verified.ErrorValue());
            return Namespace::Success(std::move(names));
        }

        /** @brief Issues a nonzero stable identity from the editor's local entropy source. */
        template <typename Identity> Result<Identity> IssueQueryIdentity(const std::set<Identity> &used = {}) {
            std::random_device entropy;
            std::uniform_int_distribution<std::uint64_t> ids;
            for (unsigned attempt = 0; attempt < 32; ++attempt) {
                const auto identity = Identity::Create(ids(entropy));
                if (identity.HasValue() && !used.contains(identity.Value()))
                    return identity;
            }
            return Result<Identity>::Failure(MakeError(AI::AIErrors::EnvironmentQueryIdentityConflict));
        }

        /** @brief Retains the selected source's exact stage and result contracts beneath a new QueryId. */
        Result<std::vector<std::uint8_t>> PrepareQueryCopy(const QueryNamespace &names) {
            using Copy = Result<std::vector<std::uint8_t>>;
            const auto id = IssueQueryIdentity(names.queries);
            if (id.HasError())
                return Copy::Failure(id.ErrorValue());
            const auto &original = *names.selected;
            const auto stages = original.Stages();
            const auto query = AI::EnvironmentQueryAsset::Capture({.id = id.Value(),
                                                                   .displayName = original.DisplayName(),
                                                                   .result = original.ResultSchema(),
                                                                   .stages = {stages.begin(), stages.end()}});
            return query.HasValue() ? EnvironmentQuerySourceCodec::Encode(query.Value()) : Copy::Failure(query.ErrorValue());
        }

        /** @brief Checks current durable pairs under the publisher's Project lease, including other editors' creations. */
        Result<void> AdmitInitialQueryNamespace(const std::filesystem::path &root, AI::QueryId id, AI::QueryResultSchemaId result) {
            auto current = Assets::PrepareAssetRegistryCandidate(root);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (current.Value().status != Assets::AssetRegistryBuildStatus::Complete)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            Assets::AssetRegistry detached;
            const auto rebuilt = detached.Publish(std::move(current).Value());
            if (rebuilt.status != Assets::AssetRegistryBuildStatus::Complete)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            const auto names = CaptureQueryNamespace(root, detached.Snapshot());
            if (names.HasError())
                return Result<void>::Failure(names.ErrorValue());
            if (names.Value().queries.contains(id) || names.Value().results.contains(result))
                return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryIdentityConflict));
            return Result<void>::Success();
        }

        /** @brief Checks source descriptors without activating the plan or hiding unresolved authored stages. */
        QueryDescriptorAvailability DescriptorAvailability(const AI::EnvironmentQueryAsset &query,
                                                           const AI::QuerySchemaRegistry *registry) {
            if (!registry)
                return QueryDescriptorAvailability::RegistryUnavailable;
            return AI::EnvironmentQueryPlan::Compile(query, *registry).HasValue() ? QueryDescriptorAvailability::Validated
                                                                                  : QueryDescriptorAvailability::Incompatible;
        }

        /** @brief Localizes the exact qualification state of this saved document. */
        std::string_view DescriptorLabel(QueryDescriptorAvailability availability) {
            using enum QueryDescriptorAvailability;
            switch (availability) {
                case Validated:
                    return "workspace.environment_query.descriptors_valid";
                case Incompatible:
                    return "workspace.environment_query.descriptors_incompatible";
                case RegistryUnavailable:
                    return "workspace.environment_query.descriptors_unavailable";
            }
            return "workspace.environment_query.descriptors_unavailable";
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
        candidate.descriptors = DescriptorAvailability(*candidate.query, m_querySchema);
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
            const auto names = CaptureQueryNamespace(plan.projectRoot, registry, plan.assetId);
            if (names.HasError())
                return fail("workspace.environment_query.copy_failed");
            const auto prepared = PrepareQueryCopy(names.Value());
            if (prepared.HasError() || m_mutableAssetRegistry->Snapshot().Revision() != registry.Revision())
                return fail("workspace.environment_query.copy_failed");
            const auto bytes = std::as_bytes(std::span{prepared.Value()});
            Application::PreparedAuthoredProjectAssetBatch batch{newId.ToString(),
                                                                 plan.projectRoot,
                                                                 {{newId, type.Value(), path.Value(), {bytes.begin(), bytes.end()}}}};
            Application::ProjectAssetImportCommitter writer{*m_mutations, *m_durableFiles, m_mutableAssetRegistry};
            const auto written = writer.CommitAuthored(batch);
            return FinishEnvironmentQueryPublication(writer.LastReceipt(), written, "workspace.environment_query.copy_failed");
        } catch (...) {
            return fail("workspace.environment_query.copy_failed");
        }
    }

    /** @copydoc EditorWorkspaceController::PublishInitialEnvironmentQuery */
    void EditorWorkspaceController::PublishInitialEnvironmentQuery(const std::filesystem::path &destination,
                                                                   const EnvironmentQueryCreationRequest &request, Assets::AssetId newId) {
        auto &error = m_viewModel.contentBrowserOperationError;
        error = "workspace.environment_query.create_failed";
        try {
            const auto root = ProjectRoot();
            const auto path = ProjectPath::Parse(WorkspacePortablePathText(destination.lexically_relative(root)));
            const auto type = Assets::AssetTypeId::Parse(AI::EnvironmentQuerySourceAssetType);
            const auto id = IssueQueryIdentity<AI::QueryId>();
            const auto stage = IssueQueryIdentity<AI::QueryStageId>();
            const auto result = IssueQueryIdentity<AI::QueryResultSchemaId>();
            if (!newId.IsValid() || path.HasError() || type.HasError() || id.HasError() || stage.HasError() || result.HasError())
                return;
            const auto source =
                m_latticeQueryFactory(id.Value(), stage.Value(), result.Value(), request.name, request.lattice, request.maximumResults);
            if (source.HasError() || source.Value().Id() != id.Value() || source.Value().ResultSchema().id != result.Value() ||
                source.Value().Stages().size() != 1 || source.Value().Stages().front().id != stage.Value() ||
                DescriptorAvailability(source.Value(), m_querySchema) != QueryDescriptorAvailability::Validated)
                return;
            const auto encoded = EnvironmentQuerySourceCodec::Encode(source.Value());
            if (encoded.HasError())
                return;
            const auto bytes = std::as_bytes(std::span{encoded.Value()});
            const Application::PreparedAuthoredProjectAssetBatch batch{newId.ToString(),
                                                                       root,
                                                                       {{newId, type.Value(), path.Value(), {bytes.begin(), bytes.end()}}}};
            Application::ProjectAssetImportCommitter writer{*m_mutations, *m_durableFiles, m_mutableAssetRegistry};
            const auto written = writer.CommitAuthored(batch, {}, [&] {
                return AdmitInitialQueryNamespace(root, id.Value(), result.Value());
            });
            if (FinishEnvironmentQueryPublication(writer.LastReceipt(), written, "workspace.environment_query.create_failed"))
                OpenSourceFile({.path = destination, .origin = SourceOpenOrigin::Command, .mode = SourceOpenMode::EmbeddedOnly});
        } catch (...) {
            // The publisher contains native and admission failures and returns its durable receipt.
        }
    }

    /** @copydoc EditorWorkspaceController::FinishEnvironmentQueryPublication */
    bool EditorWorkspaceController::FinishEnvironmentQueryPublication(const Application::ProjectAssetImportReceipt &receipt,
                                                                      const Result<void> &outcome, std::string_view failureKey) {
        if (receipt.registryPublished) {
            m_assetRegistry = m_mutableAssetRegistry->Snapshot();
            m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
            if (m_collisionInventory)
                *m_collisionInventory = {};
            if (m_collisionSettings)
                static_cast<void>(m_collisionSettings->ObserveInventory({}));
            RebuildContentBrowserProjection(ProjectRoot(), WorkspaceNativePath(m_viewModel.contentBrowser.absoluteCurrentPath));
        }
        if (outcome.HasValue())
            return true;
        m_viewModel.contentBrowserOperationError = receipt.status == Application::ProjectAssetImportStatus::ReconciliationRequired
                                                       ? "workspace.content_browser.operation.rollback_failed"
                                                       : failureKey;
        return false;
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
        label(text(DescriptorLabel(opened->descriptors)), Theme::Muted());
        for (const auto &stage : query.Stages()) {
            const auto stageType = text(StageLabel(stage.kind));
            label(std::format("{} / {} ({})", stageType, stage.displayName, stage.id.Value()), Theme::Text());
        }
        return true;
    }
}  // namespace Horo::Editor
