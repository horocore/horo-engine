#pragma once

#include "Horo/Runtime/Ui/UiBindingStore.h"
#include "UiBindingInternal.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    struct UiBindingStore::Storage final {
        struct Provider final {
            UiBindingProviderInstanceId instance;
            UiBindingProviderSchema schema;
            UiBindingSnapshotRevision revision;
            bool active{true};
            std::vector<std::vector<std::size_t>> targets;
        };

        struct Target final {
            UiBoundTarget bound;
            UiBindingValueLimits limits;
            std::optional<UiBindingValue> fallback;
            UiBindingDirty categories{UiBindingDirty::None};
            UiBindingDirty pending{UiBindingDirty::None};
        };

        struct Staged final {
            std::size_t target{};
            const UiBindingValue *value{};
            UiBindingValueOrigin origin{};
        };

        /** @brief Ensures borrowed staging references never survive a synchronous call, including validation failure. */
        struct StagingScope final {
            Storage &storage;

            explicit StagingScope(Storage &owner) noexcept : storage(owner) {
                storage.staged.clear();
                storage.invalidations.clear();
            }

            ~StagingScope() {
                storage.staged.clear();
                storage.invalidations.clear();
            }

            StagingScope(const StagingScope &) = delete;
            StagingScope &operator=(const StagingScope &) = delete;
        };

        RuntimeUiInstanceId instance;
        UiCanvasInstanceId canvas;
        UiDocumentId document;
        UiDocumentRevision documentRevision;
        UiRuntimeTreeRevision treeRevision;
        UiBindingStoreLimits limits;
        bool active{true};
        UiBindingApplyResult current{UiBindingUpdateRevision::Create(1).Value(), UiLayoutContentRevision::Create(1).Value()};
        std::vector<Provider> providers;
        std::vector<Target> targets;
        std::vector<Staged> staged;
        std::vector<UiLayoutInvalidation> invalidations;

        /** @brief Preallocates lifetime-bounded target and dirty storage before activation. */
        Storage(const UiElementTree &tree, const UiBindingStoreLimits &bounds)
            : instance(tree.Instance()), canvas(tree.Canvas()), document(tree.SourceDocument()),
              documentRevision(tree.SourceDocumentRevision()), treeRevision(tree.Revision()), limits(bounds) {
            providers.reserve(bounds.providers);
            targets.reserve(bounds.bindings);
            staged.reserve(bounds.bindings);
            invalidations.reserve(bounds.bindings);
        }

        /** @brief Finds only the exact registered provider incarnation; no type/scope fallback occurs. */
        [[nodiscard]] Provider *FindProvider(const UiBindingProviderInstanceId id) noexcept {
            const auto found = std::ranges::lower_bound(providers, id, {}, &Provider::instance);
            return found != providers.end() && found->instance == id ? std::to_address(found) : nullptr;
        }

        /** @brief Validates the exact tree and binding admission before any borrowed inputs are inspected. */
        [[nodiscard]] Result<void> ValidateTree(const UiElementTree &tree) const;
        /** @brief Checks readable exact tree generation without constructing an error or allocating. */
        [[nodiscard]] bool MatchesTree(const UiElementTree &tree) const noexcept;
        /** @brief Copies provider schemas and validates initial snapshot shape and values at load time. */
        [[nodiscard]] Result<void> PrepareProviders(std::span<const UiBindingProviderRegistration> registrations);
        /** @brief Resolves target handles, builds source adjacency and preallocates typed target values at load time. */
        [[nodiscard]] Result<void> PrepareTargets(const UiElementTree &tree, std::span<const UiBindingProviderRegistration> registrations,
                                                  std::span<const UiResolvedBindingDescriptor> bindings);
        /** @brief Resolves one validated conflict-free current target handle before value storage is prepared. */
        [[nodiscard]] Result<UiElementHandle> ResolveTarget(const UiElementTree &tree, const UiBindingDescriptor &binding,
                                                            const UiBindingProviderSchema &schema) const;
        /** @brief Prepares one target's owned typed value and source adjacency within the aggregate text budget. */
        [[nodiscard]] Result<void> PrepareTarget(const UiElementTree &tree, const UiResolvedBindingDescriptor &resolved,
                                                 std::span<const UiBindingProviderRegistration> registrations, std::size_t &reservedBytes);
        /** @brief Validates an entire bounded ordered update before staging any target changes. */
        [[nodiscard]] Result<void> ValidateBatches(std::span<const UiBindingChangeBatch> batches);
        /** @brief Stages only targets subscribed to the changed source property; equal values produce no dirty work. */
        [[nodiscard]] Result<void> Stage(std::size_t target, const UiBindingValue *value, UiBindingValueOrigin origin);
        /** @brief Stages source fanout after complete batch validation, preserving any target failure for atomic rollback. */
        [[nodiscard]] Result<void> StageBatches(std::span<const UiBindingChangeBatch> batches);
        /** @brief Visits one changed property's subscribers after complete source validation. */
        [[nodiscard]] Result<void> StageProperty(const Provider &provider, const UiBindingPropertyUpdate &change);
        /** @brief Stages required unavailability and optional fallback for one exact provider. */
        [[nodiscard]] Result<void> StageUnregister(const Provider &provider);
        /** @brief Atomically admits layout work before publishing staged targets and store revisions. */
        [[nodiscard]] Result<UiBindingApplyResult> Publish(const UiElementTree &tree, UiLayoutEngine &layout);
    };

    namespace BindingStoreInternal {
        /** @brief Constructs typed boundary failure without copying provider data or exposing bound values. */
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Counts variable-sized input before validation can scan text or identifiers. */
        [[nodiscard]] inline std::size_t ValueBytes(const UiBindingValue &value) noexcept {
            if (const auto *text = std::get_if<std::string>(&value))
                return text->size();
            if (const auto *message = std::get_if<UiBindingLocalizedMessage>(&value))
                return message->key.size();
            if (const auto *reference = std::get_if<UiBindingReference>(&value))
                return reference->value.size();
            return 0;
        }

        /** @brief Copies a same-type target into its reserved storage without changing variant alternatives or allocating. */
        inline void CopyValue(UiBindingValue &target, const UiBindingValue &source) noexcept {
            std::visit([&target]<typename Value>(const Value &value) {
                auto &retained = std::get<Value>(target);
                if constexpr (std::is_same_v<Value, UiBindingLocalizedMessage>)
                    retained.key.assign(value.key);
                else
                    retained = value;
            }, source);
        }

        /** @brief Clears revoked target contents while preserving reserved storage and the admitted type. */
        inline void ClearValue(UiBindingValue &target) noexcept {
            std::visit([]<typename Value>(Value &value) {
                if constexpr (std::is_same_v<Value, std::string>)
                    value.clear();
                else if constexpr (std::is_same_v<Value, UiBindingLocalizedMessage>)
                    value.key.clear();
                else if constexpr (std::is_same_v<Value, UiBindingReference>)
                    value.value.clear();
                else
                    value = {};
            }, target);
        }
    }  // namespace BindingStoreInternal
}  // namespace Horo::Runtime::Ui
