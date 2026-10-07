#include "Horo/Runtime/Save/SaveArchiveContainerWriter.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "RuntimeSceneErrors.h"
#include "SaveContentInternal.h"
#include "SaveContentRequirementsInternal.h"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>

namespace Horo::Runtime {
    namespace {
        /** @brief Reacquires actual Scene ownership and checks the same private source/install authority on the owner thread. */
        Result<RuntimeSceneView> CurrentWorld(const std::shared_ptr<SaveContentDetail::WorldState> &state) {
            if (!state || !state->content || state->content->installedOwner->ownerThread != std::this_thread::get_id())
                return Result<RuntimeSceneView>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            const auto &content = *state->content;
            if (content.cancellation.IsCancellationRequested())
                return Result<RuntimeSceneView>::Failure(MakeError(SaveErrors::OperationCancelled));
            if (content.installedOwner->closed || content.installedOwner->current != content.installed)
                return Result<RuntimeSceneView>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            for (const auto &installation : content.installed->modules)
                if (!installation.CanAdmit())
                    return Result<RuntimeSceneView>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            const auto view = state->service->ActiveScene();
            if (!view || view->RuntimeId() != state->boundScene || !view->IsCurrent())
                return Result<RuntimeSceneView>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            return Result<RuntimeSceneView>::Success(*view);
        }

        /** @brief Retains logical asset declarations and derives only actual installed native capture requirements. */
        Result<CanonicalEncodedValue> CapturedRequirements(const SaveContentDetail::WorldState &world,
                                                           const SaveParticipantRegistrySnapshot &participants) {
            try {
                auto requirements = world.content->requirements;
                if (requirements.empty() && world.content->legacy) {
                    requirements.emplace_back(SaveContentRequirementsParticipant(), SaveContentNecessity::Required,
                                              SaveAssetContentRequirement{Assets::AssetId::FromBytes(world.descriptor.baseScene.Bytes()),
                                                                          world.descriptor.expectedAssetType,
                                                                          world.descriptor.contentDigest});
                }
                // Logical asset identities and expected source digests remain stable. The typed project policy
                // resolves admitted physical substitutions again on load; capture is not an asset-ID migration.
                for (const auto &installation : world.content->installed->modules) {
                    const auto &native = installation.Descriptor();
                    const auto *binding = participants.Find(native.participant.participant);
                    if (!binding || !HasSaveParticipantRole(binding->Descriptor().roles, SaveParticipantRole::Capture))
                        continue;
                    if (!installation.CanAdmit() || binding->Adapter().get() != installation.AcquireAdapter().get() ||
                        binding->Descriptor() != native.participant)
                        return Result<CanonicalEncodedValue>::Failure(MakeError(SaveErrors::RestoreActivationStale));
                    const SaveModuleContentRequirement declaration{native.moduleId, native.moduleVersion};
                    const auto existing = std::ranges::find_if(requirements, [&](const SaveContentRequirement &entry) {
                        return entry.owner == native.participant.participant &&
                               std::holds_alternative<SaveModuleContentRequirement>(entry.content);
                    });
                    if (existing != requirements.end()) {
                        if (std::get<SaveModuleContentRequirement>(existing->content) != declaration)
                            return Result<CanonicalEncodedValue>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                        existing->necessity = native.participant.required ? SaveContentNecessity::Required : SaveContentNecessity::Optional;
                    } else {
                        requirements.emplace_back(native.participant.participant,
                                                  native.participant.required ? SaveContentNecessity::Required
                                                                              : SaveContentNecessity::Optional,
                                                  declaration);
                    }
                }
                if (auto sorted = SaveContentDetail::SortRequirements(requirements); sorted.HasError())
                    return Result<CanonicalEncodedValue>::Failure(sorted.ErrorValue());
                return EncodeSaveContentRequirements(requirements);
            } catch (const std::bad_alloc &) {
                return Result<CanonicalEncodedValue>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
            }
        }

        /** @brief Required owner-bound declaration; generic unadmitted capture cannot bypass degraded-world project policy. */
        class RequirementsAdapter final : public ICanonicalStateAdapter {
        public:
            explicit RequirementsAdapter(std::shared_ptr<SaveContentDetail::WorldState> world) : world_(std::move(world)) {}

            Result<CanonicalCaptureDisposition> Capture(const CanonicalCaptureContext &context,
                                                        ICanonicalCaptureSink &sink) const override {
                if (!world_->captureAdmission || !world_->captureRequirements || context.provenance != *world_->captureAdmission ||
                    context.participant != SaveContentRequirementsParticipant() || context.schemaVersion.Value() != 1)
                    return Result<CanonicalCaptureDisposition>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
                if (auto current = CurrentWorld(world_); current.HasError())
                    return Result<CanonicalCaptureDisposition>::Failure(current.ErrorValue());
                if (auto written = sink.WriteCopied(SaveContentRequirementsRecord(), world_->captureRequirements->Bytes());
                    written.HasError())
                    return Result<CanonicalCaptureDisposition>::Failure(written.ErrorValue());
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
            }

        private:
            std::shared_ptr<SaveContentDetail::WorldState> world_;
        };

        /** @brief Allocates the immutable accepted-source seal only after exact published receipt and dataset admission. */
        Result<std::shared_ptr<const SaveContentDetail::AcceptedCaptureSeal>> PrepareCaptureSeal(
            const SaveContentDetail::WorldState &world, const RuntimeSaveCaptureProvenance &provenance) {
            using Seal = std::shared_ptr<const SaveContentDetail::AcceptedCaptureSeal>;
            auto publication = world.publication->Snapshot();
            if (publication.HasError())
                return Result<Seal>::Failure(publication.ErrorValue());
            if (publication.Value().status != ScenePublicationStatus::Published || publication.Value().scene != world.boundScene ||
                publication.Value().datasets != SceneCanonicalDatasetProjection::Absent)
                return Result<Seal>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            return Result<Seal>::Success(std::make_shared<const SaveContentDetail::AcceptedCaptureSeal>(world.content, provenance,
                                                                                                        world.descriptor.world,
                                                                                                        world.descriptor.baseScene));
        }

        /** @brief Closes owner callback admission on every return, including foreign callback failures handled by the barrier. */
        struct CaptureAdmission final {
            explicit CaptureAdmission(SaveContentDetail::WorldState &owner) noexcept : world(owner) {}

            CaptureAdmission(const CaptureAdmission &) = delete;
            CaptureAdmission &operator=(const CaptureAdmission &) = delete;

            SaveContentDetail::WorldState &world;

            ~CaptureAdmission() {
                world.captureAdmission.reset();
                world.captureRequirements.reset();
            }
        };

        /** @brief Rejects retention/capture ownership overlap and binds the actual registered declaration adapter. */
        Result<void> ValidateCaptureRegistry(const SaveContentDetail::WorldState &world,
                                             const SaveParticipantRegistrySnapshot &participants) {
            for (const auto &binding : participants.Bindings())
                if (binding.Descriptor().participant == SaveSceneCanonicalLayoutParticipant())
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                else if (binding.Descriptor().scope == SaveParticipantScope::PersistentWorld)
                    return Result<void>::Failure(MakeError(SceneErrors::SaveBootstrapDatasetUnsupported));
            const auto declaration = world.declarationAdapter.lock();
            if (const auto *binding = participants.Find(SaveContentRequirementsParticipant());
                !declaration || !binding || binding->Adapter().get() != declaration.get())
                return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            for (const auto &entry : participants.CaptureBindings()) {
                const auto owner = entry.Descriptor().participant;
                if (std::ranges::binary_search(world.content->quarantinedOwners, owner) ||
                    std::ranges::any_of(world.content->opaque.preserved, [&](const PreservedSaveChunk &chunk) {
                    return chunk.entry.owner == owner;
                }))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            }
            const auto admittedOwner = [&](const SaveParticipantId &owner) {
                if (std::ranges::any_of(world.content->opaque.preserved, [&](const PreservedSaveChunk &chunk) {
                    return chunk.entry.owner == owner;
                }))
                    return true;
                const auto *candidate = participants.Find(owner);
                return candidate && HasSaveParticipantRole(candidate->Descriptor().roles, SaveParticipantRole::Capture);
            };
            for (const auto &requirement : world.content->requirements)
                if (!admittedOwner(requirement.owner))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
            for (const auto &owner : world.content->sourceReader.Manifest().participants)
                if (owner.required && owner.participant != SaveSceneCanonicalLayoutParticipant() && !admittedOwner(owner.participant))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
            return Result<void>::Success();
        }

        /** @brief Prevents a skipped declaration owner from producing a manifest that cannot be reconciled again. */
        Result<void> ValidateCapturedOwners(const SaveContentDetail::WorldState &world, const RuntimeSaveSnapshot &capture) {
            const auto retainedOrCaptured = [&](const SaveParticipantId &owner) {
                if (std::ranges::any_of(world.content->opaque.preserved, [&](const PreservedSaveChunk &chunk) {
                    return chunk.entry.owner == owner;
                }))
                    return true;
                return std::ranges::any_of(capture.Participants(), [&](const auto &participant) {
                    return participant.participant == owner && participant.disposition == CanonicalCaptureDisposition::Captured;
                });
            };
            for (const auto &requirement : world.content->requirements)
                if (!retainedOrCaptured(requirement.owner))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
            for (const auto &owner : world.content->sourceReader.Manifest().participants)
                if (owner.required && owner.participant != SaveSceneCanonicalLayoutParticipant() && !retainedOrCaptured(owner.participant))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
            for (const auto &installation : world.content->installed->modules) {
                const auto owner = installation.Descriptor().participant.participant;
                if (std::ranges::any_of(capture.Participants(), [&](const auto &participant) {
                    return participant.participant == owner && participant.disposition != CanonicalCaptureDisposition::Captured;
                }))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc SaveContentSnapshot::SaveContentSnapshot */
    /** @copydoc SaveContentWorld::Diagnostics */
    std::span<const SaveContentDiagnostic> SaveContentWorld::Diagnostics() const noexcept {
        return state_ ? std::span<const SaveContentDiagnostic>{state_->content->diagnostics} : std::span<const SaveContentDiagnostic>{};
    }

    /** @copydoc SaveContentWorld::RegisterRequirements */
    Result<SaveParticipantRegistration> SaveContentWorld::RegisterRequirements(CanonicalStateParticipantRegistry &registry) const {
        if (auto current = CurrentWorld(state_); current.HasError())
            return Result<SaveParticipantRegistration>::Failure(current.ErrorValue());
        if (!state_->declarationAdapter.expired())
            return Result<SaveParticipantRegistration>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
        try {
            auto adapter = std::make_shared<RequirementsAdapter>(state_);
            const CanonicalStateParticipantDescriptor descriptor{.participant = SaveContentRequirementsParticipant(),
                                                                 .schemaVersion = ParticipantSchemaVersion::Create(1).Value(),
                                                                 .scope = SaveParticipantScope::RuntimeScene,
                                                                 .roles = SaveParticipantRole::Capture,
                                                                 .required = true,
                                                                 .limits = {1U << 20U, 1, 4},
                                                                 .dependencies = {},
                                                                 .ownedRecords = {SaveContentRequirementsRecord()}};
            auto registered = registry.Register(descriptor, adapter);
            if (registered.HasValue())
                state_->declarationAdapter = adapter;
            return registered;
        } catch (const std::bad_alloc &) {
            return Result<SaveParticipantRegistration>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    /** @copydoc SaveContentWorld::CaptureAtSafePoint */
    Result<SaveContentCaptureOutcome> SaveContentWorld::CaptureAtSafePoint(SaveCaptureBarrier &barrier,
                                                                           const SaveContentCaptureRequest &request,
                                                                           SaveParticipantRegistrySnapshot participants,
                                                                           const SaveDegradedWorldPolicy policy) const {
        const auto &generation = request.generation;
        static_assert(std::is_nothrow_move_constructible_v<Result<SaveContentCaptureOutcome>>);
        // Error storage is prepared before admission mutates any world/barrier state. Unwinding only moves owned storage.
        auto allocationFailure = Result<SaveContentCaptureOutcome>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        try {
            auto view = CurrentWorld(state_);
            if (view.HasError())
                return Result<SaveContentCaptureOutcome>::Failure(view.ErrorValue());
            if (state_->captureAdmission ||
                (policy != SaveDegradedWorldPolicy::Reject && policy != SaveDegradedWorldPolicy::PreserveOpaque) ||
                (state_->content->degraded && policy != SaveDegradedWorldPolicy::PreserveOpaque))
                return Result<SaveContentCaptureOutcome>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
            if (generation.scene != view.Value().RuntimeId().value || generation.registry != participants.Generation())
                return Result<SaveContentCaptureOutcome>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            if (auto registry = ValidateCaptureRegistry(*state_, participants); registry.HasError())
                return Result<SaveContentCaptureOutcome>::Failure(registry.ErrorValue());
            const RuntimeSaveCaptureProvenance provenance{request.capturedState, request.epoch, generation.scene,
                                                          view.Value().StructuralRevision(), generation.registry};
            auto requirements = CapturedRequirements(*state_, participants);
            if (requirements.HasError())
                return Result<SaveContentCaptureOutcome>::Failure(requirements.ErrorValue());
            // Allocate the immutable admission seal before invoking any capture callback or consuming barrier readiness.
            auto seal = PrepareCaptureSeal(*state_, provenance);
            if (seal.HasError())
                return Result<SaveContentCaptureOutcome>::Failure(seal.ErrorValue());
            state_->captureRequirements = std::move(requirements).Value();
            state_->captureAdmission = provenance;
            const CaptureAdmission admission{*state_};
            auto captured = barrier.CaptureAtSafePoint(request.phase, generation, provenance, std::move(participants), request.limits);
            if (captured.HasError())
                return Result<SaveContentCaptureOutcome>::Failure(captured.ErrorValue());
            auto outcome = std::move(captured).Value();
            SaveContentCaptureOutcome result{outcome.barrier, {}, std::move(outcome.error)};
            if (outcome.capture) {
                if (auto owners = ValidateCapturedOwners(*state_, *outcome.capture); owners.HasError())
                    result.error = owners.ErrorValue();
                else
                    result.capture = SaveContentSnapshot{std::move(*outcome.capture), std::move(seal).Value()};
            }
            return Result<SaveContentCaptureOutcome>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return allocationFailure;
        }
    }
}  // namespace Horo::Runtime
