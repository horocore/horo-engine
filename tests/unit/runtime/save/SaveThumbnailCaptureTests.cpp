#include "Horo/Runtime/Save/SaveThumbnailCapture.h"
#include "SaveTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>

namespace Horo::Runtime {
    namespace {
        using namespace Test;
        using Clock = std::chrono::steady_clock;
        constexpr auto kStart = Clock::time_point{};

        [[nodiscard]] SaveThumbnailRequest Request(const SaveThumbnailPolicy policy = SaveThumbnailPolicy::Optional) {
            return {.slot = Id<SaveGameSlotId>(4),
                    .generation = Id<SlotGenerationId>(5),
                    .thumbnail = Id<SaveThumbnailId>(6),
                    .policy = policy,
                    .source = {.runtime = 1, .scene = 2, .view = 3, .frame = 10}};
        }

        [[nodiscard]] SaveThumbnailCompletion Completion(const std::uint64_t serial) {
            return {.requestSerial = serial,
                    .slot = Id<SaveGameSlotId>(4),
                    .generation = Id<SlotGenerationId>(5),
                    .thumbnail = Id<SaveThumbnailId>(6),
                    .source = {.runtime = 1, .scene = 2, .view = 3, .frame = 12},
                    .width = 320,
                    .height = 180,
                    .encoded = {std::byte{1}, std::byte{2}}};
        }

        [[nodiscard]] SaveSlotPublicationMetadata Publication() {
            auto publication = Entry(4, 5).publication;
            publication.thumbnail = Id<SaveThumbnailId>(6);
            return publication;
        }

        TEST_CASE("Async thumbnail capture publishes CPU bytes with actual frame and exact generation", "[unit][save][thumbnail]") {
            SaveThumbnailCapture capture;
            const auto admitted = capture.Request(Request(), SaveThumbnailAvailability::Available, kStart);
            REQUIRE(admitted.HasValue());
            REQUIRE(capture.Snapshot().state == SaveThumbnailCaptureState::Pending);
            REQUIRE_FALSE(capture.Acknowledge(admitted.Value()));
            REQUIRE(capture.Request(Request(), SaveThumbnailAvailability::Available, kStart).HasError());
            auto pending = MakeSaveCommittedPresentation(Publication(), {}, capture.Snapshot());
            REQUIRE(pending.HasError());
            auto result = capture.Complete(Completion(admitted.Value()), kStart + std::chrono::milliseconds{1}, Request().source);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value());
            REQUIRE(capture.Snapshot().state == SaveThumbnailCaptureState::Captured);
            auto presentation =
                MakeSaveCommittedPresentation(Publication(), {.displayName = "Kayıt 🌍", .summary = "Checkpoint"}, capture.Snapshot());
            REQUIRE(presentation.HasValue());
            REQUIRE(presentation.Value().artifact->Source().frame == 12);
            REQUIRE(presentation.Value().artifact->Request().source.frame == 10);
            REQUIRE(presentation.Value().artifact->Bytes().size() == 2);
            REQUIRE(presentation.Value().catalog.display.displayName == "Kayıt 🌍");
            REQUIRE(capture.Acknowledge(admitted.Value()));
            REQUIRE(presentation.Value().artifact->Bytes().size() == 2);
        }

        TEST_CASE("Headless unavailable and disabled captures require no renderer dispatch", "[unit][save][thumbnail]") {
            for (const auto availability : {SaveThumbnailAvailability::Headless, SaveThumbnailAvailability::RendererUnavailable}) {
                for (const auto policy : {SaveThumbnailPolicy::Disabled, SaveThumbnailPolicy::Optional, SaveThumbnailPolicy::Required}) {
                    SaveThumbnailCapture capture;
                    auto request = Request(policy);
                    request.source.view = 0;
                    request.source.frame = 0;
                    REQUIRE(capture.Request(request, availability, kStart).HasValue());
                    REQUIRE(capture.Snapshot().state == (policy == SaveThumbnailPolicy::Required ? SaveThumbnailCaptureState::Failed
                                                                                                 : SaveThumbnailCaptureState::Omitted));
                    auto publication = Publication();
                    publication.thumbnail.reset();
                    const auto result = MakeSaveCommittedPresentation(publication, {}, capture.Snapshot());
                    REQUIRE(result.HasValue() == (policy != SaveThumbnailPolicy::Required));
                    if (policy == SaveThumbnailPolicy::Required)
                        REQUIRE(result.ErrorValue().code.Value() == SaveErrors::ThumbnailUnavailable.code.Value());
                }
            }
        }

        TEST_CASE("Finite thumbnail timeout and invalid owner clock never block save capture", "[unit][save][thumbnail]") {
            for (const auto policy : {SaveThumbnailPolicy::Optional, SaveThumbnailPolicy::Required}) {
                SaveThumbnailCapture capture;
                const auto serial = capture.Request(Request(policy), SaveThumbnailAvailability::Available, kStart).Value();
                REQUIRE(capture.Advance(kStart - std::chrono::milliseconds{1}, Request().source).HasError());
                REQUIRE(capture.Snapshot().state == SaveThumbnailCaptureState::Pending);
                REQUIRE(capture.Advance(kStart + std::chrono::milliseconds{499}, Request().source).HasValue());
                REQUIRE(capture.Snapshot().state == SaveThumbnailCaptureState::Pending);
                REQUIRE(capture.Advance(kStart + std::chrono::milliseconds{500}, Request().source).HasValue());
                REQUIRE(capture.Snapshot().error->code.Value() == SaveErrors::ThumbnailExpired.code.Value());
                REQUIRE(capture.Snapshot().state ==
                        (policy == SaveThumbnailPolicy::Required ? SaveThumbnailCaptureState::Failed : SaveThumbnailCaptureState::Omitted));
                REQUIRE_FALSE(capture.Complete(Completion(serial), kStart + std::chrono::milliseconds{501}, Request().source).Value());
            }
        }

        TEST_CASE("Runtime scene and view replacement reject pending thumbnail evidence", "[unit][save][thumbnail]") {
            for (const auto axis : {0, 1, 2}) {
                SaveThumbnailCapture capture;
                const auto serial = capture.Request(Request(), SaveThumbnailAvailability::Available, kStart).Value();
                auto current = Request().source;
                if (axis == 0)
                    ++current.runtime;
                if (axis == 1)
                    ++current.scene;
                if (axis == 2)
                    ++current.view;
                REQUIRE_FALSE(capture.Complete(Completion(serial), kStart, current).Value());
                REQUIRE(capture.Snapshot().state == SaveThumbnailCaptureState::Omitted);
                REQUIRE(capture.Snapshot().error->code.Value() == SaveErrors::ThumbnailStale.code.Value());
            }
        }

        TEST_CASE("Late thumbnail results cannot overwrite a reused operation or newer publication", "[unit][save][thumbnail]") {
            SaveThumbnailCapture capture;
            const auto first = capture.Request(Request(), SaveThumbnailAvailability::Available, kStart).Value();
            REQUIRE_FALSE(capture.Cancel(first + 1));
            REQUIRE(capture.Cancel(first));
            REQUIRE(capture.Acknowledge(first));
            const auto second = capture.Request(Request(), SaveThumbnailAvailability::Available, kStart).Value();
            REQUIRE(second != first);
            REQUIRE_FALSE(capture.Complete(Completion(first), kStart, Request().source).Value());
            REQUIRE(capture.Snapshot().state == SaveThumbnailCaptureState::Pending);
            REQUIRE(capture.Complete(Completion(second), kStart, Request().source).Value());
            auto publication = Publication();
            publication.generation = Id<SlotGenerationId>(7);
            REQUIRE(MakeSaveCommittedPresentation(publication, {}, capture.Snapshot()).ErrorValue().code.Value() ==
                    SaveErrors::ThumbnailStale.code.Value());
            publication = Publication();
            publication.slot = Id<SaveGameSlotId>(7);
            REQUIRE(MakeSaveCommittedPresentation(publication, {}, capture.Snapshot()).HasError());
            publication = Publication();
            publication.thumbnail = Id<SaveThumbnailId>(7);
            REQUIRE(MakeSaveCommittedPresentation(publication, {}, capture.Snapshot()).HasError());
        }

        TEST_CASE("Replacement coordinator cannot attach an old completion to a newer generation", "[unit][save][thumbnail]") {
            SaveThumbnailCapture capture;
            auto request = Request();
            request.generation = Id<SlotGenerationId>(7);
            const auto serial = capture.Request(request, SaveThumbnailAvailability::Available, kStart).Value();
            REQUIRE(serial == 1);
            REQUIRE_FALSE(capture.Complete(Completion(serial), kStart, request.source).Value());
            REQUIRE(capture.Snapshot().state == SaveThumbnailCaptureState::Pending);
            auto completion = Completion(serial);
            completion.generation = request.generation;
            REQUIRE(capture.Complete(std::move(completion), kStart, request.source).Value());
        }

        TEST_CASE("Malformed and oversized thumbnails follow the explicit product requirement", "[unit][save][thumbnail]") {
            for (const auto variant : {0, 1, 2, 3, 4, 5}) {
                SaveThumbnailCapture capture;
                auto request = Request(SaveThumbnailPolicy::Required);
                request.limits.maximumEncodedBytes = 2;
                const auto serial = capture.Request(request, SaveThumbnailAvailability::Available, kStart).Value();
                auto completion = Completion(serial);
                if (variant == 0)
                    completion.encoded.clear();
                if (variant == 1)
                    completion.encoded.push_back(std::byte{3});
                if (variant == 2)
                    ++completion.width;
                if (variant == 3)
                    ++completion.height;
                if (variant == 4)
                    completion.source.frame = 9;
                if (variant == 5)
                    completion.format = static_cast<SaveThumbnailFormat>(255);
                REQUIRE(capture.Complete(std::move(completion), kStart, request.source).Value());
                REQUIRE(capture.Snapshot().state == SaveThumbnailCaptureState::Failed);
                REQUIRE_FALSE(capture.Snapshot().artifact);
            }
        }

        TEST_CASE("Provider errors retain identity and optional presentation never invalidates a save", "[unit][save][thumbnail]") {
            SaveThumbnailCapture capture;
            const auto serial = capture.Request(Request(), SaveThumbnailAvailability::Available, kStart).Value();
            auto completion = Completion(serial);
            completion.error = WrapError(SaveErrors::StorageCapabilityUnsupported, MakeError(SaveErrors::IdentityInvalid));
            REQUIRE(capture.Complete(std::move(completion), kStart, Request().source).Value());
            REQUIRE(capture.Snapshot().error->code.Value() == SaveErrors::StorageCapabilityUnsupported.code.Value());
            REQUIRE(capture.Snapshot().error->cause.Get() != nullptr);
            auto publication = Publication();
            publication.thumbnail.reset();
            const auto result = MakeSaveCommittedPresentation(publication, {.displayName = std::string{"\xC3\x28", 2}}, capture.Snapshot());
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().catalog.display.displayName.empty());
            REQUIRE_FALSE(result.Value().artifact);
            publication.thumbnail = Id<SaveThumbnailId>(6);
            REQUIRE(MakeSaveCommittedPresentation(publication, {}, capture.Snapshot()).HasError());
        }

        TEST_CASE("Shutdown rejects admission and retires only its pending thumbnail", "[unit][save][thumbnail]") {
            SaveThumbnailCapture capture;
            const auto serial =
                capture.Request(Request(SaveThumbnailPolicy::Required), SaveThumbnailAvailability::Available, kStart).Value();
            capture.BeginShutdown();
            capture.BeginShutdown();
            REQUIRE(capture.Snapshot().state == SaveThumbnailCaptureState::Failed);
            REQUIRE_FALSE(capture.Complete(Completion(serial), kStart, Request().source).Value());
            REQUIRE(capture.Acknowledge(serial));
            REQUIRE(capture.Request(Request(), SaveThumbnailAvailability::Available, kStart).HasError());
        }

        TEST_CASE("Zero bounds unsupported format and invalid publications fail without state mutation", "[unit][save][thumbnail]") {
            auto zeroDimension = Request();
            zeroDimension.limits.maximumDimension = 0;
            auto zeroBytes = Request();
            zeroBytes.limits.maximumEncodedBytes = 0;
            auto badFormat = Request();
            badFormat.format = static_cast<SaveThumbnailFormat>(255);
            auto noScene = Request();
            noScene.source.scene = 0;
            auto noFrame = Request();
            noFrame.source.frame = 0;
            for (const auto &request : {zeroDimension, zeroBytes, badFormat, noScene, noFrame}) {
                SaveThumbnailCapture capture;
                REQUIRE(capture.Request(request, SaveThumbnailAvailability::Available, kStart).HasError());
                REQUIRE(capture.Snapshot().state == SaveThumbnailCaptureState::Idle);
            }
            SaveThumbnailCapture capture;
            REQUIRE(capture.Request(Request(), static_cast<SaveThumbnailAvailability>(255), kStart).HasError());
            REQUIRE(capture.Request(Request(), SaveThumbnailAvailability::Available, kStart - std::chrono::milliseconds{1}).HasError());
            REQUIRE(MakeSaveCommittedPresentation({}, {}, capture.Snapshot()).HasError());
            REQUIRE(MakeSaveCommittedPresentation(Publication(), {}, capture.Snapshot()).HasError());
            REQUIRE_FALSE(capture.Cancel(0));
            REQUIRE_FALSE(capture.Acknowledge(0));
            REQUIRE(capture.Advance(kStart, {}).HasValue());
        }

        TEST_CASE("Thumbnail request identity and policy validation is atomic", "[unit][save][thumbnail]") {
            for (const auto variant : {0, 1, 2, 3, 4, 5}) {
                SaveThumbnailCapture capture;
                auto request = Request();
                if (variant == 0)
                    request.slot = {};
                if (variant == 1)
                    request.generation = {};
                if (variant == 2)
                    request.thumbnail = {};
                if (variant == 3)
                    request.source.runtime = 0;
                if (variant == 4)
                    request.source.view = 0;
                if (variant == 5)
                    request.policy = static_cast<SaveThumbnailPolicy>(255);
                REQUIRE(capture.Request(request, SaveThumbnailAvailability::Available, kStart).HasError());
                REQUIRE(capture.Snapshot().state == SaveThumbnailCaptureState::Idle);
                REQUIRE(capture.Snapshot().requestSerial == 0);
            }
        }

        TEST_CASE("Thumbnail request dimension and resource ceiling validation is atomic", "[unit][save][thumbnail]") {
            for (const auto variant : {0, 1, 2, 3, 4, 5}) {
                SaveThumbnailCapture capture;
                auto request = Request();
                if (variant == 0)
                    request.width = 0;
                if (variant == 1)
                    request.height = 1'025;
                if (variant == 2)
                    request.limits.maximumDimension = 4'097;
                if (variant == 3)
                    request.limits.maximumEncodedBytes = 16U * 1024U * 1024U + 1U;
                if (variant == 4)
                    request.limits.timeout = std::chrono::milliseconds{0};
                if (variant == 5)
                    request.limits.timeout = std::chrono::milliseconds{60'001};
                REQUIRE(capture.Request(request, SaveThumbnailAvailability::Available, kStart).HasError());
                REQUIRE(capture.Snapshot().state == SaveThumbnailCaptureState::Idle);
                REQUIRE(capture.Snapshot().requestSerial == 0);
            }
        }
    }  // namespace
}  // namespace Horo::Runtime
