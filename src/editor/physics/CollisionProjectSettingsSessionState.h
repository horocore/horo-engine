#pragma once

#include "Horo/Editor/CollisionProjectSettingsSession.h"
#include "Horo/Scene/SceneSource.h"

namespace Horo::Editor {
    /** @brief One exact review root; retained values never lend runtime or document mutation authority. */
    struct CollisionSettingsReview::State final {
        std::weak_ptr<const std::uint8_t> opening;
        std::uint64_t revision{};
        Assets::AssetRegistryRevision registryRevision;
        Assets::AssetRegistrySnapshot registrySnapshot;
        CollisionInventoryEvidence inventory;
        CollisionActivationEvidence activation;
        std::optional<SceneTransactionSnapshot> activeScene;
        std::shared_ptr<const Application::ProjectCollisionSettings> candidate;
        std::vector<CollisionSourceFence> sources;
        std::vector<CollisionReferenceImpact> impacts;
        bool allowed{};
    };

    /** @brief Borrowed source and publication authorities; the session revokes before their owners retire. */
    struct CollisionSourceAuthorities final {
        const Assets::AssetRegistry *registry{};
        const Assets::IAssetRegistrySource *source{};
        DurableFileSystem *files{};
        ProjectMutationCoordinator *mutations{};
    };

    /** @brief Owner-thread cache; Close revokes before any borrowed owner can retire. No draw path performs I/O. */
    struct CollisionProjectSettingsSession::State final {
        std::filesystem::path root;
        std::string projectId;
        CollisionSourceAuthorities authorities;
        SceneDocumentTransactions *activeScene{};
        std::filesystem::path activeScenePath;
        std::optional<SceneSource::TriggerVolumeConversionContext> triggerBinding;
        std::optional<SceneTransactionSession> sceneOpening;
        CollisionInventoryEvidence inventory;
        CollisionActivationEvidence activation;
        std::unique_ptr<ProjectCollisionSettingsStore> store;
        Application::ProjectCollisionFileSnapshot saved;
        std::shared_ptr<const Application::ProjectCollisionSettings> draft;
        std::vector<std::shared_ptr<const Application::ProjectCollisionSettings>> history;
        std::size_t cursor{};
        std::uint64_t revision{1};
        std::shared_ptr<const std::uint8_t> opening{std::make_shared<const std::uint8_t>(0)};
        std::thread::id owner{std::this_thread::get_id()};
        bool dirty{};
        bool savedDurable{true};
        bool closed{};

        /** @brief Rejects closed/foreign callers before touching any borrowed owner. */
        [[nodiscard]] Result<void> Admit() const;
        /** @brief Generates a correctly tagged inert identity only after owner/lifecycle admission. */
        template <typename Id> [[nodiscard]] Result<Id> NewIdentity() const;
        /** @brief Captures real registered sources and canonical active authored state for one complete candidate. */
        [[nodiscard]] Result<std::shared_ptr<CollisionSettingsReview::State>> Review(
            std::shared_ptr<const Application::ProjectCollisionSettings> candidate) const;
        /** @brief Captures every known source and unsupported owner without fabricating empty reference evidence. */
        [[nodiscard]] Result<void> CaptureSources(const Assets::AssetRegistrySnapshot &snapshot, bool retires,
                                                  CollisionSettingsReview::State &review) const;
        /** @brief Captures one registered source using its owning typed codec, or retains an explicit unsupported impact. */
        [[nodiscard]] Result<void> CaptureRecord(const Assets::AssetRecord &record, bool retires,
                                                 CollisionSettingsReview::State &review) const;
        /** @brief Rechecks the exact review at the final owner-thread publication fence. */
        [[nodiscard]] Result<void> Fresh(const CollisionSettingsReview::State &review) const;
    };
}  // namespace Horo::Editor
