#include "AnimationCookedFormat.h"

#include <algorithm>
#include <new>

namespace Horo::Animation::CookedInternal {
    namespace {
        struct GraphView final {
            AnimationGraphContractVersion contractVersion{CurrentAnimationGraphContractVersion};
            AnimationGraphId id;
            SkeletonId skeleton;
            GraphDefinitionId entry;
            std::span<const GraphParameter> parameters;
            std::span<const GraphActionSlot> actionSlots;
            std::span<const GraphActionDefinition> actions;
            std::span<const GraphCompiledDefinition> definitions;
        };

        struct ClipView final {
            const AnimationClipData &data;
            std::optional<AnimationReferencePoseId> referencePose;
        };
    }  // namespace

    template <> struct Fields<GraphView> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.contractVersion, value.id, value.skeleton, value.entry, value.parameters, value.actionSlots,
                            value.actions, value.definitions);
        }
    };

    template <> struct Fields<ClipView> final {
        template <typename Value> static auto Get(Value &value) {
            const auto &data = value.data;
            const auto &descriptor = data.descriptor;
            return std::tie(descriptor.contractVersion, descriptor.id, descriptor.skeleton, descriptor.duration, descriptor.sampleRate,
                            descriptor.wrapMode, descriptor.kind, descriptor.compression, value.referencePose, descriptor.hasRootMotion,
                            data.tracks, data.syncMarkers, data.eventSchemas, data.events);
        }
    };

    namespace {
        template <typename Version> std::array<std::uint16_t, 3> VersionParts(Version value) {
            return {value.major, value.minor, value.patch};
        }

        std::array<std::uint16_t, 3> CurrentVersion(AnimationCookedKind kind) {
            switch (kind) {
                using enum Horo::Animation::AnimationCookedKind;
                case Skeleton:
                    return VersionParts(CurrentSkeletonAssetContractVersion);
                case Clip:
                    return VersionParts(CurrentAnimationClipContractVersion);
                case Graph:
                    return VersionParts(CurrentAnimationGraphContractVersion);
            }
            return {};
        }

        /** @brief Reserves independent cold scratch/output shares within the caller's one aggregate logical allocation policy. */
        AnimationCookedContext ScratchContext(const AnimationCookedContext &context) {
            auto result = context;
            result.limits.ownedBytes /= 3;
            return result;
        }

        Result<std::vector<Assets::AssetDependency>> Roles(SkeletonId skeleton, std::span<const AnimationClipId> clips,
                                                           const AnimationCookedContext &context) {
            using Return = Result<std::vector<Assets::AssetDependency>>;
            if (clips.size() >= context.limits.dependencies)
                return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
            const auto count = clips.size() + 1;
            if (count > context.limits.ownedBytes / (sizeof(Assets::AssetDependency) + 64))
                return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
            std::vector<Assets::AssetDependency> result;
            result.reserve(count);
            result.emplace_back(skeleton.Asset(), Assets::AssetTypeId::Parse("core.animation.skeleton").Value());
            const auto clipType = Assets::AssetTypeId::Parse("core.animation.clip").Value();
            for (const auto clip : clips)
                result.emplace_back(clip.Asset(), clipType);
            std::ranges::sort(result, {}, &Assets::AssetDependency::id);
            return Return::Success(std::move(result));
        }

        /** @brief Retains complete declarations for the existing Assets cook sink, with no content-authority shortcut. */
        std::vector<Assets::AssetDependency> Declarations(std::span<const AnimationCookedDependency> bindings) {
            std::vector<Assets::AssetDependency> result;
            result.reserve(bindings.size());
            for (const auto &binding : bindings)
                result.push_back(binding.metadata);
            return result;
        }

        bool ValidRole(const AnimationCookedDependency &binding, AnimationCookedKind kind, Assets::AssetId primary) {
            const auto &role = binding.metadata;
            if (!role.id.IsValid() || role.id == primary || binding.payloadDigest == Sha256Digest{})
                return false;
            if (role.expectedType.Value() == "core.animation.skeleton")
                return true;
            return kind == AnimationCookedKind::Graph && role.expectedType.Value() == "core.animation.clip";
        }

        /** @brief Rejects extra/duplicate roles without mistaking a payload hash for publication authority. */
        bool ValidRoles(std::span<const AnimationCookedDependency> roles, AnimationCookedKind kind, Assets::AssetId primary) {
            if (kind == AnimationCookedKind::Skeleton)
                return roles.empty();
            std::size_t skeletons{};
            for (std::size_t index = 0; index < roles.size(); ++index) {
                if (!ValidRole(roles[index], kind, primary))
                    return false;
                if (index != 0 && !(roles[index - 1].metadata.id < roles[index].metadata.id))
                    return false;
                if (roles[index].metadata.expectedType.Value() == "core.animation.skeleton")
                    ++skeletons;
            }
            return skeletons == 1;
        }

        /** @brief Checks descriptor/prefix counts before allocating the complete content-commitment table. */
        Result<std::vector<AnimationCookedDependency>> ReadRoles(const Sections &sections, const AnimationCookedContext &context,
                                                                 std::size_t maximumReturned, std::uint64_t work = 0,
                                                                 std::uint64_t owned = 0) {
            using Return = Result<std::vector<AnimationCookedDependency>>;
            if (sections.dependencyCount > maximumReturned)
                return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
            Reader reader{sections.dependencies, context};
            reader.policy.work = std::max(work, sections.work);
            reader.policy.owned = owned;
            const auto count = reader.Scalar<std::uint32_t>();
            if (count != sections.dependencyCount)
                return Return::Failure(MakeError(AnimationAssetErrors::Malformed));
            if (!reader.Table(count, sizeof(AnimationCookedDependency)))
                return Return::Failure(MakeError(*reader.policy.failure));
            std::vector<AnimationCookedDependency> roles(count);
            for (auto &role : roles)
                Transfer(reader, role);
            if (!reader.policy.Good())
                return Return::Failure(MakeError(*reader.policy.failure));
            if (reader.position != reader.bytes.size() || !ValidRoles(roles, sections.kind, sections.asset))
                return Return::Failure(MakeError(AnimationAssetErrors::Malformed));
            return Return::Success(std::move(roles));
        }

        /** @brief Hashes only actual checked immutable buffers owned by the same cold cook capture. */
        Result<std::vector<AnimationCookedDependency>> BindRoles(std::span<const Assets::AssetDependency> roles,
                                                                 std::span<const AnimationCookedDependencyInput> inputs,
                                                                 const AnimationCookedContext &context) {
            using Return = Result<std::vector<AnimationCookedDependency>>;
            if (roles.size() != inputs.size())
                return Return::Failure(MakeError(AnimationAssetErrors::Malformed));
            std::uint64_t bytes{};
            std::vector<AnimationCookedDependency> bindings;
            bindings.reserve(roles.size());
            for (std::size_t index = 0; index < roles.size(); ++index) {
                if (roles[index].id != inputs[index].metadata.id || roles[index].expectedType != inputs[index].metadata.expectedType)
                    return Return::Failure(MakeError(AnimationAssetErrors::Malformed));
                if (inputs[index].payload.size() > context.limits.dependencyBytes - bytes)
                    return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
                bytes += inputs[index].payload.size();
                const auto kind = roles[index].expectedType.Value() == "core.animation.skeleton" ? AnimationCookedKind::Skeleton
                                                                                                 : AnimationCookedKind::Clip;
                if (auto checked = ReadSections(inputs[index].payload, kind, roles[index].id, context); checked.HasError())
                    return Return::Failure(checked.ErrorValue());
                bindings.emplace_back(roles[index], ComputeSha256(std::as_bytes(inputs[index].payload)));
            }
            return Return::Success(std::move(bindings));
        }

        template <typename Data> struct Decoded final {
            Data value;
            std::vector<AnimationCookedDependency> roles;
        };

        template <typename Data>
        Result<Decoded<Data>> ReadPayload(std::span<const std::uint8_t> bytes, AnimationCookedKind kind, Assets::AssetId asset,
                                          const AnimationCookedContext &context) {
            using Return = Result<Decoded<Data>>;
            auto sections = ReadSections(bytes, kind, asset, context);
            if (sections.HasError())
                return Return::Failure(sections.ErrorValue());
            if (sections.Value().version != CurrentVersion(kind))
                return Return::Failure(MakeError(AnimationAssetErrors::Unsupported));
            const auto scratch = ScratchContext(context);
            Reader reader{sections.Value().body, scratch};
            reader.policy.work = sections.Value().work;
            Data data;
            Transfer(reader, data);
            if (!reader.policy.Good())
                return Return::Failure(MakeError(*reader.policy.failure));
            if (reader.position != reader.bytes.size() || VersionParts(data.contractVersion) != sections.Value().version)
                return Return::Failure(MakeError(AnimationAssetErrors::Malformed));
            auto roles = ReadRoles(sections.Value(), scratch, context.limits.dependencies, reader.policy.work, reader.policy.owned);
            if (roles.HasError())
                return Return::Failure(roles.ErrorValue());
            return Return::Success({std::move(data), std::move(roles).Value()});
        }

        template <typename Data>
        Result<AnimationCookedPayload> WritePayload(const Data &data, AnimationCookedKind kind, Assets::AssetId asset,
                                                    std::vector<AnimationCookedDependency> roles, const AnimationCookedContext &context) {
            using Return = Result<AnimationCookedPayload>;
            if (auto valid = ValidateContext(context); valid.HasError())
                return Return::Failure(valid.ErrorValue());
            if (!asset.IsValid() || !ValidRoles(roles, kind, asset))
                return Return::Failure(MakeError(AnimationAssetErrors::Malformed));
            const auto scratch = ScratchContext(context);
            Writer body{scratch};
            Transfer(body, data);
            if (!body.policy.Good())
                return Return::Failure(MakeError(*body.policy.failure));
            Writer dependencies{scratch};
            dependencies.policy.work = body.policy.work;
            Transfer(dependencies, roles);
            if (!dependencies.policy.Good())
                return Return::Failure(MakeError(*dependencies.policy.failure));
            auto bytes = WriteSections(kind, asset, CurrentVersion(kind), body.bytes, dependencies.bytes,
                                       static_cast<std::uint32_t>(roles.size()), scratch);
            if (bytes.HasError())
                return Return::Failure(bytes.ErrorValue());
            return Return::Success({asset, kind, Declarations(roles), std::move(bytes).Value()});
        }

        bool HasSkeleton(std::span<const AnimationCookedDependency> roles, SkeletonId skeleton) {
            return std::ranges::any_of(roles, [skeleton](const auto &role) {
                return role.metadata.id == skeleton.Asset() && role.metadata.expectedType.Value() == "core.animation.skeleton";
            });
        }
    }  // namespace
}  // namespace Horo::Animation::CookedInternal

namespace Horo::Animation {
    using namespace CookedInternal;

    /** @copydoc EncodeSkeletonAnimationPayload */
    Result<AnimationCookedPayload> EncodeSkeletonAnimationPayload(const SkeletonAsset &asset, const AnimationCookedContext &context) {
        try {
            return WritePayload(asset.Data(), AnimationCookedKind::Skeleton, asset.Data().skeleton.Asset(), {}, context);
        } catch (const std::bad_alloc &) {
            return Result<AnimationCookedPayload>::Failure(MakeError(AnimationAssetErrors::Capacity));
        }
    }

    /** @copydoc DecodeSkeletonAnimationPayload */
    Result<SkeletonAsset> DecodeSkeletonAnimationPayload(std::span<const std::uint8_t> bytes, SkeletonId expected,
                                                         const AnimationCookedContext &context) {
        try {
            auto decoded = ReadPayload<SkeletonAssetData>(bytes, AnimationCookedKind::Skeleton, expected.Asset(), context);
            if (decoded.HasError())
                return Result<SkeletonAsset>::Failure(decoded.ErrorValue());
            if (decoded.Value().value.skeleton != expected)
                return Result<SkeletonAsset>::Failure(MakeError(AnimationAssetErrors::Malformed));
            auto skeleton = SkeletonAsset::Create(std::move(decoded).Value().value);
            if (skeleton.HasError())
                return skeleton;
            const auto canonical = EncodeSkeletonAnimationPayload(skeleton.Value(), context);
            if (canonical.HasError())
                return Result<SkeletonAsset>::Failure(canonical.ErrorValue());
            if (!std::ranges::equal(canonical.Value().bytes, bytes))
                return Result<SkeletonAsset>::Failure(MakeError(AnimationAssetErrors::Malformed));
            return skeleton;
        } catch (const std::bad_alloc &) {
            return Result<SkeletonAsset>::Failure(MakeError(AnimationAssetErrors::Capacity));
        }
    }

    /** @copydoc EncodeClipAnimationPayload */
    Result<AnimationCookedPayload> EncodeClipAnimationPayload(const AnimationClipAsset &asset,
                                                              std::span<const std::uint8_t> skeletonPayload,
                                                              const AnimationCookedContext &context) {
        try {
            const auto &data = asset.Data();
            auto roles = Roles(data.descriptor.skeleton, {}, context);
            if (roles.HasError())
                return Result<AnimationCookedPayload>::Failure(roles.ErrorValue());
            const std::array input{AnimationCookedDependencyInput{roles.Value().front(), skeletonPayload}};
            auto bindings = BindRoles(roles.Value(), input, context);
            if (bindings.HasError())
                return Result<AnimationCookedPayload>::Failure(bindings.ErrorValue());
            const auto reference = data.descriptor.referencePose ? std::optional{data.descriptor.referencePose->id} : std::nullopt;
            return WritePayload(ClipView{data, reference}, AnimationCookedKind::Clip, data.descriptor.id.Asset(),
                                std::move(bindings).Value(), context);
        } catch (const std::bad_alloc &) {
            return Result<AnimationCookedPayload>::Failure(MakeError(AnimationAssetErrors::Capacity));
        }
    }

    /** @copydoc DecodeClipAnimationPayload */
    Result<AnimationClipCookedData> DecodeClipAnimationPayload(std::span<const std::uint8_t> bytes, AnimationClipId expected,
                                                               const AnimationCookedContext &context) {
        try {
            auto decoded = ReadPayload<AnimationClipCookedData>(bytes, AnimationCookedKind::Clip, expected.Asset(), context);
            if (decoded.HasError())
                return Result<AnimationClipCookedData>::Failure(decoded.ErrorValue());
            const auto &value = decoded.Value().value;
            if (value.id != expected || !HasSkeleton(decoded.Value().roles, value.skeleton))
                return Result<AnimationClipCookedData>::Failure(MakeError(AnimationAssetErrors::Malformed));
            if ((value.kind == AnimationClipKind::Additive) != value.referencePose.has_value())
                return Result<AnimationClipCookedData>::Failure(MakeError(AnimationAssetErrors::Malformed));
            return Result<AnimationClipCookedData>::Success(std::move(decoded).Value().value);
        } catch (const std::bad_alloc &) {
            return Result<AnimationClipCookedData>::Failure(MakeError(AnimationAssetErrors::Capacity));
        }
    }

    /** @copydoc EncodeGraphAnimationPayload */
    Result<AnimationCookedPayload> EncodeGraphAnimationPayload(const AnimationGraphProgram &program,
                                                               std::span<const AnimationCookedDependencyInput> dependencies,
                                                               const AnimationCookedContext &context) {
        try {
            if (!program.Id().IsValid())
                return Result<AnimationCookedPayload>::Failure(MakeError(AnimationAssetErrors::Unsupported));
            auto roles = Roles(program.Skeleton(), program.Clips(), context);
            if (roles.HasError())
                return Result<AnimationCookedPayload>::Failure(roles.ErrorValue());
            if (program.EntryIndex() >= program.Definitions().size())
                return Result<AnimationCookedPayload>::Failure(MakeError(AnimationAssetErrors::Malformed));
            const GraphView view{CurrentAnimationGraphContractVersion,
                                 program.Id(),
                                 program.Skeleton(),
                                 program.Definitions()[program.EntryIndex()].id,
                                 program.Parameters(),
                                 program.ActionSlots(),
                                 program.Actions(),
                                 program.Definitions()};
            auto bindings = BindRoles(roles.Value(), dependencies, context);
            if (bindings.HasError())
                return Result<AnimationCookedPayload>::Failure(bindings.ErrorValue());
            return WritePayload(view, AnimationCookedKind::Graph, program.Id().Asset(), std::move(bindings).Value(), context);
        } catch (const std::bad_alloc &) {
            return Result<AnimationCookedPayload>::Failure(MakeError(AnimationAssetErrors::Capacity));
        }
    }

    /** @copydoc DecodeGraphAnimationPayload */
    Result<AnimationGraphCookedData> DecodeGraphAnimationPayload(std::span<const std::uint8_t> bytes, AnimationGraphId expected,
                                                                 const AnimationCookedContext &context) {
        try {
            auto decoded = ReadPayload<AnimationGraphCookedData>(bytes, AnimationCookedKind::Graph, expected.Asset(), context);
            if (decoded.HasError())
                return Result<AnimationGraphCookedData>::Failure(decoded.ErrorValue());
            if (decoded.Value().value.id != expected || !HasSkeleton(decoded.Value().roles, decoded.Value().value.skeleton))
                return Result<AnimationGraphCookedData>::Failure(MakeError(AnimationAssetErrors::Malformed));
            return Result<AnimationGraphCookedData>::Success(std::move(decoded).Value().value);
        } catch (const std::bad_alloc &) {
            return Result<AnimationGraphCookedData>::Failure(MakeError(AnimationAssetErrors::Capacity));
        }
    }

    /** @copydoc InspectAnimationPayloadBindings */
    Result<std::vector<AnimationCookedDependency>> InspectAnimationPayloadBindings(std::span<const std::uint8_t> bytes,
                                                                                   AnimationCookedKind kind, Assets::AssetId expected,
                                                                                   std::size_t maximumReturnedDependencies,
                                                                                   const AnimationCookedContext &context) {
        using Return = Result<std::vector<AnimationCookedDependency>>;
        try {
            auto sections = ReadSections(bytes, kind, expected, context);
            if (sections.HasError())
                return Return::Failure(sections.ErrorValue());
            if (sections.Value().version != CurrentVersion(kind))
                return Return::Failure(MakeError(AnimationAssetErrors::Unsupported));
            const auto scratch = ScratchContext(context);
            return ReadRoles(sections.Value(), scratch, maximumReturnedDependencies);
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
        }
    }

    /** @copydoc InspectAnimationPayloadDependencies */
    Result<std::vector<Assets::AssetDependency>> InspectAnimationPayloadDependencies(std::span<const std::uint8_t> bytes,
                                                                                     AnimationCookedKind kind, Assets::AssetId expected,
                                                                                     std::size_t maximumReturnedDependencies,
                                                                                     const AnimationCookedContext &context) {
        using Return = Result<std::vector<Assets::AssetDependency>>;
        try {
            auto bindings = InspectAnimationPayloadBindings(bytes, kind, expected, maximumReturnedDependencies, context);
            if (bindings.HasError())
                return Return::Failure(bindings.ErrorValue());
            return Return::Success(Declarations(bindings.Value()));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
        }
    }
}  // namespace Horo::Animation
