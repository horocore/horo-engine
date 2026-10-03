#pragma once

/** @file
 * @brief Private retained-tree and provider fixtures for the binding write regression executable.
 */

#include "Horo/Runtime/Ui/UiBindingStore.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace Horo::Runtime::Ui::BindingWriteTests {
    template <typename T> T Take(Result<T> result) {
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    template <typename Id> Id Stable(const std::uint8_t marker) {
        SerializedUiId bytes{};
        bytes.back() = marker;
        return Take(Id::Create(bytes));
    }

    template <typename Revision> Revision Rev(const std::uint64_t value = 1) {
        return Take(Revision::Create(value));
    }

    UiOwnershipGeneration Owner();

    UiActionText Text(std::string_view text);

    template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &error) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == error.code.Value());
    }

    /** @brief Producer-owned typed values and callback evidence shared by authority leases. */
    struct ProviderState final {
        UiBindingSnapshotRevision revision{Rev<UiBindingSnapshotRevision>()};
        std::array<UiActionValue, 4> values{false, Text("old"), 0.5, true};
        std::size_t prepares{};
        std::size_t commits{};
        std::size_t abandons{};
        std::size_t destroyed{};
        bool commitFenceValid{true};
    };

    /** @brief Typed producer adapter that reserves against the full fence before publication. */
    class TypedAuthority final : public UiBindingWriteAuthority {
    public:
        enum class Reentry {
            None,
            Retirement,
            Shutdown
        };
        UiBindingWriteFence fence;
        std::shared_ptr<ProviderState> state;
        UiBindingWriteDisposition disposition{UiBindingWriteDisposition::Ready};
        std::optional<Error> failure;
        bool translatePrivateFailure{};
        std::optional<UiBindingWriteCommand> reservation;
        bool active{true};
        UiBindingStore *reentrantStore{};
        Reentry prepareReentry{Reentry::None};
        bool abandonRetires{};

        TypedAuthority(const UiBindingWriteFence &permission, std::shared_ptr<ProviderState> owner);
        ~TypedAuthority() override;
        TypedAuthority(const TypedAuthority &) = delete;
        TypedAuthority &operator=(const TypedAuthority &) = delete;
        TypedAuthority(TypedAuthority &&) = delete;
        TypedAuthority &operator=(TypedAuthority &&) = delete;
        const UiBindingWriteFence &Fence() const noexcept override;
        bool Active() const noexcept override;
        Result<UiBindingWriteDisposition> Prepare(const UiBindingWriteCommand &command) noexcept override;
        void Commit(const UiBindingWriteCommand &command) noexcept override;
        void Abandon(const UiBindingWriteCommand &command) noexcept override;
        void Revoke() noexcept;

    private:
        bool FenceMatches(const UiBindingWriteCommand &command) const noexcept;
        std::optional<Error> ValidateValue(const UiBindingWriteCommand &command) const;
        std::optional<Error> ValidatePrivateValue(const UiBindingWriteCommand &command) const noexcept;
        void ApplyReentry();
    };

    /** @brief Matching authored control, binding target, property, and commit policy. */
    struct ControlCase final {
        UiControlKind kind;
        std::uint8_t element;
        std::uint8_t binding;
        std::uint16_t property;
        UiBindingTargetProperty target;
        UiBindingCommitTrigger trigger;
        UiBindingValue initial;
    };

    ControlCase DescribeControl(UiControlKind kind, bool authored = false);

    /** @brief Load-time retained tree and schema whose storage outlives the tested write owner. */
    struct Fixture {
        UiElementSlotAllocator slots{Take(UiElementSlotAllocator::Create(Owner()))};
        UiElementTree tree{MakeTree()};
        UiBindingProviderSchema schema{MakeSchema()};
        UiBindingProviderInstanceId provider{Owner(), 9, 1};
        std::shared_ptr<ProviderState> state{std::make_shared<ProviderState>()};

        UiElementTree MakeTree(const std::uint32_t generation = 1);
        static UiBindingProviderSchema MakeSchema();
        UiResolvedBindingDescriptor Binding(const std::uint8_t id, const std::uint8_t element, const std::uint16_t property,
                                            const UiBindingTargetProperty target,
                                            const UiBindingDirection direction = UiBindingDirection::TwoWay) const;
        UiBindingStore Store() const;
        UiLayoutEngine Layout(const std::uint32_t capacity = 8) const;
        UiActionOwnerContext Context() const;
        UiActionSource Source(const std::uint8_t element) const;
        UiActionId Action() const;
        std::shared_ptr<TypedAuthority> Authority(const std::uint16_t property) const;
        UiBindingWriteAdmission Admission(const std::uint8_t binding, const std::shared_ptr<TypedAuthority> &authority,
                                          const UiBindingCommitTrigger trigger = UiBindingCommitTrigger::Change) const;
        void Admit(UiBindingStore &store, const std::uint8_t binding, const std::shared_ptr<TypedAuthority> &authority,
                   const UiBindingCommitTrigger trigger = UiBindingCommitTrigger::Change) const;
        UiActionRouter Router(const std::uint32_t capacity = 4) const;
        UiActionRequest Request(UiActionRouter &router, const std::uint8_t element, const UiActionValue &value) const;
        UiControlStateMachine Control(const UiControlKind kind) const;
        UiControlInput Input(const UiControlStateMachine &control, const UiControlInputKind kind, const std::uint64_t sequence,
                             const UiActionText &text = {}) const;
        UiActionRequest DefaultRequest(UiActionRouter &router, const UiControlStateMachine &control) const;
        UiBindingWriteResult QueueChange(UiBindingStore &store, UiActionRouter &router, std::uint8_t binding = 10, std::uint8_t element = 2,
                                         const UiActionValue &value = true) const;
        UiControlStateMachine Control(const ControlCase &test) const;
        void StageToggle(UiControlStateMachine &control) const;
        void StageDefault(UiControlStateMachine &control, const UiActionText &appended) const;
        UiBindingStore WriteOnlyStore(const UiResolvedBindingDescriptor &binding) const;
    };

    /** @brief Composes the binding, layout, and action owners after the retained fixture exists. */
    struct WriteSession final : Fixture {
        UiBindingStore store;
        UiLayoutEngine layout;
        UiActionRouter router;
        explicit WriteSession(std::uint32_t layoutCapacity = 8, std::uint32_t routerCapacity = 4);
        UiBindingWriteResult QueueCurrentChange(std::uint8_t binding = 10, std::uint8_t element = 2, const UiActionValue &value = true);
    };

    UiBindingWriteResult Process(UiBindingStore &store, const UiElementTree &tree, UiLayoutEngine &layout);
    void ClearDirty(UiBindingStore &store);
    void CheckControlValue(const UiControlStateMachine &control, const UiBindingValue &expected);
    void CheckCommittedValue(const Fixture &fixture, const UiBindingStore &store, UiControlKind kind, UiLayoutContentRevision previous);
    void CommitFreshPresentation(WriteSession &fixture, const UiActionOwnerContext &owner, const UiActionSource &source);
    std::atomic<std::size_t> &WriteAllocations() noexcept;
}  // namespace Horo::Runtime::Ui::BindingWriteTests
