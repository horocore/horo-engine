#pragma once

#include "Horo/Assets/AssetCook.h"
#include "Horo/Runtime/Save/SaveSceneCanonicalState.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/Runtime/Scene/SaveContentReconciliation.h"
#include "Horo/Runtime/Scene/SavedSceneBootstrap.h"

#include <thread>

namespace Horo::Runtime::SaveContentDetail {
    struct InstalledGeneration final {
        std::shared_ptr<const Assets::AssetArchiveProvider> provider;
        std::vector<GameplayPersistenceInstallation> modules;
        std::uint64_t generation{};
    };

    struct InstalledState final {
        std::shared_ptr<const InstalledGeneration> current;
        std::thread::id ownerThread;
        bool closed{};
    };

    struct ResolvedAsset final {
        Assets::AssetId original;
        Assets::AssetId installed;
        Assets::AssetTypeId type;
        Sha256Digest envelopeDigest;
        Assets::AssetCookArtifact artifact;
    };

    struct ReconciliationState final {
        ReconciliationState(std::shared_ptr<InstalledState> owner, std::shared_ptr<const InstalledGeneration> generation,
                            ImmutableSaveArchive source, ValidatedSaveArchive reader, SaveContentProjectPolicy project,
                            CancellationToken cancel)
            : installedOwner(std::move(owner)), installed(std::move(generation)), sourceArchive(std::move(source)),
              sourceReader(std::move(reader)), policy(std::move(project)), cancellation(std::move(cancel)) {}

        std::shared_ptr<InstalledState> installedOwner;
        std::shared_ptr<const InstalledGeneration> installed;
        ImmutableSaveArchive sourceArchive;
        ValidatedSaveArchive sourceReader;
        SaveContentProjectPolicy policy;
        SaveCompatibilityPolicy preservationPolicy;
        CancellationToken cancellation;
        std::vector<SaveContentRequirement> requirements;
        std::vector<SaveSceneCanonicalLayoutEntry> canonicalLayout;
        std::vector<SaveContentDiagnostic> diagnostics;
        std::vector<ResolvedAsset> assets;
        std::vector<SaveParticipantId> quarantinedOwners;
        SaveUnknownDataReport opaque;
        std::uint64_t readBytes{};
        bool degraded{};
        bool legacy{};
    };

    /** @brief Immutable owner-admitted facts and const source aliases; none observes live installation or Scene state.
     * @details Source metadata is frozen before its sole content-owned Queue. Aliases pin those immutable fields and
     *          native generation storage without reading its admission flags or issuing another native callback.
     */
    struct AcceptedCaptureSeal final {
        AcceptedCaptureSeal(const std::shared_ptr<ReconciliationState> &source, const RuntimeSaveCaptureProvenance &admitted,
                            SaveWorldId worldIdentity, SaveBaseSceneId baseIdentity)
            : provenance(admitted), project(source->sourceReader.Header().project), world(worldIdentity), baseScene(baseIdentity),
              sourceArchive(source, &source->sourceArchive), sourceReader(source, &source->sourceReader), opaque(source, &source->opaque),
              canonicalLayout(source, &source->canonicalLayout), preservationPolicy(source, &source->preservationPolicy),
              maximumPreservedBytes(source->policy.maximumPreservedBytes), diagnostics(source, &source->diagnostics),
              nativePins(source->installed) {}

        const RuntimeSaveCaptureProvenance provenance;
        const SaveProjectId project;
        const SaveWorldId world;
        const SaveBaseSceneId baseScene;
        const std::shared_ptr<const ImmutableSaveArchive> sourceArchive;
        const std::shared_ptr<const ValidatedSaveArchive> sourceReader;
        const std::shared_ptr<const SaveUnknownDataReport> opaque;
        const std::shared_ptr<const std::vector<SaveSceneCanonicalLayoutEntry>> canonicalLayout;
        const std::shared_ptr<const SaveCompatibilityPolicy> preservationPolicy;
        const std::uint64_t maximumPreservedBytes;
        const std::shared_ptr<const std::vector<SaveContentDiagnostic>> diagnostics;
        const std::shared_ptr<const InstalledGeneration> nativePins;
    };

    struct WorldState final {
        WorldState(std::shared_ptr<RuntimeSceneService> owner, SavedSceneBootstrapDescriptor source,
                   std::shared_ptr<ReconciliationState> reconciliation)
            : service(std::move(owner)), descriptor(std::move(source)), content(std::move(reconciliation)) {}

        std::shared_ptr<RuntimeSceneService> service;
        SavedSceneBootstrapDescriptor descriptor;
        std::shared_ptr<ReconciliationState> content;
        std::optional<ScenePublicationReceipt> publication;
        SceneRuntimeId boundScene;
        std::weak_ptr<const ICanonicalStateAdapter> declarationAdapter;
        std::optional<RuntimeSaveCaptureProvenance> captureAdmission;
        std::optional<CanonicalEncodedValue> captureRequirements;
    };

    /** @brief Resolves the explicit legacy baseline from actual mounted bytes under the same bounded admission. */
    Result<void> ResolveLegacyBaseline(ReconciliationState &state, const SaveAssetContentRequirement &required);
}  // namespace Horo::Runtime::SaveContentDetail
