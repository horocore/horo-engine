#pragma once

/**
 * @file UiIdentity.h
 * @brief Stable Runtime UI identities, transient generation-safe handles, and revisions.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <array>
#include <compare>
#include <cstdint>
#include <limits>

namespace Horo::Runtime::Ui {
    /** @brief Persistent 128-bit representation shared by stable Runtime UI identities. */
    using SerializedUiId = std::array<std::uint8_t, 16>;

    /** @brief Strong stable identity whose tag prevents cross-domain substitution. */
    template <typename Tag> class UiStableId final {
    public:
        /** @brief Constructs the reserved invalid identity. */
        UiStableId() = default;

        /**
         * @brief Validates persistent identity bytes without consulting ambient state.
         * @param bytes Canonical 128-bit identity representation.
         * @return Typed identity or UiErrors::IdentityInvalid for the all-zero value.
         */
        [[nodiscard]] static Result<UiStableId> Create(const SerializedUiId &bytes) {
            if (bytes == SerializedUiId{})
                return Result<UiStableId>::Failure(MakeError(UiErrors::IdentityInvalid));
            return Result<UiStableId>::Success(UiStableId{bytes});
        }

        /** @brief Checks representation, not document residency. @return Whether any identity byte is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return bytes_ != SerializedUiId{};
        }

        /** @brief Returns the persistent representation. @return Borrowed immutable identity bytes. */
        [[nodiscard]] constexpr const SerializedUiId &Bytes() const noexcept {
            return bytes_;
        }

        [[nodiscard]] auto operator<=>(const UiStableId &) const noexcept = default;

    private:
        explicit constexpr UiStableId(const SerializedUiId &bytes) noexcept : bytes_(bytes) {}

        SerializedUiId bytes_{};
    };

    struct UiDocumentIdentityTag;
    struct UiElementIdentityTag;
    struct UiCanvasIdentityTag;
    struct UiActionIdentityTag;
    struct UiFontFaceIdentityTag;
    struct UiBindingIdentityTag;
    struct UiRouteIdentityTag;

    /** @brief Stable authored identity of one Runtime UI document. */
    using UiDocumentId = UiStableId<UiDocumentIdentityTag>;
    /** @brief Stable authored identity of one element within its owning document. */
    using UiElementId = UiStableId<UiElementIdentityTag>;
    /** @brief Stable authored identity of one canvas within its owning document. */
    using UiCanvasId = UiStableId<UiCanvasIdentityTag>;
    /** @brief Stable authored identity shared by a typed Runtime UI action contract. */
    using UiActionId = UiStableId<UiActionIdentityTag>;
    /** @brief Stable authored identity of one Runtime UI font face instance source. */
    using UiFontFaceId = UiStableId<UiFontFaceIdentityTag>;
    /** @brief Stable authored identity of one Runtime UI binding descriptor. */
    using UiBindingId = UiStableId<UiBindingIdentityTag>;
    /** @brief Stable authored identity of one Runtime UI route definition. */
    using UiRouteId = UiStableId<UiRouteIdentityTag>;

    /** @brief Non-zero process-local service/runtime/scope incarnation that must never be serialized. */
    class UiOwnershipGeneration final {
    public:
        /** @brief Constructs the reserved invalid generation. */
        UiOwnershipGeneration() = default;

        /** @brief Validates an owner-issued value. @param value Non-zero generation.
         * @return Typed generation or UiErrors::OwnershipGenerationInvalid.
         */
        [[nodiscard]] static Result<UiOwnershipGeneration> Create(std::uint64_t value);

        /** @brief Compares process-local ownership generations. @return Structural ordering and equality. */
        [[nodiscard]] constexpr auto operator<=>(const UiOwnershipGeneration &) const noexcept = default;

        /** @brief Returns the owner-issued value. @return Zero only for the invalid identity. */
        [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
            return value_;
        }

        /** @brief Checks representation, not owner liveness. @return Whether the value is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value_ != std::uint64_t{};
        }

    private:
        explicit constexpr UiOwnershipGeneration(const std::uint64_t value) noexcept : value_(value) {}

        std::uint64_t value_{};
    };

    /** @brief Fixed 128-bit transient handle owned by one exact Runtime UI incarnation. */
    template <typename Tag> struct UiRuntimeHandle final {
        UiOwnershipGeneration ownership; /**< Service/runtime/scope incarnation that issued the handle. */
        std::uint32_t slot{};            /**< Non-zero slot in the owning typed registry. */
        std::uint32_t generation{};      /**< Non-zero slot generation that never wraps. */

        /** @brief Checks representation, not registry residency. @return True when every identity component is present. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return ownership.IsValid() && slot != 0 && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiRuntimeHandle &) const noexcept = default;
    };

    struct RuntimeUiInstanceHandleTag;
    struct UiCanvasInstanceHandleTag;
    struct UiElementHandleTag;
    struct RuntimeUiInputContextHandleTag;
    struct UiRouteStackHandleTag;
    struct UiRouteInstanceHandleTag;
    /** @brief Generation-safe identity of one runtime document-tree instance. */
    using RuntimeUiInstanceId = UiRuntimeHandle<RuntimeUiInstanceHandleTag>;
    /** @brief Generation-safe identity of one instantiated canvas. */
    using UiCanvasInstanceId = UiRuntimeHandle<UiCanvasInstanceHandleTag>;
    /** @brief Generation-safe identity of one element in a runtime tree. */
    using UiElementHandle = UiRuntimeHandle<UiElementHandleTag>;
    /** @brief Generation-safe identity of one Runtime UI input audience/viewport context. */
    using RuntimeUiInputContextId = UiRuntimeHandle<RuntimeUiInputContextHandleTag>;
    /** @brief Generation-safe identity of one owner-scoped Runtime UI route stack. */
    using UiRouteStackId = UiRuntimeHandle<UiRouteStackHandleTag>;
    /** @brief Generation-safe identity of one committed route activation in a stack. */
    using UiRouteInstanceId = UiRuntimeHandle<UiRouteInstanceHandleTag>;

    /** @brief Rejects malformed and cross-owner handles before registry access.
     * @param handle Handle submitted to a Runtime UI owner boundary.
     * @param expectedOwnership Exact active incarnation receiving the operation.
     * @return Success for a well-formed same-owner handle, otherwise a stable typed error.
     * @post Success does not prove that the slot is resident or current.
     */
    template <typename Tag>
    [[nodiscard]] Result<void> ValidateUiHandleOwner(const UiRuntimeHandle<Tag> &handle, const UiOwnershipGeneration expectedOwnership) {
        if (!handle.IsValid())
            return Result<void>::Failure(MakeError(UiErrors::HandleMalformed));
        if (!expectedOwnership.IsValid() || handle.ownership != expectedOwnership)
            return Result<void>::Failure(MakeError(UiErrors::HandleOwnerMismatch));
        return Result<void>::Success();
    }

    /** @brief Validates exact registry residency and rejects stale slot generations.
     * @param handle Handle submitted to the owning typed registry.
     * @param expectedOwnership Exact active incarnation receiving the operation.
     * @param residentSlot Slot currently inspected by the registry.
     * @param residentGeneration Current non-zero generation stored in that slot.
     * @param occupied Whether the slot currently contains a live value.
     * @return Success only for an occupied exact slot generation, otherwise a stable typed error.
     */
    template <typename Tag>
    [[nodiscard]] Result<void> ValidateUiHandleResidency(const UiRuntimeHandle<Tag> &handle, const UiOwnershipGeneration expectedOwnership,
                                                         const std::uint32_t residentSlot, const std::uint32_t residentGeneration,
                                                         const bool occupied) {
        const Result<void> owner = ValidateUiHandleOwner(handle, expectedOwnership);
        if (owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        if (!occupied || residentSlot == 0 || residentGeneration == 0 || handle.slot != residentSlot ||
            handle.generation != residentGeneration)
            return Result<void>::Failure(MakeError(UiErrors::HandleStale));
        return Result<void>::Success();
    }

    /** @brief Ordering result for two revisions from the same typed revision domain. */
    enum class UiRevisionRelation : std::uint8_t {
        Older,
        Equal,
        Newer
    };

    /** @brief Strong monotonic revision whose tag prevents cross-domain comparisons. */
    template <typename Tag> class UiRevision final {
    public:
        /** @brief Constructs the reserved invalid revision. */
        UiRevision() = default;

        /** @brief Validates an owner-published revision. @param value Non-zero monotonic value.
         * @return Typed revision or UiErrors::RevisionInvalid.
         */
        [[nodiscard]] static Result<UiRevision> Create(const std::uint64_t value) {
            if (value == std::uint64_t{})
                return Result<UiRevision>::Failure(MakeError(UiErrors::RevisionInvalid));
            const UiRevision revision{value};
            return Result<UiRevision>::Success(revision);
        }

        /** @brief Compares typed revision representations. @return Structural ordering and equality. */
        [[nodiscard]] constexpr auto operator<=>(const UiRevision &) const noexcept = default;

        /** @brief Returns the owner-published value. @return Zero only for the invalid revision. */
        [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
            return value_;
        }

        /** @brief Checks representation, not whether the revision remains current. @return Whether the value is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value_ != std::uint64_t{};
        }

        /** @brief Compares this revision with one from the same domain. @param baseline Revision to compare against.
         * @return Older, Equal, or Newer using monotonic ordering.
         * @pre Both revisions are valid and belong to the same owner generation.
         */
        [[nodiscard]] constexpr UiRevisionRelation Compare(const UiRevision baseline) const noexcept {
            if (value_ < baseline.value_)
                return UiRevisionRelation::Older;
            if (value_ > baseline.value_)
                return UiRevisionRelation::Newer;
            return UiRevisionRelation::Equal;
        }

        /** @brief Advances without wrapping. @return Next revision or UiErrors::GenerationExhausted at UINT64_MAX. */
        [[nodiscard]] Result<UiRevision> Next() const {
            if (!IsValid())
                return Result<UiRevision>::Failure(MakeError(UiErrors::RevisionInvalid));
            if (value_ == std::numeric_limits<std::uint64_t>::max())
                return Result<UiRevision>::Failure(MakeError(UiErrors::GenerationExhausted));
            return Result<UiRevision>::Success(UiRevision{value_ + 1});
        }

    private:
        explicit constexpr UiRevision(const std::uint64_t value) noexcept : value_(value) {}

        std::uint64_t value_{};
    };

    struct UiDocumentRevisionTag;
    struct UiRuntimeTreeRevisionTag;
    struct UiInteractionRevisionTag;
    struct UiActionSequenceTag;
    struct UiActionOperationTag;
    struct UiTextContentRevisionTag;
    struct UiTextFontRevisionTag;
    struct UiTextShapeRevisionTag;
    struct UiRouteStackRevisionTag;
    struct UiRouteOperationSequenceTag;
    /** @brief Monotonic revision of one authored Runtime UI document. */
    using UiDocumentRevision = UiRevision<UiDocumentRevisionTag>;
    /** @brief Monotonic revision of one published runtime tree generation. */
    using UiRuntimeTreeRevision = UiRevision<UiRuntimeTreeRevisionTag>;
    /** @brief Monotonic revision of one immutable interaction/layout publication. */
    using UiInteractionRevision = UiRevision<UiInteractionRevisionTag>;
    /** @brief Monotonic owner-local sequence assigned to one admitted UI action request. */
    using UiActionSequence = UiRevision<UiActionSequenceTag>;
    /** @brief Owner-local identity correlating a pending action with its terminal result. */
    using UiActionOperationSequence = UiRevision<UiActionOperationTag>;
    /** @brief Monotonic source-content revision consumed by one text shaping request. */
    using UiTextContentRevision = UiRevision<UiTextContentRevisionTag>;
    /** @brief Monotonic immutable font-registry revision consumed by one text shaping request. */
    using UiTextFontRevision = UiRevision<UiTextFontRevisionTag>;
    /** @brief Monotonic immutable shaped-result revision published by one text shaper. */
    using UiTextShapeRevision = UiRevision<UiTextShapeRevisionTag>;
    /** @brief Monotonic revision of one committed route stack state. */
    using UiRouteStackRevision = UiRevision<UiRouteStackRevisionTag>;
    /** @brief Owner-local sequence assigned to one route operation transaction. */
    using UiRouteOperationSequence = UiRevision<UiRouteOperationSequenceTag>;

    /** @brief Requires an expected revision to match the current owner-published revision.
     * @param expected Revision captured when the caller prepared its command.
     * @param current Revision currently published by the owner.
     * @return Success for an exact valid match, UiErrors::RevisionInvalid for malformed input,
     * or UiErrors::RevisionStale when the owner has advanced.
     */
    template <typename Tag>
    [[nodiscard]] Result<void> ValidateExpectedUiRevision(const UiRevision<Tag> expected, const UiRevision<Tag> current) {
        if (!expected.IsValid() || !current.IsValid())
            return Result<void>::Failure(MakeError(UiErrors::RevisionInvalid));
        if (expected != current)
            return Result<void>::Failure(MakeError(UiErrors::RevisionStale));
        return Result<void>::Success();
    }

    static_assert(sizeof(RuntimeUiInstanceId) == 16);
    static_assert(sizeof(UiCanvasInstanceId) == 16);
    static_assert(sizeof(UiElementHandle) == 16);
    static_assert(sizeof(RuntimeUiInputContextId) == 16);
    static_assert(sizeof(UiRouteStackId) == 16);
    static_assert(sizeof(UiRouteInstanceId) == 16);
}  // namespace Horo::Runtime::Ui
