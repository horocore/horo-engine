#include "InputErrors.h"
#include "InputRouterInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Horo::Input {
    namespace {
        template <typename Value> Result<Value> Stale() {
            return Result<Value>::Failure(MakeError(Errors::CollectionStale));
        }

        template <typename Value> Result<Value> Invalid() {
            return Result<Value>::Failure(MakeError(Errors::CollectionInvalid));
        }

        [[nodiscard]] bool ValidValue(const CollectedActionValue &value, const ActionValueType type) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::abs(value.x) <= 1.0F && std::abs(value.y) <= 1.0F &&
                   (type == ActionValueType::Axis2D || value.y == 0.0F) &&
                   (type != ActionValueType::Digital || value.x == 0.0F || value.x == 1.0F) &&
                   (value.active || (value.x == 0.0F && value.y == 0.0F));
        }
    }  // namespace

    Result<InputRouter *> InputCollectionLifetime::Borrow() const {
        if (thread != std::this_thread::get_id())
            return Result<InputRouter *>::Failure(MakeError(Errors::CollectionWrongThread));
        return router ? Result<InputRouter *>::Success(router) : Stale<InputRouter *>();
    }

    Result<void> InputCollectionLifetime::ValidateSource(const std::size_t slot, const std::uint64_t generation) const {
        if (auto owner = Borrow(); owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        if (slot >= sources.size() || generation == 0)
            return Stale<void>();
        if (const auto &source = sources[slot];
            source.generation != generation || source.retiring || source.configuration != router->impl_->configurationRevision ||
            source.assignment != router->impl_->assignmentRevision || !router->TokenActive(source.context) ||
            (source.device &&
             (!router->Snapshot().FindGamepad(*source.device) || router->PlayerForGamepad(*source.device) != source.player)))
            return Stale<void>();
        return Result<void>::Success();
    }

    Result<void> InputCollectionLifetime::ValidateFrame(const std::uint64_t frame) const {
        if (auto owner = Borrow(); owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        return collecting && frame != 0 && frame == serial ? Result<void>::Success() : Stale<void>();
    }

    Result<void> InputCollectionLifetime::Revoke(const std::size_t slot, const std::uint64_t generation) {
        if (auto owner = Borrow(); owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        if (slot >= sources.size() || generation == 0 || sources[slot].generation != generation)
            return Stale<void>();
        sources[slot].retiring = true;
        return Result<void>::Success();
    }

    void InputCollectionLifetime::AbandonSource(const std::size_t slot, const std::uint64_t generation) noexcept {
        if (thread == std::this_thread::get_id() && router && slot < sources.size() && generation != 0 &&
            sources[slot].generation == generation)
            sources[slot].retiring = true;
    }

    void InputCollectionLifetime::AbandonFrame(const std::uint64_t frame) noexcept {
        if (thread == std::this_thread::get_id() && router && collecting && frame != 0 && frame == serial) {
            collecting = false;
            candidate = nullptr;
        }
    }

    void InputCollectionLifetime::Reconfigure() noexcept {
        if (!collecting && std::ranges::none_of(sources, [](const auto &source) {
            return source.generation != 0;
        }))
            return;
        for (auto &source : sources)
            if (source.generation != 0)
                source.retiring = true;
        for (auto &entry : entries) {
            entry.current = {};
            entry.previous = {};
        }
        collecting = false;
        candidate = nullptr;
        routingAllowed = false;
    }

    void InputCollectionLifetime::CommitValues(const bool focused) noexcept {
        for (auto &entry : entries) {
            if (entry.generation == 0)
                continue;
            auto &source = sources[entry.source];
            const bool current = !source.retiring && source.generation == entry.generation &&
                                 source.configuration == router->impl_->configurationRevision &&
                                 source.assignment == router->impl_->assignmentRevision && router->TokenActive(source.context) &&
                                 (!source.device || ((candidate ? *candidate : router->Snapshot()).FindGamepad(*source.device) &&
                                                     router->PlayerForGamepad(*source.device) == source.player));
            entry.previous = entry.current;
            entry.current = focused && current ? entry.pending : Value{};
            entry.pending = {};
            entry.appended = false;
            entry.consumed = false;
            if (!current)
                source.retiring = true;
        }
        // Keep the neutral/release evidence readable for this committed frame.
        for (auto &source : sources)
            if (source.retiring)
                source.neutralCommitted = true;
        collecting = false;
        candidate = nullptr;
        routingAllowed = true;
    }

    void InputCollectionLifetime::ReclaimNeutral() noexcept {
        for (std::size_t slot = 0; slot < sources.size(); ++slot) {
            if (!sources[slot].retiring || !sources[slot].neutralCommitted)
                continue;
            for (auto &entry : entries)
                if (entry.generation != 0 && entry.source == slot)
                    entry = {};
            sources[slot] = {};
        }
    }

    void InputCollectionLifetime::CommitEmpty() noexcept {
        if (std::ranges::none_of(sources, [](const auto &source) {
            return source.generation != 0;
        })) {
            collecting = false;
            candidate = nullptr;
            routingAllowed = true;
            return;
        }
        for (auto &entry : entries)
            entry.pending = {};
        CommitValues(true);
    }

    void InputCollectionLifetime::Accumulate(const std::size_t action, const std::optional<PlayerId> player, ActionValue &value) noexcept {
        for (auto &entry : entries) {
            if (entry.generation == 0 || entry.action != action || (player.has_value() && sources[entry.source].player != player))
                continue;
            if (const auto &source = sources[entry.source];
                source.configuration != router->impl_->configurationRevision || source.assignment != router->impl_->assignmentRevision)
                continue;
            const bool down = entry.current.active && (entry.current.x != 0.0F || entry.current.y != 0.0F);
            const bool previous = entry.previous.active && (entry.previous.x != 0.0F || entry.previous.y != 0.0F);
            value.x += entry.current.x;
            value.y += entry.current.y;
            value.down = value.down || down;
            value.pressed = value.pressed || (down && !previous && !entry.consumed);
            value.released = value.released || (!down && previous);
            entry.consumed = true;
        }
    }

    /** @copydoc InputRouter::ActionCollection */
    Result<InputActionCollection> InputRouter::ActionCollection(const InputContextToken &context,
                                                                const std::optional<GamepadDeviceId> assignedDevice) const {
        if (auto owner = impl_->collection->Borrow(); owner.HasError())
            return Result<InputActionCollection>::Failure(owner.ErrorValue());
        const auto routing = RoutingState(context);
        if (routing.contextIdentity == 0)
            return Invalid<InputActionCollection>();
        InputActionCollection port;
        port.owner_ = impl_->collection;
        port.context_ = routing.contextIdentity;
        port.configuration_ = impl_->configurationRevision;
        port.assignment_ = impl_->assignmentRevision;
        port.device_ = assignedDevice;
        if (assignedDevice) {
            port.player_ = PlayerForGamepad(*assignedDevice);
            if (!port.player_.has_value() || !Snapshot().FindGamepad(*assignedDevice))
                return Invalid<InputActionCollection>();
        }
        return Result<InputActionCollection>::Success(std::move(port));
    }

    /** @copydoc InputActionCollection::RegisterSource */
    Result<InputActionSourceToken> InputActionCollection::RegisterSource(const std::span<const ActionDescriptor> actions) const {
        const auto owner = owner_.lock();
        if (!owner)
            return Stale<InputActionSourceToken>();
        auto router = owner->Borrow();
        if (router.HasError())
            return Result<InputActionSourceToken>::Failure(router.ErrorValue());
        if (const auto &impl = *router.Value()->impl_; configuration_ != impl.configurationRevision ||
                                                       assignment_ != impl.assignmentRevision || !router.Value()->TokenActive(context_) ||
                                                       owner->collecting || actions.empty())
            return Stale<InputActionSourceToken>();
        auto registered = owner->Register({.context = context_,
                                           .configuration = configuration_,
                                           .assignment = assignment_,
                                           .player = player_,
                                           .device = device_,
                                           .count = actions.size()},
                                          actions);
        if (registered.HasError())
            return Result<InputActionSourceToken>::Failure(registered.ErrorValue());
        const auto [slotIndex, generation] = registered.Value();
        InputActionSourceToken token;
        token.owner_ = owner;
        token.generation_ = generation;
        token.slot_ = slotIndex;
        return Result<InputActionSourceToken>::Success(std::move(token));
    }

    /** @copydoc InputActionSourceToken::Validate */
    Result<void> InputActionSourceToken::Validate() const {
        const auto owner = owner_.lock();
        return owner ? owner->ValidateSource(slot_, generation_) : Stale<void>();
    }

    Result<std::array<std::size_t, MaximumCollectedInputActions>> InputCollectionLifetime::ResolveActions(
        const std::uint64_t contextToken, const std::span<const ActionDescriptor> actions) const {
        const auto &impl = *router->impl_;
        const auto context = std::ranges::find(impl.contexts, contextToken, &InputRouter::Impl::Context::token);
        if (context == impl.contexts.end())
            return Invalid<std::array<std::size_t, MaximumCollectedInputActions>>();
        std::array<std::size_t, MaximumCollectedInputActions> indices{};
        for (std::size_t index = 0; index < actions.size(); ++index) {
            const auto found = std::ranges::find(impl.actions, actions[index].id, &ActionDescriptor::id);
            if (found == impl.actions.end() || found->valueType != actions[index].valueType || found->context != actions[index].context ||
                context->id != found->context)
                return Invalid<decltype(indices)>();
            indices[index] = static_cast<std::size_t>(found - impl.actions.begin());
            if (std::find(indices.begin(), indices.begin() + index, indices[index]) != indices.begin() + index)
                return Invalid<decltype(indices)>();
        }
        return Result<decltype(indices)>::Success(indices);
    }

    Result<std::pair<std::size_t, std::uint64_t>> InputCollectionLifetime::Register(Source source,
                                                                                    const std::span<const ActionDescriptor> actions) {
        using Registration = std::pair<std::size_t, std::uint64_t>;
        if (actions.size() > MaximumCollectedInputActions)
            return Result<Registration>::Failure(MakeError(Errors::CollectionCapacity));
        auto indices = ResolveActions(source.context, actions);
        if (indices.HasError())
            return Result<Registration>::Failure(indices.ErrorValue());
        if (std::ranges::any_of(sources, [](const auto &value) {
            return value.retiring && !value.neutralCommitted;
        }))
            return Stale<Registration>();
        const auto reclaimable = [&](const Entry &entry) {
            return entry.generation == 0 || (sources[entry.source].retiring && sources[entry.source].neutralCommitted);
        };
        const auto free = std::ranges::count_if(entries, reclaimable);
        if (const bool slotAvailable = std::ranges::any_of(sources,
                                                           [](const auto &value) {
            return value.generation == 0 || (value.retiring && value.neutralCommitted);
        });
            !slotAvailable || actions.size() > static_cast<std::size_t>(free) || nextGeneration == 0)
            return Result<Registration>::Failure(MakeError(Errors::CollectionCapacity));
        // Reclamation is a successful registration mutation, never a side effect of rejected metadata.
        ReclaimNeutral();
        const auto slot = std::ranges::find(sources, std::uint64_t{}, &Source::generation);
        const auto slotIndex = static_cast<std::size_t>(slot - sources.begin());
        source.generation = nextGeneration;
        nextGeneration = nextGeneration == std::numeric_limits<std::uint64_t>::max() ? 0 : nextGeneration + 1;
        *slot = source;
        for (std::size_t local = 0; local < actions.size(); ++local) {
            auto entry = std::ranges::find(entries, std::uint64_t{}, &Entry::generation);
            *entry = {.generation = source.generation,
                      .source = slotIndex,
                      .local = local,
                      .action = indices.Value()[local],
                      .type = actions[local].valueType};
        }
        return Result<Registration>::Success({slotIndex, source.generation});
    }

    /** @copydoc InputActionCollection::Begin */
    Result<InputCollectionFrame> InputActionCollection::Begin(const RawInputSnapshot &raw) const {
        const auto owner = owner_.lock();
        if (!owner)
            return Stale<InputCollectionFrame>();
        if (auto router = owner->Borrow(); router.HasError())
            return Result<InputCollectionFrame>::Failure(router.ErrorValue());
        if (configuration_ != owner->router->impl_->configurationRevision || assignment_ != owner->router->impl_->assignmentRevision ||
            !owner->router->TokenActive(context_))
            return Stale<InputCollectionFrame>();
        if (owner->collecting || owner->serial == std::numeric_limits<std::uint64_t>::max())
            return Stale<InputCollectionFrame>();
        for (auto &entry : owner->entries) {
            entry.pending = {};
            entry.appended = false;
        }
        owner->candidate = &raw;
        owner->collecting = true;
        owner->routingAllowed = false;
        InputCollectionFrame frame;
        frame.owner_ = owner;
        frame.serial_ = ++owner->serial;
        return Result<InputCollectionFrame>::Success(std::move(frame));
    }

    /** @copydoc InputCollectionFrame::Append */
    Result<void> InputCollectionFrame::Append(const InputActionSourceToken &source,
                                              const std::span<const CollectedActionValue> values) const {
        const auto owner = owner_.lock();
        if (!owner || owner != source.owner_.lock())
            return Stale<void>();
        if (auto frame = owner->ValidateFrame(serial_); frame.HasError())
            return frame;
        if (auto valid = owner->ValidateSource(source.slot_, source.generation_); valid.HasError())
            return valid;
        if (values.size() > MaximumCollectedInputActions)
            return Result<void>::Failure(MakeError(Errors::CollectionCapacity));
        std::bitset<MaximumCollectedInputActions> selected;
        for (const auto &value : values) {
            const auto entry = std::ranges::find_if(owner->entries, [&](const auto &candidate) {
                return candidate.generation == source.generation_ && candidate.source == source.slot_ &&
                       candidate.local == value.actionIndex;
            });
            if (entry == owner->entries.end() || entry->appended || !ValidValue(value, entry->type))
                return Invalid<void>();
            const auto index = static_cast<std::size_t>(entry - owner->entries.begin());
            if (selected.test(index))
                return Invalid<void>();
            selected.set(index);
        }
        for (const auto &value : values)
            for (auto &entry : owner->entries)
                if (entry.generation == source.generation_ && entry.source == source.slot_ && entry.local == value.actionIndex) {
                    entry.pending = {value.x, value.y, value.active};
                    entry.appended = true;
                }
        return Result<void>::Success();
    }

    /** @copydoc InputCollectionFrame::Commit */
    Result<void> InputCollectionFrame::Commit() {
        const auto owner = owner_.lock();
        if (!owner)
            return Stale<void>();
        if (auto frame = owner->ValidateFrame(serial_); frame.HasError())
            return frame;
        auto *raw = owner->candidate;
        owner->CommitValues(raw->window.focused);
        owner->router->CommitRawFrame(*raw);
        serial_ = 0;
        return Result<void>::Success();
    }

    /** @copydoc InputCollectionFrame::Cancel */
    Result<void> InputCollectionFrame::Cancel() {
        const auto owner = owner_.lock();
        if (!owner)
            return Stale<void>();
        if (auto frame = owner->ValidateFrame(serial_); frame.HasError())
            return frame;
        owner->collecting = false;
        owner->candidate = nullptr;
        serial_ = 0;
        return Result<void>::Success();
    }

    /** @copydoc InputActionSourceToken::Reset */
    Result<void> InputActionSourceToken::Reset() {
        if (generation_ == 0)
            return Result<void>::Success();
        const auto owner = owner_.lock();
        if (!owner)
            return Stale<void>();
        auto result = owner->Revoke(slot_, generation_);
        if (result.HasValue())
            generation_ = 0;
        return result;
    }

    void InputActionSourceToken::Abandon() noexcept {
        if (const auto owner = owner_.lock())
            owner->AbandonSource(slot_, generation_);
        generation_ = 0;
    }

    InputActionSourceToken::~InputActionSourceToken() {
        Abandon();
    }

    InputActionSourceToken::InputActionSourceToken(InputActionSourceToken &&other) noexcept
        : owner_(std::move(other.owner_)), generation_(std::exchange(other.generation_, 0)), slot_(other.slot_) {}

    InputActionSourceToken &InputActionSourceToken::operator=(InputActionSourceToken &&other) noexcept {
        if (this != &other) {
            Abandon();
            owner_ = std::move(other.owner_);
            generation_ = std::exchange(other.generation_, 0);
            slot_ = other.slot_;
        }
        return *this;
    }

    void InputCollectionFrame::Abandon() noexcept {
        if (const auto owner = owner_.lock())
            owner->AbandonFrame(serial_);
        serial_ = 0;
    }

    InputCollectionFrame::~InputCollectionFrame() {
        Abandon();
    }

    InputCollectionFrame::InputCollectionFrame(InputCollectionFrame &&other) noexcept
        : owner_(std::move(other.owner_)), serial_(std::exchange(other.serial_, 0)) {}

    InputCollectionFrame &InputCollectionFrame::operator=(InputCollectionFrame &&other) noexcept {
        if (this != &other) {
            Abandon();
            owner_ = std::move(other.owner_);
            serial_ = std::exchange(other.serial_, 0);
        }
        return *this;
    }
}  // namespace Horo::Input
