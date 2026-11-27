#include "Horo/Animation/SkeletonAsset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <ranges>

namespace Horo::Animation {
    namespace {
        constexpr std::size_t NoJoint = std::numeric_limits<std::size_t>::max();
        constexpr float BindPoseTolerance = 0.001F;

        /** @brief Returns one stable skeleton failure without exposing validation implementation state. */
        template <typename Value> [[nodiscard]] Result<Value> Fail(const ErrorCodeDescriptor &error) {
            return Result<Value>::Failure(MakeError(error));
        }

        /** @brief Checks that an enum belongs to its closed public vocabulary. */
        template <typename Enum> [[nodiscard]] constexpr bool IsKnown(const Enum value) noexcept {
            return value < Enum::Count;
        }

        /** @brief Validates finite non-zero caller policy beneath hard engine ceilings. */
        [[nodiscard]] constexpr bool AreLimitsValid(const SkeletonAssetLimits &limits) noexcept {
            return limits.maximumJoints > 0 && limits.maximumJoints <= SkeletonAssetHardLimits::Joints &&
                   limits.maximumHierarchyDepth > 0 && limits.maximumHierarchyDepth <= SkeletonAssetHardLimits::HierarchyDepth &&
                   limits.maximumSockets <= SkeletonAssetHardLimits::Sockets && limits.maximumNameBytes > 0 &&
                   limits.maximumNameBytes <= SkeletonAssetHardLimits::NameBytes;
        }

        /** @brief Validates an advisory name without treating it as identity. */
        [[nodiscard]] bool IsValidName(const std::string &name, const SkeletonAssetLimits &limits) noexcept {
            return !name.empty() && name.size() <= limits.maximumNameBytes && std::ranges::find(name, '\0') == name.end();
        }

        /** @brief Resolves one joint identity in stable-ID-sorted candidate storage. */
        [[nodiscard]] std::optional<std::size_t> FindJoint(const std::vector<SkeletonJoint> &joints, const JointId id) {
            const auto found = std::ranges::lower_bound(joints, id, {}, &SkeletonJoint::id);
            if (found == joints.end() || found->id != id)
                return std::nullopt;
            return static_cast<std::size_t>(std::distance(joints.begin(), found));
        }

        /** @brief Validates the stable identities referenced by one joint. */
        [[nodiscard]] bool HasValidIdentities(const SkeletonJoint &joint) noexcept {
            const bool parentValid = !joint.parent || joint.parent->IsValid();
            const bool mirrorValid = !joint.mirror || joint.mirror->IsValid();
            return joint.id.IsValid() && parentValid && mirrorValid;
        }

        /** @brief Validates the closed and bounded descriptive metadata of one joint. */
        [[nodiscard]] bool HasValidMetadata(const SkeletonJoint &joint, const SkeletonAssetLimits &limits) noexcept {
            return IsValidName(joint.name, limits) && IsKnown(joint.retargetRole) && IsKnown(joint.side);
        }

        /** @brief Validates the finite invertible transform representation of one joint. */
        [[nodiscard]] bool HasValidTransform(const SkeletonJoint &joint) {
            return joint.referenceLocalTransform.TryToMatrix().HasValue() && Math::IsFinite(joint.inverseBindMatrix) &&
                   Math::TryInverseAffine(joint.inverseBindMatrix).HasValue();
        }

        /** @brief Validates stable identity, bounded metadata and local transform representation. */
        [[nodiscard]] Result<void> ValidateJointRepresentations(const std::vector<SkeletonJoint> &joints,
                                                                const SkeletonAssetLimits &limits) {
            for (const SkeletonJoint &joint : joints) {
                if (!HasValidIdentities(joint))
                    return Fail<void>(AnimationErrors::IdentityInvalid);
                if (!HasValidMetadata(joint, limits))
                    return Fail<void>(AnimationErrors::SkeletonMetadataInvalid);
                if (!HasValidTransform(joint))
                    return Fail<void>(AnimationErrors::SkeletonTransformInvalid);
            }
            return Result<void>::Success();
        }

        /** @brief Checks that a declared mirror has the opposite lateral side. */
        [[nodiscard]] constexpr bool HasOppositeSide(const SkeletonJointSide side, const SkeletonJointSide mirrorSide) noexcept {
            using enum SkeletonJointSide;
            if (side == Left)
                return mirrorSide == Right;
            if (side == Right)
                return mirrorSide == Left;
            return false;
        }

        /** @brief Checks the reciprocal identity and semantic invariants of a mirror pair. */
        [[nodiscard]] bool IsValidMirrorPair(const SkeletonJoint &joint, const SkeletonJoint &mirror) noexcept {
            return mirror.id != joint.id && mirror.mirror == joint.id && mirror.retargetRole == joint.retargetRole &&
                   HasOppositeSide(joint.side, mirror.side);
        }

        /** @brief Validates reciprocal typed mirror metadata after all joint identities are known. */
        [[nodiscard]] Result<void> ValidateMirrorMetadata(const std::vector<SkeletonJoint> &joints) {
            for (const SkeletonJoint &joint : joints) {
                if (!joint.mirror)
                    continue;
                const auto mirrorIndex = FindJoint(joints, *joint.mirror);
                if (!mirrorIndex.has_value())
                    return Fail<void>(AnimationErrors::SkeletonJointMissing);
                const SkeletonJoint &mirror = joints[*mirrorIndex];
                if (!IsValidMirrorPair(joint, mirror))
                    return Fail<void>(AnimationErrors::SkeletonMetadataInvalid);
            }
            return Result<void>::Success();
        }

        /** @brief Resolves all parent identities before topology processing. */
        [[nodiscard]] Result<std::vector<std::size_t>> ResolveParents(const std::vector<SkeletonJoint> &joints) {
            std::vector parents(joints.size(), NoJoint);
            for (std::size_t index = 0; index < joints.size(); ++index) {
                if (!joints[index].parent)
                    continue;
                const auto parentIndex = FindJoint(joints, *joints[index].parent);
                if (!parentIndex.has_value())
                    return Fail<std::vector<std::size_t>>(AnimationErrors::SkeletonJointMissing);
                parents[index] = *parentIndex;
            }
            return Result<std::vector<std::size_t>>::Success(std::move(parents));
        }

        /** @brief Maintains a minimum-identity ready heap for canonical topological ordering. */
        class JointReadyQueue final {
        public:
            explicit JointReadyQueue(const std::vector<SkeletonJoint> &joints) : joints_(joints) {
                ready_.reserve(joints.size());
            }

            void Push(const std::size_t index) {
                ready_.push_back(index);
                std::ranges::push_heap(ready_, Greater{joints_});
            }

            [[nodiscard]] std::size_t Pop() {
                std::ranges::pop_heap(ready_, Greater{joints_});
                const std::size_t index = ready_.back();
                ready_.pop_back();
                return index;
            }

            [[nodiscard]] bool Empty() const noexcept {
                return ready_.empty();
            }

        private:
            struct Greater final {
                const std::vector<SkeletonJoint> &joints;

                [[nodiscard]] bool operator()(const std::size_t left, const std::size_t right) const noexcept {
                    return joints[left].id > joints[right].id;
                }
            };

            const std::vector<SkeletonJoint> &joints_;
            std::vector<std::size_t> ready_;
        };

        /** @brief Computes the lexicographically least complete topological order and bounded depths. */
        [[nodiscard]] Result<std::vector<std::size_t>> BuildCanonicalOrder(const std::vector<SkeletonJoint> &joints,
                                                                           const std::vector<std::size_t> &parents,
                                                                           const std::uint32_t maximumDepth) {
            JointReadyQueue ready{joints};
            std::vector<std::uint32_t> depths(joints.size());
            std::vector<bool> emitted(joints.size());
            std::vector<std::size_t> order;
            order.reserve(joints.size());
            for (std::size_t index = 0; index < joints.size(); ++index) {
                if (parents[index] == NoJoint) {
                    depths[index] = 1;
                    ready.Push(index);
                }
            }

            while (!ready.Empty()) {
                const std::size_t parent = ready.Pop();
                emitted[parent] = true;
                order.push_back(parent);
                for (std::size_t child = 0; child < joints.size(); ++child) {
                    if (parents[child] != parent || emitted[child])
                        continue;
                    depths[child] = depths[parent] + 1U;
                    if (depths[child] > maximumDepth)
                        return Fail<std::vector<std::size_t>>(AnimationErrors::SkeletonLimitExceeded);
                    ready.Push(child);
                }
            }
            if (order.size() != joints.size())
                return Fail<std::vector<std::size_t>>(AnimationErrors::SkeletonHierarchyCycle);
            return Result<std::vector<std::size_t>>::Success(std::move(order));
        }

        /** @brief Checks an affine product against identity with a fixed portable tolerance. */
        [[nodiscard]] bool IsNearIdentity(const Math::Mat4 &matrix) noexcept {
            const Math::Mat4 identity = Math::Mat4::Identity();
            for (std::size_t index = 0; index < matrix.values.size(); ++index) {
                if (std::fabs(matrix.values[index] - identity.values[index]) > BindPoseTolerance)
                    return false;
            }
            return true;
        }

        /** @brief Validates inverse bind matrices against the canonical reference hierarchy. */
        [[nodiscard]] Result<void> ValidateBindPose(const std::vector<SkeletonJoint> &joints) {
            std::vector<Math::Mat4> modelTransforms;
            modelTransforms.reserve(joints.size());
            for (std::size_t index = 0; index < joints.size(); ++index) {
                const Math::Mat4 local = joints[index].referenceLocalTransform.ToMatrix();
                Math::Mat4 model = local;
                if (joints[index].parent) {
                    const auto end = joints.begin() + static_cast<std::ptrdiff_t>(index);
                    const auto parent = std::find_if(joints.begin(), end, [&](const SkeletonJoint &candidate) {
                        return candidate.id == *joints[index].parent;
                    });
                    if (parent == end)
                        return Fail<void>(AnimationErrors::SkeletonHierarchyCycle);
                    const auto parentIndex = static_cast<std::size_t>(std::distance(joints.begin(), parent));
                    model = Math::Multiply(modelTransforms[parentIndex], local);
                }
                if (!IsNearIdentity(Math::Multiply(model, joints[index].inverseBindMatrix)))
                    return Fail<void>(AnimationErrors::SkeletonTransformInvalid);
                modelTransforms.push_back(model);
            }
            return Result<void>::Success();
        }

        /** @brief Validates sockets and canonicalizes them by stable identity. */
        [[nodiscard]] Result<void> ValidateSockets(std::vector<SkeletonSocket> &sockets, const std::vector<SkeletonJoint> &joints,
                                                   const SkeletonAssetLimits &limits) {
            std::ranges::sort(sockets, {}, &SkeletonSocket::id);
            for (std::size_t index = 0; index < sockets.size(); ++index) {
                const SkeletonSocket &socket = sockets[index];
                if (!socket.id.IsValid() || !socket.joint.IsValid())
                    return Fail<void>(AnimationErrors::IdentityInvalid);
                if (index > 0 && sockets[index - 1].id == socket.id)
                    return Fail<void>(AnimationErrors::SkeletonDuplicateIdentity);
                if (std::ranges::find(joints, socket.joint, &SkeletonJoint::id) == joints.end())
                    return Fail<void>(AnimationErrors::SkeletonJointMissing);
                if (!IsValidName(socket.name, limits))
                    return Fail<void>(AnimationErrors::SkeletonMetadataInvalid);
                if (!socket.localTransform.TryToMatrix().HasValue())
                    return Fail<void>(AnimationErrors::SkeletonTransformInvalid);
            }
            return Result<void>::Success();
        }

        /** @brief Reorders joints from stable-ID lookup order into canonical hierarchy order. */
        [[nodiscard]] std::vector<SkeletonJoint> ReorderJoints(std::vector<SkeletonJoint> joints, const std::vector<std::size_t> &order) {
            std::vector<SkeletonJoint> ordered;
            ordered.reserve(joints.size());
            for (const std::size_t index : order)
                ordered.push_back(std::move(joints[index]));
            return ordered;
        }

        /** @brief Checks that optional reload identity agrees with the candidate asset. */
        [[nodiscard]] bool MatchesReloadIdentity(const SkeletonAssetData &candidate, const SkeletonAssetBuildContext &context) noexcept {
            if (!context.replacing)
                return true;
            return context.replacing->IsValid() && *context.replacing == candidate.skeleton;
        }

        /** @brief Checks candidate cardinalities against validated caller limits. */
        [[nodiscard]] bool FitsLimits(const SkeletonAssetData &candidate, const SkeletonAssetLimits &limits) noexcept {
            const bool jointCountValid = !candidate.joints.empty() && candidate.joints.size() <= limits.maximumJoints;
            return jointCountValid && candidate.sockets.size() <= limits.maximumSockets;
        }

        /** @brief Validates admission, version, identity and caller-supplied bounds. */
        [[nodiscard]] Result<void> ValidateBuildRequest(const SkeletonAssetData &candidate, const SkeletonAssetBuildContext &context) {
            if (context.admission == SkeletonAssetAdmissionState::CancellationRequested)
                return Fail<void>(AnimationErrors::SkeletonValidationCancelled);
            if (context.admission != SkeletonAssetAdmissionState::Accepting)
                return Fail<void>(AnimationErrors::SkeletonAdmissionRejected);
            if (candidate.contractVersion != CurrentSkeletonAssetContractVersion)
                return Fail<void>(AnimationErrors::SkeletonVersionUnsupported);
            if (!candidate.skeleton.IsValid())
                return Fail<void>(AnimationErrors::IdentityInvalid);
            if (!MatchesReloadIdentity(candidate, context))
                return Fail<void>(AnimationErrors::SkeletonReloadMismatch);
            if (!AreLimitsValid(context.limits))
                return Fail<void>(AnimationErrors::SkeletonLimitExceeded);
            if (!FitsLimits(candidate, context.limits))
                return Fail<void>(AnimationErrors::SkeletonLimitExceeded);
            return Result<void>::Success();
        }

        /** @brief Validates stable joint data and derives its canonical hierarchy order. */
        [[nodiscard]] Result<std::vector<std::size_t>> ValidateJoints(std::vector<SkeletonJoint> &joints,
                                                                      const SkeletonAssetLimits &limits) {
            std::ranges::sort(joints, {}, &SkeletonJoint::id);
            if (auto validation = ValidateJointRepresentations(joints, limits); validation.HasError())
                return Result<std::vector<std::size_t>>::Failure(validation.ErrorValue());
            if (const auto duplicate = std::ranges::adjacent_find(joints, {}, &SkeletonJoint::id); duplicate != joints.end())
                return Fail<std::vector<std::size_t>>(AnimationErrors::SkeletonDuplicateIdentity);
            if (auto validation = ValidateMirrorMetadata(joints); validation.HasError())
                return Result<std::vector<std::size_t>>::Failure(validation.ErrorValue());
            auto parents = ResolveParents(joints);
            if (parents.HasError())
                return Result<std::vector<std::size_t>>::Failure(parents.ErrorValue());
            return BuildCanonicalOrder(joints, parents.Value(), limits.maximumHierarchyDepth);
        }
    }  // namespace

    /** @copydoc SkeletonAsset::Create */
    Result<SkeletonAsset> SkeletonAsset::Create(SkeletonAssetData candidate, const SkeletonAssetBuildContext &context) {
        if (auto validation = ValidateBuildRequest(candidate, context); validation.HasError())
            return Result<SkeletonAsset>::Failure(validation.ErrorValue());
        auto canonicalOrder = ValidateJoints(candidate.joints, context.limits);
        if (canonicalOrder.HasError())
            return Result<SkeletonAsset>::Failure(canonicalOrder.ErrorValue());

        std::vector<JointId> roots;
        roots.reserve(candidate.joints.size());
        for (const SkeletonJoint &joint : candidate.joints) {
            if (!joint.parent)
                roots.push_back(joint.id);
        }
        candidate.joints = ReorderJoints(std::move(candidate.joints), canonicalOrder.Value());
        if (auto validation = ValidateBindPose(candidate.joints); validation.HasError())
            return Result<SkeletonAsset>::Failure(validation.ErrorValue());
        if (auto validation = ValidateSockets(candidate.sockets, candidate.joints, context.limits); validation.HasError())
            return Result<SkeletonAsset>::Failure(validation.ErrorValue());
        std::vector<std::size_t> jointsById(candidate.joints.size());
        std::iota(jointsById.begin(), jointsById.end(), std::size_t{0});
        std::ranges::sort(jointsById, {}, [&candidate](const std::size_t index) {
            return candidate.joints[index].id;
        });
        return Result<SkeletonAsset>::Success(SkeletonAsset{std::move(candidate), std::move(roots), std::move(jointsById)});
    }

    /** @copydoc SkeletonAsset::Data */
    const SkeletonAssetData &SkeletonAsset::Data() const noexcept {
        return data_;
    }

    /** @copydoc SkeletonAsset::RootJoints */
    std::span<const JointId> SkeletonAsset::RootJoints() const noexcept {
        return roots_;
    }

    /** @copydoc SkeletonAsset::FindJointIndex */
    std::optional<std::size_t> SkeletonAsset::FindJointIndex(const JointId joint) const noexcept {
        const auto found = std::ranges::lower_bound(jointsById_, joint, {}, [this](const std::size_t index) {
            return data_.joints[index].id;
        });
        if (found == jointsById_.end() || data_.joints[*found].id != joint)
            return std::nullopt;
        return *found;
    }

    /** @copydoc SkeletonAsset::FindSocket */
    const SkeletonSocket *SkeletonAsset::FindSocket(const SkeletonSocketId socket) const noexcept {
        const auto found = std::ranges::lower_bound(data_.sockets, socket, {}, &SkeletonSocket::id);
        return found != data_.sockets.end() && found->id == socket ? &*found : nullptr;
    }
}  // namespace Horo::Animation
