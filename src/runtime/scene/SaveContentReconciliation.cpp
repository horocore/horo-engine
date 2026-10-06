#include "Horo/Assets/AssetCook.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "RuntimeSceneErrors.h"
#include "SaveContentInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Runtime {
    namespace {
        using State = SaveContentDetail::ReconciliationState;

        /** @brief Checks owner sequencing before touching mutable installation authority. */
        Result<void> CheckAdmission(const std::shared_ptr<SaveContentDetail::InstalledState> &owner,
                                    const std::shared_ptr<const SaveContentDetail::InstalledGeneration> &generation,
                                    const CancellationToken &cancellation) {
            if (!owner || owner->ownerThread != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(SaveErrors::OperationCancelled));
            if (owner->closed || !generation || owner->current != generation)
                return Result<void>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            for (const auto &module : generation->modules)
                if (!module.CanAdmit())
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            return Result<void>::Success();
        }

        /** @brief Loads only mounted bytes after aggregate work admission and verifies the actual cooked envelope. */
        Result<std::optional<Assets::AssetCookArtifact>> ResolveAsset(State &state, const SaveAssetContentRequirement &required) {
            const auto &provider = *state.installed->provider;
            const auto length = provider.StoredByteLength(required.asset);
            if (!length)
                return Result<std::optional<Assets::AssetCookArtifact>>::Success(std::nullopt);
            if (*length > state.policy.maximumContentReadBytes - state.readBytes)
                return Result<std::optional<Assets::AssetCookArtifact>>::Failure(MakeError(SceneErrors::AssetBudgetExceeded));
            state.readBytes += *length;
            auto loaded = provider.Load(required.asset, state.cancellation);
            if (loaded.HasError())
                return Result<std::optional<Assets::AssetCookArtifact>>::Failure(loaded.ErrorValue());
            const auto &bytes = loaded.Value();
            const auto digest = ComputeSha256({reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()});
            Assets::AssetCookLimits limits;
            limits.maximumArtifactBytes = *length;
            auto artifact = Assets::DecodeCookedArtifact(bytes, limits);
            if (artifact.HasError())
                return Result<std::optional<Assets::AssetCookArtifact>>::Failure(artifact.ErrorValue());
            if (artifact.Value().id != required.asset || artifact.Value().target != provider.Target())
                return Result<std::optional<Assets::AssetCookArtifact>>::Failure(MakeError(SceneErrors::SaveBootstrapIncompatible));
            if (artifact.Value().type != required.type || digest != required.envelopeDigest)
                return Result<std::optional<Assets::AssetCookArtifact>>::Success(std::nullopt);
            return Result<std::optional<Assets::AssetCookArtifact>>::Success(std::move(artifact).Value());
        }

        /** @brief Resolves an asset or an explicitly approved same-type replacement, retaining verified envelope ownership. */
        Result<bool> ResolveAssetRequirement(State &state, const SaveAssetContentRequirement &required, SaveContentDiagnostic &diagnostic) {
            auto artifact = ResolveAsset(state, required);
            if (artifact.HasError())
                return Result<bool>::Failure(artifact.ErrorValue());
            if (artifact.Value()) {
                state.assets.push_back(
                    {required.asset, required.asset, required.type, required.envelopeDigest, std::move(*artifact.Value())});
                return Result<bool>::Success(true);
            }
            const auto replacement = std::ranges::find(state.policy.substitutions, required.asset, &SaveAssetContentSubstitution::original);
            if (replacement == state.policy.substitutions.end())
                return Result<bool>::Success(false);
            if (replacement->type != required.type || replacement->replacement == required.asset)
                return Result<bool>::Failure(MakeError(SceneErrors::SaveBootstrapIncompatible));
            const SaveAssetContentRequirement substituted{replacement->replacement, replacement->type,
                                                          replacement->replacementEnvelopeDigest};
            auto resolved = ResolveAsset(state, substituted);
            if (resolved.HasError())
                return Result<bool>::Failure(resolved.ErrorValue());
            if (!resolved.Value())
                return Result<bool>::Success(false);
            state.assets.push_back(
                {required.asset, substituted.asset, substituted.type, substituted.envelopeDigest, std::move(*resolved.Value())});
            diagnostic.disposition = SaveContentDisposition::Substituted;
            diagnostic.remedy = SaveContentRemedy::ReviewApprovedSubstitution;
            diagnostic.replacement = substituted.asset;
            state.degraded = true;
            return Result<bool>::Success(true);
        }

        /** @brief Resolves exact mounted chunk selection or an actual frozen module registration, never authored availability. */
        Result<bool> ResolveRequirement(State &state, const SaveContentRequirement &required, SaveContentDiagnostic &diagnostic) {
            if (const auto *asset = std::get_if<SaveAssetContentRequirement>(&required.content))
                return ResolveAssetRequirement(state, *asset, diagnostic);
            if (const auto *chunk = std::get_if<SaveChunkContentRequirement>(&required.content)) {
                const auto mounted = state.installed->provider->MountedChunks();
                return Result<bool>::Success(std::ranges::any_of(mounted, [&](const auto &definition) {
                    return definition.id == chunk->chunk && definition.kind == chunk->kind;
                }));
            }
            const auto &module = std::get<SaveModuleContentRequirement>(required.content);
            return Result<bool>::Success(std::ranges::any_of(state.installed->modules, [&](const auto &receipt) {
                return receipt.CanAdmit() && receipt.Descriptor().participant.participant == required.owner &&
                       receipt.Descriptor().moduleId == module.module && receipt.Descriptor().moduleVersion == module.version;
            }));
        }

        /** @brief Adds exact declared identity and a safe remedy at the content admission boundary. */
        Error MissingContentError(const SaveContentRequirement &requirement) {
            std::string identity;
            if (const auto *asset = std::get_if<SaveAssetContentRequirement>(&requirement.content))
                identity = "asset=" + asset->asset.ToString() + "; type=" + asset->type.Value();
            else if (const auto *chunk = std::get_if<SaveChunkContentRequirement>(&requirement.content))
                identity = "chunk=" + chunk->chunk.Value() + "; kind=" + std::to_string(static_cast<unsigned>(chunk->kind));
            else {
                const auto &module = std::get<SaveModuleContentRequirement>(requirement.content);
                identity = "module=" + module.module.Value() + "; version=" + std::to_string(module.version);
            }
            auto error = MakeError(SceneErrors::SaveBootstrapAssetUnavailable);
            error.diagnostics.push_back(
                {DiagnosticCode{"scene.save_content.install_compatible"},
                 DiagnosticSeverity::Error,
                 "owner=" + requirement.owner.Value() + "; " + identity +
                     "; install compatible declared content, or explicitly authorize preservation only for an optional owner",
                 {},
                 "contentRequirements/" + requirement.owner.Value()});
            return error;
        }

        /** @brief Requires a declared optional archive owner and an explicit non-dropping project disposition. */
        Result<void> RecordAbsence(State &state, SaveContentDiagnostic &diagnostic) {
            const auto &required = diagnostic.requirement;
            const auto &participants = state.sourceReader.Manifest().participants;
            const auto owner = std::ranges::find(participants, required.owner, &SaveManifestParticipant::participant);
            const auto policy = std::ranges::find(state.policy.optionalOwners, required.owner, &SaveOptionalContentPolicy::owner);
            if (required.necessity == SaveContentNecessity::Required || owner == participants.end() || owner->required ||
                policy == state.policy.optionalOwners.end() || policy->absence == SaveOptionalContentAbsence::Reject)
                return Result<void>::Failure(MissingContentError(required));
            if (policy->absence != SaveOptionalContentAbsence::Preserve && policy->absence != SaveOptionalContentAbsence::Quarantine)
                return Result<void>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
            diagnostic.remedy = SaveContentRemedy::PreserveOpaqueData;
            diagnostic.disposition = policy->absence == SaveOptionalContentAbsence::Preserve ? SaveContentDisposition::Preservable
                                                                                             : SaveContentDisposition::Quarantined;
            if (std::ranges::find(state.quarantinedOwners, required.owner) == state.quarantinedOwners.end())
                state.quarantinedOwners.push_back(required.owner);
            state.degraded = true;
            return Result<void>::Success();
        }

        /** @brief Authenticates complete schema2 state before installed-content work or host decoding; legacy semantics need explicit
         * trust. */
        Result<void> ValidateCanonicalSource(State &state) {
            const auto schema = state.sourceReader.Manifest().saveSchemaVersion.Value();
            if (schema == 1) {
                return state.policy.legacy == SaveLegacyContentPolicy::ValidateBaselineOnly
                           ? Result<void>::Success()
                           : Result<void>::Failure(MakeError(SaveErrors::MigrationSourceUnsupported));
            }
            auto layout = ValidateSaveSceneCanonicalArchive(state.sourceReader);
            if (layout.HasError())
                return Result<void>::Failure(layout.ErrorValue());
            state.canonicalLayout = std::move(layout).Value();
            return Result<void>::Success();
        }

        /** @brief Reads the reserved required record before any project callback and rejects unsupported declaration shapes. */
        Result<void> ReadRequirements(State &state) {
            const auto builtin = SaveContentRequirementsParticipant();
            const auto &participants = state.sourceReader.Manifest().participants;
            const auto owner = std::ranges::find(participants, builtin, &SaveManifestParticipant::participant);
            if (owner == participants.end()) {
                if (state.policy.legacy != SaveLegacyContentPolicy::ValidateBaselineOnly)
                    return Result<void>::Failure(MakeError(SaveErrors::MigrationSourceUnsupported));
                state.legacy = true;
                return Result<void>::Success();
            }
            if (!owner->required || owner->schemaVersion.Value() != 1 || owner->chunks.size() != 1 ||
                owner->chunks.front() != SaveContentRequirementsRecord())
                return Result<void>::Failure(MakeError(SaveErrors::MigrationSourceUnsupported));
            auto bytes = state.sourceReader.SelectChunk(SaveContentRequirementsRecord());
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            if (!bytes.Value())
                return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
            auto requirements = DecodeSaveContentRequirements(*bytes.Value());
            if (requirements.HasError())
                return Result<void>::Failure(requirements.ErrorValue());
            state.requirements = std::move(requirements).Value();
            return Result<void>::Success();
        }

        /** @brief Validates finite trusted decisions; duplicate identities cannot choose policy by traversal order. */
        Result<void> ValidateProjectPolicy(const SaveContentProjectPolicy &policy) {
            if (policy.maximumContentReadBytes == 0 || policy.maximumPreservedBytes == 0 ||
                policy.optionalOwners.size() > MaximumSaveParticipantCount ||
                policy.substitutions.size() > MaximumSaveContentRequirements ||
                (policy.legacy != SaveLegacyContentPolicy::Reject && policy.legacy != SaveLegacyContentPolicy::ValidateBaselineOnly))
                return Result<void>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
            for (std::size_t index = 0; index < policy.optionalOwners.size(); ++index) {
                const auto &entry = policy.optionalOwners[index];
                if (!entry.owner.IsValid() || (index != 0 && !(policy.optionalOwners[index - 1].owner < entry.owner)) ||
                    (entry.absence != SaveOptionalContentAbsence::Reject && entry.absence != SaveOptionalContentAbsence::Preserve &&
                     entry.absence != SaveOptionalContentAbsence::Quarantine))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
            }
            for (std::size_t index = 0; index < policy.substitutions.size(); ++index) {
                const auto &entry = policy.substitutions[index];
                if (!entry.original.IsValid() || !entry.replacement.IsValid() || entry.original == entry.replacement ||
                    entry.type.Value().empty() || (index != 0 && !(policy.substitutions[index - 1].original < entry.original)) ||
                    !std::ranges::any_of(entry.replacementEnvelopeDigest.bytes, [](std::uint8_t byte) {
                    return byte != 0;
                }))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Resolves every declaration before any callback; absence policy cannot weaken required owner semantics. */
        Result<void> ResolveDeclarations(State &state) {
            const auto &participants = state.sourceReader.Manifest().participants;
            for (const auto &module : state.installed->modules) {
                const auto &descriptor = module.Descriptor().participant;
                if (descriptor.scope == SaveParticipantScope::PersistentWorld &&
                    std::ranges::find(participants, descriptor.participant, &SaveManifestParticipant::participant) != participants.end())
                    return Result<void>::Failure(MakeError(SceneErrors::SaveBootstrapDatasetUnsupported));
            }
            const auto baseline = Assets::AssetId::FromBytes(state.sourceReader.Header().baseScene.Bytes());
            bool baselineRequired = state.legacy;
            state.diagnostics.reserve(state.requirements.size());
            for (const auto &required : state.requirements) {
                if (auto admission = CheckAdmission(state.installedOwner, state.installed, state.cancellation); admission.HasError())
                    return admission;
                if (std::ranges::find(participants, required.owner, &SaveManifestParticipant::participant) == participants.end())
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                if (const auto *asset = std::get_if<SaveAssetContentRequirement>(&required.content))
                    baselineRequired =
                        baselineRequired || (asset->asset == baseline && required.necessity == SaveContentNecessity::Required);
                SaveContentDiagnostic diagnostic{required,
                                                 required.necessity == SaveContentNecessity::Required ? SaveContentDisposition::Required
                                                                                                      : SaveContentDisposition::Optional,
                                                 {}};
                auto resolved = ResolveRequirement(state, required, diagnostic);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                if (!resolved.Value())
                    if (auto absence = RecordAbsence(state, diagnostic); absence.HasError())
                        return absence;
                state.diagnostics.push_back(std::move(diagnostic));
            }
            if (!baselineRequired)
                return Result<void>::Failure(MakeError(SceneErrors::SaveBootstrapInvalid));
            std::ranges::sort(state.quarantinedOwners);
            return Result<void>::Success();
        }

        /** @brief Removes unavailable optional decoders while preserving their exact bytes and enforcing direct-read compatibility. */
        Result<void> PreserveOpaqueOwners(State &state) {
            state.preservationPolicy = state.policy.compatibility;
            auto &participants = state.preservationPolicy.participants;
            std::erase_if(participants, [&](const SaveParticipantCompatibility &entry) {
                if (std::ranges::binary_search(state.quarantinedOwners, entry.participant))
                    return true;
                const auto directory = state.sourceReader.Directory().Entries();
                return std::ranges::any_of(state.canonicalLayout, [&](const SaveSceneCanonicalLayoutEntry &tag) {
                    if (tag.representation != SaveSceneCanonicalRepresentation::Opaque)
                        return false;
                    const auto source = std::ranges::lower_bound(directory, tag.record, {}, &SaveChunkDirectoryEntry::record);
                    return source != directory.end() && source->owner == entry.participant;
                });
            });
            if (!state.legacy) {
                const auto builtin = SaveContentRequirementsParticipant();
                std::erase_if(participants, [&](const SaveParticipantCompatibility &entry) {
                    return entry.participant == builtin;
                });
                const auto version = ParticipantSchemaVersion::Create(1).Value();
                participants.push_back({builtin, {{version, version}, {}}, true, {}});
                if (!state.canonicalLayout.empty()) {
                    const auto layoutOwner = SaveSceneCanonicalLayoutParticipant();
                    std::erase_if(participants, [&](const SaveParticipantCompatibility &entry) {
                        return entry.participant == layoutOwner;
                    });
                    participants.push_back({layoutOwner, {{version, version}, {}}, true, {}});
                }
                std::ranges::sort(participants, {}, &SaveParticipantCompatibility::participant);
            }
            // Reconciliation retains every optional unknown owner; this path never spends drop permission.
            state.preservationPolicy.droppableUnknownParticipants.clear();
            const auto decision = EvaluateSaveCompatibility(state.sourceReader.Preamble().archiveFormatVersion, state.sourceReader.Header(),
                                                            state.sourceReader.Manifest(), state.preservationPolicy);
            if (decision.disposition != SaveCompatibilityDisposition::DirectRead)
                return Result<void>::Failure(MakeError(SaveErrors::MigrationSourceUnsupported));
            auto opaque = [&]() {
                if (state.canonicalLayout.empty())
                    return state.sourceReader.InspectUnknownData(state.preservationPolicy, state.policy.maximumPreservedBytes);
                auto proof = ValidatedSaveSceneCanonicalPreservation::Create(state.sourceReader);
                if (proof.HasError())
                    return Result<SaveUnknownDataReport>::Failure(proof.ErrorValue());
                return proof.Value().InspectUnknownData(state.preservationPolicy, state.policy.maximumPreservedBytes);
            }();
            if (opaque.HasError())
                return Result<void>::Failure(opaque.ErrorValue());
            state.opaque = std::move(opaque).Value();
            state.degraded = state.degraded || !state.opaque.preserved.empty();
            return Result<void>::Success();
        }
    }  // namespace

    namespace SaveContentDetail {
        Result<void> ResolveLegacyBaseline(ReconciliationState &state, const SaveAssetContentRequirement &required) {
            SaveContentDiagnostic diagnostic{{SaveContentRequirementsParticipant(), SaveContentNecessity::Required, required},
                                             SaveContentDisposition::Required,
                                             {}};
            auto resolved = ResolveAssetRequirement(state, required, diagnostic);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            if (!resolved.Value())
                return Result<void>::Failure(MakeError(SceneErrors::SaveBootstrapAssetUnavailable));
            state.diagnostics.push_back(std::move(diagnostic));
            return Result<void>::Success();
        }
    }  // namespace SaveContentDetail

    /** @copydoc ReconciledSaveContent::ReconciledSaveContent */
    ReconciledSaveContent::ReconciledSaveContent(std::shared_ptr<SaveContentDetail::ReconciliationState> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc ReconciledSaveContent::Prepare */
    Result<ReconciledSaveContent> ReconciledSaveContent::Prepare(InstalledSaveContent &installed, ImmutableSaveArchive archive,
                                                                 SaveContentProjectPolicy policy, CancellationToken cancellation) {
        if (!installed.state_ || installed.state_->ownerThread != std::this_thread::get_id())
            return Result<ReconciledSaveContent>::Failure(MakeError(SaveErrors::RestoreActivationStale));
        auto generation = installed.state_->current;
        if (auto admission = CheckAdmission(installed.state_, generation, cancellation); admission.HasError())
            return Result<ReconciledSaveContent>::Failure(admission.ErrorValue());
        if (auto valid = ValidateProjectPolicy(policy); valid.HasError())
            return Result<ReconciledSaveContent>::Failure(valid.ErrorValue());
        try {
            auto reader = SaveArchiveReader{}.Read(archive.bytes);
            if (reader.HasError())
                return Result<ReconciledSaveContent>::Failure(reader.ErrorValue());
            auto state = std::make_shared<State>(installed.state_, std::move(generation), std::move(archive), std::move(reader).Value(),
                                                 std::move(policy), std::move(cancellation));
            if (auto canonical = ValidateCanonicalSource(*state); canonical.HasError())
                return Result<ReconciledSaveContent>::Failure(canonical.ErrorValue());
            if (auto declaration = ReadRequirements(*state); declaration.HasError())
                return Result<ReconciledSaveContent>::Failure(declaration.ErrorValue());
            if (auto resolved = ResolveDeclarations(*state); resolved.HasError())
                return Result<ReconciledSaveContent>::Failure(resolved.ErrorValue());
            if (auto retained = PreserveOpaqueOwners(*state); retained.HasError())
                return Result<ReconciledSaveContent>::Failure(retained.ErrorValue());
            if (auto admission = CheckAdmission(state->installedOwner, state->installed, state->cancellation); admission.HasError())
                return Result<ReconciledSaveContent>::Failure(admission.ErrorValue());
            return Result<ReconciledSaveContent>::Success(ReconciledSaveContent{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Result<ReconciledSaveContent>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    /** @copydoc ReconciledSaveContent::ValidateAdmission */
    Result<void> ReconciledSaveContent::ValidateAdmission() const {
        if (!state_)
            return Result<void>::Failure(MakeError(SaveErrors::RestoreActivationStale));
        return CheckAdmission(state_->installedOwner, state_->installed, state_->cancellation);
    }

    /** @copydoc ReconciledSaveContent::Diagnostics */
    std::span<const SaveContentDiagnostic> ReconciledSaveContent::Diagnostics() const noexcept {
        return state_ ? std::span<const SaveContentDiagnostic>{state_->diagnostics} : std::span<const SaveContentDiagnostic>{};
    }

    /** @copydoc ReconciledSaveContent::IsDegraded */
    bool ReconciledSaveContent::IsDegraded() const noexcept {
        return state_ && state_->degraded;
    }
}  // namespace Horo::Runtime
