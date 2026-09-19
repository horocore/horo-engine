#include "Horo/Runtime/Save/SaveParticipantRegistry.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Horo::Runtime {
    struct SaveParticipantRegistryDetail::SnapshotStorage final {
        std::vector<SaveParticipantBinding> bindings;
        std::vector<SaveParticipantBinding> captureBindings;
        std::vector<SaveParticipantBinding> restoreBindings;
    };

    namespace {
        using ParticipantIndices = std::unordered_map<SaveParticipantId, std::size_t, SaveParticipantIdHash>;

#ifdef _WIN32
        void DebugRegisterStage(const char *stage) noexcept {
            DWORD written{};
            ::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), stage, static_cast<DWORD>(std::strlen(stage)), &written, nullptr);
        }
#endif

        struct PhasePlanGraph final {
            std::vector<std::vector<std::size_t>> dependents;
            std::vector<std::vector<std::size_t>> dependencies;
            std::vector<std::size_t> dependencyCounts;
            std::size_t participantCount{0};
        };

        /** @brief Projects every ordered registry operation to the canonical participant text. */
        [[nodiscard]] const SaveParticipantId &ParticipantOrderKey(const SaveParticipantBinding &binding) noexcept {
            return binding.Descriptor().participant;
        }

        /** @brief Encodes phase coverage so only overlapping declarations are duplicates. */
        [[nodiscard]] std::byte DependencyPhaseMask(const SaveParticipantDependencyPhase phase) noexcept {
            using enum SaveParticipantDependencyPhase;
            if (phase == CaptureAndRestore)
                return std::byte{0b11};
            return phase == Capture ? std::byte{0b01} : std::byte{0b10};
        }

        /** @brief Reports whether the descriptor declares one supported semantic scope. */
        [[nodiscard]] bool IsValidScope(const SaveParticipantScope scope) noexcept {
            using enum SaveParticipantScope;
            switch (scope) {
                case RuntimeScene:
                case SlotPlayer:
                case PersistentWorld:
                    return true;
            }
            return false;
        }

        /** @brief Reports whether role flags are non-empty and contain only supported bits. */
        [[nodiscard]] bool HasValidRoles(const SaveParticipantRole roles) noexcept {
            using enum SaveParticipantRole;
            constexpr auto kSupportedRoles = std::byte{static_cast<std::uint8_t>(Capture)} | std::byte{static_cast<std::uint8_t>(Restore)};
            const auto encodedRoles = std::byte{static_cast<std::uint8_t>(roles)};
            return encodedRoles != std::byte{} && (encodedRoles & ~kSupportedRoles) == std::byte{};
        }

        /** @brief Reports whether all local limits are finite and cover declared record ownership. */
        [[nodiscard]] bool HasValidLimits(const CanonicalStateParticipantDescriptor &descriptor) noexcept {
            return descriptor.limits.maximumPayloadBytes != 0 && descriptor.limits.maximumRecordCount != 0 &&
                   descriptor.limits.maximumNestingDepth != 0 && descriptor.ownedRecords.size() <= descriptor.limits.maximumRecordCount;
        }

        /** @brief Validates required scalar and owned-record descriptor fields. */
        [[nodiscard]] bool HasValidRequiredFields(const CanonicalStateParticipantDescriptor &descriptor) {
            return descriptor.participant.IsValid() && descriptor.schemaVersion.IsValid() && IsValidScope(descriptor.scope) &&
                   HasValidRoles(descriptor.roles) && HasValidLimits(descriptor) && !descriptor.ownedRecords.empty();
        }

        /** @brief Reports whether a dependency requirement is a supported closed value. */
        [[nodiscard]] bool IsKnown(const SaveParticipantDependencyRequirement requirement) noexcept {
            switch (requirement) {
                case SaveParticipantDependencyRequirement::Required:
                case SaveParticipantDependencyRequirement::Optional:
                    return true;
            }
            return false;
        }

        /** @brief Reports whether a dependency phase is a supported closed value. */
        [[nodiscard]] bool IsKnown(const SaveParticipantDependencyPhase phase) noexcept {
            using enum SaveParticipantDependencyPhase;
            switch (phase) {
                case Capture:
                case Restore:
                case CaptureAndRestore:
                    return true;
            }
            return false;
        }

        /** @brief Reports whether an edge applies to one operation role. */
        [[nodiscard]] bool AppliesTo(const SaveParticipantDependencyPhase phase, const SaveParticipantRole role) noexcept {
            using enum SaveParticipantRole;
            if (phase == SaveParticipantDependencyPhase::CaptureAndRestore)
                return true;
            if (role == Capture)
                return phase == SaveParticipantDependencyPhase::Capture;
            return phase == SaveParticipantDependencyPhase::Restore;
        }

        /** @brief Formats one dependency phase for actionable diagnostics. */
        [[nodiscard]] std::string_view PhaseName(const SaveParticipantRole role) noexcept {
            using enum SaveParticipantRole;
            return role == Capture ? "capture" : "restore";
        }

        /** @brief Validates dependency identity, self-reference, and uniqueness rules. */
        [[nodiscard]] Result<void> ValidateDependencyMetadata(const CanonicalStateParticipantDescriptor &descriptor) {
            using enum SaveParticipantRole;
            if (descriptor.dependencies.size() > MaximumSaveParticipantCount)
                return Result<void>::Failure(MakeError(SaveErrors::ParticipantDescriptorInvalid));
            std::unordered_map<SaveParticipantId, std::byte, SaveParticipantIdHash> coveredPhases;
            coveredPhases.reserve(descriptor.dependencies.size());
            for (const SaveParticipantDependency &dependency : descriptor.dependencies) {
                if (!dependency.participant.IsValid() || dependency.participant == descriptor.participant ||
                    !IsKnown(dependency.requirement) || !IsKnown(dependency.phase))
                    return Result<void>::Failure(MakeError(SaveErrors::ParticipantDescriptorInvalid));
                const std::byte phaseMask = DependencyPhaseMask(dependency.phase);
                std::byte &existingMask = coveredPhases[dependency.participant];
                if ((existingMask & phaseMask) != std::byte{})
                    return Result<void>::Failure(MakeError(SaveErrors::ParticipantDescriptorInvalid));
                existingMask |= phaseMask;
                const bool captureCompatible = !AppliesTo(dependency.phase, Capture) || HasSaveParticipantRole(descriptor.roles, Capture);
                const bool restoreCompatible = !AppliesTo(dependency.phase, Restore) || HasSaveParticipantRole(descriptor.roles, Restore);
                if (!captureCompatible || !restoreCompatible) {
                    return Result<void>::Failure(MakeError(SaveErrors::ParticipantDependencyPhaseIncompatible,
                                                           "Participant '" + descriptor.participant.Value() + "' declares dependency '" +
                                                               dependency.participant.Value() + "' for a phase it does not support."));
                }
            }
            return Result<void>::Success();
        }

        /** @brief Validates one inert descriptor before registry mutation. */
        [[nodiscard]] Result<void> ValidateDescriptor(const CanonicalStateParticipantDescriptor &descriptor) {
            if (!HasValidRequiredFields(descriptor))
                return Result<void>::Failure(MakeError(SaveErrors::ParticipantDescriptorInvalid));
            if (const Result<void> dependencies = ValidateDependencyMetadata(descriptor); dependencies.HasError())
                return dependencies;
            if (const Result<void> records = ValidateUniqueSaveIdentities<SaveRecordIdentityTag>(descriptor.ownedRecords);
                records.HasError())
                return Result<void>::Failure(MakeError(SaveErrors::ParticipantDescriptorInvalid));
            return Result<void>::Success();
        }

        /** @brief Builds the immutable participant-to-binding index used for graph validation. */
        [[nodiscard]] ParticipantIndices BuildParticipantIndices(const std::vector<SaveParticipantBinding> &bindings) {
            ParticipantIndices indices;
            indices.reserve(bindings.size());
            for (std::size_t index = 0; index < bindings.size(); ++index)
                indices.try_emplace(bindings[index].Descriptor().participant, index);
            return indices;
        }

        /** @brief Adds one applicable validated dependency edge to a phase graph. */
        [[nodiscard]] Result<void> AddPhaseDependency(PhasePlanGraph &graph, const std::vector<SaveParticipantBinding> &bindings,
                                                      const ParticipantIndices &indices, const SaveParticipantRole role,
                                                      const std::size_t dependentIndex,
                                                      const CanonicalStateParticipantDescriptor &descriptor,
                                                      const SaveParticipantDependency &dependency) {
            if (!AppliesTo(dependency.phase, role))
                return Result<void>::Success();
            const auto found = indices.find(dependency.participant);
            if (found == indices.end()) {
                if (dependency.requirement == SaveParticipantDependencyRequirement::Optional)
                    return Result<void>::Success();
                return Result<void>::Failure(
                    MakeError(SaveErrors::ParticipantDependencyMissing, "Participant '" + descriptor.participant.Value() +
                                                                            "' requires missing " + std::string{PhaseName(role)} +
                                                                            " dependency '" + dependency.participant.Value() + "'."));
            }
            if (const CanonicalStateParticipantDescriptor &provider = bindings[found->second].Descriptor();
                !HasSaveParticipantRole(provider.roles, role)) {
                return Result<void>::Failure(MakeError(SaveErrors::ParticipantDependencyPhaseIncompatible,
                                                       "Participant '" + descriptor.participant.Value() + "' depends on '" +
                                                           provider.participant.Value() + "' during " + std::string{PhaseName(role)} +
                                                           ", but the dependency does not support that phase."));
            }
            graph.dependents[found->second].push_back(dependentIndex);
            graph.dependencies[dependentIndex].push_back(found->second);
            ++graph.dependencyCounts[dependentIndex];
            return Result<void>::Success();
        }

        /** @brief Builds and validates the dependency graph for one operation phase. */
        [[nodiscard]] Result<PhasePlanGraph> PreparePhaseGraph(const std::vector<SaveParticipantBinding> &bindings,
                                                               const ParticipantIndices &indices, const SaveParticipantRole role) {
            PhasePlanGraph graph{std::vector<std::vector<std::size_t>>(bindings.size()),
                                 std::vector<std::vector<std::size_t>>(bindings.size()), std::vector<std::size_t>(bindings.size()), 0};
            for (std::size_t index = 0; index < bindings.size(); ++index) {
                const CanonicalStateParticipantDescriptor &descriptor = bindings[index].Descriptor();
                if (!HasSaveParticipantRole(descriptor.roles, role))
                    continue;
                ++graph.participantCount;
                for (const SaveParticipantDependency &dependency : descriptor.dependencies) {
                    if (auto added = AddPhaseDependency(graph, bindings, indices, role, index, descriptor, dependency); added.HasError()) {
                        return Result<PhasePlanGraph>::Failure(added.ErrorValue());
                    }
                }
            }
            return Result<PhasePlanGraph>::Success(std::move(graph));
        }

        /** @brief Recovers one deterministic cycle from the graph nodes left by Kahn traversal. */
        [[nodiscard]] bool FindCycleFrom(const std::size_t index, const PhasePlanGraph &graph, const std::vector<bool> &emitted,
                                         std::vector<std::uint8_t> &colors, std::vector<std::size_t> &path,
                                         std::vector<std::size_t> &cycle) {
            colors[index] = 1;
            path.push_back(index);
            for (const std::size_t dependency : graph.dependencies[index]) {
                if (emitted[dependency])
                    continue;
                if (colors[dependency] == 1) {
                    cycle.assign(std::ranges::find(path, dependency), path.end());
                    return true;
                }
                if (colors[dependency] == 0 && FindCycleFrom(dependency, graph, emitted, colors, path, cycle))
                    return true;
            }
            path.pop_back();
            colors[index] = 2;
            return false;
        }

        /** @brief Creates an actionable diagnostic naming only one actual dependency cycle. */
        [[nodiscard]] Error MakeDependencyCycleError(const std::vector<SaveParticipantBinding> &bindings, const PhasePlanGraph &graph,
                                                     const std::vector<bool> &emitted, const SaveParticipantRole role) {
            std::vector<std::uint8_t> colors(bindings.size());
            std::vector<std::size_t> path;
            std::vector<std::size_t> cycle;
            std::size_t index{};
            for (const std::uint8_t color : colors) {
                if (!cycle.empty())
                    break;
                if (!emitted[index] && color == 0)
                    static_cast<void>(FindCycleFrom(index, graph, emitted, colors, path, cycle));
                ++index;
            }
            std::ranges::sort(cycle, {}, [&bindings](const std::size_t index) {
                return ParticipantOrderKey(bindings[index]);
            });
            std::string message = std::string{PhaseName(role)} + " participant dependency cycle involves";
            for (const std::size_t cycleIndex : cycle)
                message += " '" + bindings[cycleIndex].Descriptor().participant.Value() + "'";
            message += ".";
            return MakeError(SaveErrors::ParticipantDependencyCycle, std::move(message));
        }

        /** @brief Emits one stable topological phase plan from a validated graph. */
        [[nodiscard]] Result<std::vector<SaveParticipantBinding>> EmitStablePhasePlan(const std::vector<SaveParticipantBinding> &bindings,
                                                                                      PhasePlanGraph graph,
                                                                                      const SaveParticipantRole role) {
            std::vector<bool> emitted(bindings.size());
            std::vector<SaveParticipantBinding> plan;
            plan.reserve(graph.participantCount);
            while (plan.size() < graph.participantCount) {
                std::size_t selected = bindings.size();
                for (std::size_t index = 0; index < bindings.size(); ++index) {
                    if (!emitted[index] && graph.dependencyCounts[index] == 0 &&
                        HasSaveParticipantRole(bindings[index].Descriptor().roles, role)) {
                        selected = index;
                        break;
                    }
                }
                if (selected == bindings.size())
                    return Result<std::vector<SaveParticipantBinding>>::Failure(MakeDependencyCycleError(bindings, graph, emitted, role));
                emitted[selected] = true;
                plan.push_back(bindings[selected]);
                for (const std::size_t dependent : graph.dependents[selected])
                    --graph.dependencyCounts[dependent];
            }
            return Result<std::vector<SaveParticipantBinding>>::Success(std::move(plan));
        }

        /** @brief Builds one stable topological phase plan with actionable dependency diagnostics. */
        [[nodiscard]] Result<std::vector<SaveParticipantBinding>> BuildPhasePlan(const std::vector<SaveParticipantBinding> &bindings,
                                                                                 const ParticipantIndices &indices,
                                                                                 const SaveParticipantRole role) {
            auto graph = PreparePhaseGraph(bindings, indices, role);
            if (graph.HasError())
                return Result<std::vector<SaveParticipantBinding>>::Failure(graph.ErrorValue());
            return EmitStablePhasePlan(bindings, std::move(graph).Value(), role);
        }

        /** @brief Computes the next registry generation without mutating published state. */
        [[nodiscard]] Result<std::uint64_t> NextGeneration(const std::uint64_t generation) {
            if (generation == std::numeric_limits<std::uint64_t>::max())
                return Result<std::uint64_t>::Failure(MakeError(SaveErrors::ParticipantRegistryGenerationExhausted));
            return Result<std::uint64_t>::Success(generation + 1);
        }
    }  // namespace

    SaveParticipantBinding::SaveParticipantBinding(SaveParticipantBinding::RegistryConstructionToken,
                                                   CanonicalStateParticipantDescriptor descriptor,
                                                   std::shared_ptr<const ICanonicalStateAdapter> adapter)
        : descriptor_(std::move(descriptor)), adapter_(std::move(adapter)) {}

    /** @copydoc SaveParticipantBinding::Descriptor */
    const CanonicalStateParticipantDescriptor &SaveParticipantBinding::Descriptor() const noexcept {
        return descriptor_;
    }

    /** @copydoc SaveParticipantBinding::Adapter */
    const std::shared_ptr<const ICanonicalStateAdapter> &SaveParticipantBinding::Adapter() const noexcept {
        return adapter_;
    }

    SaveParticipantRegistrySnapshot::SaveParticipantRegistrySnapshot(
        const std::uint64_t generation, std::shared_ptr<const SaveParticipantRegistryDetail::SnapshotStorage> storage)
        : generation_(generation), storage_(std::move(storage)) {}

    /** @copydoc SaveParticipantRegistrySnapshot::IsValid */
    bool SaveParticipantRegistrySnapshot::IsValid() const noexcept {
        return generation_ != 0 && storage_ != nullptr;
    }

    /** @copydoc SaveParticipantRegistrySnapshot::Generation */
    std::uint64_t SaveParticipantRegistrySnapshot::Generation() const noexcept {
        return generation_;
    }

    /** @copydoc SaveParticipantRegistrySnapshot::Bindings */
    std::span<const SaveParticipantBinding> SaveParticipantRegistrySnapshot::Bindings() const noexcept {
        return storage_ == nullptr ? std::span<const SaveParticipantBinding>{}
                                   : std::span<const SaveParticipantBinding>{storage_->bindings};
    }

    /** @copydoc SaveParticipantRegistrySnapshot::CaptureBindings */
    std::span<const SaveParticipantBinding> SaveParticipantRegistrySnapshot::CaptureBindings() const noexcept {
        return storage_ == nullptr ? std::span<const SaveParticipantBinding>{}
                                   : std::span<const SaveParticipantBinding>{storage_->captureBindings};
    }

    /** @copydoc SaveParticipantRegistrySnapshot::RestoreBindings */
    std::span<const SaveParticipantBinding> SaveParticipantRegistrySnapshot::RestoreBindings() const noexcept {
        return storage_ == nullptr ? std::span<const SaveParticipantBinding>{}
                                   : std::span<const SaveParticipantBinding>{storage_->restoreBindings};
    }

    /** @copydoc SaveParticipantRegistrySnapshot::Find */
    const SaveParticipantBinding *SaveParticipantRegistrySnapshot::Find(const SaveParticipantId &participant) const noexcept {
        const auto bindings = Bindings();
        const auto found = std::ranges::lower_bound(bindings, participant, {}, ParticipantOrderKey);
        if (found == bindings.end() || found->Descriptor().participant != participant)
            return nullptr;
        return std::to_address(found);
    }

    CanonicalStateParticipantRegistry::~CanonicalStateParticipantRegistry() {
        Close();
    }

    /** @copydoc CanonicalStateParticipantRegistry::Register */
    Result<SaveParticipantRegistration> CanonicalStateParticipantRegistry::Register(const CanonicalStateParticipantDescriptor &descriptor,
                                                                                    std::shared_ptr<const ICanonicalStateAdapter> adapter) {
        try {
#ifdef _WIN32
            DebugRegisterStage("[register] start\n");
#endif
            if (closed_)
                return Result<SaveParticipantRegistration>::Failure(MakeError(SaveErrors::ParticipantRegistryClosed));
            auto nextGeneration = NextGeneration(generation_);
            if (nextGeneration.HasError())
                return Result<SaveParticipantRegistration>::Failure(nextGeneration.ErrorValue());
            if (adapter == nullptr)
                return Result<SaveParticipantRegistration>::Failure(MakeError(SaveErrors::ParticipantAdapterMissing));
            if (const Result<void> valid = ValidateDescriptor(descriptor); valid.HasError())
                return Result<SaveParticipantRegistration>::Failure(valid.ErrorValue());
            if (bindings_.size() >= MaximumSaveParticipantCount)
                return Result<SaveParticipantRegistration>::Failure(MakeError(SaveErrors::ParticipantRegistryCapacityExceeded));
            if (std::ranges::find(bindings_, descriptor.participant, [](const SaveParticipantBinding &binding) {
                return binding.Descriptor().participant;
            }) != bindings_.end())
                return Result<SaveParticipantRegistration>::Failure(MakeError(SaveErrors::ParticipantDuplicate));
#ifdef _WIN32
            DebugRegisterStage("[register] before copy\n");
#endif
            for (const SaveParticipantBinding &binding : bindings_) {
                for (const SaveRecordId &record : descriptor.ownedRecords) {
                    if (std::ranges::find(binding.Descriptor().ownedRecords, record) != binding.Descriptor().ownedRecords.end())
                        return Result<SaveParticipantRegistration>::Failure(MakeError(SaveErrors::ParticipantRecordOwnershipDuplicate));
                }
            }
            CanonicalStateParticipantDescriptor ownedDescriptor = descriptor;
#ifdef _WIN32
            DebugRegisterStage("[register] after copy\n");
#endif
            SaveParticipantRegistration registration{ownedDescriptor.participant, nextGeneration.Value()};
#ifdef _WIN32
            DebugRegisterStage("[register] after registration\n");
#endif
            std::ranges::sort(ownedDescriptor.dependencies);
#ifdef _WIN32
            DebugRegisterStage("[register] before emplace\n");
#endif
            bindings_.emplace_back(SaveParticipantBinding::RegistryConstructionToken{}, std::move(ownedDescriptor), std::move(adapter));
#ifdef _WIN32
            DebugRegisterStage("[register] after emplace\n");
#endif
            generation_ = nextGeneration.Value();
            return Result<SaveParticipantRegistration>::Success(std::move(registration));
        } catch (const std::bad_alloc &) {
#ifdef _WIN32
            DebugRegisterStage("[register] caught bad_alloc\n");
#endif
            return Result<SaveParticipantRegistration>::Failure(MakeError(SaveErrors::ParticipantRegistryAllocationFailed));
        }
    }

    /** @copydoc CanonicalStateParticipantRegistry::Unregister */
    Result<bool> CanonicalStateParticipantRegistry::Unregister(const SaveParticipantId &participant) {
        if (closed_)
            return Result<bool>::Failure(MakeError(SaveErrors::ParticipantRegistryClosed));
        const auto found = std::ranges::find(bindings_, participant, [](const SaveParticipantBinding &binding) {
            return binding.Descriptor().participant;
        });
        if (found == bindings_.end())
            return Result<bool>::Success(false);
        auto nextGeneration = NextGeneration(generation_);
        if (nextGeneration.HasError())
            return Result<bool>::Failure(nextGeneration.ErrorValue());
        bindings_.erase(found);
        generation_ = nextGeneration.Value();
        return Result<bool>::Success(true);
    }

    /** @copydoc CanonicalStateParticipantRegistry::Snapshot */
    Result<SaveParticipantRegistrySnapshot> CanonicalStateParticipantRegistry::Snapshot() const {
        try {
            if (closed_)
                return Result<SaveParticipantRegistrySnapshot>::Failure(MakeError(SaveErrors::ParticipantRegistryClosed));
            auto storage = std::make_shared<SaveParticipantRegistryDetail::SnapshotStorage>();
            storage->bindings = bindings_;
            std::ranges::sort(storage->bindings, {}, ParticipantOrderKey);
            const ParticipantIndices indices = BuildParticipantIndices(storage->bindings);
            auto capturePlan = BuildPhasePlan(storage->bindings, indices, SaveParticipantRole::Capture);
            if (capturePlan.HasError())
                return Result<SaveParticipantRegistrySnapshot>::Failure(capturePlan.ErrorValue());
            auto restorePlan = BuildPhasePlan(storage->bindings, indices, SaveParticipantRole::Restore);
            if (restorePlan.HasError())
                return Result<SaveParticipantRegistrySnapshot>::Failure(restorePlan.ErrorValue());
            storage->captureBindings = std::move(capturePlan).Value();
            storage->restoreBindings = std::move(restorePlan).Value();
            return Result<SaveParticipantRegistrySnapshot>::Success({generation_, std::move(storage)});
        } catch (const std::bad_alloc &) {
            return Result<SaveParticipantRegistrySnapshot>::Failure(MakeError(SaveErrors::ParticipantRegistryAllocationFailed));
        }
    }

    /** @copydoc CanonicalStateParticipantRegistry::Close */
    void CanonicalStateParticipantRegistry::Close() noexcept {
        if (closed_)
            return;
        closed_ = true;
        bindings_.clear();
    }

    /** @copydoc CanonicalStateParticipantRegistry::IsClosed */
    bool CanonicalStateParticipantRegistry::IsClosed() const noexcept {
        return closed_;
    }

    /** @copydoc CanonicalStateParticipantRegistry::Generation */
    std::uint64_t CanonicalStateParticipantRegistry::Generation() const noexcept {
        return generation_;
    }

}  // namespace Horo::Runtime
