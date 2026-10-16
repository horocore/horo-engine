#pragma once
#include "Horo/Assets/NavMeshAssetType.h"
#include "Horo/Editor/NavigationProjectSettingsSession.h"
#include "Horo/Scene/SceneSource.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief State-local typed source/session rejection. */
        Error RejectNavigationSessionState(const Navigation::NavigationSettingsFailure reason) {
            return MakeError(Navigation::NavigationSettingsError(reason));
        }

        /** @brief Sole production record support; generated/unknown source is never silently erased. */
        auto NavigationSessionSupport() {
            return std::array{Navigation::NavigationDefinitionRecordSupport()};
        }
    }  // namespace

    struct NavigationProjectSettingsSession::State final {
        struct Definition final {
            std::filesystem::path path;
            Assets::AssetSourceIdentity source;
            Navigation::NavigationAuthoredRecordId record;
            std::unique_ptr<NavigationDefinitionDocument> document;
        };

        struct Scene final {
            std::filesystem::path path;
            Assets::AssetSourceIdentity source;
            std::unique_ptr<SceneDocumentTransactions> owned;
            std::weak_ptr<SceneDocumentTransactions::Lifetime> port;
        };

        /** @brief Loads the sole accepted authored definition record without dropping unknown/generated data. */
        Result<Definition> LoadDefinition(const Assets::AssetRecord &record, const Assets::AssetSourceCapture &source,
                                          const std::uint64_t generation, const std::filesystem::path &path) {
            using Return = Result<Definition>;
            const auto support = NavigationSessionSupport();
            const Navigation::NavigationSourceLoadContext context{.supportedRecords = support};
            auto envelope = Navigation::DeserializeNavigationSourceRecords(source.bytes, context);
            if (envelope.HasError())
                return Return::Failure(envelope.ErrorValue());
            if (envelope.Value().Records().size() != 1 || !envelope.Value().GeneratedPayloads().empty() ||
                envelope.Value().HasQuarantinedGeneratedPayloads())
                return Return::Failure(RejectNavigationSessionState(Navigation::NavigationSettingsFailure::IncompleteInventory));
            const auto &authored = envelope.Value().Records().front();
            auto definition = Navigation::DecodeNavigationDefinitionRecord(authored);
            if (definition.HasError())
                return Return::Failure(definition.ErrorValue());
            auto document = NavigationDefinitionDocument::Open(record.id, generation, 1, std::move(definition).Value());
            if (document.HasError())
                return Return::Failure(document.ErrorValue());
            return Return::Success(
                {path, source.identity, authored.id, std::make_unique<NavigationDefinitionDocument>(std::move(document).Value())});
        }

        /** @brief Reuses the active canonical owner or loads an inactive Scene through its real source codec. */
        Result<Scene> LoadScene(const Assets::AssetRecord &record, const Assets::AssetSourceCapture &source, const std::uint64_t generation,
                                const std::filesystem::path &path, SceneDocumentTransactions *activeScene,
                                const std::optional<SceneTransactionSnapshot> &active) {
            using Return = Result<Scene>;
            Scene scene;
            scene.path = path;
            scene.source = source.identity;
            if (active && active->session.owner == record.id)
                scene.port = activeScene->lifetime_;
            else {
                const auto &bytes = source.bytes;
                auto decoded = SceneSource::DecodeSceneSource({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
                if (decoded.HasError())
                    return Return::Failure(decoded.ErrorValue());
                auto sceneSource = std::move(decoded).Value();
                auto document = SceneDocumentTransactions::Create({record.id, generation}, std::move(sceneSource.objects),
                                                                  std::move(sceneSource.prefabInstances));
                if (document.HasError())
                    return Return::Failure(document.ErrorValue());
                scene.owned = std::make_unique<SceneDocumentTransactions>(std::move(document).Value());
                scene.port = scene.owned->lifetime_;
            }
            return Return::Success(std::move(scene));
        }

        /** @brief Retains the exact publication lock through the existing Assets native replacement protocol. */
        Result<Assets::AssetSourcePublication> Publish(const std::filesystem::path &path, const Assets::AssetSourceIdentity &expected,
                                                       const std::span<const std::byte> bytes) {
            auto lockPath = Assets::AssetSourcePublicationLockPath(root, path);
            if (lockPath.HasError())
                return Result<Assets::AssetSourcePublication>::Failure(lockPath.ErrorValue());
            auto lock = files->TryAcquireExclusive(lockPath.Value(), "horo.navigation.document");
            if (lock.HasError())
                return Result<Assets::AssetSourcePublication>::Failure(lock.ErrorValue());
            return Assets::PublishAssetSource(root, path, expected, bytes, lock.Value(), *files);
        }

        /** @brief Serializes the exact guarded source ticket using the sole production envelope support. */
        static Result<std::vector<std::byte>> DefinitionBytes(const NavigationDefinitionSave &save) {
            const auto support = NavigationSessionSupport();
            const Navigation::NavigationSourceLoadContext context{.supportedRecords = support};
            auto envelope =
                Navigation::NavigationSourceRecords::Create(Navigation::CurrentNavigationSourceSchemaVersion, {save.Record()}, context);
            if (envelope.HasError())
                return Result<std::vector<std::byte>>::Failure(envelope.ErrorValue());
            return Navigation::SerializeNavigationSourceRecords(envelope.Value());
        }

        /** @brief Publishes exact source and acknowledges only confirmed durability; invokes the borrowed callback synchronously.
         * @details Source identity advances after replacement even when directory durability is uncertain. The stack-only
         * callback retains no owner and runs only after a durable receipt, preserving each document's exact save fence.
         */
        template <typename Acknowledge>
        NavigationDocumentSaveOutcome PublishDocument(const std::filesystem::path &path, Assets::AssetSourceIdentity &source,
                                                      const std::span<const std::byte> bytes, NavigationDocumentSaveOutcome outcome,
                                                      Acknowledge &&acknowledge) {
            auto published = Publish(path, source, bytes);
            if (published.HasError()) {
                outcome.error = published.ErrorValue();
                return outcome;
            }
            auto receipt = std::move(published).Value();
            source = receipt.identity;
            if (receipt.status == Assets::AssetSourcePublicationStatus::Durable) {
                const auto accepted = acknowledge();
                if (accepted.HasError())
                    outcome.error = accepted.ErrorValue();
                else
                    outcome.durable = true;
            } else if (receipt.durabilityError)
                outcome.error = std::move(receipt.durabilityError);
            else
                outcome.error = RejectNavigationSessionState(Navigation::NavigationSettingsFailure::Stale);
            return outcome;
        }

        /** @brief Publishes one definition and acknowledges only its durable exact source ticket. */
        NavigationDocumentSaveOutcome SaveDefinition(Definition &entry, NavigationDocumentSaveOutcome outcome) {
            auto save = entry.document->CaptureSave(entry.record);
            if (save.HasError()) {
                outcome.error = save.ErrorValue();
                return outcome;
            }
            auto bytes = DefinitionBytes(save.Value());
            if (bytes.HasError()) {
                outcome.error = bytes.ErrorValue();
                return outcome;
            }
            return PublishDocument(entry.path, entry.source, bytes.Value(), std::move(outcome), [&entry, &save] {
                return entry.document->AcknowledgeSave(save.Value(), save.Value().ExpectedSourceRevision() + 1);
            });
        }

        /** @brief Saves the captured canonical Scene without replacing its history or clearing later edits. */
        NavigationDocumentSaveOutcome SaveScene(Scene &entry, SceneDocumentTransactions &owner, const SceneTransactionSnapshot &v,
                                                NavigationDocumentSaveOutcome outcome) {
            const auto text = SceneSource::EncodeSceneSource({v.objects, v.prefabInstances});
            return PublishDocument(entry.path, entry.source, std::as_bytes(std::span{text.data(), text.size()}), std::move(outcome),
                                   [&owner, &v] {
                return owner.MarkSaved({v.session, v.revision, v.state, true, {}});
            });
        }

        std::filesystem::path root;
        const Assets::AssetRegistry *registry{};
        Assets::AssetRegistryRevision revision;
        DurableFileSystem *files{};
        ProjectMutationCoordinator *mutations{};
        std::vector<Definition> definitions;
        std::vector<NavigationDefinitionSnapshot> definitionProjection;
        std::vector<Scene> scenes;
        std::unique_ptr<NavigationSettingsWorkflow> workflow;
        std::unique_ptr<Application::NavigationProjectSettingsStore> store;
        std::optional<Application::NavigationSettingsFileSnapshot> policy;
        std::vector<Application::NavigationTargetProviderEvidence> targets;
        std::optional<Assets::AssetId> openDefinition;
        std::thread::id owner{std::this_thread::get_id()};
        bool closed{};
    };
}  // namespace Horo::Editor
