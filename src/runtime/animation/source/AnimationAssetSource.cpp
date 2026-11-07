#include "AnimationAssetSourceInternal.h"

namespace Horo::Animation {
    namespace {
        using namespace AssetSourceInternal;
        constexpr std::array<std::string_view, 5> EnvelopeKeys{"schemaVersion", "assetType", "contractVersion", "dependencies", "payload"};

        Json Envelope(std::string_view type, Json data, Json dependencies, std::string_view identity) {
            auto version = std::move(data["contractVersion"]);
            data.erase("contractVersion");
            data.erase(std::string(identity));
            return Json{{"schemaVersion", 1U},
                        {"assetType", type},
                        {"contractVersion", std::move(version)},
                        {"dependencies", std::move(dependencies)},
                        {"payload", std::move(data)}};
        }

        void CheckEnvelope(const Json &value, std::string_view type) {
            Keys(value, EnvelopeKeys);
            if (Integer<std::uint32_t>(value.at("schemaVersion")) != 1)
                Reject(AnimationAssetErrors::Unsupported);
            if (value.at("assetType").get_ref<const std::string &>() != type)
                Reject(AnimationAssetErrors::Malformed);
        }

        Json ClipDependency(SkeletonId skeleton) {
            return Json::array({Json{{"id", skeleton.Asset().ToString()}, {"assetType", "core.animation.skeleton"}}});
        }

        void CheckClipDependency(const Json &dependencies, SkeletonId skeleton) {
            if (!dependencies.is_array() || dependencies.size() != 1)
                Reject(AnimationAssetErrors::Malformed);
            constexpr std::array<std::string_view, 2> keys{"id", "assetType"};
            const auto &dependency = dependencies.front();
            Keys(dependency, keys);
            if (Asset(dependency.at("id")) != skeleton.Asset() || dependency.at("assetType") != "core.animation.skeleton")
                Reject(AnimationAssetErrors::Malformed);
        }

        template <typename T> void BodyKeys(const Json &body, std::string_view identity) {
            if (!body.is_object() || body.size() != SourceFields<T>::names.size() - 2)
                Reject(AnimationAssetErrors::Malformed);
            for (const auto key : SourceFields<T>::names) {
                if (key == "contractVersion" || key == identity)
                    continue;
                if (!body.contains(std::string(key)))
                    Reject(AnimationAssetErrors::Malformed);
            }
        }

        template <typename T>
        T Data(const Json &envelope, Assets::AssetId identity, std::string_view identityKey, const AnimationCookedContext &context) {
            if (!identity.IsValid())
                Reject(AnimationAssetErrors::Malformed);
            const auto &body = envelope.at("payload");
            BodyKeys<T>(body, identityKey);
            const Json ownIdentity(identity.ToString());
            CookedInternal::Policy policy(context);
            T data;
            Spend(policy);
            std::size_t index{};
            const auto read = [&](auto &field) {
                const auto name = SourceFields<T>::names[index++];
                const auto &wire = name == "contractVersion" ? envelope.at("contractVersion")
                                   : name == identityKey     ? ownIdentity
                                                             : body.at(std::string(name));
                Decode(wire, field, policy);
            };
            std::apply([&](auto &...field) {
                (read(field), ...);
            }, Fields<T>::Get(data));
            return data;
        }

        template <typename T, typename Work> Result<T> Boundary(Work &&work) {
            try {
                return work();
            } catch (const Rejection &error) {
                return Result<T>::Failure(MakeError(*error.code));
            } catch (const Json::exception &) {
                return Result<T>::Failure(MakeError(AnimationAssetErrors::Malformed));
            } catch (const std::bad_alloc &) {
                return Result<T>::Failure(MakeError(AnimationAssetErrors::Capacity));
            }
        }

        template <typename T>
        Json EncodeBounded(const T &data, const AnimationCookedContext &context, const AnimationAssetSourceLimits &limits) {
            Check(context, limits);
            auto bounded = context;
            bounded.limits.values = std::min<std::uint64_t>({context.limits.values, limits.values, context.limits.ownedBytes / 128});
            CookedInternal::Policy policy(bounded);
            return Encode(data, policy);
        }

        Json EncodeClipBounded(const AnimationClipAsset &source, const AnimationCookedContext &context,
                               const AnimationAssetSourceLimits &limits) {
            Check(context, limits);
            auto bounded = context;
            bounded.limits.values = std::min<std::uint64_t>({context.limits.values, limits.values, context.limits.ownedBytes / 128});
            CookedInternal::Policy policy(bounded);
            const auto &data = source.Data();
            const auto &d = data.descriptor;
            const std::optional<AnimationReferencePoseId> reference = d.referencePose ? std::optional{d.referencePose->id} : std::nullopt;
            const auto fields = std::tie(d.contractVersion, d.id, d.skeleton, d.duration, d.sampleRate, d.wrapMode, d.kind, d.compression,
                                         reference, d.hasRootMotion, data.tracks, data.syncMarkers, data.eventSchemas, data.events);
            Spend(policy);
            auto output = Json::object();
            std::size_t index{};
            std::apply([&](const auto &...field) {
                ((output[std::string(SourceFields<AnimationClipCookedData>::names[index++])] = Encode(field, policy)), ...);
            }, fields);
            return output;
        }
    }  // namespace

    Result<SkeletonAsset> DeserializeSkeletonAnimationSource(std::string_view source, SkeletonId identity,
                                                             const AnimationCookedContext &context,
                                                             const AnimationAssetSourceLimits &limits) {
        return Boundary<SkeletonAsset>([&] {
            const auto envelope = Parse(source, context, limits);
            CheckEnvelope(envelope, "core.animation.skeleton");
            if (envelope.at("dependencies") != Json::array())
                Reject(AnimationAssetErrors::Malformed);
            auto data = Data<SkeletonAssetData>(envelope, identity.Asset(), "skeleton", context);
            auto skeleton = SkeletonAsset::Create(std::move(data));
            if (skeleton.HasError())
                return skeleton;
            if (context.cancellation.IsCancellationRequested())
                Reject(AnimationAssetErrors::Cancelled);
            return skeleton;
        });
    }

    Result<std::string> SerializeSkeletonAnimationSource(const SkeletonAsset &source, const AnimationCookedContext &context,
                                                         const AnimationAssetSourceLimits &limits) {
        return Boundary<std::string>([&] {
            auto data = EncodeBounded(source.Data(), context, limits);
            return Result<std::string>::Success(
                Emit(Envelope("core.animation.skeleton", std::move(data), Json::array(), "skeleton"), context, limits));
        });
    }

    Result<AnimationClipCookedData> DeserializeClipAnimationSource(std::string_view source, AnimationClipId identity,
                                                                   const AnimationCookedContext &context,
                                                                   const AnimationAssetSourceLimits &limits) {
        return Boundary<AnimationClipCookedData>([&] {
            const auto envelope = Parse(source, context, limits);
            CheckEnvelope(envelope, "core.animation.clip");
            auto data = Data<AnimationClipCookedData>(envelope, identity.Asset(), "id", context);
            if (data.contractVersion != CurrentAnimationClipContractVersion)
                Reject(AnimationAssetErrors::Unsupported);
            if (!data.skeleton.IsValid() || (data.kind == AnimationClipKind::Additive) != data.referencePose.has_value())
                Reject(AnimationAssetErrors::Malformed);
            CheckClipDependency(envelope.at("dependencies"), data.skeleton);
            Check(context, limits);
            return Result<AnimationClipCookedData>::Success(std::move(data));
        });
    }

    Result<std::string> SerializeClipAnimationSource(const AnimationClipAsset &source, const AnimationCookedContext &context,
                                                     const AnimationAssetSourceLimits &limits) {
        return Boundary<std::string>([&] {
            auto data = EncodeClipBounded(source, context, limits);
            return Result<std::string>::Success(
                Emit(Envelope("core.animation.clip", std::move(data), ClipDependency(source.Data().descriptor.skeleton), "id"), context,
                     limits));
        });
    }
}  // namespace Horo::Animation
