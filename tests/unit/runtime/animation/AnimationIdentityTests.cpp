#include "AnimationTestFixtures.h"
#include "Horo/Animation/AnimationIdentity.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <set>
#include <string_view>
#include <type_traits>

namespace Horo::Animation {
    namespace {
        using Test::Asset;
        using Test::Id;

        AnimationInstanceHandle Instance(const std::uint64_t owner = 1, const std::uint64_t component = 2, const std::uint32_t slot = 3,
                                         const std::uint32_t generation = 4) {
            return {Id<AnimationRuntimeId>(owner), Id<AnimationComponentId>(component), {slot, generation}};
        }

        PoseHandle Pose(const AnimationInstanceHandle instance = Instance(), const std::uint32_t slot = 5,
                        const std::uint32_t slotGeneration = 6, const std::uint64_t poseGeneration = 7) {
            return {instance, {slot, slotGeneration}, Id<PoseGeneration>(poseGeneration)};
        }

        RootMotionRequestId Request(const AnimationInstanceHandle instance = Instance(), const std::uint64_t tick = 8,
                                    const std::uint64_t generation = 9) {
            return {instance, Id<AnimationTickId>(tick), Id<RootMotionGeneration>(generation)};
        }

        template <typename Value> void RequireCode(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            const auto &error = result.ErrorValue();
            CHECK((error.domain.Value() == "horo.animation" && error.code.Value() == expected.code.Value()));
        }
    }  // namespace

    TEST_CASE("Animation asset and authored identities remain distinct stable values", "[unit][animation][identity]") {
        const auto skeleton = Asset<SkeletonId>(1);
        const auto clip = Asset<AnimationClipId>(1);
        const auto graph = Asset<AnimationGraphId>(1);
        const auto retarget = Asset<RetargetProfileId>(1);

        CHECK(skeleton.Asset() == clip.Asset());
        CHECK(graph.Asset() == retarget.Asset());
        static_assert(!std::is_same_v<SkeletonId, AnimationClipId>);
        static_assert(!std::is_same_v<AnimationGraphId, RetargetProfileId>);
        static_assert(!std::is_same_v<JointId, SkeletonSocketId>);
        static_assert(!std::is_convertible_v<Assets::AssetId, SkeletonId>);
        static_assert(std::is_trivially_copyable_v<AnimationComponentId>);
        REQUIRE(SkeletonId::Create({}).HasError());
        REQUIRE(AnimationComponentId::Create(0).HasError());
        REQUIRE(JointId::Create(0).HasError());

        const std::set ordered{Id<JointId>(5), Id<JointId>(2), Id<JointId>(9)};
        CHECK(ordered == std::set{Id<JointId>(2), Id<JointId>(5), Id<JointId>(9)});
    }

    TEST_CASE("Animation instance access fences runtime component and slot generations", "[unit][animation][identity]") {
        const auto current = Instance();
        REQUIRE(ValidateAnimationInstanceAccess(current, current).HasValue());
        RequireCode(ValidateAnimationInstanceAccess({}, current), AnimationErrors::HandleMalformed);
        RequireCode(ValidateAnimationInstanceAccess(Instance(10), current), AnimationErrors::HandleOwnerMismatch);
        RequireCode(ValidateAnimationInstanceAccess(Instance(1, 20), current), AnimationErrors::HandleOwnerMismatch);
        RequireCode(ValidateAnimationInstanceAccess(Instance(1, 2, 30), current), AnimationErrors::HandleStale);
        RequireCode(ValidateAnimationInstanceAccess(Instance(1, 2, 3, 40), current), AnimationErrors::HandleStale);
        CHECK_FALSE(AnimationInstanceHandle{Id<AnimationRuntimeId>(1), Id<AnimationComponentId>(2), {3, 0}}.IsValid());
        CHECK_FALSE(Instance(1, 2, MaximumAnimationHandleSlots).IsValid());
    }

    TEST_CASE("Pose handles reject cross-instance storage and retired semantic generations", "[unit][animation][identity]") {
        const auto current = Instance();
        REQUIRE(ValidatePoseAccess(Pose(current), current, Id<PoseGeneration>(7)).HasValue());
        RequireCode(ValidatePoseAccess({}, current, Id<PoseGeneration>(7)), AnimationErrors::HandleMalformed);
        RequireCode(ValidatePoseAccess(Pose(Instance(10)), current, Id<PoseGeneration>(7)), AnimationErrors::HandleOwnerMismatch);
        RequireCode(ValidatePoseAccess(Pose(current, 5, 6, 70), current, Id<PoseGeneration>(7)), AnimationErrors::HandleStale);
        RequireCode(ValidatePoseAccess(Pose(current), current, {}), AnimationErrors::HandleMalformed);
        CHECK_FALSE(Pose(current, MaximumAnimationHandleSlots).IsValid());
        static_assert(std::is_trivially_copyable_v<PoseHandle>);
    }

    TEST_CASE("Root motion request identity is exact for instance tick and request generation", "[unit][animation][identity]") {
        const auto current = Instance();
        REQUIRE(ValidateRootMotionRequestAccess(Request(current), current, Id<AnimationTickId>(8), Id<RootMotionGeneration>(9)).HasValue());
        RequireCode(ValidateRootMotionRequestAccess({}, current, Id<AnimationTickId>(8), Id<RootMotionGeneration>(9)),
                    AnimationErrors::HandleMalformed);
        RequireCode(ValidateRootMotionRequestAccess(Request(Instance(11)), current, Id<AnimationTickId>(8), Id<RootMotionGeneration>(9)),
                    AnimationErrors::HandleOwnerMismatch);
        RequireCode(ValidateRootMotionRequestAccess(Request(current, 80), current, Id<AnimationTickId>(8), Id<RootMotionGeneration>(9)),
                    AnimationErrors::HandleStale);
        RequireCode(ValidateRootMotionRequestAccess(Request(current, 8, 90), current, Id<AnimationTickId>(8), Id<RootMotionGeneration>(9)),
                    AnimationErrors::HandleStale);
    }

    TEST_CASE("Presentation pose handles are frame-local and cannot alias replacement poses", "[unit][animation][identity]") {
        const auto source = Pose();
        const PresentationPoseHandle current{source, Id<AnimationFrameId>(10), {11, 12}};
        REQUIRE(ValidatePresentationPoseAccess(current, source, Id<AnimationFrameId>(10)).HasValue());
        RequireCode(ValidatePresentationPoseAccess({}, source, Id<AnimationFrameId>(10)), AnimationErrors::HandleMalformed);

        auto foreign = current;
        foreign.source = Pose(Instance(20));
        RequireCode(ValidatePresentationPoseAccess(foreign, source, Id<AnimationFrameId>(10)), AnimationErrors::HandleOwnerMismatch);
        auto stale = current;
        stale.source.generation = Id<PoseGeneration>(70);
        RequireCode(ValidatePresentationPoseAccess(stale, source, Id<AnimationFrameId>(10)), AnimationErrors::HandleStale);
        RequireCode(ValidatePresentationPoseAccess(current, source, Id<AnimationFrameId>(100)), AnimationErrors::HandleStale);

        CHECK_FALSE(PresentationPoseHandle{source, Id<AnimationFrameId>(10), {MaximumAnimationHandleSlots, 1}}.IsValid());
        static_assert(std::is_trivially_copyable_v<PresentationPoseHandle>);
    }

    TEST_CASE("Animation generations advance without wrap or reserved values", "[unit][animation][identity]") {
        RequireCode(AdvanceAnimationGeneration(PoseGeneration{}), AnimationErrors::IdentityInvalid);
        CHECK(AdvanceAnimationGeneration(Id<PoseGeneration>(41)).Value() == Id<PoseGeneration>(42));
        RequireCode(AdvanceAnimationGeneration(Id<PoseGeneration>(std::numeric_limits<std::uint64_t>::max())),
                    AnimationErrors::GenerationExhausted);
        CHECK(AdvanceAnimationGeneration(Id<RootMotionGeneration>(9)).Value() == Id<RootMotionGeneration>(10));
        CHECK(AdvanceAnimationGeneration(Id<AnimationClipGeneration>(12)).Value() == Id<AnimationClipGeneration>(13));
    }

    TEST_CASE("Animation failures expose unique stable public identities", "[unit][animation][errors]") {
        const std::array descriptors{
            &AnimationErrors::IdentityInvalid,
            &AnimationErrors::HandleMalformed,
            &AnimationErrors::HandleOwnerMismatch,
            &AnimationErrors::HandleStale,
            &AnimationErrors::GenerationExhausted,
            &AnimationErrors::ComponentInvalid,
            &AnimationErrors::ComponentBindingMismatch,
            &AnimationErrors::ContractVersionUnsupported,
            &AnimationErrors::SkeletonVersionUnsupported,
            &AnimationErrors::SkeletonAdmissionRejected,
            &AnimationErrors::SkeletonValidationCancelled,
            &AnimationErrors::SkeletonReloadMismatch,
            &AnimationErrors::SkeletonLimitExceeded,
            &AnimationErrors::SkeletonDuplicateIdentity,
            &AnimationErrors::SkeletonJointMissing,
            &AnimationErrors::SkeletonHierarchyCycle,
            &AnimationErrors::SkeletonTransformInvalid,
            &AnimationErrors::SkeletonMetadataInvalid,
            &AnimationErrors::SkinningVersionUnsupported,
            &AnimationErrors::SkinningAdmissionRejected,
            &AnimationErrors::SkinningValidationCancelled,
            &AnimationErrors::SkinningReloadMismatch,
            &AnimationErrors::SkinningSkeletonMismatch,
            &AnimationErrors::SkinningBindingStale,
            &AnimationErrors::SkinningLimitExceeded,
            &AnimationErrors::SkinningDuplicateIdentity,
            &AnimationErrors::SkinningJointMissing,
            &AnimationErrors::SkinningInfluenceInvalid,
            &AnimationErrors::SkinningLayoutInvalid,
            &AnimationErrors::PoseVersionUnsupported,
            &AnimationErrors::PoseAdmissionRejected,
            &AnimationErrors::PoseEvaluationCancelled,
            &AnimationErrors::PoseSkeletonMismatch,
            &AnimationErrors::PoseSkeletonStale,
            &AnimationErrors::PoseLimitExceeded,
            &AnimationErrors::PoseArenaExhausted,
            &AnimationErrors::PoseLeaseConflict,
            &AnimationErrors::PoseTransformInvalid,
            &AnimationErrors::PoseJointMissing,
            &AnimationErrors::PoseSocketMissing,
            &AnimationErrors::PoseNotEvaluated,
            &AnimationErrors::PoseFrameStale,
            &AnimationErrors::PoseThreadViolation,
            &AnimationErrors::ClipVersionUnsupported,
            &AnimationErrors::ClipAdmissionRejected,
            &AnimationErrors::ClipOperationCancelled,
            &AnimationErrors::ClipReloadMismatch,
            &AnimationErrors::ClipSkeletonMismatch,
            &AnimationErrors::ClipBindingStale,
            &AnimationErrors::ClipLimitExceeded,
            &AnimationErrors::ClipDuplicateIdentity,
            &AnimationErrors::ClipJointMissing,
            &AnimationErrors::ClipMalformed,
            &AnimationErrors::ClipUnsupported,
            &AnimationErrors::ClipTimeOverflow,
            &AnimationErrors::ClipReferencePoseMismatch,
        };
        std::set<std::string_view> codes;
        for (const ErrorCodeDescriptor *descriptor : descriptors) {
            CHECK(descriptor->domain.Value() == "horo.animation");
            CHECK(codes.insert(descriptor->code.Value()).second);
            CHECK_FALSE(descriptor->summary.empty());
            CHECK_FALSE(descriptor->remediationHint.empty());
        }
    }
}  // namespace Horo::Animation
