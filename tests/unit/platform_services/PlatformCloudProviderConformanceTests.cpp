#include "Horo/PlatformServices/PlatformServiceErrors.h"
#include "PlatformCloudTestSupport.h"
#include "PlatformServicesMockBackend.h"

namespace Horo::PlatformServices::TestSupport {
    namespace {
        using Operation = MockPlatformServicesOperation;
        using namespace std::chrono_literals;

        /** @brief Models an older adapter implementing only the legacy read surface. */
        class LegacyCloudProvider final : public ICloudService {
        public:
            std::uint32_t legacyReads{};

            /** @brief Records any fallback into the older object-ID route. */
            Result<PlatformRequestHandle<CloudReadResult>> ReadCloudObject(CloudReadRequest) override {
                ++legacyReads;
                return Result<PlatformRequestHandle<CloudReadResult>>::Failure(MakeError(BackendErrors::ServiceUnavailable));
            }
        };

        /** @brief Activates one network-free provider with an explicit session and exact operation script. */
        void Prepare(MockPlatformServicesBackend &backend, std::vector<Operation> sequence) {
            REQUIRE(backend.ExpectSequence(std::move(sequence)).HasValue());
            REQUIRE(backend.SetCloudSubject(CloudSubject()).HasValue());
            REQUIRE(backend.Activate({}).HasValue());
        }

        /** @brief Confirms one immutable terminal failure without accepting a payload as success. */
        template <typename T>
        void TerminalError(MockPlatformServicesBackend &backend, const PlatformRequestHandle<T> &handle, const ErrorCodeDescriptor &error) {
            const auto snapshot = backend.Requests().Query(handle);
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().terminal.has_value());
            REQUIRE(snapshot.Value().terminal->ErrorValue() != nullptr);
            CHECK(snapshot.Value().terminal->ErrorValue()->code.Value() == error.code.Value());
        }

        /** @brief Replaces the session between cloud admission and provider completion for any typed result. */
        template <typename Submit>
        void RejectStaleCloudResult(const Operation operation, MockPlatformServicesResponse response, Submit submit) {
            MockPlatformServicesBackend backend;
            Prepare(backend, {operation});
            response.delay = 1ms;
            REQUIRE(backend.SetResponse(operation, std::move(response)).HasValue());
            const auto pending = submit(backend);
            REQUIRE(pending.HasValue());
            REQUIRE(backend.SetCloudSubject(CloudSubject(2, std::byte{2})).HasValue());
            REQUIRE(backend.AdvanceClock(1ms).HasValue());
            REQUIRE(backend.DispatchDueCompletions() == 1);
            TerminalError(backend, pending.Value(), CloudObjectErrors::StaleSession);
            CHECK(backend.VerifyExpectations().HasValue());
        }
    }  // namespace

    TEST_CASE("Legacy cloud adapters reject opaque operations without implicit fallback", "[platform-services][cloud][conformance]") {
        LegacyCloudProvider legacy;
        ICloudService &cloud = legacy;
        const auto write = CloudWrite();
        const auto commit = CloudCommit(write);
        RequireError(cloud.ListCloudObjects({.subject = write.subject, .pageSize = 1}), CloudObjectErrors::UnsupportedCapability);
        RequireError(cloud.ReadCloudObject(CloudBlobReadRequest{write.subject, write.key, 1}), CloudObjectErrors::UnsupportedCapability);
        RequireError(cloud.WriteCloudObject(CloudBlobWriteRequest{write}), CloudObjectErrors::UnsupportedCapability);
        RequireError(cloud.DeleteCloudObject({write.subject, write.key, commit.committedObject->revision, write.mutation}),
                     CloudObjectErrors::UnsupportedCapability);
        RequireError(cloud.QueryCloudQuota(write.subject), CloudObjectErrors::UnsupportedCapability);
        CHECK(legacy.legacyReads == 0);
    }

    TEST_CASE("Cloud providers expose precise Null or complete scripted opaque operations", "[platform-services][cloud][conformance]") {
        const auto write = CloudWrite();
        const auto commit = CloudCommit(write);
        const CloudListRequest list{.subject = write.subject, .pageSize = 1};
        const CloudBlobReadRequest read{.subject = write.subject, .key = write.key, .maximumBytes = 1};
        const CloudBlobDeleteRequest remove{.subject = write.subject,
                                            .key = write.key,
                                            .expectedRevision = commit.committedObject->revision,
                                            .mutation = write.mutation};
        NullPlatformServicesBackend null;
        REQUIRE(null.Activate({}).HasValue());
        ICloudService &absent = null;
        RequireError(absent.ListCloudObjects(list), BackendErrors::NullProvider);
        RequireError(absent.ReadCloudObject(read), BackendErrors::NullProvider);
        RequireError(absent.WriteCloudObject(CloudBlobWriteRequest{write}), BackendErrors::NullProvider);
        RequireError(absent.DeleteCloudObject(CloudBlobDeleteRequest{remove}), BackendErrors::NullProvider);
        RequireError(absent.QueryCloudQuota(write.subject), BackendErrors::NullProvider);

        MockPlatformServicesBackend backend;
        Prepare(backend, {Operation::ListCloudObjects, Operation::ReadCloudBlob, Operation::WriteCloudObject, Operation::DeleteCloudObject,
                          Operation::QueryCloudQuota});
        MockPlatformServicesResponse response;
        response.payload = CloudObjectPage{.subject = write.subject,
                                           .sessionGeneration = write.subject.SessionGeneration(),
                                           .objects = {*commit.committedObject}};
        REQUIRE(backend.SetResponse(Operation::ListCloudObjects, response).HasValue());
        response.payload = CloudBlobReadResult{.subject = write.subject,
                                               .sessionGeneration = write.subject.SessionGeneration(),
                                               .head = *commit.committedObject,
                                               .bytes = write.bytes};
        REQUIRE(backend.SetResponse(Operation::ReadCloudBlob, response).HasValue());
        response.payload = commit;
        REQUIRE(backend.SetResponse(Operation::WriteCloudObject, response).HasValue());
        auto deletion = commit;
        deletion.committedObject.reset();
        response.payload = deletion;
        REQUIRE(backend.SetResponse(Operation::DeleteCloudObject, response).HasValue());
        response.payload = CloudQuotaObservation{.subject = write.subject,
                                                 .sessionGeneration = write.subject.SessionGeneration(),
                                                 .usedBytes = 1,
                                                 .objectCount = 1};
        REQUIRE(backend.SetResponse(Operation::QueryCloudQuota, response).HasValue());
        ICloudService &cloud = backend;
        const auto listed = cloud.ListCloudObjects(list);
        const auto downloaded = cloud.ReadCloudObject(read);
        const auto uploaded = cloud.WriteCloudObject(CloudBlobWriteRequest{write});
        const auto deleted = cloud.DeleteCloudObject(CloudBlobDeleteRequest{remove});
        const auto quota = cloud.QueryCloudQuota(write.subject);
        REQUIRE(listed.HasValue());
        REQUIRE(downloaded.HasValue());
        REQUIRE(uploaded.HasValue());
        REQUIRE(deleted.HasValue());
        REQUIRE(quota.HasValue());
        CHECK(backend.DispatchDueCompletions() == 5);
        CHECK(backend.Requests().Query(listed.Value()).Value().state == PlatformRequestState::Succeeded);
        CHECK(backend.Requests().Query(downloaded.Value()).Value().terminal->Value()->bytes == write.bytes);
        CHECK(backend.Requests().Query(uploaded.Value()).Value().state == PlatformRequestState::Succeeded);
        CHECK(backend.Requests().Query(deleted.Value()).Value().state == PlatformRequestState::Succeeded);
        CHECK(backend.Requests().Query(quota.Value()).Value().terminal->Value()->usedBytes == 1);
        CHECK(backend.VerifyExpectations().HasValue());
    }

    TEST_CASE("Cloud interruption publishes once and retains native transfer capacity until retirement",
              "[platform-services][cloud][conformance]") {
        MockPlatformServicesBackend backend;
        std::vector<Operation> sequence(MockPlatformServicesBackend::MaximumCloudTransfers, Operation::WriteCloudObject);
        sequence.insert(sequence.end(), {Operation::RequestCancel, Operation::RequestCancel, Operation::WriteCloudObject});
        Prepare(backend, std::move(sequence));
        MockPlatformServicesResponse response;
        response.payload = CloudCommit(CloudWrite());
        response.delay = 20ms;
        response.timeoutAfter = 5ms;
        response.duplicateDelay = 10ms;
        for (std::uint32_t i = 0; i <= MockPlatformServicesBackend::MaximumCloudTransfers; ++i)
            REQUIRE(backend.SetResponse(Operation::WriteCloudObject, response).HasValue());
        std::vector<PlatformRequestHandle<CloudMutationResult>> handles;
        for (std::uint32_t i = 0; i < MockPlatformServicesBackend::MaximumCloudTransfers; ++i) {
            auto admitted = backend.WriteCloudObject(CloudWrite());
            REQUIRE(admitted.HasValue());
            handles.push_back(std::move(admitted).Value());
        }
        RequireError(backend.WriteCloudObject(CloudWrite()), RequestErrors::CapacityExceeded);
        std::uint32_t notifications{};
        auto subscription =
            backend.Requests().OnComplete<CloudMutationResult>(handles.front(),
                                                               [&notifications](
                                                                   const PlatformRequestSnapshot<CloudMutationResult> &snapshot) {
            ++notifications;
            CHECK(snapshot.state == PlatformRequestState::Cancelled);
        });
        REQUIRE(subscription.HasValue());
        REQUIRE(backend.RequestCancel(handles.front().Id(), handles.front().Generation()).HasValue());
        REQUIRE(backend.RequestCancel(handles.front().Id(), handles.front().Generation()).HasValue());
        CHECK(backend.DispatchDueCompletions() == 1);
        TerminalError(backend, handles.front(), RequestErrors::Cancelled);
        CHECK(notifications == 0);
        CHECK(backend.Requests().DispatchCompletions() == 1);
        CHECK(notifications == 1);
        REQUIRE(backend.AdvanceClock(5ms).HasValue());
        CHECK(backend.DispatchDueCompletions() == MockPlatformServicesBackend::MaximumCloudTransfers);
        TerminalError(backend, handles.back(), RequestErrors::TimedOut);
        RequireError(backend.WriteCloudObject(CloudWrite()), RequestErrors::CapacityExceeded);
        REQUIRE(backend.AdvanceClock(25ms).HasValue());
        CHECK(backend.DispatchDueCompletions() == 2 * MockPlatformServicesBackend::MaximumCloudTransfers);
        TerminalError(backend, handles.front(), RequestErrors::Cancelled);
        TerminalError(backend, handles.back(), RequestErrors::TimedOut);
        REQUIRE(backend.WriteCloudObject(CloudWrite()).HasValue());
        CHECK(backend.Requests().DispatchCompletions() == 0);
        CHECK(notifications == 1);
        CHECK(backend.VerifyExpectations().HasValue());
        CHECK(backend.Calls().size() == MockPlatformServicesBackend::MaximumCloudTransfers + 3);
    }

    TEST_CASE("Cloud account replacement fences delayed results including provider failures", "[platform-services][cloud][conformance]") {
        MockPlatformServicesBackend backend;
        Prepare(backend, {Operation::WriteCloudObject});
        MockPlatformServicesResponse response;
        response.payload = CloudCommit(CloudWrite());
        response.delay = 1ms;
        SECTION("success after account replacement") {
            REQUIRE(backend.SetResponse(Operation::WriteCloudObject, response).HasValue());
            const auto pending = backend.WriteCloudObject(CloudWrite());
            REQUIRE(pending.HasValue());
            REQUIRE(backend.SetCloudSubject(CloudSubject(2, std::byte{2})).HasValue());
            RequireError(backend.SetCloudSubject(CloudSubject()), CloudObjectErrors::StaleSession);
            REQUIRE(backend.AdvanceClock(1ms).HasValue());
            REQUIRE(backend.DispatchDueCompletions() == 1);
            TerminalError(backend, pending.Value(), CloudObjectErrors::StaleSession);
        }
        SECTION("failure after sign-out") {
            response.payload = std::monostate{};
            response.failure = MockPlatformServicesFailure{&CloudObjectErrors::QuotaExceeded, "private-payload-account-token"};
            REQUIRE(backend.SetResponse(Operation::WriteCloudObject, response).HasValue());
            const auto pending = backend.WriteCloudObject(CloudWrite());
            REQUIRE(pending.HasValue());
            REQUIRE(backend.SetCloudSubject(std::nullopt).HasValue());
            RequireError(backend.SetCloudSubject(CloudSubject()), CloudObjectErrors::StaleSession);
            REQUIRE(backend.AdvanceClock(1ms).HasValue());
            REQUIRE(backend.DispatchDueCompletions() == 1);
            TerminalError(backend, pending.Value(), CloudObjectErrors::StaleSession);
        }
        CHECK(backend.VerifyExpectations().HasValue());
    }

    TEST_CASE("Cloud scripts reject incomplete reads and preserve portable mutation failures", "[platform-services][cloud][conformance]") {
        const auto write = CloudWrite();
        const auto commit = CloudCommit(write);
        MockPlatformServicesBackend backend;
        SECTION("partial read never publishes bytes") {
            Prepare(backend, {Operation::ReadCloudBlob});
            auto head = *commit.committedObject;
            head.sizeBytes = 2;
            MockPlatformServicesResponse response;
            response.payload = CloudBlobReadResult{.subject = write.subject,
                                                   .sessionGeneration = write.subject.SessionGeneration(),
                                                   .head = head,
                                                   .bytes = write.bytes};
            REQUIRE(backend.SetResponse(Operation::ReadCloudBlob, response).HasValue());
            const auto pending = backend.ReadCloudObject(CloudBlobReadRequest{write.subject, write.key, 2});
            REQUIRE(pending.HasValue());
            REQUIRE(backend.DispatchDueCompletions() == 1);
            CHECK(backend.Requests().Query(pending.Value()).Value().state == PlatformRequestState::Failed);
            TerminalError(backend, pending.Value(), CloudObjectErrors::PartialTransfer);
            CHECK(backend.Requests().Query(pending.Value()).Value().terminal->Value() == nullptr);
        }
        SECTION("conflict and quota do not become success or automatic retries") {
            Prepare(backend, {Operation::WriteCloudObject, Operation::WriteCloudObject, Operation::WriteCloudObject});
            for (const auto *error :
                 {&CloudObjectErrors::PreconditionFailed, &CloudObjectErrors::QuotaExceeded, &CloudObjectErrors::IdempotencyConflict}) {
                MockPlatformServicesResponse response;
                response.failure = MockPlatformServicesFailure{error, "private-payload-account-token"};
                REQUIRE(backend.SetResponse(Operation::WriteCloudObject, response).HasValue());
                const auto pending = backend.WriteCloudObject(CloudWrite());
                REQUIRE(pending.HasValue());
                REQUIRE(backend.DispatchDueCompletions() == 1);
                TerminalError(backend, pending.Value(), *error);
                CHECK(backend.Requests().Query(pending.Value()).Value().terminal->ErrorValue()->message.find("private-") ==
                      std::string::npos);
            }
        }
        CHECK(backend.VerifyExpectations().HasValue());
    }

    TEST_CASE("Every opaque cloud result is fenced by the current session", "[platform-services][cloud][conformance]") {
        const auto write = CloudWrite();
        const auto commit = CloudCommit(write);
        MockPlatformServicesResponse response;
        SECTION("metadata page") {
            response.payload = CloudObjectPage{.subject = write.subject,
                                               .sessionGeneration = write.subject.SessionGeneration(),
                                               .objects = {*commit.committedObject}};
            RejectStaleCloudResult(Operation::ListCloudObjects, std::move(response), [&write](MockPlatformServicesBackend &backend) {
                return backend.ListCloudObjects({.subject = write.subject, .pageSize = 1});
            });
        }
        SECTION("complete read") {
            response.payload = CloudBlobReadResult{.subject = write.subject,
                                                   .sessionGeneration = write.subject.SessionGeneration(),
                                                   .head = *commit.committedObject,
                                                   .bytes = write.bytes};
            RejectStaleCloudResult(Operation::ReadCloudBlob, std::move(response), [&write](MockPlatformServicesBackend &backend) {
                return backend.ReadCloudObject(CloudBlobReadRequest{write.subject, write.key, 1});
            });
        }
        SECTION("delete commit") {
            auto deletion = commit;
            deletion.committedObject.reset();
            response.payload = deletion;
            RejectStaleCloudResult(Operation::DeleteCloudObject, std::move(response),
                                   [&write, &commit](MockPlatformServicesBackend &backend) {
                return backend.DeleteCloudObject({write.subject, write.key, commit.committedObject->revision, write.mutation});
            });
        }
        SECTION("quota observation") {
            response.payload = CloudQuotaObservation{.subject = write.subject,
                                                     .sessionGeneration = write.subject.SessionGeneration(),
                                                     .usedBytes = 1,
                                                     .objectCount = 1};
            RejectStaleCloudResult(Operation::QueryCloudQuota, std::move(response), [&write](MockPlatformServicesBackend &backend) {
                return backend.QueryCloudQuota(write.subject);
            });
        }
    }

    TEST_CASE("Cloud provider normalization preserves categories without native text", "[platform-services][cloud][conformance]") {
        MockPlatformServicesBackend backend;
        Prepare(backend, {Operation::WriteCloudObject, Operation::WriteCloudObject, Operation::WriteCloudObject});
        for (const auto category : {PlatformProviderFailureCategory::Offline, PlatformProviderFailureCategory::RateLimited,
                                    PlatformProviderFailureCategory::TransientFailure}) {
            MockPlatformServicesResponse response;
            response.providerFailure = category;
            REQUIRE(backend.SetResponse(Operation::WriteCloudObject, response).HasValue());
            const auto pending = backend.WriteCloudObject(CloudWrite());
            REQUIRE(pending.HasValue());
            REQUIRE(backend.DispatchDueCompletions() == 1);
            const auto snapshot = backend.Requests().Query(pending.Value());
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().terminal->ErrorValue());
            CHECK(PlatformProviderCategory(*snapshot.Value().terminal->ErrorValue()) == category);
        }
        CHECK(backend.VerifyExpectations().HasValue());
    }

    TEST_CASE("Cloud scripts apply advertised bounds and close callback admission on shutdown", "[platform-services][cloud][conformance]") {
        MockPlatformServicesBackend backend;
        Prepare(backend, {Operation::ReadCloudBlob});
        const auto write = CloudWrite();
        const auto commit = CloudCommit(write);
        RequireError(backend.ReadCloudObject(
                         CloudBlobReadRequest{write.subject, write.key, MockPlatformServicesBackend::MaximumPayloadBytes + 1}),
                     CloudObjectErrors::PayloadTooLarge);
        RequireError(backend.ListCloudObjects({.subject = write.subject, .pageSize = MockPlatformServicesBackend::MaximumPageEntries + 1}),
                     CloudObjectErrors::InvalidRequest);
        MockPlatformServicesResponse response;
        response.payload = CloudBlobReadResult{.subject = write.subject,
                                               .sessionGeneration = write.subject.SessionGeneration(),
                                               .head = *commit.committedObject,
                                               .bytes = write.bytes};
        response.delay = 10ms;
        REQUIRE(backend.SetResponse(Operation::ReadCloudBlob, response).HasValue());
        auto pending = backend.ReadCloudObject(CloudBlobReadRequest{write.subject, write.key, 1});
        REQUIRE(pending.HasValue());
        std::uint32_t notifications{};
        auto subscription =
            backend.Requests().OnComplete<CloudBlobReadResult>(pending.Value(),
                                                               [&notifications](const PlatformRequestSnapshot<CloudBlobReadResult> &) {
            ++notifications;
        });
        REQUIRE(subscription.HasValue());
        REQUIRE(backend.Shutdown().HasValue());
        REQUIRE(backend.Shutdown().HasValue());
        CHECK(backend.DispatchDueCompletions() == 0);
        CHECK(backend.Requests().DispatchCompletions() == 0);
        CHECK(notifications == 0);
        RequireError(backend.ReadCloudObject(CloudBlobReadRequest{write.subject, write.key, 1}), RequestErrors::FrontendUnavailable);
        CHECK(backend.VerifyExpectations().HasValue());
    }
}  // namespace Horo::PlatformServices::TestSupport
