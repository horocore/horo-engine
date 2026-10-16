#include "SettingsModalCollisionInternal.h"

#include <algorithm>

namespace Horo::Editor {
    using SettingsModalInternal::CollisionItemKind;

    void SettingsModalInternal::CollisionEdited(SettingsCollisionDraft &draft) noexcept {
        draft.pending = true;
        draft.review.reset();
        draft.error.reset();
    }

    /** @copydoc RefreshCollisionDraft */
    void RefreshCollisionDraft(SettingsCollisionDraft &draft, CollisionProjectSettingsSession &owner) {
        auto snapshot = owner.Snapshot();
        if (snapshot.HasError()) {
            draft.error = snapshot.ErrorValue();
            draft.input.reset();
            return;
        }
        draft.input = snapshot.Value()->Input();
        draft.review.reset();
        draft.error.reset();
        draft.pending = false;
        draft.selected = 0;
    }

    namespace {
        Result<void> AddLayer(Application::ProjectCollisionSettingsInput &input, CollisionProjectSettingsSession &owner,
                              const std::string &name, const int response) {
            if (input.schema.layers.size() >= 4096 ||
                std::ranges::count(input.schema.layers, true, &Physics::CollisionLayerDefinition::enabled) >= 64)
                return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
            auto id = owner.NewLayerId();
            if (id.HasError())
                return Result<void>::Failure(id.ErrorValue());
            for (const auto &layer : input.schema.layers)
                if (layer.enabled)
                    input.schema.pairs.push_back({layer.id, id.Value(), static_cast<Physics::SimulationPairResponse>(response)});
            input.schema.pairs.push_back({id.Value(), id.Value(), static_cast<Physics::SimulationPairResponse>(response)});
            input.schema.layers.push_back({id.Value()});
            input.layers.push_back({id.Value(), name, {}});
            return Result<void>::Success();
        }

        Result<void> AddChannel(Application::ProjectCollisionSettingsInput &input, CollisionProjectSettingsSession &owner,
                                const std::string &name, const int response) {
            if (input.schema.queryChannels.size() >= 4096 ||
                std::ranges::count(input.schema.queryChannels, true, &Physics::CollisionQueryChannelDefinition::enabled) >= 64)
                return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
            auto id = owner.NewChannelId();
            if (id.HasError())
                return Result<void>::Failure(id.ErrorValue());
            input.schema.queryChannels.push_back({id.Value()});
            input.channels.push_back({id.Value(), name, {}});
            for (auto &profile : input.schema.profiles)
                profile.queryResponses.push_back({id.Value(), static_cast<Physics::CollisionQueryResponse>(response)});
            return Result<void>::Success();
        }

        Result<void> AddProfile(Application::ProjectCollisionSettingsInput &input, CollisionProjectSettingsSession &owner,
                                const std::string &name, const int response, const int selectedLayer) {
            if (selectedLayer < 0 || static_cast<std::size_t>(selectedLayer) >= input.layers.size())
                return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Invalid)));
            if (input.schema.profiles.size() >= 1024)
                return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
            auto id = owner.NewProfileId();
            if (id.HasError())
                return Result<void>::Failure(id.ErrorValue());
            Physics::CollisionProfileDefinition profile{.id = id.Value(),
                                                        .layer = input.layers[static_cast<std::size_t>(selectedLayer)].id};
            for (const auto &channel : input.schema.queryChannels)
                if (channel.enabled)
                    profile.queryResponses.push_back({channel.id, static_cast<Physics::CollisionQueryResponse>(response)});
            input.schema.profiles.push_back(std::move(profile));
            input.profiles.push_back({id.Value(), name, {}});
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CreateCollisionItem */
    Result<void> CreateCollisionItem(SettingsCollisionDraft &draft, CollisionProjectSettingsSession &owner) {
        try {
            if (!draft.input || draft.newName.empty() || draft.initialResponse < 0 || draft.initialResponse > 2)
                return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Invalid)));
            auto input = *draft.input;
            Result<void> added = Result<void>::Success();
            switch (static_cast<CollisionItemKind>(draft.kind)) {
                case CollisionItemKind::Layer:
                    added = AddLayer(input, owner, draft.newName, draft.initialResponse);
                    break;
                case CollisionItemKind::Profile:
                    added = AddProfile(input, owner, draft.newName, draft.initialResponse, draft.initialLayer);
                    break;
                case CollisionItemKind::Channel:
                    added = AddChannel(input, owner, draft.newName, draft.initialResponse);
                    break;
                default:
                    return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Invalid)));
            }
            if (added.HasError())
                return added;
            switch (static_cast<CollisionItemKind>(draft.kind)) {
                case CollisionItemKind::Layer:
                    draft.selected = static_cast<int>(input.layers.size()) - 1;
                    break;
                case CollisionItemKind::Profile:
                    draft.selected = static_cast<int>(input.profiles.size()) - 1;
                    break;
                case CollisionItemKind::Channel:
                    draft.selected = static_cast<int>(input.channels.size()) - 1;
                    break;
            }
            draft.input = std::move(input);
            draft.newName.clear();
            SettingsModalInternal::CollisionEdited(draft);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
        }
    }

    namespace {
        template <typename Id, typename Definition>
        std::optional<Id> RemoveDefinition(const std::size_t index, std::vector<Application::CollisionDisplayMetadata<Id>> &names,
                                           std::vector<Definition> &definitions) {
            if (index >= names.size())
                return std::nullopt;
            const Id id = names[index].id;
            std::erase_if(definitions, [id](const auto &value) {
                return value.id == id;
            });
            names.erase(names.begin() + static_cast<std::ptrdiff_t>(index));
            return id;
        }

        bool RemoveLayer(Application::ProjectCollisionSettingsInput &input, const std::size_t index) {
            const auto id = RemoveDefinition(index, input.layers, input.schema.layers);
            if (!id)
                return false;
            std::erase_if(input.schema.pairs, [&](const auto &v) {
                return v.first == *id || v.second == *id;
            });
            // Profiles retain their actual references: the user must reassign them before canonical validation succeeds.
            return true;
        }

        bool RemoveChannel(Application::ProjectCollisionSettingsInput &input, const std::size_t index) {
            const auto id = RemoveDefinition(index, input.channels, input.schema.queryChannels);
            if (!id)
                return false;
            for (auto &profile : input.schema.profiles)
                std::erase_if(profile.queryResponses, [&](const auto &v) {
                    return v.channel == *id;
                });
            return true;
        }
    }  // namespace

    /** @copydoc RemoveCollisionItem */
    Result<void> RemoveCollisionItem(SettingsCollisionDraft &draft) {
        if (!draft.input || draft.selected < 0)
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Invalid)));
        const auto index = static_cast<std::size_t>(draft.selected);
        auto &input = *draft.input;
        bool removed{};
        switch (static_cast<CollisionItemKind>(draft.kind)) {
            case CollisionItemKind::Layer:
                removed = RemoveLayer(input, index);
                break;
            case CollisionItemKind::Profile:
                removed = RemoveDefinition(index, input.profiles, input.schema.profiles).has_value();
                break;
            case CollisionItemKind::Channel:
                removed = RemoveChannel(input, index);
                break;
        }
        if (!removed)
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Invalid)));
        SettingsModalInternal::CollisionEdited(draft);
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
