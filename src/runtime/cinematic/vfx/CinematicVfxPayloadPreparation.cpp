#include "CinematicVfxEventAdapterState.h"
#include "Horo/Cinematic/EventTrackErrors.h"
#include "Horo/Extensions/ScriptValue.h"

#include <cmath>
#include <limits>

namespace Horo::Cinematic::VfxEventDetail {
    namespace {
        using Value = Extensions::ScriptValue;
        using Payload = Gameplay::GameplayVfxEventPayload;

        /** @brief Returns a stable schema failure without exposing source payload data in diagnostics. */
        Result<Payload> InvalidPayload() {
            return Result<Payload>::Failure(MakeError(EventTrackErrors::SchemaMismatch));
        }

        /** @brief Reads only an exact canonical SignedInteger argument, preserving every millimeter bit. */
        bool Millimeters(const std::span<const Value> arguments, const std::uint16_t index, std::int64_t &output) noexcept {
            if (index >= arguments.size())
                return false;
            const auto *value = arguments[index].AsSignedInteger();
            if (!value)
                return false;
            output = *value;
            return true;
        }

        /** @brief Narrows only an exact finite Number argument inside the finite float representation. */
        bool Number(const std::span<const Value> arguments, const std::uint16_t index, float &output) noexcept {
            if (index >= arguments.size())
                return false;
            const auto *value = arguments[index].AsNumber();
            if (!value || !std::isfinite(*value) || std::abs(*value) > std::numeric_limits<float>::max())
                return false;
            output = static_cast<float>(*value);
            return std::isfinite(output);
        }

        /** @brief Reads fixed scalar components without accepting integer-to-number coercion. */
        template <std::size_t Count>
        bool Numbers(const std::span<const Value> arguments, const std::array<std::uint16_t, Count> &indices,
                     std::array<float, Count> &output) noexcept {
            for (std::size_t index = 0; index < Count; ++index)
                if (!Number(arguments, indices[index], output[index]))
                    return false;
            return true;
        }

        /** @brief Checks unit components with the same tolerance used by canonical VFX spawn admission. */
        template <std::size_t Count> bool Unit(const std::array<float, Count> &components) noexcept {
            double length{};
            for (const auto component : components)
                length += static_cast<double>(component) * component;
            return std::abs(length - 1.0) <= 0.0001;
        }

        /** @brief Projects exact global position and an optional explicitly supplied unit orientation. */
        bool Placement(const std::span<const Value> arguments, const VfxEventPayloadProjection &projection, Payload &payload) noexcept {
            std::array<std::int64_t, 3> position{};
            for (std::size_t index = 0; index < position.size(); ++index)
                if (!Millimeters(arguments, projection.positionArguments[index], position[index]))
                    return false;
            payload.transform.position = Math::WorldCoordinate64::FromMillimeters(position[0], position[1], position[2]);
            payload.transform.orientation = Math::Quaternion::Identity();
            if (!projection.orientationArguments)
                return true;
            std::array<float, 4> rotation{};
            if (!Numbers(arguments, *projection.orientationArguments, rotation) || !Unit(rotation))
                return false;
            payload.transform.orientation = {rotation[0], rotation[1], rotation[2], rotation[3]};
            return true;
        }

        /** @brief Projects optional positive scale and a genuine optional unit impact normal. */
        bool Shape(const std::span<const Value> arguments, const VfxEventPayloadProjection &projection, Payload &payload) noexcept {
            if (projection.scaleArgument.has_value())
                if (!Number(arguments, *projection.scaleArgument, payload.scale) || payload.scale <= 0.0F)
                    return false;
            if (!projection.impactNormalArguments)
                return true;
            std::array<float, 3> normal{};
            if (!Numbers(arguments, *projection.impactNormalArguments, normal) || !Unit(normal))
                return false;
            payload.impactNormal = Math::Vec3{normal[0], normal[1], normal[2]};
            return true;
        }

        /** @brief Copies finite semantic scalar channels into fixed storage in their declared canonical order. */
        bool Parameters(const std::span<const Value> arguments, const VfxEventPayloadProjection &projection, Payload &payload) noexcept {
            payload.parameters.schema = projection.parameterSchema;
            payload.parameters.count = static_cast<std::uint32_t>(projection.parameters.size());
            for (std::size_t index = 0; index < projection.parameters.size(); ++index) {
                const auto &source = projection.parameters[index];
                auto &target = payload.parameters.values[index];
                target.channel = source.channel;
                if (!Number(arguments, source.argument, target.value))
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc ValidProjection */
    bool ValidProjection(const VfxEventPayloadProjection &projection) noexcept {
        if (projection.parameters.size() > Vfx::VfxSpawnHardLimits::Parameters ||
            (projection.parameterSchema == 0) != projection.parameters.empty())
            return false;
        const auto &position = projection.positionArguments;
        if (position[0] == position[1] || position[0] == position[2] || position[1] == position[2])
            return false;
        for (std::size_t index = 1; index < projection.parameters.size(); ++index)
            if (projection.parameters[index - 1].channel >= projection.parameters[index].channel)
                return false;
        return true;
    }

    /** @copydoc PreparePayload */
    Result<Gameplay::GameplayVfxEventPayload> PreparePayload(const std::span<const std::byte> bytes,
                                                             const VfxEventPayloadProjection &projection) {
        Extensions::ScriptValueLimits limits;
        limits.maximumEncodedBytes = MaximumPayloadBytes;
        limits.maximumBytes = MaximumPayloadBytes;
        auto decoded = Extensions::DecodeScriptValue(bytes, limits);
        if (decoded.HasError())
            return InvalidPayload();
        if (decoded.Value().GetKind() != Value::Kind::Array)
            return InvalidPayload();
        const auto arguments = decoded.Value().AsArray();
        Payload payload;
        if (!Placement(arguments, projection, payload) || !Shape(arguments, projection, payload) ||
            !Parameters(arguments, projection, payload))
            return InvalidPayload();
        return Result<Payload>::Success(payload);
    }
}  // namespace Horo::Cinematic::VfxEventDetail
