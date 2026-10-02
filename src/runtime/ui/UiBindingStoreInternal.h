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
            UiBindingProviderScopeKind scope{UiBindingProviderScopeKind::Count};
            bool writesRevoked{};
        };

        struct Target final {
            UiBoundTarget bound;
            UiBindingValueLimits limits;
            std::optional<UiBindingValue> fallback;
            UiBindingDirty categories{UiBindingDirty::None};
            UiBindingDirty pending{UiBindingDirty::None};
            std::size_t provider{};
            std::uint16_t property{};
            UiBindingDirection direction{UiBindingDirection::SourceToTarget};
            std::optional<UiBindingWriteAdmission> admission;
            UiBindingWriteFence fence;
            UiActionSequence lastRequest;
            UiBindingEditId edit;
            UiActionSource source;
            UiBindingSnapshotRevision expected;
            std::optional<UiBindingWriteCommand> command;
            std::optional<UiBindingWriteResult> outcome;
            UiBindingValue draft;
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
        bool writesAdmitted{};
        bool processingWrite{};
        bool reentryAttempted{};
        std::uint64_t editSequence{};
        std::size_t writeCursor{};
        UiBindingApplyResult current;
        std::vector<Provider> providers;
        std::vector<Target> targets;
        std::vector<Staged> staged;
        std::vector<UiLayoutInvalidation> invalidations;

        /** @brief Resolves stable binding identity within finite retained storage. */
        [[nodiscard]] Target *FindTarget(UiBindingId binding) noexcept;
        /** @brief Selects one pending command in deterministic round-robin order so slow producers cannot starve peers. */
        [[nodiscard]] Target *NextWrite() noexcept;
        /** @brief Closes an old command once, retaining outcome evidence until acknowledgement. */
        void CancelWrite(Target &target, UiBindingWriteCancellationReason reason) noexcept;
        /** @brief Validates admitted source against exact owner, tree and retained element evidence. */
        [[nodiscard]] Result<void> ValidateWriteSource(const UiElementTree &tree, const Target &target, UiActionSource source) const;
        /** @brief Atomically publishes a prepared command through existing delta and invalidation storage. */
        [[nodiscard]] Result<void> PublishWrite(const UiElementTree &tree, Target &target, UiLayoutEngine &layout);

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

        /** @brief Stops reservations before releasing authority/module leases, including move assignment. */
        ~Storage() {
            for (auto &target : targets)
                CancelWrite(target, UiBindingWriteCancellationReason::Shutdown);
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
