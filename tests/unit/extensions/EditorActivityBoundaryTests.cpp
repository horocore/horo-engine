#include "AllocationProbe.h"
#include "EditorActivitySessionState.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <type_traits>

namespace Horo::Extensions::Tests {
    namespace {
        enum class ProviderFailure {
            None,
            Standard,
            NonStandard,
            InvalidCopy
        };

        struct ProviderAudit {
            ProviderFailure failure{};
            HoroExtensionStatus copyStatus{HORO_EXTENSION_ERROR_INIT_FAILED};
            std::atomic_bool entered{};
            std::atomic_bool release{true};
            bool cancellationObserved{};
        };

        class ProviderContractViolation final : public std::runtime_error {
        public:
            ProviderContractViolation() : std::runtime_error("provider violated C ABI") {}
        };
    }  // namespace
}  // namespace Horo::Extensions::Tests

/** @brief Exercises a real C callback, including a provider violating its no-throw contract after writing output. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif
extern "C" {
HoroExtensionStatus HoroTestActivityProvider(void *context, const HoroEditorActivityAction *action,
                                             const HoroEditorActivitySnapshotSink *sink) {
    using namespace Horo::Extensions::Tests;
    auto &audit = *static_cast<ProviderAudit *>(context);
    const HoroEditorActivityNode node{.structSize = sizeof(HoroEditorActivityNode),
                                      .kind = HORO_EDITOR_ACTIVITY_TEXT,
                                      .flags = HORO_EDITOR_ACTIVITY_TEXT_TECHNICAL,
                                      .id = {"output", 6},
                                      .text = {"copied provider output", 22}};
    const HoroEditorActivitySnapshot snapshot{.structSize = sizeof(HoroEditorActivitySnapshot),
                                              .schemaVersion = HORO_EDITOR_ACTIVITY_SCHEMA_VERSION,
                                              .revision = action->revision + 1,
                                              .badgeCount = 7,
                                              .nodes = audit.failure == ProviderFailure::InvalidCopy ? nullptr : &node,
                                              .nodeCount = 1};
    audit.copyStatus = sink->publish(sink->context, &snapshot);
    audit.entered.store(true, std::memory_order_release);
    while (!audit.release.load(std::memory_order_acquire))
        std::this_thread::yield();
    audit.cancellationObserved = action->cancellation.isCancellationRequested(action->cancellation.context) != 0;
    if (audit.failure == ProviderFailure::Standard)
        throw ProviderContractViolation{};
    if (audit.failure == ProviderFailure::NonStandard)
        throw 42;
    return audit.copyStatus;
}
}  // extern "C"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

namespace Horo::Extensions::Tests {
    namespace {

        /** @brief Keeps fixture context alive through drained jobs, including failed assertions before release. */
        struct ActionFixture {
            JobSystem jobs;
            std::shared_ptr<ExtensionRetirement> retirement{
                std::make_shared<ExtensionRetirement>("fixture", std::vector<std::string>{"native"})};
            std::shared_ptr<ProviderAudit> audit{std::make_shared<ProviderAudit>()};
            std::shared_ptr<EditorActivitySession> session{std::make_shared<EditorActivitySession>()};

            explicit ActionFixture(const ProviderFailure failure) {
                audit->failure = failure;
                REQUIRE(retirement->BindModuleCode("native", audit));
                session->retirement = retirement;
                session->activity.surface.provider = {"fixture", "native", 1};
                session->activity.surface.id = "activity";
                session->drawer.surface.id = "drawer";
                session->drawer.surface.labelLocalizationKey = "fixture.title";
                session->revision = 1;
                session->committed = true;
                const HoroEditorActivityDescriptor descriptor{.moduleContext = audit.get(), .invokeAction = HoroTestActivityProvider};
                session->actions.provider = EditorActivityProviderAction{descriptor};
                REQUIRE(retirement->RegisterContribution("native", session));
            }

            ~ActionFixture() {
                audit->release.store(true, std::memory_order_release);
                jobs.Shutdown(ShutdownPolicy::Drain);
            }

            void Start() const {
                session->actions.pending = EditorActivitySession::PendingAction{"node", "run", 1};
                PumpEditorActivityAction(*session, jobs, session);
                REQUIRE(session->actions.job);
                REQUIRE(session->actions.result);
            }
        };
    }  // namespace

    static_assert(std::is_standard_layout_v<EditorActivityProviderAction>);
    static_assert(std::is_trivially_copyable_v<EditorActivityProviderAction>);
    static_assert(std::is_convertible_v<decltype(&HoroTestActivityProvider), HoroEditorActivityActionFunc>);
    static_assert(std::is_same_v<HoroRegisterEditorActivityFunc,
                                 HoroExtensionStatus (*)(void *, const HoroEditorActivityDescriptor *, HoroEditorActivitySessionApi *)>);
    static_assert(noexcept(std::declval<const EditorActivityProviderAction &>().Invoke(
        std::declval<const HoroEditorActivityAction &>(), std::declval<const HoroEditorActivitySnapshotSink &>())));

    TEST_CASE("Activity native endpoint rejects absent callbacks without invocation", "[Extensions][Activity][ABI]") {
        const EditorActivityProviderAction provider;
        CHECK_FALSE(provider.IsAvailable());
        CHECK(provider.Invoke({}, {}) == HORO_EXTENSION_ERROR_INIT_FAILED);
    }

    TEST_CASE("Activity worker contains standard and non-standard provider throws and discards copied output",
              "[Extensions][Activity][ABI][Retirement]") {
        auto failure = ProviderFailure::Standard;
        SECTION("standard exception") {
            failure = ProviderFailure::Standard;
        }
        SECTION("non-standard exception") {
            failure = ProviderFailure::NonStandard;
        }
        ActionFixture fixture{failure};
        fixture.Start();
        fixture.jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(fixture.audit->entered.load(std::memory_order_acquire));
        CHECK(fixture.audit->copyStatus == HORO_EXTENSION_SUCCESS);
        REQUIRE(fixture.session->actions.result->ready.load(std::memory_order_acquire));
        CHECK_FALSE(fixture.session->actions.result->form);
        CHECK(fixture.session->revision == 1);
        CHECK(fixture.retirement->Inspect().outstanding.empty());
        PumpEditorActivityAction(*fixture.session, fixture.jobs, fixture.session);
        CHECK_FALSE(fixture.session->actions.result);
        CHECK_FALSE(fixture.session->actions.job);
        CHECK(fixture.session->revision == 1);
    }

    TEST_CASE("Activity worker rejects malformed copy without partially publishing a result", "[Extensions][Activity][ABI]") {
        ActionFixture fixture{ProviderFailure::InvalidCopy};
        fixture.Start();
        fixture.jobs.Shutdown(ShutdownPolicy::Drain);
        CHECK(fixture.audit->copyStatus == HORO_EXTENSION_ERROR_OUTPUT_REJECTED);
        REQUIRE(fixture.session->actions.result->ready.load(std::memory_order_acquire));
        CHECK_FALSE(fixture.session->actions.result->form);
        CHECK(fixture.session->actions.result->revision == 1);
        CHECK(fixture.session->revision == 1);
        CHECK(fixture.retirement->Inspect().outstanding.empty());
    }

    TEST_CASE("Activity C result sink contains allocation failure and preserves the previous handoff", "[Extensions][Activity][ABI]") {
        EditorActivitySession::ActionResult result;
        result.drawerId = std::string(64, 'd');
        result.titleKey = "fixture.title";
        result.revision = 1;
        const auto sink = MakeEditorActivityResultSink(result);
        const HoroEditorActivityNode node{.structSize = sizeof(HoroEditorActivityNode),
                                          .kind = HORO_EDITOR_ACTIVITY_TEXT,
                                          .flags = HORO_EDITOR_ACTIVITY_TEXT_TECHNICAL,
                                          .id = {"output", 6},
                                          .text = {"owned output", 12}};
        const HoroEditorActivitySnapshot snapshot{.structSize = sizeof(HoroEditorActivitySnapshot),
                                                  .schemaVersion = HORO_EDITOR_ACTIVITY_SCHEMA_VERSION,
                                                  .revision = 2,
                                                  .nodes = &node,
                                                  .nodeCount = 1};
        HoroExtensionStatus status{};
        {
            Horo::Tests::AllocationProbe::ScopedFailure failure;
            status = sink.publish(sink.context, &snapshot);
        }
        CHECK(status == HORO_EXTENSION_ERROR_OUTPUT_REJECTED);
        CHECK_FALSE(result.form);
        CHECK(result.revision == 1);
        CHECK(sink.publish(nullptr, &snapshot) == HORO_EXTENSION_ERROR_INVALID_ARGS);
        CHECK(sink.publish(sink.context, nullptr) == HORO_EXTENSION_ERROR_INVALID_ARGS);
        CHECK(sink.publish(sink.context, &snapshot) == HORO_EXTENSION_SUCCESS);
        REQUIRE(result.form);
        CHECK(result.revision == 2);
        CHECK(sink.publish(sink.context, &snapshot) == HORO_EXTENSION_ERROR_OUTPUT_REJECTED);
    }

    TEST_CASE("Activity retirement pins admitted provider work and rejects late output after non-standard throw",
              "[Extensions][Activity][ABI][Retirement]") {
        ActionFixture fixture{ProviderFailure::NonStandard};
        fixture.audit->release.store(false, std::memory_order_release);
        fixture.Start();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!fixture.audit->entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE(fixture.audit->entered.load(std::memory_order_acquire));
        const auto report = fixture.retirement->BeginRetirement();
        CHECK(report.disposition == ExtensionRetirementDisposition::Draining);
        REQUIRE(report.outstanding.size() == 1);
        CHECK(report.outstanding.front().kind == ExtensionLeaseKind::Job);
        CHECK(fixture.session->revoked);
        CHECK_FALSE(fixture.session->committed);
        CHECK_FALSE(fixture.retirement->Acquire("native", ExtensionLeaseKind::Job, "late", fixture.session));
        fixture.audit->release.store(true, std::memory_order_release);
        fixture.jobs.Shutdown(ShutdownPolicy::Drain);
        CHECK(fixture.audit->cancellationObserved);
        CHECK(fixture.retirement->IsDrained());
        CHECK_FALSE(fixture.session->actions.result->form);
        CHECK(fixture.session->revision == 1);
    }
}  // namespace Horo::Extensions::Tests
