#include "CliHostErrorTranslation.h"
#include "Horo/Animation/AnimationCookedAssets.h"
#include "Horo/Animation/AnimationErrors.h"
#include "Horo/Foundation/JobSystem.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace Horo::Application::Internal {
    TEST_CASE("Native host translates the exact Animation Scene producer error contract", "[animation][cli][errors]") {
        using enum Hosts::ExitCategory;
        const auto translator = MakeCliHostErrorTranslator();
        REQUIRE(translator);
        const std::array cases{std::pair{&Animation::AnimationErrors::IdentityInvalid, Validation},
                               std::pair{&Animation::AnimationErrors::HandleMalformed, Validation},
                               std::pair{&Animation::AnimationErrors::HandleOwnerMismatch, Validation},
                               std::pair{&Animation::AnimationErrors::HandleStale, Validation},
                               std::pair{&Animation::AnimationErrors::GenerationExhausted, Invariant},
                               std::pair{&Animation::AnimationErrors::ComponentInvalid, Validation},
                               std::pair{&Animation::AnimationErrors::ComponentBindingMismatch, Validation},
                               std::pair{&Animation::AnimationErrors::ContractVersionUnsupported, Capability},
                               std::pair{&Animation::AnimationErrors::SkeletonVersionUnsupported, Capability},
                               std::pair{&Animation::AnimationErrors::SkeletonAdmissionRejected, Operation},
                               std::pair{&Animation::AnimationErrors::SkeletonValidationCancelled, Cancelled},
                               std::pair{&Animation::AnimationErrors::SkeletonReloadMismatch, Validation},
                               std::pair{&Animation::AnimationErrors::SkeletonLimitExceeded, Validation},
                               std::pair{&Animation::AnimationErrors::SkeletonDuplicateIdentity, Validation},
                               std::pair{&Animation::AnimationErrors::SkeletonJointMissing, Validation},
                               std::pair{&Animation::AnimationErrors::SkeletonHierarchyCycle, Validation},
                               std::pair{&Animation::AnimationErrors::SkeletonTransformInvalid, Validation},
                               std::pair{&Animation::AnimationErrors::SkeletonMetadataInvalid, Validation},
                               std::pair{&Animation::AnimationErrors::PoseFrameStale, Validation},
                               std::pair{&Animation::AnimationErrors::PoseThreadViolation, Invariant},
                               std::pair{&Animation::AnimationErrors::ClipVersionUnsupported, Capability},
                               std::pair{&Animation::AnimationErrors::ClipAdmissionRejected, Operation},
                               std::pair{&Animation::AnimationErrors::ClipOperationCancelled, Cancelled},
                               std::pair{&Animation::AnimationErrors::ClipReloadMismatch, Validation},
                               std::pair{&Animation::AnimationErrors::ClipSkeletonMismatch, Validation},
                               std::pair{&Animation::AnimationErrors::ClipBindingStale, Validation},
                               std::pair{&Animation::AnimationErrors::ClipLimitExceeded, Validation},
                               std::pair{&Animation::AnimationErrors::ClipDuplicateIdentity, Validation},
                               std::pair{&Animation::AnimationErrors::ClipJointMissing, Validation},
                               std::pair{&Animation::AnimationErrors::ClipMalformed, Validation},
                               std::pair{&Animation::AnimationErrors::ClipUnsupported, Capability},
                               std::pair{&Animation::AnimationErrors::ClipTimeOverflow, Validation},
                               std::pair{&Animation::AnimationErrors::ClipReferencePoseMismatch, Validation},
                               std::pair{&Animation::AnimationErrors::GraphVersionUnsupported, Capability},
                               std::pair{&Animation::AnimationErrors::GraphAdmissionRejected, Operation},
                               std::pair{&Animation::AnimationErrors::GraphOperationCancelled, Cancelled},
                               std::pair{&Animation::AnimationErrors::GraphReloadMismatch, Validation},
                               std::pair{&Animation::AnimationErrors::GraphLimitExceeded, Operation},
                               std::pair{&Animation::AnimationErrors::GraphMalformed, Validation},
                               std::pair{&Animation::AnimationErrors::GraphTypeMismatch, Validation},
                               std::pair{&Animation::AnimationErrors::GraphCycle, Validation},
                               std::pair{&Animation::AnimationErrors::GraphBindingMismatch, Validation},
                               std::pair{&Animation::AnimationAssetErrors::Malformed, Validation},
                               std::pair{&Animation::AnimationAssetErrors::Unsupported, Capability},
                               std::pair{&Animation::AnimationAssetErrors::Capacity, Operation},
                               std::pair{&Animation::AnimationAssetErrors::Cancelled, Cancelled},
                               std::pair{&Animation::AnimationAssetErrors::Unavailable, Operation},
                               std::pair{&Animation::AnimationAssetErrors::Stale, Validation}};
        for (const auto &[descriptor, category] : cases) {
            const auto translated = translator->Translate(MakeError(*descriptor));
            REQUIRE(translated);
            CHECK(translated->Category() == category);
            CHECK(translated->Failure().domain.Value() == descriptor->domain.Value());
            CHECK(translated->Failure().code.Value() == descriptor->code.Value());
        }
    }

    TEST_CASE("Native Animation host refuses guessed identities and preserves registered causes", "[animation][cli][errors]") {
        const auto translator = MakeCliHostErrorTranslator();
        REQUIRE(translator);
        const ErrorCodeDescriptor unknown{.domain = ErrorDomainId{"horo.animation"},
                                          .code = ErrorCode{"animation.graph.undeclared"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "Undeclared Animation failure."};
        CHECK_FALSE(translator->Translate(MakeError(unknown)));
        const auto unknownCause = WrapError(Animation::AnimationAssetErrors::Malformed, MakeError(unknown));
        CHECK_FALSE(translator->Translate(unknownCause));
        auto wrongDomain = MakeError(Animation::AnimationErrors::GraphMalformed);
        wrongDomain.domain = ErrorDomainId{"horo.animation.asset"};
        CHECK_FALSE(translator->Translate(wrongDomain));
        const auto cancelled = JobCancelled();
        REQUIRE(cancelled.HasError());
        const auto wrapped = WrapError(Animation::AnimationErrors::GraphAdmissionRejected, cancelled.ErrorValue());
        const auto presented = translator->Translate(wrapped);
        REQUIRE(presented);
        CHECK(presented->Category() == Hosts::ExitCategory::Operation);
        REQUIRE(presented->Failure().cause.Get());
        CHECK(presented->Failure().cause.Get()->domain.Value() == cancelled.ErrorValue().domain.Value());
        CHECK(presented->Failure().cause.Get()->code.Value() == cancelled.ErrorValue().code.Value());
    }
}  // namespace Horo::Application::Internal
