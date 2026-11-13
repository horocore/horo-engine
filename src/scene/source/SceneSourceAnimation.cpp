#include "SceneSourceInternal.h"

#include <stdexcept>

namespace Horo::SceneSource::Detail {
    namespace {
        using Component = Animation::AnimationAuthoringComponent;

        /** @brief Rejects representation errors without resolving any animation resource. */
        Error InvalidAnimation() {
            return MakeError(Animation::AnimationErrors::ComponentInvalid);
        }

        /** @brief Reads one exact canonical persistent identity into its existing typed wrapper. */
        template <typename Identity> Result<Identity> AnimationAsset(const Json &value) {
            if (!value.is_string())
                return Result<Identity>::Failure(InvalidAnimation());
            const auto &text = value.get_ref<const Json::string_t &>();
            auto asset = Assets::AssetId::Parse(text);
            if (asset.HasError() || asset.Value().ToString() != text)
                return Result<Identity>::Failure(InvalidAnimation());
            return Identity::Create(asset.Value());
        }

        /** @brief Reads the closed clip-or-graph binding; no other field or source form is admitted. */
        Result<Animation::AnimationSourceReference> AnimationSource(const Json &value) {
            using Return = Result<Animation::AnimationSourceReference>;
            if (!HasFields(value, {"kind", "asset"}) || value.size() != 2 || !value["kind"].is_string())
                return Return::Failure(InvalidAnimation());
            if (value["kind"] == "clip") {
                auto clip = AnimationAsset<Animation::AnimationClipId>(value["asset"]);
                if (clip.HasError())
                    return Return::Failure(clip.ErrorValue());
                return Return::Success({Animation::AnimationSourceKind::Clip, clip.Value(), {}});
            }
            if (value["kind"] == "graph") {
                auto graph = AnimationAsset<Animation::AnimationGraphId>(value["asset"]);
                if (graph.HasError())
                    return Return::Failure(graph.ErrorValue());
                return Return::Success({Animation::AnimationSourceKind::Graph, {}, graph.Value()});
            }
            return Return::Failure(InvalidAnimation());
        }

        /** @brief Checks the complete fixed schema before indexed reads and preserves version rejection. */
        Result<void> AnimationShape(const Json &value) {
            if (!HasFields(value, {"contractVersion", "component", "skeleton", "source", "retarget", "rootMotion", "startsPlaying"}) ||
                value.size() != 7)
                return Result<void>::Failure(InvalidAnimation());
            const auto &version = value["contractVersion"];
            if (!HasFields(version, {"major", "minor", "patch"}) || version.size() != 3 ||
                !HasUnsignedFields(version, {"major", "minor", "patch"}))
                return Result<void>::Failure(InvalidAnimation());
            if (version["major"] != 1 || version["minor"] != 0 || version["patch"] != 0)
                return Result<void>::Failure(MakeError(Animation::AnimationErrors::ContractVersionUnsupported));
            return Result<void>::Success();
        }

        /** @brief Reads scalar owner intent without interpreting it as live playback state. */
        Result<void> AnimationIntent(const Json &value, Component &component) {
            if (!value["component"].is_number_unsigned() || !value["startsPlaying"].is_boolean() || !value["rootMotion"].is_string())
                return Result<void>::Failure(InvalidAnimation());
            auto identity = Animation::AnimationComponentId::Create(value["component"].get<std::uint64_t>());
            if (identity.HasError())
                return Result<void>::Failure(identity.ErrorValue());
            component.component = identity.Value();
            component.startsPlaying = value["startsPlaying"].get<bool>();
            if (value["rootMotion"] == "ignore")
                component.rootMotion = Animation::RootMotionPolicy::Ignore;
            else if (value["rootMotion"] == "requestCharacterMovement")
                component.rootMotion = Animation::RootMotionPolicy::RequestCharacterMovement;
            else
                return Result<void>::Failure(InvalidAnimation());
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ParseAnimationComponent */
    Result<Animation::AnimationAuthoringComponent> ParseAnimationComponent(const Json &value) {
        using Return = Result<Component>;
        if (auto shape = AnimationShape(value); shape.HasError())
            return Return::Failure(shape.ErrorValue());
        Component component;
        if (auto intent = AnimationIntent(value, component); intent.HasError())
            return Return::Failure(intent.ErrorValue());
        auto skeleton = AnimationAsset<Animation::SkeletonId>(value["skeleton"]);
        auto source = AnimationSource(value["source"]);
        if (skeleton.HasError())
            return Return::Failure(skeleton.ErrorValue());
        if (source.HasError())
            return Return::Failure(source.ErrorValue());
        component.skeleton = skeleton.Value();
        component.source = source.Value();
        if (!value["retarget"].is_null()) {
            auto retarget = AnimationAsset<Animation::RetargetProfileId>(value["retarget"]);
            if (retarget.HasError())
                return Return::Failure(retarget.ErrorValue());
            component.retarget = retarget.Value();
        }
        return Return::Success(component);
    }

    /** @copydoc AppendAnimationComponent */
    void AppendAnimationComponent(WireJson &value, const Animation::AnimationAuthoringComponent &component) {
        if (Animation::ValidateAnimationAuthoringComponent(component).HasError())
            throw std::invalid_argument("Scene contains an invalid animation component.");
        const bool clip = component.source.kind == Animation::AnimationSourceKind::Clip;
        value["animation"] = {{"contractVersion",
                               {{"major", component.contractVersion.major},
                                {"minor", component.contractVersion.minor},
                                {"patch", component.contractVersion.patch}}},
                              {"component", component.component.Value()},
                              {"skeleton", component.skeleton.Asset().ToString()},
                              {"source",
                               {{"kind", clip ? "clip" : "graph"},
                                {"asset", clip ? component.source.clip.Asset().ToString() : component.source.graph.Asset().ToString()}}},
                              {"retarget",
                               component.retarget.IsValid() ? WireJson(component.retarget.Asset().ToString()) : WireJson(nullptr)},
                              {"rootMotion",
                               component.rootMotion == Animation::RootMotionPolicy::Ignore ? "ignore" : "requestCharacterMovement"},
                              {"startsPlaying", component.startsPlaying}};
    }
}  // namespace Horo::SceneSource::Detail
