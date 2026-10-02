#pragma once

#include "GameplayModuleTestSupport.h"
#include "Horo/Cinematic/EventDispatcher.h"
#include "Horo/Cinematic/EventSession.h"
#include "Horo/Cinematic/EventTrackErrors.h"
#include "Horo/Cinematic/GameplayEventAdapter.h"
#include "Horo/Cinematic/ScriptEventCook.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace Horo::Cinematic {
    namespace EventTestSupport {
        template <class T>
        concept UncheckedCook = requires(std::vector<CookedEventKey> keys) { T::Create(std::move(keys)); };
        static_assert(!UncheckedCook<CookedEventPlan>);
        template <class T>
        concept UncheckedConstruction = requires(std::vector<CookedEventKey> keys) { T({}, std::move(keys)); };
        static_assert(!UncheckedConstruction<CookedEventPlan>);
        constexpr CinematicRuntimeSessionId Session{70, 1};
        constexpr EventBindingId Binding{90, 1};
        constexpr EventPayloadSchemaId Schema{91, 1};
        constexpr TrackId Track{10, 1};
        constexpr KeyframeId Key{11, 1};

        [[nodiscard]] inline Extensions::ScriptExportDescriptorSnapshotPtr ExportSnapshot() {
            using namespace Extensions;
            ScriptExportDescriptor api{
                .moduleId = "gameplay.quest",
                .id = "gameplay.quest.events",
                .nameSpace = "gameplay.quest",
                .version = {1, 0, 0},
                .compatibility = {.minimum = {1, 0, 0}, .maximum = {1, 0, 0}},
                .service = {.capability = {"gameplay.quest"},
                            .serviceId = "gameplay.quest.service",
                            .contractId = "gameplay.quest.contract",
                            .minimumVersion = {1, 0, 0}},
            };
            api.functions.push_back(
                {.id = "complete",
                 .introducedVersion = {1, 0, 0},
                 .invocation = ScriptExportInvocationMode::Synchronous,
                 .parameters = {
                     {.id = "quest", .type = {.primitive = ScriptExportPrimitiveKind::SignedInteger}, .introducedVersion = {1, 0, 0}}}});
            auto snapshot = BuildScriptExportDescriptorSnapshot(std::array{api});
            REQUIRE(snapshot.HasValue());
            return snapshot.Value();
        }

        [[nodiscard]] inline ScriptEventDescriptor Descriptor() {
            return {.qualifiedName = "gameplay.quest.complete",
                    .binding = Binding,
                    .schema = Schema,
                    .exportApiId = "gameplay.quest.events",
                    .exportFunctionId = "complete",
                    .exportVersion = {1, 0, 0},
                    .allowedContexts =
                        static_cast<std::byte>(EventRuntimeContext::Runtime) | static_cast<std::byte>(EventRuntimeContext::Headless)};
        }

        [[nodiscard]] inline AuthoredScriptEventKey Authored(const KeyframeId key = Key, const std::int64_t quest = 42) {
            return {Track, key, "gameplay.quest.complete", {Extensions::ScriptValue(quest)}, 2, true};
        }

        [[nodiscard]] inline std::shared_ptr<const CookedEventPlan> Cooked() {
            const auto source = Authored();
            const auto descriptor = Descriptor();
            auto cooked =
                CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, ExportSnapshot(), EventRuntimeContext::Headless);
            REQUIRE(cooked.HasValue());
            return cooked.Value();
        }

        [[nodiscard]] inline SequenceFrameEvaluationPlan Evaluation() {
            const std::array events{SequenceFrameEventKey{Track, Key, 2, true}};
            auto evaluation = SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, {}, events, {});
            REQUIRE(evaluation.HasValue());
            return std::move(evaluation).Value();
        }

        struct HandlerProbe final {
            std::size_t calls{};
            std::int64_t lastQuest{};
            std::uint64_t lastTick{};
            std::array<std::uint64_t, 64> deliveredPlayers{};
            std::array<std::uint64_t, 64> deliveredKeys{};
        };

        struct PublicationProbe {
            float value{};
            std::size_t writes{};
            std::size_t finished{};
        };

        inline Result<float> SamplePublication(const void *, const SequenceTime time) {
            return Result<float>::Success(static_cast<float>(time));
        }

        inline void ApplyPublication(void *context, const float value) noexcept {
            auto &probe = *static_cast<PublicationProbe *>(context);
            probe.value = value;
            ++probe.writes;
        }

        inline void FinishPublication(void *context, const SequencePlayerHandle &) noexcept {
            ++static_cast<PublicationProbe *>(context)->finished;
        }

        inline EventDispatchOutcome GameplayCallback(const BorrowedCallbackContext &context, const EventDispatchRequest &request) {
            auto *handler = context.Get<HandlerProbe>();
            if (handler == nullptr)
                return EventDispatchOutcome::HandlerFailed;
            auto &probe = *handler;
            const auto decoded = Extensions::DecodeScriptValue(request.payload);
            if (decoded.HasError() || decoded.Value().AsArray().size() != 1)
                return EventDispatchOutcome::HandlerFailed;
            const std::int64_t *quest = decoded.Value().AsArray().front().AsSignedInteger();
            if (quest == nullptr)
                return EventDispatchOutcome::HandlerFailed;
            probe.lastQuest = *quest;
            probe.lastTick = request.committedTick;
            probe.deliveredPlayers[probe.calls] = request.occurrence.player.player.stableValue;
            probe.deliveredKeys[probe.calls] = request.occurrence.key.stableValue;
            ++probe.calls;
            return EventDispatchOutcome::Accepted;
        }

        [[nodiscard]] inline CinematicEventDispatcher Dispatcher() {
            auto dispatcher = CinematicEventDispatcher::Create(Session, EventRuntimeContext::Headless, 4, 4);
            REQUIRE(dispatcher.HasValue());
            return std::move(dispatcher).Value();
        }

        inline void Register(CinematicEventDispatcher &dispatcher, const std::shared_ptr<HandlerProbe> &probe,
                             const std::uint64_t generation = 1) {
            REQUIRE(dispatcher
                        .Register({Binding, Schema, EventRuntimeContext::Headless, generation, BorrowedCallbackContext{probe.get()},
                                   GameplayCallback, probe})
                        .HasValue());
        }

        [[nodiscard]] inline SequenceFrameEventOccurrence Occurrence(const std::uint64_t player = 1, const std::uint64_t traversal = 1) {
            return {{Session, {player, 1}}, Track, Key, 2, traversal, SequenceTraversalDirection::Forward};
        }

        template <typename T> void RequireCode(const Result<T> &result, const ErrorCodeDescriptor &code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code.code.Value());
        }
    }  // namespace EventTestSupport

}  // namespace Horo::Cinematic
