#include "MixerDocumentCommands.h"

#include <algorithm>

namespace Horo::Editor::MixerDocumentInternal {
    namespace {
        /** @brief Finds a stable identity without resolving display labels or array positions. */
        template <typename Values, typename Id> auto Find(Values &values, const Id id) {
            return std::ranges::find_if(values, [id](const auto &value) {
                return value.id == id;
            });
        }

        /** @brief Creates a typed rejected-command result. */
        Result<void> Invalid() {
            return Result<void>::Failure(MakeError(MixerDocumentErrors::InvalidCommand));
        }

        /** @brief Stages bus insertion and its primary edge as one operation. */
        Result<void> ApplyCommand(Audio::MixerAssetSchema &asset, const InsertMixerBus &command,
                                  const Audio::MixerAssetSchemaLimits &limits) {
            if (asset.buses.size() >= limits.maximumBuses || asset.routes.size() >= limits.maximumRoutes ||
                command.bus.displayName.size() > limits.maximumBusDisplayNameBytes || command.bus.effects.size() > limits.maximumEffects ||
                command.bus.layout.orderedChannels.size() > Audio::MaximumAudioChannels ||
                Find(asset.buses, command.bus.id) != asset.buses.end() ||
                Find(asset.routes, command.primaryRoute.id) != asset.routes.end() ||
                command.bus.role == Audio::MixerBusRole::MasterOutput || command.primaryRoute.source != command.bus.id ||
                command.primaryRoute.kind != Audio::MixerRouteKind::Primary)
                return Invalid();
            asset.buses.push_back(command.bus);
            asset.routes.push_back(command.primaryRoute);
            return Result<void>::Success();
        }

        /** @brief Stages property changes without replacing bus identity, role or effects. */
        Result<void> ApplyCommand(Audio::MixerAssetSchema &asset, const EditMixerBus &command,
                                  const Audio::MixerAssetSchemaLimits &limits) {
            const auto bus = Find(asset.buses, command.id);
            if (bus == asset.buses.end() || command.displayName.size() > limits.maximumBusDisplayNameBytes ||
                command.layout.orderedChannels.size() > Audio::MaximumAudioChannels)
                return Invalid();
            bus->displayName = command.displayName;
            bus->layout = command.layout;
            bus->defaults = command.defaults;
            return Result<void>::Success();
        }

        /** @brief Rejects referenced deletion and removes only the bus's own primary edge. */
        Result<void> ApplyCommand(Audio::MixerAssetSchema &asset, const RemoveMixerBus &command, const Audio::MixerAssetSchemaLimits &) {
            const auto bus = Find(asset.buses, command.id);
            if (bus == asset.buses.end() || bus->role == Audio::MixerBusRole::MasterOutput)
                return Invalid();
            for (const auto &route : asset.routes) {
                if (route.destination == command.id || (route.source == command.id && route.kind != Audio::MixerRouteKind::Primary))
                    return Invalid();
            }
            std::erase_if(asset.routes, [&](const auto &route) {
                return route.source == command.id;
            });
            asset.buses.erase(bus);
            return Result<void>::Success();
        }

        /** @brief Stages a stable route replacement or bounded insertion. */
        Result<void> ApplyCommand(Audio::MixerAssetSchema &asset, const SetMixerRoute &command,
                                  const Audio::MixerAssetSchemaLimits &limits) {
            const auto route = Find(asset.routes, command.route.id);
            if (route != asset.routes.end())
                *route = command.route;
            else {
                if (asset.routes.size() >= limits.maximumRoutes)
                    return Invalid();
                asset.routes.push_back(command.route);
            }
            return Result<void>::Success();
        }

        /** @brief Stages explicit route removal for final transaction validation. */
        Result<void> ApplyCommand(Audio::MixerAssetSchema &asset, const RemoveMixerRoute &command, const Audio::MixerAssetSchemaLimits &) {
            const auto route = Find(asset.routes, command.id);
            if (route == asset.routes.end())
                return Invalid();
            asset.routes.erase(route);
            return Result<void>::Success();
        }

        /** @brief Stages bounded insert-chain addition at an authored position. */
        Result<void> ApplyCommand(Audio::MixerAssetSchema &asset, const InsertMixerEffect &command,
                                  const Audio::MixerAssetSchemaLimits &limits) {
            const auto bus = Find(asset.buses, command.bus);
            if (bus == asset.buses.end() || bus->effects.size() >= limits.maximumEffects || command.index > bus->effects.size() ||
                Find(bus->effects, command.effect.id) != bus->effects.end())
                return Invalid();
            bus->effects.insert(bus->effects.begin() + static_cast<std::ptrdiff_t>(command.index), command.effect);
            return Result<void>::Success();
        }

        /** @brief Stages replacement while preserving the stable insert identity and order. */
        Result<void> ApplyCommand(Audio::MixerAssetSchema &asset, const EditMixerEffect &command, const Audio::MixerAssetSchemaLimits &) {
            const auto bus = Find(asset.buses, command.bus);
            if (bus == asset.buses.end())
                return Invalid();
            const auto effect = Find(bus->effects, command.effect.id);
            if (effect == bus->effects.end())
                return Invalid();
            *effect = command.effect;
            return Result<void>::Success();
        }

        /** @brief Stages removal of an explicitly owned insert identity. */
        Result<void> ApplyCommand(Audio::MixerAssetSchema &asset, const RemoveMixerEffect &command, const Audio::MixerAssetSchemaLimits &) {
            const auto bus = Find(asset.buses, command.bus);
            if (bus == asset.buses.end())
                return Invalid();
            const auto effect = Find(bus->effects, command.effect);
            if (effect == bus->effects.end())
                return Invalid();
            bus->effects.erase(effect);
            return Result<void>::Success();
        }

        /** @brief Rotates an insert chain without recreating an effect identity. */
        Result<void> ApplyCommand(Audio::MixerAssetSchema &asset, const MoveMixerEffect &command, const Audio::MixerAssetSchemaLimits &) {
            const auto bus = Find(asset.buses, command.bus);
            if (bus == asset.buses.end() || command.index >= bus->effects.size())
                return Invalid();
            const auto effect = Find(bus->effects, command.effect);
            if (effect == bus->effects.end())
                return Invalid();
            const auto target = bus->effects.begin() + static_cast<std::ptrdiff_t>(command.index);
            if (effect < target)
                std::rotate(effect, effect + 1, target + 1);
            else
                std::rotate(target, effect, effect + 1);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc Apply */
    Result<void> Apply(Audio::MixerAssetSchema &candidate, const MixerDocumentCommand &command,
                       const Audio::MixerAssetSchemaLimits &limits) {
        return std::visit([&](const auto &operation) {
            return ApplyCommand(candidate, operation, limits);
        }, command);
    }
}  // namespace Horo::Editor::MixerDocumentInternal
