#pragma once

#include "Horo/Runtime/Ui/UiBindingStore.h"
#include "UiBindingInternal.h"

#include <algorithm>
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
        UiBindingApplyResult current;
        std::vector<Provider> providers;
        std::vector<Target> targets;
        std::vector<Staged> staged;
        std::vector<UiLayoutInvalidation> invalidations;

        /** @brief Preallocates lifetime-bounded target and dirty storage before activation. */
        Storage(const UiElementTree &tree, const UiBindingStoreLimits &bounds)
            : instance(tree.Instance()), canvas(tree.Canvas()), document(tree.SourceDocument()),
              documentRevision(tree.SourceDocumentRevision()), treeRevision(tree.Revision()), limits(bounds),
              current{UiBindingUpdateRevision::Create(1).Value(), UiLayoutContentRevision::Create(1).Value()} {
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
        /** @brief Copies provider schemas and validates initial snapshot shape and values at load time. */
        [[nodiscard]] Result<void> PrepareProviders(std::span<const UiBindingProviderRegistration> registrations);
        /** @brief Resolves target handles, builds source adjacency and preallocates typed target values at load time. */
        [[nodiscard]] Result<void> PrepareTargets(const UiElementTree &tree, std::span<const UiBindingProviderRegistration> registrations,
                                                  std::span<const UiResolvedBindingDescriptor> bindings);
        /** @brief Validates an entire bounded ordered update before staging any target changes. */
        [[nodiscard]] Result<void> ValidateBatches(std::span<const UiBindingChangeBatch> batches);
        /** @brief Stages only targets subscribed to the changed source property; equal values produce no dirty work. */
        [[nodiscard]] Result<void> Stage(std::size_t target, const UiBindingValue *value, UiBindingValueOrigin origin);
        /** @brief Atomically admits layout work before publishing staged targets and store revisions. */
        [[nodiscard]] Result<UiBindingApplyResult> Publish(const UiElementTree &tree, UiLayoutEngine &layout);
    };

    namespace BindingStoreInternal {
        /** @brief Copies a same-type target into its reserved storage without changing variant alternatives or allocating. */
        inline void CopyValue(UiBindingValue &target, const UiBindingValue &source) noexcept {
            if (auto *text = std::get_if<std::string>(&target))
                text->assign(std::get<std::string>(source));
            else if (auto *message = std::get_if<UiBindingLocalizedMessage>(&target))
                message->key.assign(std::get<UiBindingLocalizedMessage>(source).key);
            else if (auto *boolean = std::get_if<bool>(&target))
                *boolean = std::get<bool>(source);
            else
                std::get<double>(target) = std::get<double>(source);
        }

        /** @brief Clears revoked target contents while preserving reserved storage and the admitted type. */
        inline void ClearValue(UiBindingValue &target) noexcept {
            if (auto *text = std::get_if<std::string>(&target))
                text->clear();
            else if (auto *message = std::get_if<UiBindingLocalizedMessage>(&target))
                message->key.clear();
            else if (auto *boolean = std::get_if<bool>(&target))
                *boolean = false;
            else
                std::get<double>(target) = 0.0;
        }
    }  // namespace BindingStoreInternal
}  // namespace Horo::Runtime::Ui
