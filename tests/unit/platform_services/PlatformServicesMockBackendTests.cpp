#include "Horo/PlatformServices/PlatformRequestErrors.h"
#include "PlatformServicesMockBackend.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::PlatformServices::TestSupport {
    namespace {
        void ActivateMock(MockPlatformServicesBackend &backend) {
            const auto activated = backend.Activate({});
            REQUIRE(activated.HasValue());
        }

        [[nodiscard]] bool HasDiagnostic(const MockPlatformServicesBackend &backend, const MockDiagnosticKind kind) {
            for (const auto &diagnostic : backend.Diagnostics()) {
                if (diagnostic.kind == kind)
                    return true;
            }
            return false;
        }
    }  // namespace

    TEST_CASE("Deterministic mock routes typed operations and replays bounded payloads in call order",
              "[platform-services][mock][routing]") {
        MockPlatformServicesBackend backend({7});
        REQUIRE(backend
                    .ExpectSequence({MockPlatformServicesOperation::UnlockAchievement, MockPlatformServicesOperation::SubmitScore,
                                     MockPlatformServicesOperation::WriteStat, MockPlatformServicesOperation::ReadCloudObject,
                                     MockPlatformServicesOperation::WriteCloudObject, MockPlatformServicesOperation::SetPresence,
                                     MockPlatformServicesOperation::ClearPresence, MockPlatformServicesOperation::QueryFriends,
                                     MockPlatformServicesOperation::QueryCurrentSession})
                    .HasValue());

        MockPlatformServicesResponse voidResponse;
        for (const auto operation : {MockPlatformServicesOperation::UnlockAchievement, MockPlatformServicesOperation::SubmitScore,
                                     MockPlatformServicesOperation::WriteStat, MockPlatformServicesOperation::WriteCloudObject,
                                     MockPlatformServicesOperation::SetPresence, MockPlatformServicesOperation::ClearPresence})
            REQUIRE(backend.SetResponse(operation, voidResponse).HasValue());

        MockPlatformServicesResponse cloudResponse;
        cloudResponse.payload = CloudReadResult{.object = {11}, .bytes = {std::byte{0x2a}}};
        REQUIRE(backend.SetResponse(MockPlatformServicesOperation::ReadCloudObject, std::move(cloudResponse)).HasValue());

        MockPlatformServicesResponse friendsResponse;
        friendsResponse.payload = FriendsPage{.entries = {}, .hasMore = true};
        REQUIRE(backend.SetResponse(MockPlatformServicesOperation::QueryFriends, std::move(friendsResponse)).HasValue());

        const auto sessionSnapshot = BuildPlatformSessionSnapshot(PlatformSessionCandidate{});
        REQUIRE(sessionSnapshot.HasValue());
        MockPlatformServicesResponse sessionResponse;
        sessionResponse.payload = std::move(sessionSnapshot).Value();
        REQUIRE(backend.SetResponse(MockPlatformServicesOperation::QueryCurrentSession, std::move(sessionResponse)).HasValue());

        ActivateMock(backend);
        auto achievement = backend.UnlockAchievement({});
        auto score = backend.SubmitScore({});
        auto stat = backend.WriteStat({});
        auto cloud = backend.ReadCloudObject({});
        auto cloudWrite = backend.WriteCloudObject({});
        auto presence = backend.SetPresence({});
        auto clearPresence = backend.ClearPresence({});
        auto friends = backend.QueryFriends({.pageSize = 1});
        auto session = backend.QueryCurrentSession();

        REQUIRE(achievement.HasValue());
        REQUIRE(score.HasValue());
        REQUIRE(stat.HasValue());
        REQUIRE(cloud.HasValue());
        REQUIRE(cloudWrite.HasValue());
        REQUIRE(presence.HasValue());
        REQUIRE(clearPresence.HasValue());
        REQUIRE(friends.HasValue());
        REQUIRE(session.HasValue());
        CHECK(backend.DispatchDueCompletions() == 9);

        const auto cloudSnapshot = backend.Requests().Query(cloud.Value());
        REQUIRE(cloudSnapshot.HasValue());
        REQUIRE(cloudSnapshot.Value().terminal.has_value());
        REQUIRE(cloudSnapshot.Value().terminal->Value() != nullptr);
        CHECK(cloudSnapshot.Value().terminal->Value()->object == CloudObjectId{11});
        CHECK(cloudSnapshot.Value().terminal->Value()->bytes == std::vector<std::byte>{std::byte{0x2a}});

        const auto friendsSnapshot = backend.Requests().Query(friends.Value());
        REQUIRE(friendsSnapshot.HasValue());
        REQUIRE(friendsSnapshot.Value().terminal.has_value());
        REQUIRE(friendsSnapshot.Value().terminal->Value() != nullptr);
        CHECK(friendsSnapshot.Value().terminal->Value()->hasMore);
        CHECK(backend.Calls().size() == 9);
        REQUIRE(backend.CompletionOrder().size() == 9);
        for (std::size_t index = 0; index < backend.Calls().size(); ++index)
            CHECK(backend.CompletionOrder()[index].operation == backend.Calls()[index]);
        CHECK(backend.VerifyExpectations().HasValue());
    }

    TEST_CASE("Manual clock reorders completions and resolves deadline ties before timeout",
              "[platform-services][mock][ordering][timeout]") {
        MockPlatformServicesBackend backend({7});
        REQUIRE(backend
                    .ExpectSequence({MockPlatformServicesOperation::QueryFriends, MockPlatformServicesOperation::ReadCloudObject,
                                     MockPlatformServicesOperation::SetPresence})
                    .HasValue());

        MockPlatformServicesResponse friendsResponse;
        friendsResponse.payload = FriendsPage{};
        friendsResponse.delay = std::chrono::milliseconds{4};
        friendsResponse.timeoutAfter = std::chrono::milliseconds{4};
        REQUIRE(backend.SetResponse(MockPlatformServicesOperation::QueryFriends, std::move(friendsResponse)).HasValue());

        MockPlatformServicesResponse cloudResponse;
        cloudResponse.payload = CloudReadResult{.object = {3}, .bytes = {std::byte{0x03}}};
        cloudResponse.delay = std::chrono::milliseconds{10};
        cloudResponse.timeoutAfter = std::chrono::milliseconds{4};
        REQUIRE(backend.SetResponse(MockPlatformServicesOperation::ReadCloudObject, std::move(cloudResponse)).HasValue());

        MockPlatformServicesResponse presenceResponse;
        presenceResponse.delay = std::chrono::milliseconds{1};
        presenceResponse.duplicateDelay = std::chrono::milliseconds{1};
        REQUIRE(backend.SetResponse(MockPlatformServicesOperation::SetPresence, std::move(presenceResponse)).HasValue());

        ActivateMock(backend);
        auto friends = backend.QueryFriends({.pageSize = 1});
        auto cloud = backend.ReadCloudObject({});
        auto presence = backend.SetPresence({});
        REQUIRE(friends.HasValue());
        REQUIRE(cloud.HasValue());
        REQUIRE(presence.HasValue());

        REQUIRE(backend.AdvanceClock(std::chrono::milliseconds{1}).HasValue());
        CHECK(backend.DispatchDueCompletions() == 1);
        REQUIRE(backend.AdvanceClock(std::chrono::milliseconds{1}).HasValue());
        CHECK(backend.DispatchDueCompletions() == 1);
        CHECK(HasDiagnostic(backend, MockDiagnosticKind::DuplicateCompletionIgnored));

        REQUIRE(backend.AdvanceClock(std::chrono::milliseconds{2}).HasValue());
        CHECK(backend.CurrentTimeMilliseconds() == 4);
        CHECK(backend.DispatchDueCompletions() == 3);
        CHECK(backend.Requests().Query(friends.Value()).Value().state == PlatformRequestState::Succeeded);
        const auto timedOutCloud = backend.Requests().Query(cloud.Value());
        REQUIRE(timedOutCloud.HasValue());
        CHECK(timedOutCloud.Value().state == PlatformRequestState::TimedOut);
        REQUIRE(timedOutCloud.Value().terminal.has_value());
        REQUIRE(timedOutCloud.Value().terminal->ErrorValue() != nullptr);
        CHECK(timedOutCloud.Value().terminal->ErrorValue()->code.Value() == "platform.request.timed_out");
        REQUIRE(backend.CompletionOrder().size() >= 5);
        CHECK(backend.CompletionOrder()[2].operation == MockPlatformServicesOperation::QueryFriends);
        CHECK(backend.CompletionOrder()[3].operation == MockPlatformServicesOperation::QueryFriends);
        CHECK(backend.CompletionOrder()[4].operation == MockPlatformServicesOperation::ReadCloudObject);

        REQUIRE(backend.AdvanceClock(std::chrono::milliseconds{6}).HasValue());
        CHECK(backend.DispatchDueCompletions() == 1);
        CHECK(backend.Requests().Query(cloud.Value()).Value().state == PlatformRequestState::TimedOut);
        CHECK(HasDiagnostic(backend, MockDiagnosticKind::LateCompletionIgnored));
        CHECK(backend.VerifyExpectations().HasValue());
    }

    TEST_CASE("Mock replays scripted provider failures with their typed error identity", "[platform-services][mock][errors]") {
        const ErrorCodeDescriptor providerFailure{.domain = ErrorDomainId{"horo.platform.provider"},
                                                  .code = ErrorCode{"platform.provider.failed"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "The test provider failed.",
                                                  .remediationHint = "Inspect the scripted provider outcome.",
                                                  .retryable = true,
                                                  .userActionable = false};
        MockPlatformServicesBackend backend({7});
        REQUIRE(backend.ExpectSequence({MockPlatformServicesOperation::SubmitScore}).HasValue());
        MockPlatformServicesResponse response;
        response.failure = MockPlatformServicesFailure{.descriptor = &providerFailure, .message = "scripted failure"};
        REQUIRE(backend.SetResponse(MockPlatformServicesOperation::SubmitScore, std::move(response)).HasValue());
        ActivateMock(backend);

        auto request = backend.SubmitScore({});
        REQUIRE(request.HasValue());
        CHECK(backend.DispatchDueCompletions() == 1);
        const auto snapshot = backend.Requests().Query(request.Value());
        REQUIRE(snapshot.HasValue());
        CHECK(snapshot.Value().state == PlatformRequestState::Failed);
        REQUIRE(snapshot.Value().terminal.has_value());
        REQUIRE(snapshot.Value().terminal->ErrorValue() != nullptr);
        CHECK(snapshot.Value().terminal->ErrorValue()->code.Value() == "platform.provider.failed");
        CHECK(snapshot.Value().terminal->ErrorValue()->message == "scripted failure");
        CHECK(backend.VerifyExpectations().HasValue());
    }

    TEST_CASE("Mock cancellation is delayed deterministically and late provider completion cannot replace it",
              "[platform-services][mock][cancellation]") {
        MockPlatformServicesBackend backend({7});
        REQUIRE(
            backend.ExpectSequence({MockPlatformServicesOperation::QueryFriends, MockPlatformServicesOperation::RequestCancel}).HasValue());

        MockPlatformServicesResponse response;
        response.payload = FriendsPage{};
        response.delay = std::chrono::milliseconds{10};
        response.cancellationDelay = std::chrono::milliseconds{3};
        REQUIRE(backend.SetResponse(MockPlatformServicesOperation::QueryFriends, std::move(response)).HasValue());
        ActivateMock(backend);

        auto request = backend.QueryFriends({.pageSize = 1});
        REQUIRE(request.HasValue());
        REQUIRE(backend.RequestCancel(request.Value().Id(), request.Value().Generation()).HasValue());
        CHECK(backend.Requests().Query(request.Value()).Value().state == PlatformRequestState::Cancelling);

        REQUIRE(backend.AdvanceClock(std::chrono::milliseconds{3}).HasValue());
        CHECK(backend.DispatchDueCompletions() == 1);
        CHECK(backend.Requests().Query(request.Value()).Value().state == PlatformRequestState::Cancelled);

        REQUIRE(backend.AdvanceClock(std::chrono::milliseconds{7}).HasValue());
        CHECK(backend.DispatchDueCompletions() == 1);
        CHECK(backend.Requests().Query(request.Value()).Value().state == PlatformRequestState::Cancelled);
        CHECK(HasDiagnostic(backend, MockDiagnosticKind::LateCompletionIgnored));
        CHECK(backend.VerifyExpectations().HasValue());
    }

    TEST_CASE("Mock reports unexpected and missing provider calls with typed bounded diagnostics",
              "[platform-services][mock][diagnostics]") {
        MockPlatformServicesBackend backend({7});
        REQUIRE(backend.ExpectSequence({MockPlatformServicesOperation::ReadCloudObject}).HasValue());
        ActivateMock(backend);

        CHECK(backend.QueryFriends({.pageSize = 1}).HasError());
        CHECK_FALSE(backend.AllExpectationsMet());
        CHECK(backend.VerifyExpectations().HasError());
        CHECK(HasDiagnostic(backend, MockDiagnosticKind::UnexpectedCall));
        CHECK(HasDiagnostic(backend, MockDiagnosticKind::MissingExpectedCall));

        MockPlatformServicesBackend overflow({7});
        REQUIRE(overflow.ExpectSequence({}).HasValue());
        ActivateMock(overflow);
        for (std::size_t index = 0; index < MockPlatformServicesBackend::MaximumDiagnostics + 12; ++index)
            CHECK(overflow.QueryCurrentSession().HasError());
        CHECK(overflow.Diagnostics().size() == MockPlatformServicesBackend::MaximumDiagnostics);
        CHECK(overflow.DiagnosticOverflowCount() > 0);
        CHECK(overflow.Diagnostics().back().kind == MockDiagnosticKind::DiagnosticOverflow);
    }

    TEST_CASE("Mock rejects oversized scripted payloads and request payloads before admission", "[platform-services][mock][bounds]") {
        MockPlatformServicesBackend backend({7});
        MockPlatformServicesResponse oversized;
        oversized.payload =
            CloudReadResult{.object = {1}, .bytes = std::vector<std::byte>(MockPlatformServicesBackend::MaximumPayloadBytes + 1)};
        CHECK(backend.SetResponse(MockPlatformServicesOperation::ReadCloudObject, std::move(oversized)).HasError());
        CHECK(HasDiagnostic(backend, MockDiagnosticKind::InvalidScript));

        REQUIRE(backend.ExpectSequence({MockPlatformServicesOperation::WriteCloudObject}).HasValue());
        ActivateMock(backend);
        CloudWriteRequest request{.object = {1}, .bytes = std::vector<std::byte>(MockPlatformServicesBackend::MaximumPayloadBytes + 1)};
        CHECK(backend.WriteCloudObject(std::move(request)).HasError());
        CHECK(HasDiagnostic(backend, MockDiagnosticKind::InvalidRequest));
        CHECK(backend.Requests().RecordCount() == 0);
    }
}  // namespace Horo::PlatformServices::TestSupport
