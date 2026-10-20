#include "Horo/Foundation/ErrorCodeRegistry.h"
#include "Horo/Foundation/FoundationErrorDomains.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Math/SceneMath.h"

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <set>
#include <string>

namespace Horo {
    namespace {
        /** @brief Composes the actual public metadata under its real Foundation owner. */
        ErrorCodeRegistry Registry() {
            const ModuleDescriptor foundation{.id = {"horo.foundation"},
                                              .version = {1, 0, 0},
                                              .errorDomains = {JobSystemErrorDomain(), SceneMathErrorDomain()}};
            auto registry = BuildErrorCodeRegistry(std::span{&foundation, 1});
            REQUIRE(registry.HasValue());
            return std::move(registry).Value();
        }
    }  // namespace

    TEST_CASE("Foundation host metadata registers complete distinct scheduler and Scene Math domains", "[foundation][errors]") {
        const auto registry = Registry();
        CHECK(registry.Size() == 25);
        CHECK(registry.DomainCount() == 2);
        for (const auto &domain : {JobSystemErrorDomain(), SceneMathErrorDomain()}) {
            std::set<std::string, std::less<>> identities;
            for (const auto *descriptor : domain.descriptors) {
                REQUIRE(descriptor != nullptr);
                CHECK(descriptor->domain.Value() == domain.id.Value());
                CHECK(identities.insert(descriptor->code.Value()).second);
                const auto *registered = registry.Resolve(descriptor->domain, descriptor->code);
                REQUIRE(registered != nullptr);
                CHECK(registered->summary == descriptor->summary);
                CHECK(registered->remediationHint == descriptor->remediationHint);
                CHECK(registered->defaultSeverity == descriptor->defaultSeverity);
                CHECK(registered->retryable == descriptor->retryable);
                CHECK(registered->userActionable == descriptor->userActionable);
                REQUIRE(registry.OwnerOf(descriptor->domain, descriptor->code) != nullptr);
                CHECK(registry.OwnerOf(descriptor->domain, descriptor->code)->value == "horo.foundation");
            }
        }
    }

    TEST_CASE("Foundation metadata resolves actual scheduler cancellation and shutdown failures", "[foundation][errors][jobs]") {
        const auto registry = Registry();
        const auto cancelled = JobCancelled();
        REQUIRE(cancelled.HasError());
        REQUIRE(registry.Resolve(cancelled.ErrorValue()) != nullptr);
        CHECK(registry.Resolve(cancelled.ErrorValue())->code.Value() == "job.cancelled");
        JobSystem jobs{{.workerCount = 0, .maxQueuedJobs = 1}};
        jobs.Shutdown(ShutdownPolicy::Cancel);
        const auto rejected = jobs.SubmitResult({}, [](const CancellationToken &) {
            return Result<void>::Success();
        });
        REQUIRE(rejected.HasError());
        REQUIRE(registry.Resolve(rejected.ErrorValue()) != nullptr);
        CHECK(registry.Resolve(rejected.ErrorValue())->code.Value() == "job.shutdown");
    }

    TEST_CASE("Foundation metadata resolves actual Scene Math failure and preserves owned contributions", "[foundation][errors][math]") {
        const auto registry = Registry();
        const auto normalized = Math::TryNormalize(Math::Vec3{});
        REQUIRE(normalized.HasError());
        REQUIRE(registry.Resolve(normalized.ErrorValue()) != nullptr);
        CHECK(registry.Resolve(normalized.ErrorValue())->code.Value() == "math.zero_length");
        auto contribution = SceneMathErrorDomain();
        const auto retained = contribution.descriptors.front();
        contribution.descriptors.clear();
        CHECK(SceneMathErrorDomain().descriptors.size() == 10);
        CHECK(retained->domain.Value() == "horo.foundation.math");
        CHECK(registry.Resolve(retained->domain, retained->code) != nullptr);
    }
}  // namespace Horo
