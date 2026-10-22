#include "Horo/Foundation/CancellationToken.h"
#include "RuntimeSceneTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using RuntimeSceneTestSupport::Context;

    /** @brief Immutable caller identity, not an authority that publishes Scene state. */
    class BaselineOperation final : public SceneBaselineOwnership {
    public:
        BaselineOperation(SceneDefinitionId source, std::uint64_t revision, std::uint64_t attempt)
            : source_(source), revision_(revision), attempt_(attempt) {}

        bool Matches(const SceneBaselineOwnership &other) const noexcept override {
            const auto *operation = dynamic_cast<const BaselineOperation *>(&other);
            return operation && operation->source_ == source_;
        }

        bool MatchesOperation(const SceneBaselineOwnership &other) const noexcept override {
            const auto *operation = dynamic_cast<const BaselineOperation *>(&other);
            return operation && Matches(other) && operation->revision_ == revision_ && operation->attempt_ == attempt_;
        }

    private:
        const SceneDefinitionId source_;
        const std::uint64_t revision_;
        const std::uint64_t attempt_;
    };

    /** @brief This asset-free caller has no external readiness authority to revalidate. */
    class BaselineAdmission final : public ScenePublicationCheck {
    public:
        Result<void> ValidatePublication() const override {
            return Result<void>::Success();
        }
    };

    /** @brief Builds disjoint asset-free baselines through the production definition builder. */
    RuntimeSceneDefinition Baseline(std::uint64_t id) {
        SceneDefinitionBuilder builder{SceneDefinitionId{id}, SceneDefinitionRevision{1}};
        builder.Add(RuntimeSceneTestSupport::Entity(id + 100));
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        return std::move(definition).Value();
    }

    /** @brief Creates a valid single baseline command for the actual active runtime. */
    SceneCommandBuffer Commands(RuntimeSceneService &service, std::uint64_t id, std::shared_ptr<const SceneBaselineOwnership> operation) {
        REQUIRE(service.ActiveScene().has_value());
        // No registry is installed: the canonical service compares registry revision zero to zero.
        // All three default cancellation tokens are live; no Assets/Jobs readiness is simulated.
        const SceneStructuralAdmission admission{service.ActiveScene()->RuntimeId(), {}, {}, {}, {}};
        // These are aggregate resident counts, not bytes: at most two 1-entity, 0-resource baselines coexist.
        const SceneBaselineAttachmentLimits limits{4, 16, 16};
        SceneCommandBuffer commands;
        REQUIRE(
            commands.AttachBaseline(Baseline(id), {}, admission, limits, {}, std::make_shared<BaselineAdmission>(), std::move(operation))
                .HasValue());
        return commands;
    }

    /** @brief Retains only Scene-issued transaction evidence, never an active Scene borrow. */
    std::shared_ptr<const SceneStructuralReceipt> Queue(RuntimeSceneService &service, std::uint64_t id,
                                                        std::shared_ptr<const SceneBaselineOwnership> operation) {
        auto queued = service.QueueTrackedStructuralCommands(Commands(service, id, std::move(operation)));
        REQUIRE(queued.HasValue());
        auto receipt = std::move(queued).Value();
        REQUIRE(receipt != nullptr);
        REQUIRE_FALSE(receipt->Complete());
        REQUIRE(receipt->Failure() == nullptr);
        REQUIRE(receipt->ResultValue() == nullptr);
        return receipt;
    }

    /** @brief Gives the canonical service a real lifecycle processing opportunity. */
    void Commit(RuntimeSceneService &service, const CancellationToken &token) {
        REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(token)).HasValue());
        REQUIRE_FALSE(service.TakeOperationError().has_value());
    }

    /** @brief Proves rejected cancellation retains both unpublished evidence and queued work. */
    void CheckPending(RuntimeSceneService &service, const SceneStructuralReceipt &receipt) {
        REQUIRE_FALSE(receipt.Complete());
        REQUIRE(receipt.Failure() == nullptr);
        REQUIRE(receipt.ResultValue() == nullptr);
        REQUIRE(service.QueueStructuralCommands(Commands(service, 24, std::make_shared<BaselineOperation>(SceneDefinitionId{24}, 1, 1)))
                    .HasError());
        REQUIRE_FALSE(service.ActiveScene()->FindBaseline({21}).has_value());
    }

    /** @brief Wrong domain, baseline, owner namespace, revision and attempt must not clear the admitted batch. */
    void RejectForeignCancels(RuntimeSceneService &service, const SceneStructuralReceipt &receipt, const BaselineOperation &operation) {
        const auto runtime = service.ActiveScene()->RuntimeId();
        REQUIRE(service.CancelPendingBaseline(SceneRuntimeId{900}, {21}, operation).HasError());
        CheckPending(service, receipt);
        REQUIRE(service.CancelPendingBaseline(runtime, {999}, operation).HasError());
        CheckPending(service, receipt);
        for (const auto &foreign : {BaselineOperation{{999}, 1, 1}, BaselineOperation{{21}, 2, 1}, BaselineOperation{{21}, 1, 2}}) {
            REQUIRE(service.CancelPendingBaseline(runtime, {21}, foreign).HasError());
            CheckPending(service, receipt);
        }
    }

    /** @brief Cancellation must retire its own receipt without manufacturing a successful commit. */
    void CheckCancelled(const SceneStructuralReceipt &receipt) {
        CHECK(receipt.Complete());
        CHECK(receipt.Failure() != nullptr);
        CHECK(receipt.ResultValue() == nullptr);
    }

    /** @brief Compares retained owned failure evidence after later publication and shutdown. */
    void CheckStableFailure(const SceneStructuralReceipt &receipt, const std::optional<Error> &failure) {
        CheckCancelled(receipt);
        if (!failure)
            return;
        REQUIRE(receipt.Failure() != nullptr);
        CHECK(receipt.Failure()->domain.Value() == failure->domain.Value());
        CHECK(receipt.Failure()->code.Value() == failure->code.Value());
        CHECK(receipt.Failure()->severity == failure->severity);
        CHECK(receipt.Failure()->message == failure->message);
        CHECK(receipt.Failure()->diagnostics.size() == failure->diagnostics.size());
        CHECK(receipt.Failure()->cause.Get() == failure->cause.Get());
    }

    TEST_CASE("Tracked baseline cancellation retires only its own Scene receipt", "[unit][runtime][scene][tracked_baseline][cancel]") {
        CancellationSource cancellation;
        RuntimeSceneService service;
        REQUIRE(service.Startup(cancellation.Token()).HasValue());
        REQUIRE(service.QueuePreparation(RuntimeSceneTestSupport::Definition()).HasValue());
        Commit(service, cancellation.Token());
        REQUIRE(service.ActiveScene().has_value());
        const auto runtime = service.ActiveScene()->RuntimeId();
        const auto control = Queue(service, 20, std::make_shared<BaselineOperation>(SceneDefinitionId{20}, 1, 1));
        Commit(service, cancellation.Token());
        REQUIRE(control->Complete());
        REQUIRE(control->Failure() == nullptr);
        REQUIRE(control->ResultValue() != nullptr);
        REQUIRE(control->ResultValue()->baselinesAttached == 1);
        REQUIRE(service.ActiveScene()->FindBaseline({20}).has_value());
        const auto operation = std::make_shared<BaselineOperation>(SceneDefinitionId{21}, 1, 1);
        const auto receipt = Queue(service, 21, operation);
        RejectForeignCancels(service, *receipt, *operation);
        REQUIRE(service.CancelPendingBaseline(runtime, {21}, *operation).HasValue());
        Commit(service, cancellation.Token());
        CheckCancelled(*receipt);
        const auto failure = receipt->Failure() ? std::optional<Error>{*receipt->Failure()} : std::nullopt;
        const auto later = Queue(service, 22, std::make_shared<BaselineOperation>(SceneDefinitionId{22}, 1, 1));
        REQUIRE(later != receipt);
        Commit(service, cancellation.Token());
        REQUIRE(later->Complete());
        REQUIRE(later->Failure() == nullptr);
        REQUIRE(later->ResultValue() != nullptr);
        CHECK(later->ResultValue()->baselinesAttached == 1);
        CheckStableFailure(*receipt, failure);
        CHECK(service.ActiveScene()->RuntimeId() == runtime);
        CHECK(service.ActiveScene()->FindBaseline({20}).has_value());
        CHECK_FALSE(service.ActiveScene()->FindBaseline({21}).has_value());
        CHECK(service.ActiveScene()->FindBaseline({22}).has_value());
        service.Shutdown();
        CheckStableFailure(*receipt, failure);
    }
}  // namespace
