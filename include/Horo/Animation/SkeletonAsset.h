#pragma once

/**
 * @file SkeletonAsset.h
 * @brief Immutable typed skeleton asset model and deterministic hierarchy validation.
 */

#include "Horo/Animation/AnimationIdentity.h"
#include "Horo/Math/SceneMath.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Animation {
    /** @brief Exact portable skeleton asset contract version. */
    struct SkeletonAssetContractVersion final {
        std::uint16_t major{};
        std::uint16_t minor{};
        std::uint16_t patch{};

        [[nodiscard]] constexpr auto operator<=>(const SkeletonAssetContractVersion &) const noexcept = default;
    };

    /** @brief Skeleton asset version implemented by this API slice. */
    inline constexpr SkeletonAssetContractVersion CurrentSkeletonAssetContractVersion{1, 0, 0};

    /** @brief Compile-time safety ceilings for one portable skeleton asset. */
    struct SkeletonAssetHardLimits final {
        static constexpr std::uint32_t Joints = 1'024;       /**< Absolute joint ceiling. */
        static constexpr std::uint32_t HierarchyDepth = 256; /**< Absolute root-inclusive hierarchy depth. */
        static constexpr std::uint32_t Sockets = 256;        /**< Absolute socket ceiling. */
        static constexpr std::uint32_t NameBytes = 127;      /**< Absolute byte ceiling for advisory names. */
    };

    /** @brief Finite policy captured before a skeleton validation transaction begins. */
    struct SkeletonAssetLimits final {
        std::uint32_t maximumJoints{SkeletonAssetHardLimits::Joints};
        std::uint32_t maximumHierarchyDepth{SkeletonAssetHardLimits::HierarchyDepth};
        std::uint32_t maximumSockets{SkeletonAssetHardLimits::Sockets};
        std::uint32_t maximumNameBytes{SkeletonAssetHardLimits::NameBytes};

        [[nodiscard]] constexpr auto operator<=>(const SkeletonAssetLimits &) const noexcept = default;
    };

    /** @brief Closed humanoid retargeting role vocabulary; Unspecified preserves non-humanoid joints. */
    enum class SkeletonRetargetRole : std::uint8_t {
        Unspecified,
        Root,
        Pelvis,
        Spine,
        Chest,
        Neck,
        Head,
        Shoulder,
        UpperArm,
        LowerArm,
        Hand,
        UpperLeg,
        LowerLeg,
        Foot,
        Toe,
        Count
    };

    /** @brief Semantic side used to validate retargeting and reciprocal mirror metadata. */
    enum class SkeletonJointSide : std::uint8_t {
        Center,
        Left,
        Right,
        Count
    };

    /** @brief One authored joint keyed exclusively by stable skeleton-local identity. */
    struct SkeletonJoint final {
        JointId id{};                                         /**< Stable identity, never a dense array index. */
        std::optional<JointId> parent{};                      /**< Empty for a root; otherwise an exact stable parent identity. */
        std::string name{};                                   /**< Bounded advisory name, never identity. */
        Math::Transform referenceLocalTransform{};            /**< Local reference-pose transform. */
        Math::Mat4 inverseBindMatrix{Math::Mat4::Identity()}; /**< Inverse of the reference model-space transform. */
        SkeletonRetargetRole retargetRole{SkeletonRetargetRole::Unspecified}; /**< Typed optional retargeting semantic. */
        SkeletonJointSide side{SkeletonJointSide::Center};                    /**< Semantic side for retarget/mirror policy. */
        std::optional<JointId> mirror{};                                      /**< Reciprocal opposite-side joint, when authored. */

        [[nodiscard]] bool operator==(const SkeletonJoint &) const noexcept = default;
    };

    /** @brief Stable attachment point expressed relative to one exact joint. */
    struct SkeletonSocket final {
        SkeletonSocketId id{};            /**< Stable socket identity. */
        JointId joint{};                  /**< Stable owning joint identity. */
        std::string name{};               /**< Bounded advisory name, never identity. */
        Math::Transform localTransform{}; /**< Socket transform in owning-joint space. */

        [[nodiscard]] bool operator==(const SkeletonSocket &) const noexcept = default;
    };

    /** @brief Mutable candidate copied into an immutable skeleton snapshot only after complete validation. */
    struct SkeletonAssetData final {
        SkeletonAssetContractVersion contractVersion{CurrentSkeletonAssetContractVersion};
        SkeletonId skeleton{};                 /**< Persistent Assets-owned skeleton identity. */
        std::vector<SkeletonJoint> joints{};   /**< Candidate order; canonicalized during creation. */
        std::vector<SkeletonSocket> sockets{}; /**< Candidate order; canonicalized by stable identity. */

        [[nodiscard]] bool operator==(const SkeletonAssetData &) const noexcept = default;
    };

    /** @brief Owner-state snapshot used to fail closed before a validation transaction allocates work storage. */
    enum class SkeletonAssetAdmissionState : std::uint8_t {
        Accepting,
        CancellationRequested,
        ShuttingDown,
        Count
    };

    /** @brief Immutable operation inputs captured by the asset owner before validation. */
    struct SkeletonAssetBuildContext final {
        SkeletonAssetAdmissionState admission{SkeletonAssetAdmissionState::Accepting}; /**< Owner admission snapshot. */
        std::optional<SkeletonId> replacing{}; /**< Stable current identity for an atomic reload candidate. */
        SkeletonAssetLimits limits{};          /**< Finite limits fixed for the complete operation. */
    };

    /** @brief Immutable, validated skeleton whose joints have one deterministic parent-before-child order. */
    class SkeletonAsset final {
    public:
        /**
         * @brief Validates and takes ownership of a complete skeleton candidate transactionally.
         * @param candidate Candidate identity, hierarchy, reference/bind poses, metadata, and sockets.
         * @param context Captured admission, reload identity, and finite-limit policy.
         * @return Immutable skeleton or a stable version, admission, hierarchy, metadata, transform, or limit failure.
         * @pre Load/cook/control boundary; never invoke from frame-hot pose evaluation.
         * @post Success owns canonical data; failure publishes no partial hierarchy or replacement.
         */
        [[nodiscard]] static Result<SkeletonAsset> Create(SkeletonAssetData candidate, const SkeletonAssetBuildContext &context = {});

        /** @brief Returns the immutable canonical source data. @return Borrowed data owned by this snapshot. */
        [[nodiscard]] const SkeletonAssetData &Data() const noexcept;

        /** @brief Returns roots sorted by stable identity. @return Borrowed immutable root identities. */
        [[nodiscard]] std::span<const JointId> RootJoints() const noexcept;

        /** @brief Resolves a stable joint identity to its canonical pose index.
         * @param joint Skeleton-local identity.
         * @return Parent-before-child index, or empty if the identity is absent.
         */
        [[nodiscard]] std::optional<std::size_t> FindJointIndex(JointId joint) const noexcept;

        /** @brief Resolves a socket by its stable identity.
         * @param socket Skeleton-local identity.
         * @return Immutable metadata borrowed from this snapshot, or null if absent.
         */
        [[nodiscard]] const SkeletonSocket *FindSocket(SkeletonSocketId socket) const noexcept;

    private:
        SkeletonAsset(SkeletonAssetData data, std::vector<JointId> roots, std::vector<std::size_t> jointsById) noexcept
            : data_(std::move(data)), roots_(std::move(roots)), jointsById_(std::move(jointsById)) {}

        SkeletonAssetData data_;
        std::vector<JointId> roots_;
        std::vector<std::size_t> jointsById_;
    };
}  // namespace Horo::Animation
