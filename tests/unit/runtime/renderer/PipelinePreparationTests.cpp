#include "Horo/Runtime/Render/PipelineCacheErrors.h"
#include "Horo/Runtime/Render/PipelinePreparation.h"
#include "Horo/Runtime/Render/PipelinePreparationErrors.h"
#include "support/TypedIdentityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace Horo::Render;
using Horo::Tests::RequireError;
namespace PreparationErrors = Horo::Render::PipelinePreparationErrors;

namespace {
    PipelineUsage Usage(const std::uint8_t id, const bool cooked = true, const bool required = true) {
        PipelineUsage usage;
        usage.key.digest.bytes[0] = id;
        usage.cookedArtifactAvailable = cooked;
        usage.required = required;
        return usage;
    }

    PipelinePreparation Plan(std::vector<PipelineUsage> usages, const PipelineCompilationMode mode = PipelineCompilationMode::Packaged,
                             const PipelinePreparationBudget budget = {}) {
        auto plan = PipelinePreparation::Prepare({17, std::move(usages)}, mode, budget);
        REQUIRE(plan.HasValue());
        return std::move(plan).Value();
    }
}  // namespace

TEST_CASE("Release activation waits for exact cooked native pipelines", "[renderer][pipeline-preparation]") {
    auto plan = Plan({Usage(1), Usage(2)});
    RequireError(plan.CheckReady(), PreparationErrors::Pending);
    RequireError(plan.Bind(0), PreparationErrors::Pending);
    const auto batch = plan.Dispatch({});
    REQUIRE(batch.HasValue());
    REQUIRE(batch.Value().size() == 2);
    CHECK(batch.Value()[0].action == PipelinePreparationAction::RealizeCooked);
    REQUIRE(plan.Complete(batch.Value()[0], Horo::Result<void>::Success(), 100).HasValue());
    RequireError(plan.CheckReady(), PreparationErrors::Pending);
    REQUIRE(plan.Complete(batch.Value()[1], Horo::Result<void>::Success(), 200).HasValue());
    CHECK(plan.CheckReady().HasValue());
    const auto binding = plan.Bind(0);
    REQUIRE(binding.HasValue());
    CHECK(binding.Value().key == Usage(1).key);
    CHECK_FALSE(binding.Value().usedFallback);
    CHECK(plan.Dispatch({}).Value().empty());
}

TEST_CASE("Source permission and native residency are separate preparation stages", "[renderer][pipeline-preparation]") {
    RequireError(PipelinePreparation::Prepare({1, {Usage(1, false)}}, PipelineCompilationMode::Packaged),
                 PreparationErrors::MissingCookedArtifact);
    auto plan = Plan({Usage(1, false)}, PipelineCompilationMode::Development);
    const auto source = plan.Dispatch({});
    REQUIRE(source.HasValue());
    REQUIRE(source.Value().size() == 1);
    CHECK(source.Value()[0].action == PipelinePreparationAction::CompileSource);
    REQUIRE(plan.Complete(source.Value()[0], Horo::Result<void>::Success(), 2500).HasValue());
    RequireError(plan.CheckReady(), PreparationErrors::Pending);
    const auto native = plan.Dispatch({});
    REQUIRE(native.HasValue());
    REQUIRE(native.Value().size() == 1);
    CHECK(native.Value()[0].action == PipelinePreparationAction::RealizeCooked);
    RequireError(plan.Complete(source.Value()[0], Horo::Result<void>::Success(), 0), PreparationErrors::StaleCompletion);
    REQUIRE(plan.Complete(native.Value()[0], Horo::Result<void>::Success(), 100).HasValue());
    CHECK(plan.CheckReady().HasValue());
    CHECK(plan.Metrics().hitches == 1);
    CHECK(plan.Metrics().completedWork == 2);
    CHECK(plan.Metrics().totalWorkMicroseconds == 2600);
    CHECK(plan.Metrics().maximumWorkMicroseconds == 2500);
}

TEST_CASE("Dispatch obeys concurrency count and estimated time budgets", "[renderer][pipeline-preparation]") {
    PipelinePreparationBudget budget;
    budget.maximumInFlight = 2;
    budget.maximumDispatchesPerBatch = 3;
    budget.maximumBatchMicroseconds = 1000;
    auto plan = Plan({Usage(1), Usage(2), Usage(3)}, PipelineCompilationMode::Packaged, budget);
    const auto first = plan.Dispatch({});
    const auto second = plan.Dispatch({});
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    REQUIRE(first.Value().size() == 1);
    REQUIRE(second.Value().size() == 1);
    CHECK(plan.Dispatch({}).Value().empty());
    REQUIRE(plan.Complete(first.Value()[0], Horo::Result<void>::Success(), 1000).HasValue());
    const auto third = plan.Dispatch({});
    REQUIRE(third.HasValue());
    REQUIRE(third.Value().size() == 1);
    CHECK(third.Value()[0].index == 2);
    CHECK(plan.Metrics().hitches == 0);
}

TEST_CASE("Optional fallback is cooked explicit resident and observable", "[renderer][pipeline-preparation]") {
    auto optional = Usage(1, true, false);
    optional.fallback = 1;
    auto plan = Plan({optional, Usage(2)});
    const auto batch = plan.Dispatch({});
    REQUIRE(batch.HasValue());
    REQUIRE(batch.Value().size() == 2);
    RequireError(plan.Bind(0), PreparationErrors::Pending);
    REQUIRE(plan.Complete(batch.Value()[1], Horo::Result<void>::Success(), 20).HasValue());
    REQUIRE(plan.Complete(batch.Value()[0], Horo::Result<void>::Failure(Horo::MakeError(PipelineCacheErrors::CorruptBlob)), 30).HasValue());
    const auto fallback = plan.Bind(0);
    REQUIRE(fallback.HasValue());
    CHECK(fallback.Value().usedFallback);
    CHECK(fallback.Value().key == Usage(2).key);
    CHECK(plan.Metrics().fallbackBindings == 1);
    CHECK(plan.Metrics().failedWork == 1);
    CHECK(plan.CheckReady().HasValue());
}

TEST_CASE("Preparation preserves required failure diagnostics and rejects stale work", "[renderer][pipeline-preparation]") {
    auto plan = Plan({Usage(1)});
    const auto batch = plan.Dispatch({});
    REQUIRE(batch.HasValue());
    REQUIRE(batch.Value().size() == 1);
    auto stale = batch.Value()[0];
    ++stale.generation;
    RequireError(plan.Complete(stale, Horo::Result<void>::Success(), 100), PreparationErrors::StaleCompletion);
    stale = batch.Value()[0];
    stale.key = Usage(2).key;
    RequireError(plan.Complete(stale, Horo::Result<void>::Success(), 100), PreparationErrors::StaleCompletion);
    stale = batch.Value()[0];
    stale.action = static_cast<PipelinePreparationAction>(255);
    RequireError(plan.Complete(stale, Horo::Result<void>::Success(), 100), PreparationErrors::StaleCompletion);
    stale = batch.Value()[0];
    stale.index = 2;
    RequireError(plan.Complete(stale, Horo::Result<void>::Success(), 100), PreparationErrors::StaleCompletion);
    CHECK(plan.Metrics().completedWork == 0);
    auto failure = Horo::MakeError(PipelineCacheErrors::IncompatibleBlob, "driver rejected exact native realization");
    REQUIRE(plan.Complete(batch.Value()[0], Horo::Result<void>::Failure(failure), 100).HasValue());
    RequireError(plan.CheckReady(), PipelineCacheErrors::IncompatibleBlob);
    RequireError(plan.Bind(0), PipelineCacheErrors::IncompatibleBlob);
    CHECK(plan.Bind(0).ErrorValue().message == failure.message);
    CHECK(plan.Dispatch({}).Value().empty());
    RequireError(plan.Complete(batch.Value()[0], Horo::Result<void>::Success(), 0), PreparationErrors::StaleCompletion);
}

TEST_CASE("Cancellation and shutdown reject late completions and new admission", "[renderer][pipeline-preparation]") {
    auto plan = Plan({Usage(1)});
    const auto batch = plan.Dispatch({});
    REQUIRE(batch.HasValue());
    Horo::CancellationSource source;
    source.RequestCancellation();
    RequireError(plan.Dispatch(source.Token()), PreparationErrors::Cancelled);
    RequireError(plan.Dispatch({}), PreparationErrors::Cancelled);
    RequireError(plan.Complete(batch.Value()[0], Horo::Result<void>::Success(), 100), PreparationErrors::Cancelled);
    RequireError(plan.CheckReady(), PreparationErrors::Cancelled);
    RequireError(plan.Bind(0), PreparationErrors::Cancelled);
    auto closed = Plan({Usage(2)});
    closed.Close();
    closed.Close();
    RequireError(closed.Dispatch({}), PreparationErrors::Closed);
    RequireError(closed.Bind(0), PreparationErrors::Closed);
    RequireError(closed.CheckReady(), PreparationErrors::Closed);
}

TEST_CASE("Usage manifests reject malformed identities and implicit fallbacks", "[renderer][pipeline-preparation]") {
    RequireError(PipelinePreparation::Prepare({0, {Usage(1)}}, PipelineCompilationMode::Packaged), PreparationErrors::InvalidManifest);
    RequireError(PipelinePreparation::Prepare({1, {Usage(0)}}, PipelineCompilationMode::Packaged), PreparationErrors::InvalidManifest);
    RequireError(PipelinePreparation::Prepare({1, {Usage(1), Usage(1)}}, PipelineCompilationMode::Packaged),
                 PreparationErrors::InvalidManifest);
    auto usage = Usage(1);
    usage.fallback = 1;
    RequireError(PipelinePreparation::Prepare({1, {usage, Usage(2)}}, PipelineCompilationMode::Packaged),
                 PreparationErrors::InvalidManifest);
    usage.required = false;
    usage.fallback = 0;
    RequireError(PipelinePreparation::Prepare({1, {usage}}, PipelineCompilationMode::Packaged), PreparationErrors::InvalidManifest);
    usage.fallback = 3;
    RequireError(PipelinePreparation::Prepare({1, {usage}}, PipelineCompilationMode::Packaged), PreparationErrors::InvalidManifest);
    usage.fallback = 1;
    RequireError(PipelinePreparation::Prepare({1, {usage, Usage(2, false)}}, PipelineCompilationMode::Development),
                 PreparationErrors::InvalidManifest);
    auto chain = Usage(2, true, false);
    chain.fallback = 2;
    RequireError(PipelinePreparation::Prepare({1, {usage, chain, Usage(3)}}, PipelineCompilationMode::Packaged),
                 PreparationErrors::InvalidManifest);
    CHECK(Plan({}).CheckReady().HasValue());
    RequireError(Plan({Usage(1)}).Bind(1), PreparationErrors::InvalidManifest);
}

TEST_CASE("Preparation validates hard limits and saturates measured time", "[renderer][pipeline-preparation]") {
    PipelinePreparationBudget budget;
    budget.maximumPipelines = 1;
    budget.maximumInFlight = 1;
    budget.maximumDispatchesPerBatch = 1;
    RequireError(PipelinePreparation::Prepare({1, {Usage(1), Usage(2)}}, PipelineCompilationMode::Packaged, budget),
                 PreparationErrors::InvalidManifest);
    budget.maximumPipelines = PipelinePreparationBudget::HardMaximumPipelines + 1;
    RequireError(PipelinePreparation::Prepare({1, {}}, PipelineCompilationMode::Packaged, budget), PreparationErrors::InvalidBudget);
    budget = {};
    budget.estimatedWorkMicroseconds = 0;
    RequireError(PipelinePreparation::Prepare({1, {}}, PipelineCompilationMode::Packaged, budget), PreparationErrors::InvalidBudget);
    budget = {};
    budget.maximumInFlight = 0;
    RequireError(PipelinePreparation::Prepare({1, {}}, PipelineCompilationMode::Packaged, budget), PreparationErrors::InvalidBudget);
    budget = {};
    budget.maximumBatchMicroseconds = 999;
    RequireError(PipelinePreparation::Prepare({1, {}}, PipelineCompilationMode::Packaged, budget), PreparationErrors::InvalidBudget);
    auto plan = Plan({Usage(1), Usage(2)});
    const auto batch = plan.Dispatch({});
    REQUIRE(batch.HasValue());
    REQUIRE(batch.Value().size() == 2);
    REQUIRE(plan.Complete(batch.Value()[0], Horo::Result<void>::Success(), std::numeric_limits<std::uint64_t>::max()).HasValue());
    REQUIRE(plan.Complete(batch.Value()[1], Horo::Result<void>::Success(), 1).HasValue());
    CHECK(plan.Metrics().totalWorkMicroseconds == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("Failed development source work never admits native realization", "[renderer][pipeline-preparation]") {
    auto plan = Plan({Usage(1, false)}, PipelineCompilationMode::Development);
    const auto batch = plan.Dispatch({});
    REQUIRE(batch.HasValue());
    REQUIRE(batch.Value().size() == 1);
    REQUIRE(plan.Complete(batch.Value()[0], Horo::Result<void>::Failure(Horo::MakeError(PipelineCacheErrors::CorruptBlob)), 1).HasValue());
    RequireError(plan.CheckReady(), PipelineCacheErrors::CorruptBlob);
    RequireError(plan.Bind(0), PipelineCacheErrors::CorruptBlob);
    CHECK(plan.Dispatch({}).Value().empty());
}
