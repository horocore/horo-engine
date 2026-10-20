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
        template <class T> auto &Slot(auto &components) {
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
            auto &slot = Slot<T>(components);
            if (!slot)
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Unauthorized));
            if (slot->schemaVersion != value.schemaVersion)
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Stale));
            if constexpr (!std::is_same_v<T, Runtime::NavigationAgentComponent>) {
                if (slot->id != value.id || slot->generation != value.generation)
                    return Result<void>::Failure(Rejected(SceneTransactionFailure::Stale));
                if (*slot != value && value.generation == UINT64_MAX)
                    return Result<void>::Failure(Rejected(SceneTransactionFailure::Capacity));
                const bool changed = *slot != value;
                slot = value;
                if (changed)
                    ++slot->generation;
            } else
                slot = value;
            return Result<void>::Success();
        }

        /** @brief Require exact profile/filter/area descriptors from an explicit owning definition. */
        Result<void> Descriptors(std::span<const SceneSource::SceneObjectSnapshot> objects,
                                 std::span<const NavigationDefinitionSnapshot> definitions) {
            const auto definition = [&](Navigation::SurfaceId id) -> const Navigation::NavigationDefinition * {
                for (const auto &object : objects)
                    if (object.components.navigationSurface && object.components.navigationSurface->id == id)
                        for (const auto &entry : definitions)
                            if (entry.asset == object.components.navigationSurface->definition)
                                return entry.definition.get();
                return nullptr;
            };
            const auto profile = [](const Navigation::NavigationDefinition &d, Navigation::NavigationAgentProfileId id) {
                return std::ranges::any_of(d.Profiles(), [id](const auto &p) {
                    return p.id == id;
                });
            };
            for (const auto &object : objects) {
                const auto &c = object.components;
                if (c.navigationSurface) {
                    const auto *d = definition(c.navigationSurface->id);
                    if (!d || !std::ranges::all_of(c.navigationSurface->profiles, [&](auto id) {
                        return profile(*d, id);
                    }))
                        return Result<void>::Failure(Rejected(SceneTransactionFailure::Invalid));
                }
                if (c.navigationModifier && c.navigationModifier->area) {
                    const auto *d = definition(c.navigationModifier->surface);
                    if (!d || d->Registry().ResolveArea(*c.navigationModifier->area).HasError())
                        return Result<void>::Failure(Rejected(SceneTransactionFailure::Invalid));
                }
                if (c.navigationLink) {
                    const auto *start = definition(c.navigationLink->start.surface);
                    const auto *end = definition(c.navigationLink->end.surface);
                    if (!start || !end || !std::ranges::all_of(c.navigationLink->profiles, [&](auto id) {
                        return profile(*start, id) && profile(*end, id);
                    }))
                        return Result<void>::Failure(Rejected(SceneTransactionFailure::Invalid));
                }
                if (c.navigationAgent && !std::ranges::any_of(definitions, [&](const auto &entry) {
                    return entry.definition && profile(*entry.definition, c.navigationAgent->profile) &&
                           entry.definition->Registry().ResolveFilter(c.navigationAgent->filter).HasValue();
                }))
                    return Result<void>::Failure(Rejected(SceneTransactionFailure::Invalid));
            }
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
                const bool bounded = std::visit([](const auto &value) {
                    using T = std::decay_t<decltype(value)>;
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
