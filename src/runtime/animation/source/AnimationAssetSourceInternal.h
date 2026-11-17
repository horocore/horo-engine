#pragma once

#include "AnimationCookedFields.h"
#include "Horo/Animation/AnimationAssetSource.h"

#include <nlohmann/json.hpp>
#include <set>

namespace Horo::Animation::AssetSourceInternal {
    using Json = nlohmann::ordered_json;
    using namespace CookedInternal;

    struct Rejection final {
        const ErrorCodeDescriptor *code;
    };

    [[noreturn]] inline void Reject(const ErrorCodeDescriptor &code) {
        throw Rejection{&code};
    }

    inline void Good(const Policy &policy) {
        if (!policy.Good())
            Reject(*policy.failure);
    }

    inline void Spend(Policy &policy) {
        (void)policy.Value();
        Good(policy);
    }
    template <typename T> struct SourceFields;

    template <> struct SourceFields<Math::Vec3> final {
        inline static constexpr std::array<std::string_view, 3> names{"x", "y", "z"};
    };

    template <> struct SourceFields<Math::Quaternion> final {
        inline static constexpr std::array<std::string_view, 4> names{"x", "y", "z", "w"};
    };

    template <> struct SourceFields<Math::Transform> final {
        inline static constexpr std::array<std::string_view, 3> names{"translation", "rotation", "scale"};
    };

    template <> struct SourceFields<Math::Mat4> final {
        inline static constexpr std::array<std::string_view, 1> names{"values"};
    };

    template <> struct SourceFields<SkeletonAssetContractVersion> final {
        inline static constexpr std::array<std::string_view, 3> names{"major", "minor", "patch"};
    };

    template <> struct SourceFields<SkeletonJoint> final {
        inline static constexpr std::array<std::string_view, 8>
            names{"id", "parent", "name", "referenceLocalTransform", "inverseBindMatrix", "retargetRole", "side", "mirror"};
    };

    template <> struct SourceFields<SkeletonSocket> final {
        inline static constexpr std::array<std::string_view, 4> names{"id", "joint", "name", "localTransform"};
    };

    template <> struct SourceFields<SkeletonAssetData> final {
        inline static constexpr std::array<std::string_view, 4> names{"contractVersion", "skeleton", "joints", "sockets"};
    };

    template <> struct SourceFields<AnimationClipContractVersion> final {
        inline static constexpr std::array<std::string_view, 3> names{"major", "minor", "patch"};
    };

    template <> struct SourceFields<AnimationTime> final {
        inline static constexpr std::array<std::string_view, 1> names{"ticks"};
    };

    template <> struct SourceFields<AnimationDeltaTime> final {
        inline static constexpr std::array<std::string_view, 1> names{"ticks"};
    };

    template <> struct SourceFields<AnimationSampleRate> final {
        inline static constexpr std::array<std::string_view, 2> names{"numerator", "denominator"};
    };

    template <> struct SourceFields<AnimationTransformKey> final {
        inline static constexpr std::array<std::string_view, 3> names{"time", "transform", "interpolation"};
    };

    template <> struct SourceFields<AnimationJointTrack> final {
        inline static constexpr std::array<std::string_view, 2> names{"joint", "keys"};
    };

    template <> struct SourceFields<AnimationSyncMarker> final {
        inline static constexpr std::array<std::string_view, 2> names{"id", "time"};
    };

    template <> struct SourceFields<AnimationEventField> final {
        inline static constexpr std::array<std::string_view, 2> names{"id", "value"};
    };

    template <> struct SourceFields<AnimationEventPayload> final {
        inline static constexpr std::array<std::string_view, 2> names{"fields", "count"};
    };

    template <> struct SourceFields<AnimationEventFieldSchema> final {
        inline static constexpr std::array<std::string_view, 4> names{"id", "kind", "required", "allowedUnsigned"};
    };

    template <> struct SourceFields<AnimationEventSchema> final {
        inline static constexpr std::array<std::string_view, 3> names{"type", "revision", "fields"};
    };

    template <> struct SourceFields<AnimationClipEvent> final {
        inline static constexpr std::array<std::string_view, 9> names{"id",       "name",    "type",          "schemaRevision",  "time",
                                                                      "duration", "payload", "fireInReverse", "fireOnActivation"};
    };

    template <> struct SourceFields<AnimationClipCookedData> final {
        inline static constexpr std::array<std::string_view, 14>
            names{"contractVersion", "id",          "skeleton",     "duration",      "sampleRate",
                  "wrapMode",        "kind",        "compression",  "referencePose", "hasRootMotion",
                  "tracks",          "syncMarkers", "eventSchemas", "events"};
    };
    template <typename T> struct Names;

    template <> struct Names<SkeletonRetargetRole> final {
        inline static constexpr std::array<std::string_view, 15> values{"unspecified", "root",     "pelvis",   "spine",    "chest",
                                                                        "neck",        "head",     "shoulder", "upperArm", "lowerArm",
                                                                        "hand",        "upperLeg", "lowerLeg", "foot",     "toe"};
    };

    template <> struct Names<SkeletonJointSide> final {
        inline static constexpr std::array<std::string_view, 3> values{"center", "left", "right"};
    };

    template <> struct Names<AnimationWrapMode> final {
        inline static constexpr std::array<std::string_view, 4> values{"once", "loop", "pingPong", "clampForever"};
    };

    template <> struct Names<AnimationClipKind> final {
        inline static constexpr std::array<std::string_view, 2> values{"absolute", "additive"};
    };

    template <> struct Names<AnimationCompressionScheme> final {
        inline static constexpr std::array<std::string_view, 3> values{"none", "linear", "adaptive"};
    };

    template <> struct Names<AnimationInterpolation> final {
        inline static constexpr std::array<std::string_view, 3> values{"step", "linear", "cubicHermite"};
    };

    template <> struct Names<AnimationEventValueKind> final {
        inline static constexpr std::array<std::string_view, 6> values{"boolean", "signedInteger", "unsignedInteger",
                                                                       "number",  "vector3",       "asset"};
    };

    inline void Keys(const Json &value, std::span<const std::string_view> expected) {
        if (!value.is_object() || value.size() != expected.size())
            Reject(AnimationAssetErrors::Malformed);
        for (const auto key : expected)
            if (!value.contains(std::string(key)))
                Reject(AnimationAssetErrors::Malformed);
    }

    template <typename T> Json Encode(const T &value, Policy &policy);
    template <typename T> void Decode(const Json &value, T &output, Policy &policy);

    template <typename T> Json EncodeFields(const T &value, Policy &policy) {
        auto output = Json::object();
        std::size_t index{};
        std::apply([&](const auto &...field) {
            ((output[std::string(SourceFields<T>::names[index++])] = Encode(field, policy)), ...);
        }, Fields<T>::Get(value));
        return output;
    }

    template <typename T> void DecodeFields(const Json &value, T &output, Policy &policy) {
        Keys(value, SourceFields<T>::names);
        std::size_t index{};
        std::apply([&](auto &...field) {
            (Decode(value.at(std::string(SourceFields<T>::names[index++])), field, policy), ...);
        }, Fields<T>::Get(output));
    }

    template <std::integral T> T Integer(const Json &value) {
        if (!value.is_number_integer())
            Reject(AnimationAssetErrors::Malformed);
        if (value.is_number_unsigned()) {
            const auto raw = value.get<std::uint64_t>();
            if (raw > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
                Reject(AnimationAssetErrors::Malformed);
            return static_cast<T>(raw);
        }
        const auto raw = value.get<std::int64_t>();
        if constexpr (std::is_unsigned_v<T>) {
            if (raw < 0 || static_cast<std::uint64_t>(raw) > std::numeric_limits<T>::max())
                Reject(AnimationAssetErrors::Malformed);
        } else if (raw < std::numeric_limits<T>::min() || raw > std::numeric_limits<T>::max())
            Reject(AnimationAssetErrors::Malformed);
        return static_cast<T>(raw);
    }

    inline Assets::AssetId Asset(const Json &value) {
        if (!value.is_string())
            Reject(AnimationAssetErrors::Malformed);
        const auto &text = value.get_ref<const std::string &>();
        const auto parsed = Assets::AssetId::Parse(text);
        if (parsed.HasError() || parsed.Value().ToString() != text)
            Reject(AnimationAssetErrors::Malformed);
        return parsed.Value();
    }

    template <typename T> Json EncodeScalar(const T &value) {
        if constexpr (std::same_as<T, Assets::AssetId>)
            return value.ToString();
        else if constexpr (PersistentIdentity<T>)
            return value.Asset().ToString();
        else if constexpr (StableIdentity<T>)
            return value.Value();
        else if constexpr (std::is_enum_v<T>) {
            if (!KnownEnum(value))
                Reject(AnimationAssetErrors::Malformed);
            return Names<T>::values[static_cast<std::size_t>(value)];
        } else {
            if constexpr (std::floating_point<T>) {
                if (!std::isfinite(value))
                    Reject(AnimationAssetErrors::Malformed);
                return Json(value == 0 ? T{} : value);
            } else
                return value;
        }
    }

    template <std::floating_point T> T Number(const Json &value) {
        if (!value.is_number())
            Reject(AnimationAssetErrors::Malformed);
        const auto output = value.get<T>();
        if (!std::isfinite(output))
            Reject(AnimationAssetErrors::Malformed);
        return output == 0 ? T{} : output;
    }

    inline std::string String(const Json &value, Policy &policy) {
        if (!value.is_string())
            Reject(AnimationAssetErrors::Malformed);
        const auto &text = value.get_ref<const std::string &>();
        if (text.size() > policy.context.limits.stringBytes || !policy.Allocate(text.size()))
            Reject(AnimationAssetErrors::Capacity);
        return text;
    }

    template <typename T> void DecodeScalar(const Json &value, T &output, Policy &policy) {
        if constexpr (std::same_as<T, bool>) {
            if (!value.is_boolean())
                Reject(AnimationAssetErrors::Malformed);
            output = value.get<bool>();
        } else if constexpr (std::integral<T>)
            output = Integer<T>(value);
        else if constexpr (std::floating_point<T>)
            output = Number<T>(value);
        else
            output = String(value, policy);
    }

    template <PersistentIdentity T> T Persistent(const Json &value) {
        const auto id = Asset(value);
        if (!id.IsValid())
            return {};
        auto parsed = T::Create(id);
        if (parsed.HasError())
            Reject(AnimationAssetErrors::Malformed);
        return parsed.Value();
    }

    template <StableIdentity T> T Stable(const Json &value) {
        const auto raw = Integer<std::uint64_t>(value);
        if (raw == 0)
            return {};
        auto parsed = T::Create(raw);
        if (parsed.HasError())
            Reject(AnimationAssetErrors::Malformed);
        return parsed.Value();
    }

    template <typename T> void DecodeIdentity(const Json &value, T &output) {
        if constexpr (std::same_as<T, Assets::AssetId>)
            output = Asset(value);
        else if constexpr (PersistentIdentity<T>)
            output = Persistent<T>(value);
        else if constexpr (StableIdentity<T>)
            output = Stable<T>(value);
        else {
            if (!value.is_string())
                Reject(AnimationAssetErrors::Malformed);
            const auto found = std::ranges::find(Names<T>::values, value.get_ref<const std::string &>());
            if (found == Names<T>::values.end())
                Reject(AnimationAssetErrors::Malformed);
            output = static_cast<T>(found - Names<T>::values.begin());
        }
    }

    template <typename T> constexpr bool Scalar = std::is_arithmetic_v<T> || std::same_as<T, std::string>;
    template <typename T>
    constexpr bool Identity = std::same_as<T, Assets::AssetId> || PersistentIdentity<T> || StableIdentity<T> || std::is_enum_v<T>;

    template <std::size_t I = 0, typename... T>
    void Variant(const Json &value, std::variant<T...> &output, std::size_t index, Policy &policy) {
        if (index == I) {
            output.template emplace<I>();
            Decode(value, std::get<I>(output), policy);
        } else if constexpr (I + 1 < sizeof...(T))
            Variant<I + 1>(value, output, index, policy);
        else
            Reject(AnimationAssetErrors::Malformed);
    }

    template <typename T> Json EncodeContainer(const T &value, Policy &policy) {
        if constexpr (IsOptional<T>::value)
            return value ? Encode(*value, policy) : Json{};
        else if constexpr (IsVariant<T>::value) {
            return Json{{"kind", Names<AnimationEventValueKind>::values[value.index()]}, {"value", std::visit([&](const auto &item) {
                return Encode(item, policy);
            }, value)}};
        } else {
            auto output = Json::array();
            for (const auto &item : value)
                output.push_back(Encode(item, policy));
            return output;
        }
    }

    template <typename T> Json Encode(const T &value, Policy &policy) {
        Spend(policy);
        if constexpr (Scalar<T> || Identity<T>)
            return EncodeScalar(value);
        else if constexpr (IsVector<T>::value || IsArray<T>::value || IsOptional<T>::value || IsVariant<T>::value)
            return EncodeContainer(value, policy);
        else
            return EncodeFields(value, policy);
    }

    template <typename T> void DecodeSequence(const Json &value, T &output, Policy &policy) {
        if (!value.is_array())
            Reject(AnimationAssetErrors::Malformed);
        if constexpr (IsVector<T>::value) {
            if (!policy.Elements<typename T::value_type>(value.size()) || !policy.Allocate(value.size() * sizeof(typename T::value_type)))
                Reject(AnimationAssetErrors::Capacity);
            output.resize(value.size());
        } else if (value.size() != output.size())
            Reject(AnimationAssetErrors::Malformed);
        for (std::size_t i = 0; i < value.size(); ++i)
            Decode(value[i], output[i], policy);
    }

    template <typename T> void DecodeContainer(const Json &value, T &output, Policy &policy) {
        if constexpr (IsOptional<T>::value) {
            if (value.is_null())
                output.reset();
            else {
                output.emplace();
                Decode(value, *output, policy);
            }
        } else if constexpr (IsVariant<T>::value) {
            constexpr std::array<std::string_view, 2> fields{"kind", "value"};
            Keys(value, fields);
            AnimationEventValueKind kind;
            DecodeIdentity(value.at("kind"), kind);
            Variant(value.at("value"), output, static_cast<std::size_t>(kind), policy);
        } else
            DecodeSequence(value, output, policy);
    }

    template <typename T> void Decode(const Json &value, T &output, Policy &policy) {
        Spend(policy);
        if constexpr (Scalar<T>)
            DecodeScalar(value, output, policy);
        else if constexpr (Identity<T>)
            DecodeIdentity(value, output);
        else if constexpr (IsVector<T>::value || IsArray<T>::value || IsOptional<T>::value || IsVariant<T>::value)
            DecodeContainer(value, output, policy);
        else
            DecodeFields(value, output, policy);
    }

    [[nodiscard]] Json Parse(std::string_view source, const AnimationCookedContext &context, const AnimationAssetSourceLimits &limits);
    void Check(const AnimationCookedContext &context, const AnimationAssetSourceLimits &limits);
    [[nodiscard]] std::string Emit(const Json &value, const AnimationCookedContext &context, const AnimationAssetSourceLimits &limits);
}  // namespace Horo::Animation::AssetSourceInternal
