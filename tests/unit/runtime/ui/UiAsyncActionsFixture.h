#pragma once

#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Ui/UiAsyncActions.h"
#include "Horo/Runtime/Ui/UiControls.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiScreenStack.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Horo::Runtime::Ui::AsyncActionTests {
    template <typename Id> Id AuthoredId(const std::uint8_t marker = 1) {
        SerializedUiId bytes{};
        bytes.back() = marker;
        return Id::Create(bytes).Value();
    }

    inline UiActionOwnerContext Context(const std::uint64_t revision = 1) {
        const auto owner = UiOwnershipGeneration::Create(752).Value();
        return {{owner, 1, 1},
                {owner, 2, 1},
                AuthoredId<UiDocumentId>(),
                UiDocumentRevision::Create(revision).Value(),
                UiRuntimeTreeRevision::Create(revision).Value(),
                UiInteractionRevision::Create(revision).Value()};
    }

    inline UiActionSource Source(const UiActionOwnerContext &context = Context(), const std::uint32_t slot = 3) {
        return {context, {context.instance.ownership, slot, 1}};
    }

    inline UiActionRequest Request(const std::uint64_t sequence = 1, const UiActionSource &source = Source()) {
        return {{source.owner.instance.ownership, UiActionSequence::Create(sequence).Value()},
                source,
                UiActionOrigin::Button,
                UiButtonActionCommand{AuthoredId<UiActionId>(), {}}};
    }

    inline UiActionRouter Router(const std::uint32_t capacity = 2, const UiActionOwnerContext &context = Context()) {
        return std::move(UiActionRouter::Create({context, capacity})).Value();
    }

    inline UiAsyncActionStore Store(const std::uint32_t capacity = 2) {
        return std::move(UiAsyncActionStore::Create(Context(), capacity)).Value();
    }

    inline UiControlStateMachine Control(const bool enabled = true, const UiActionOwnerContext &context = Context()) {
        UiControlDescriptorBase base;
        base.owner = context;
        base.element = Source(context).element;
        base.action = AuthoredId<UiActionId>();
        base.initiallyEnabled = enabled;
        return std::move(UiControlStateMachine::Create(UiButtonControlDescriptor{base})).Value();
    }

    inline UiScreenStack ScreenStack(const std::uint32_t capacity = 2) {
        const std::array definitions{UiRouteMetadata{AuthoredId<UiRouteId>(), UiPresentationBand::Screen, 0, false},
                                     UiRouteMetadata{AuthoredId<UiRouteId>(2), UiPresentationBand::Screen, 0, false}};
        const auto owner = Context().instance.ownership;
        return std::move(UiScreenStack::Create({owner, {owner, 1, 1}, definitions, capacity})).Value();
    }

    template <typename T> void ExpectError(const Result<T> &result, const ErrorCodeDescriptor &error) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == error.code.Value());
    }

    class RetainingHandler final : public UiAsyncActionHandler {
    public:
        Result<void> Start(const UiActionRequest &request, UiAsyncActionProducer pending) override {
            lastRequest = request;
            ++calls;
            producer.emplace(std::move(pending));
            return Result<void>::Success();
        }

        std::optional<UiAsyncActionProducer> producer;
        UiActionRequest lastRequest;
        std::size_t calls{};
    };

    inline UiAsyncActionKey Dispatch(UiActionRouter &router, RetainingHandler &handler) {
        REQUIRE(router.Enqueue(Source(router.Owner()), UiButtonActionCommand{AuthoredId<UiActionId>(), {}}).HasValue());
        auto dispatched = router.DispatchNext(handler);
        REQUIRE(dispatched.HasValue());
        REQUIRE(dispatched.Value());
        CHECK(dispatched.Value()->kind == UiActionResultKind::Pending);
        REQUIRE(handler.producer);
        CHECK(dispatched.Value()->operation == handler.producer->Key().operation);
        return handler.producer->Key();
    }

}  // namespace Horo::Runtime::Ui::AsyncActionTests
