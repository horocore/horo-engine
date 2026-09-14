#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveParticipation.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace Horo::Runtime {
    namespace {
        class TestCaptureAdapter final : public ISaveParticipationCaptureAdapter {
        public:
            explicit TestCaptureAdapter(std::shared_ptr<int> destructionCount) : destructionCount_(std::move(destructionCount)) {}

            ~TestCaptureAdapter() override {
                ++*destructionCount_;
            }

            Result<CanonicalCaptureDisposition> Capture(const CanonicalCaptureContext &, ICanonicalCaptureSink &) const override {
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
            }

        private:
            std::shared_ptr<int> destructionCount_;
        };

        class TestOperationHost final : public ISaveParticipationOperationHost {
        public:
            Result<SaveOperationHandle> RequestSave(const SaveParticipationSaveRequest &request) override {
                if (throwSaveAllocation)
                    throw std::bad_alloc{};
                lastSave = request;
                return Admit(SaveOperationKind::Save);
            }

            Result<SaveOperationHandle> RequestLoad(const SaveParticipationLoadRequest &request) override {
                if (throwLoadFailure)
                    throw std::runtime_error{"load callback failure"};
                lastLoad = request;
                return Admit(SaveOperationKind::Load);
            }

            SaveParticipationSaveRequest lastSave;
            SaveParticipationLoadRequest lastLoad;
            bool throwSaveAllocation{};
            bool throwLoadFailure{};

        private:
            Result<SaveOperationHandle> Admit(const SaveOperationKind kind) {
                auto operation = CreateSaveOperation({.operation = nextOperation_++, .kind = kind, .maximumCompletionCallbacks = 1});
                if (operation.HasError())
                    return Result<SaveOperationHandle>::Failure(operation.ErrorValue());
                const SaveOperationHandle handle = operation.Value().Handle();
                operations_.push_back(std::move(operation).Value());
                return Result<SaveOperationHandle>::Success(handle);
            }

            OperationId nextOperation_{1};
            std::vector<SaveOperationController> operations_;
        };

        SaveGameSlotId Slot() {
            return SaveGameSlotId::Parse("00112233-4455-6677-8899-aabbccddeeff").Value();
        }

        CanonicalStateParticipantDescriptor Descriptor(const SaveParticipantRole roles) {
            return {
                .participant = SaveParticipantId::Parse("game.progress.campaign").Value(),
                .schemaVersion = ParticipantSchemaVersion::Create(3).Value(),
                .scope = SaveParticipantScope::SlotPlayer,
                .roles = roles,
                .required = true,
                .limits = {.maximumPayloadBytes = 4096, .maximumRecordCount = 1, .maximumNestingDepth = 8},
                .dependencies = {},
                .ownedRecords = {SaveRecordId::Parse("10112233-4455-6677-8899-aabbccddeeff").Value()},
            };
        }

        SaveParticipationCapabilities AllCapabilities() {
            return {.captureParticipants = true, .restoreParticipants = true, .saveRequests = true, .loadRequests = true};
        }

        TEST_CASE("Save participation requests expose only typed logical slots", "[unit][runtime][save][participation]") {
            CanonicalStateParticipantRegistry registry;
            TestOperationHost operations;
            auto created = SaveParticipationHost::Create(7, AllCapabilities(), registry, operations);
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            const SaveParticipationClient client = host.Client();

            const SaveParticipationSaveRequest save{.slot = Slot(), .mode = SavePolicyMode::Checkpoint};
            const auto saveHandle = client.RequestSave(save);
            REQUIRE(saveHandle.HasValue());
            CHECK(saveHandle.Value().IsValid());
            CHECK(operations.lastSave == save);

            const SaveParticipationLoadRequest load{.slot = Slot()};
            const auto loadHandle = client.RequestLoad(load);
            REQUIRE(loadHandle.HasValue());
            CHECK(loadHandle.Value().IsValid());
            CHECK(operations.lastLoad == load);

            CHECK(client.RequestSave({}).ErrorValue().code.Value() == SaveErrors::OperationInvalid.code.Value());
            CHECK(client.RequestSave({.slot = Slot(), .mode = SavePolicyMode::Load}).ErrorValue().code.Value() ==
                  SaveErrors::OperationInvalid.code.Value());
            CHECK(client.RequestLoad({}).ErrorValue().code.Value() == SaveErrors::OperationInvalid.code.Value());
        }

        TEST_CASE("Save participation enforces exact version and declared capabilities", "[unit][runtime][save][participation]") {
            CanonicalStateParticipantRegistry registry;
            TestOperationHost operations;
            auto incompatible = AllCapabilities();
            incompatible.apiVersion = SaveParticipationApiVersion + 1;
            CHECK(SaveParticipationHost::Create(1, incompatible, registry, operations).ErrorValue().code.Value() ==
                  SaveErrors::LifecycleInvalid.code.Value());
            CHECK(SaveParticipationHost::Create(0, AllCapabilities(), registry, operations).ErrorValue().code.Value() ==
                  SaveErrors::LifecycleInvalid.code.Value());

            SaveParticipationCapabilities captureOnly{.captureParticipants = true};
            auto created = SaveParticipationHost::Create(2, captureOnly, registry, operations);
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            const auto client = host.Client();
            auto destructionCount = std::make_shared<int>();
            auto adapter = std::make_shared<TestCaptureAdapter>(destructionCount);
            REQUIRE(client.RegisterParticipant(Descriptor(SaveParticipantRole::Capture), adapter).HasValue());
            CHECK(client.RegisterParticipant(Descriptor(SaveParticipantRole::Restore), adapter).ErrorValue().code.Value() ==
                  SaveErrors::PolicyCapabilityUnsupported.code.Value());
            CHECK(client.RequestSave({.slot = Slot()}).ErrorValue().code.Value() == SaveErrors::PolicyCapabilityUnsupported.code.Value());
            CHECK(client.RequestLoad({.slot = Slot()}).ErrorValue().code.Value() == SaveErrors::PolicyCapabilityUnsupported.code.Value());
        }

        TEST_CASE("Closing a module generation revokes stale clients while snapshots pin adapters",
                  "[unit][runtime][save][participation][reload]") {
            auto destructionCount = std::make_shared<int>();
            auto adapter = std::make_shared<TestCaptureAdapter>(destructionCount);
            CanonicalStateParticipantRegistry registry;
            TestOperationHost operations;
            auto created = SaveParticipationHost::Create(19, AllCapabilities(), registry, operations);
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            const SaveParticipationClient staleClient = host.Client();
            REQUIRE(staleClient.RegisterParticipant(Descriptor(SaveParticipantRole::Capture | SaveParticipantRole::Restore), adapter)
                        .HasValue());
            auto snapshot = registry.Snapshot().Value();
            REQUIRE(snapshot.Find(SaveParticipantId::Parse("game.progress.campaign").Value()) != nullptr);

            REQUIRE(host.Close().HasValue());
            CHECK_FALSE(host.IsOpen());
            CHECK_FALSE(staleClient.IsOpen());
            CHECK(staleClient.Generation() == 19);
            CHECK(registry.Snapshot().Value().Bindings().empty());
            CHECK(staleClient.RequestSave({.slot = Slot()}).ErrorValue().code.Value() == SaveErrors::LifecycleUnavailable.code.Value());
            CHECK(staleClient.RegisterParticipant(Descriptor(SaveParticipantRole::Capture), adapter).ErrorValue().code.Value() ==
                  SaveErrors::LifecycleUnavailable.code.Value());

            adapter.reset();
            CHECK(*destructionCount == 0);
            snapshot = {};
            CHECK(*destructionCount == 1);
            REQUIRE(host.Close().HasValue());
        }

        TEST_CASE("Destroying the host invalidates retained clients", "[unit][runtime][save][participation][reload]") {
            CanonicalStateParticipantRegistry registry;
            TestOperationHost operations;
            SaveParticipationClient client;
            {
                auto created = SaveParticipationHost::Create(23, AllCapabilities(), registry, operations);
                REQUIRE(created.HasValue());
                auto host = std::move(created).Value();
                client = host.Client();
                REQUIRE(client.IsOpen());
            }
            CHECK_FALSE(client.IsOpen());
            CHECK(client.RequestLoad({.slot = Slot()}).ErrorValue().code.Value() == SaveErrors::LifecycleUnavailable.code.Value());
        }

        TEST_CASE("Participation contains host callback failures and invalid default clients", "[unit][runtime][save][participation]") {
            const SaveParticipationClient invalid;
            CHECK_FALSE(invalid.IsOpen());
            CHECK(invalid.Generation() == 0);
            CHECK(invalid.RequestSave({.slot = Slot()}).ErrorValue().code.Value() == SaveErrors::LifecycleUnavailable.code.Value());
            CHECK(invalid.RequestLoad({.slot = Slot()}).ErrorValue().code.Value() == SaveErrors::LifecycleUnavailable.code.Value());

            CanonicalStateParticipantRegistry registry;
            TestOperationHost operations;
            auto created = SaveParticipationHost::Create(29, AllCapabilities(), registry, operations);
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            const auto client = host.Client();
            operations.throwSaveAllocation = true;
            CHECK(client.RequestSave({.slot = Slot()}).ErrorValue().code.Value() == SaveErrors::OperationAllocationFailed.code.Value());
            operations.throwLoadFailure = true;
            CHECK(client.RequestLoad({.slot = Slot()}).ErrorValue().code.Value() == SaveErrors::LifecycleCallbackFailed.code.Value());
        }

        TEST_CASE("Host move replacement retires its prior generation", "[unit][runtime][save][participation][reload]") {
            CanonicalStateParticipantRegistry firstRegistry;
            CanonicalStateParticipantRegistry secondRegistry;
            TestOperationHost operations;
            auto firstCreated = SaveParticipationHost::Create(31, AllCapabilities(), firstRegistry, operations);
            auto secondCreated = SaveParticipationHost::Create(32, AllCapabilities(), secondRegistry, operations);
            REQUIRE(firstCreated.HasValue());
            REQUIRE(secondCreated.HasValue());
            auto first = std::move(firstCreated).Value();
            auto second = std::move(secondCreated).Value();
            const auto firstClient = first.Client();
            const auto secondClient = second.Client();
            auto destructionCount = std::make_shared<int>();
            REQUIRE(
                firstClient
                    .RegisterParticipant(Descriptor(SaveParticipantRole::Capture), std::make_shared<TestCaptureAdapter>(destructionCount))
                    .HasValue());

            first = std::move(second);
            CHECK_FALSE(firstClient.IsOpen());
            CHECK(secondClient.IsOpen());
            CHECK(first.Client().Generation() == 32);
            CHECK(second.Client().Generation() == 0);
            CHECK(firstRegistry.Snapshot().Value().Bindings().empty());
            CHECK(*destructionCount == 1);
        }

        TEST_CASE("An independently closed registry leaves no module binding to retire", "[unit][runtime][save][participation][reload]") {
            CanonicalStateParticipantRegistry registry;
            TestOperationHost operations;
            auto created = SaveParticipationHost::Create(37, AllCapabilities(), registry, operations);
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            auto destructionCount = std::make_shared<int>();
            auto adapter = std::make_shared<TestCaptureAdapter>(destructionCount);
            REQUIRE(host.Client().RegisterParticipant(Descriptor(SaveParticipantRole::Capture), adapter).HasValue());
            registry.Close();
            adapter.reset();
            CHECK(*destructionCount == 1);
            REQUIRE(host.Close().HasValue());

            CanonicalStateParticipantRegistry closedRegistry;
            closedRegistry.Close();
            CHECK(SaveParticipationHost::Create(38, AllCapabilities(), closedRegistry, operations).ErrorValue().code.Value() ==
                  SaveErrors::LifecycleInvalid.code.Value());
        }
    }  // namespace
}  // namespace Horo::Runtime
