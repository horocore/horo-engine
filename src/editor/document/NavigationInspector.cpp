#include "Horo/Editor/NavigationInspector.h"

#include "editor/document/SceneDocumentInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Keep transaction rejection identities intact at the presentation boundary. */
        Error Rejected(SceneTransactionFailure reason) {
            return MakeError(SceneTransactionError(reason));
        }

        /** @brief Select the canonical typed slot without a property-name parser. */
        template <class T, class Components> auto &Slot(Components &components) {
            if constexpr (std::is_same_v<T, Runtime::NavigationSurfaceComponent>)
                return components.navigationSurface;
            else if constexpr (std::is_same_v<T, Runtime::NavigationRegionComponent>)
                return components.navigationRegion;
            else if constexpr (std::is_same_v<T, Runtime::NavigationModifierComponent>)
                return components.navigationModifier;
            else if constexpr (std::is_same_v<T, Runtime::NavigationLinkComponent>)
                return components.navigationLink;
            else
                return components.navigationAgent;
        }

        /** @brief Preserve source identity/schema and advance only changed generation-bearing values. */
        template <class T> Result<void> Replace(SceneSource::SceneObjectComponentSet &components, const T &value) {
            using enum SceneTransactionFailure;
            auto &slot = Slot<T>(components);
            if (!slot)
                return Result<void>::Failure(Rejected(Unauthorized));
            if (slot->schemaVersion != value.schemaVersion)
                return Result<void>::Failure(Rejected(Stale));
            if constexpr (!std::is_same_v<T, Runtime::NavigationAgentComponent>) {
                if (slot->id != value.id || slot->generation != value.generation)
                    return Result<void>::Failure(Rejected(Stale));
                if (*slot != value && value.generation == UINT64_MAX)
                    return Result<void>::Failure(Rejected(Capacity));
                const bool changed = *slot != value;
                slot = value;
                if (changed)
                    ++slot->generation;
            } else
                slot = value;
            return Result<void>::Success();
        }

        /** @brief Resolve the definition owned by an exact authored surface. */
        const Navigation::NavigationDefinition *FindDefinition(std::span<const SceneSource::SceneObjectSnapshot> objects,
                                                               std::span<const NavigationDefinitionSnapshot> definitions,
                                                               Navigation::SurfaceId id) {
            const auto surface = std::ranges::find_if(objects, [id](const auto &object) {
                return object.components.navigationSurface && object.components.navigationSurface->id == id;
            });
            if (surface == objects.end())
                return nullptr;
            const auto entry =
                std::ranges::find(definitions, surface->components.navigationSurface->definition, &NavigationDefinitionSnapshot::asset);
            return entry == definitions.end() ? nullptr : entry->definition.get();
        }

        /** @brief Require an exact profile from its owning definition. */
        bool HasProfile(const Navigation::NavigationDefinition &definition, Navigation::NavigationAgentProfileId id) {
            return std::ranges::any_of(definition.Profiles(), [id](const auto &profile) {
                return profile.id == id;
            });
        }

        /** @brief Validate one object's descriptor bindings against the complete proposed surface graph. */
        bool HasDescriptors(const SceneSource::SceneObjectComponentSet &c, std::span<const SceneSource::SceneObjectSnapshot> objects,
                            std::span<const NavigationDefinitionSnapshot> definitions) {
            const auto definition = [&](Navigation::SurfaceId id) {
                return FindDefinition(objects, definitions, id);
            };
            if (c.navigationSurface) {
                const auto *d = definition(c.navigationSurface->id);
                if (!d || !std::ranges::all_of(c.navigationSurface->profiles, [&](auto id) {
                    return HasProfile(*d, id);
                }))
                    return false;
            }
            if (c.navigationModifier && c.navigationModifier->area) {
                const auto *d = definition(c.navigationModifier->surface);
                if (!d || d->Registry().ResolveArea(*c.navigationModifier->area).HasError())
                    return false;
            }
            if (c.navigationLink) {
                const auto *start = definition(c.navigationLink->start.surface);
                const auto *end = definition(c.navigationLink->end.surface);
                if (!start || !end || !std::ranges::all_of(c.navigationLink->profiles, [&](auto id) {
                    return HasProfile(*start, id) && HasProfile(*end, id);
                }))
                    return false;
            }
            return !c.navigationAgent || std::ranges::any_of(definitions, [&](const auto &entry) {
                return entry.definition && HasProfile(*entry.definition, c.navigationAgent->profile) &&
                       entry.definition->Registry().ResolveFilter(c.navigationAgent->filter).HasValue();
            });
        }

        /** @brief Require exact profile/filter/area descriptors from an explicit owning definition. */
        Result<void> Descriptors(std::span<const SceneSource::SceneObjectSnapshot> objects,
                                 std::span<const NavigationDefinitionSnapshot> definitions) {
            if (std::ranges::any_of(objects, [&](const auto &object) {
                return !HasDescriptors(object.components, objects, definitions);
            }))
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Invalid));
            return Result<void>::Success();
        }

        /** @brief Bound root, replacement ownership and foreign profile arrays before any detached copy. */
        Result<void> ValidatePreparationBudget(const SceneTransactionSnapshot &snapshot, std::span<const NavigationInspectorEdit> edits,
                                               SceneTransactionLimits limits) {
            std::size_t bytes{};
            for (const auto &object : snapshot.objects) {
                const auto owned = sizeof(object) + SceneDocumentDetail::EstimateSceneObjectOwnedMemoryBytes(object);
                if (owned > limits.maximumPreparedBytes || bytes > limits.maximumPreparedBytes - owned)
                    return Result<void>::Failure(Rejected(SceneTransactionFailure::Capacity));
                bytes += owned;
            }
            // Reserve room for the detached root, replacement ownership and validation/fingerprint scratch.
            if (bytes > limits.maximumPreparedBytes / 3)
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Capacity));
            for (const auto &edit : edits) {
                const bool bounded = std::visit([]<class T>(const T &value) {
                    if constexpr (std::is_same_v<T, Runtime::NavigationSurfaceComponent> ||
                                  std::is_same_v<T, Runtime::NavigationLinkComponent>)
                        return value.profiles.size() <= Runtime::MaximumNavigationSurfaceProfiles;
                    return true;
                }, edit.value);
                if (!bounded)
                    return Result<void>::Failure(Rejected(SceneTransactionFailure::Capacity));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateNavigationInspectorDescriptors */
    Result<void> ValidateNavigationInspectorDescriptors(std::span<const SceneSource::SceneObjectSnapshot> objects,
                                                        std::span<const NavigationDefinitionSnapshot> definitions) {
        return Descriptors(objects, definitions);
    }

    /** @copydoc PrepareNavigationInspectorEdits */
    Result<std::vector<SceneObjectReplacement>> PrepareNavigationInspectorEdits(const SceneTransactionSnapshot &snapshot,
                                                                                std::span<const NavigationInspectorEdit> edits,
                                                                                std::span<const NavigationDefinitionSnapshot> definitions,
                                                                                SceneTransactionLimits limits) {
        using Return = Result<std::vector<SceneObjectReplacement>>;
        if (edits.empty() || edits.size() > limits.maximumChanges || snapshot.objects.size() > limits.maximumSnapshotObjects)
            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
        if (auto budget = ValidatePreparationBudget(snapshot, edits, limits); budget.HasError())
            return Return::Failure(budget.ErrorValue());
        try {
            auto candidate = snapshot.objects;
            std::vector<SceneObjectReplacement> changes;
            for (std::size_t index = 0; index < edits.size(); ++index) {
                const auto &edit = edits[index];
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (edits[prior].object == edit.object && edits[prior].value.index() == edit.value.index())
                        return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
                auto found = std::ranges::find(candidate, edit.object, &SceneSource::SceneObjectSnapshot::id);
                if (found == candidate.end() || found->generated || SceneDocumentDetail::IsEffectivelyLocked(snapshot.objects, edit.object))
                    return Return::Failure(Rejected(SceneTransactionFailure::Unauthorized));
                auto replaced = std::visit([&](const auto &value) {
                    return Replace(found->components, value);
                }, edit.value);
                if (replaced.HasError())
                    return Return::Failure(replaced.ErrorValue());
            }
            if (auto valid = SceneDocumentDetail::ValidateSceneNavigationComponents(candidate); valid.HasError())
                return Return::Failure(valid.ErrorValue());
            if (auto valid = Descriptors(candidate, definitions); valid.HasError())
                return Return::Failure(valid.ErrorValue());
            for (const auto &object : candidate) {
                if (!std::ranges::any_of(edits, [&](const auto &edit) {
                    return edit.object == object.id;
                }))
                    continue;
                const auto before = std::ranges::find(snapshot.objects, object.id, &SceneSource::SceneObjectSnapshot::id);
                auto digest =
                    FingerprintSceneObjectContent(*before, std::min(limits.maximumPreparedBytes, MaximumSceneObjectFingerprintBytes));
                if (digest.HasError())
                    return Return::Failure(digest.ErrorValue());
                changes.push_back({object.id, digest.Value(), before->generated, object, {}});
            }
            return Return::Success(std::move(changes));
        } catch (const std::bad_alloc &) {
            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
        }
    }
}  // namespace Horo::Editor
