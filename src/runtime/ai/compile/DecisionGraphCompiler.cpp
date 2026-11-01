#include "Horo/AI/DecisionGraphCompiler.h"

#include "DecisionClosureLayout.h"
#include "DecisionCompilationBuilder.h"
#include "DecisionCompileLimits.h"
#include "Horo/AI/DecisionGraphInstance.h"
#include "Horo/AI/DecisionPlanCodec.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::AI {
    namespace {
        Result<Detail::DecisionCompilationCandidate> PrepareCompilation(DecisionGraphAssetId root,
                                                                        std::span<const DecisionGraphSource> sources,
                                                                        const DecisionCompilationEnvironment &environment,
                                                                        DecisionCompilationLimits limits,
                                                                        DecisionClosureAdmission admission) {
            DecisionCodecLimits codec{.compilation = limits};
            auto encoded = DecisionPlanCodec::EncodeSource(sources, root, codec);
            if (encoded.HasError())
                return Result<Detail::DecisionCompilationCandidate>::Failure(encoded.ErrorValue());
            DecisionGraphAssetId capturedRoot;
            auto captured = DecisionPlanCodec::DecodeSource(encoded.Value(), capturedRoot, codec);
            if (captured.HasError())
                return Result<Detail::DecisionCompilationCandidate>::Failure(captured.ErrorValue());
            return Detail::BuildDecisionCompilation(capturedRoot, captured.Value(), environment, limits, admission);
        }

        SourceLocation RootLocation(DecisionGraphAssetId root, std::span<const DecisionGraphSource> sources) {
            const auto bounded = sources.first(std::min<std::size_t>(sources.size(), 128));
            const auto found = std::ranges::find(bounded, root, &DecisionGraphSource::asset);
            return found == bounded.end() ? SourceLocation{"<decision-capture>", 0, 0} : found->location;
        }

        Result<void> ChargeEnvelope(DecisionStorageRequirements &storage, std::size_t bytes, DecisionCompilationLimits limits) {
            const auto owned =
                static_cast<std::uint64_t>(bytes) + sizeof(CookedDecisionPlan) + storage.occurrences * sizeof(DecisionCallOccurrence);
            storage.coreInstanceBytes += sizeof(DecisionGraphInstance);
            if (owned > std::numeric_limits<std::uint64_t>::max() - storage.compiledBytes)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            storage.compiledBytes += owned;
            if (storage.compiledBytes > limits.maximumCoreBytes)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            auto remaining = limits.maximumCoreBytes - storage.compiledBytes;
            if (storage.coreInstanceBytes > remaining)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            remaining -= storage.coreInstanceBytes;
            if (storage.scratchBytes > remaining)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            return Result<void>::Success();
        }
    }  // namespace

    Result<DecisionGraphCompilation> DecisionGraphCompiler::Compile(DecisionGraphAssetId root, std::span<const DecisionGraphSource> sources,
                                                                    const DecisionCompilationEnvironment &environment,
                                                                    DecisionCompilationLimits limits) {
        const auto reject = [&](const Error &error) {
            std::vector<DecisionAssetValidationDiagnostic> diagnostics;
            diagnostics.push_back({error.code, DiagnosticSeverity::Error, error.message, RootLocation(root, sources), "executableClosure"});
            return Result<DecisionGraphCompilation>::Success({DecisionAssetValidationReport{std::move(diagnostics)}, {}});
        };
        if (!Detail::ValidDecisionLimits(limits))
            return reject(MakeError(AIErrors::DecisionAssetLimitExceeded));
        try {
            auto environmentIdentity = Detail::DecisionEnvironmentIdentity(environment);
            if (environmentIdentity.HasError())
                return reject(environmentIdentity.ErrorValue());
            auto built = PrepareCompilation(root, sources, environment, limits, DecisionClosureAdmission{});
            if (built.HasError())
                return reject(built.ErrorValue());
            auto candidate = std::move(built).Value();
            if (!candidate.diagnostics.empty())
                return Result<DecisionGraphCompilation>::Success({DecisionAssetValidationReport{std::move(candidate.diagnostics)}, {}});
            auto layout = Detail::BuildDecisionClosureLayout(candidate.graphs, candidate.root, limits);
            if (layout.HasError())
                return reject(layout.ErrorValue());
            auto sourceBytes = DecisionPlanCodec::EncodeSource(candidate.sources, root, {.compilation = limits});
            if (sourceBytes.HasError())
                return reject(sourceBytes.ErrorValue());
            auto storage = layout.Value().storage;
            if (auto charged = ChargeEnvelope(storage, sourceBytes.Value().capacity(), limits); charged.HasError())
                return reject(charged.ErrorValue());
            auto plan = std::shared_ptr<CookedDecisionPlan>(new CookedDecisionPlan());
            plan->graphs_ = std::move(candidate.graphs);
            plan->occurrences_ = std::move(layout).Value().occurrences;
            plan->storage_ = storage;
            plan->root_ = root;
            plan->sourceBytes_ = std::move(sourceBytes).Value();
            plan->environmentIdentity_ = environmentIdentity.Value();
            plan->runtimeLayoutVersion_ = environment.runtimeLayoutVersion;
            plan->capabilities_ = environment.capabilities;
            plan->providers_ = environment.nodes;
            auto cooked = DecisionPlanCodec::EncodeCooked(*plan, {.compilation = limits});
            if (cooked.HasError())
                return reject(cooked.ErrorValue());
            plan->digest_ = ComputeSha256(std::as_bytes(std::span{cooked.Value()}));
            return Result<DecisionGraphCompilation>::Success({{}, std::move(plan)});
        } catch (const std::bad_alloc &) {
            return Result<DecisionGraphCompilation>::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    std::span<const DecisionCompiledGraph> CookedDecisionPlan::Graphs() const noexcept {
        return graphs_;
    }

    std::span<const DecisionCallOccurrence> CookedDecisionPlan::Occurrences() const noexcept {
        return occurrences_;
    }

    const DecisionStorageRequirements &CookedDecisionPlan::Storage() const noexcept {
        return storage_;
    }

    const Sha256Digest &CookedDecisionPlan::Digest() const noexcept {
        return digest_;
    }

    DecisionGraphAssetId CookedDecisionPlan::Root() const noexcept {
        return root_;
    }

    AiCapabilitySet CookedDecisionPlan::Capabilities() const noexcept {
        return capabilities_;
    }

    bool CookedDecisionPlan::IsCurrent() const noexcept {
        return providers_ && providers_->CanUse();
    }

    Result<bool> DecisionPlanStore::TryActivate(DecisionGraphCompilation candidate) {
        if (std::this_thread::get_id() != owner_)
            return Result<bool>::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
        if (!candidate.plan || candidate.validation.HasErrors())
            return Result<bool>::Success(false);
        if (!candidate.plan->IsCurrent())
            return Result<bool>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        current_ = std::move(candidate.plan);
        return Result<bool>::Success(true);
    }

    std::shared_ptr<const CookedDecisionPlan> DecisionPlanStore::Current() const noexcept {
        return std::this_thread::get_id() == owner_ ? current_ : nullptr;
    }
}  // namespace Horo::AI
