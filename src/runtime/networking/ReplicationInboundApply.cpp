#include "Horo/Network/ReplicationInboundApply.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Network {
    namespace {
        /** @brief Preserves the requested result type when constructing a domain rejection. */
        template <typename T> Result<T> Fail(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Closes reentrant staging/application while permitting explicit retirement. */
        struct BusyGuard final {
            explicit BusyGuard(bool &busy) : flag(busy) {
                flag = true;
            }

            ~BusyGuard() {
                flag = false;
            }

            BusyGuard(const BusyGuard &) = delete;
            BusyGuard &operator=(const BusyGuard &) = delete;
            bool &flag;
        };

        /** @brief Reads routing hints only; the codec validates the complete canonical framing afterward. */
        std::uint64_t ReadWord(std::span<const std::byte> wire, std::size_t offset, std::size_t width) {
            std::uint64_t value{};
            for (std::size_t index{}; index < width; ++index)
                value |= std::uint64_t{std::to_integer<std::uint8_t>(wire[offset + index])} << (8 * index);
            return value;
        }

        /** @brief Conservatively charges complete immutable decoded backing without overflowing. */
        std::optional<std::size_t> Charge(const ReplicationDecodedState &state, std::size_t bound) {
            std::size_t bytes = sizeof(ReplicationDecodedState);
            if (bytes > bound || state.Fields().size() > (bound - bytes) / sizeof(ReplicationCapturedField))
                return std::nullopt;
            bytes += state.Fields().size() * sizeof(ReplicationCapturedField);
            for (const auto &field : state.Fields()) {
                std::size_t payload{};
                if (const auto *text = std::get_if<std::string>(&field.value))
                    payload = text->capacity();
                if (const auto *data = std::get_if<std::vector<std::byte>>(&field.value))
                    payload = data->capacity();
                if (payload > bound - bytes)
                    return std::nullopt;
                bytes += payload;
            }
            return bytes;
        }
    }  // namespace

    /** @copydoc ReplicationInboundApply::ReplicationInboundApply */
    ReplicationInboundApply::ReplicationInboundApply(ConstructionKey, const ReplicationInboundAuthority &authority,
                                                     ReplicationWorldLifecycle &world, std::weak_ptr<IReplicationApplyOwner> owner,
                                                     const ReplicationInboundLimits &limits)
        : authority_(authority), world_(world), owner_(std::move(owner)), limits_(limits) {
        objects_.reserve(limits.maximumObjects);
    }

    /** @copydoc ReplicationInboundApply::Create */
    Result<std::shared_ptr<ReplicationInboundApply>> ReplicationInboundApply::Create(const ReplicationInboundAuthority &authority,
                                                                                     ReplicationWorldLifecycle &world,
                                                                                     const std::shared_ptr<IReplicationApplyOwner> &owner,
                                                                                     const std::uint64_t now,
                                                                                     const ReplicationInboundLimits &limits) {
        if (!owner || !authority.session || !authority.connection.IsValid() || !authority.generation.IsValid() ||
            !authority.scene.IsValid() || !authority.networkSession.IsValid() || !authority.epoch.IsValid() ||
            !authority.localRecipient.IsValid() || !authority.protocol.IsValid() || !authority.message.IsValid() ||
            limits.maximumObjects == 0 || limits.maximumObjects > 4096 || limits.maximumPending == 0 ||
            limits.maximumPending > limits.maximumObjects || limits.maximumRetainedBytes == 0 ||
            limits.maximumRetainedBytes > 64 * 1024 * 1024 || limits.maximumWireBytes < 172 || limits.maximumWireBytes > 16 * 1024 * 1024)
            return Fail<std::shared_ptr<ReplicationInboundApply>>(ReplicationStateErrors::Invalid);
        try {
            auto result = std::make_shared<ReplicationInboundApply>(ConstructionKey{}, authority, world, owner, limits);
            if (const auto admitted = result->Admit(Runtime::RuntimePhase::NetworkPoll, now, {}); admitted.HasError())
                return Result<std::shared_ptr<ReplicationInboundApply>>::Failure(admitted.ErrorValue());
            return Result<std::shared_ptr<ReplicationInboundApply>>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Fail<std::shared_ptr<ReplicationInboundApply>>(ReplicationStateErrors::Capacity);
        }
    }

    /** @copydoc ReplicationInboundApply::CheckOwner */
    Result<void> ReplicationInboundApply::CheckOwner() const {
        if (thread_ != std::this_thread::get_id())
            return Fail<void>(ReplicationStateErrors::Invalid);
        if (stopped_)
            return Fail<void>(ReplicationStateErrors::Closed);
        return Result<void>::Success();
    }

    /** @copydoc ReplicationInboundApply::Admit */
    Result<ReplicationWorldReadLease> ReplicationInboundApply::Admit(const Runtime::RuntimePhase phase, const std::uint64_t now,
                                                                     const CancellationToken &cancellation) const {
        if (const auto owner = CheckOwner(); owner.HasError())
            return Result<ReplicationWorldReadLease>::Failure(owner.ErrorValue());
        if (const auto admitted = authority_.session->AdmitGameplay(authority_.connection, authority_.generation, now); admitted.HasError())
            return Result<ReplicationWorldReadLease>::Failure(admitted.ErrorValue());
        auto lease = world_.AcquireFor({authority_.scene, authority_.networkSession, phase, now, cancellation},
                                       ReplicationWorldCapability::ApplyAuthoritativeState);
        if (lease.HasError())
            return lease;
        if (lease.Value().Descriptor().authority != authority_.epoch)
            return Fail<ReplicationWorldReadLease>(ReplicationStateErrors::Stale);
        return lease;
    }

    /** @copydoc ReplicationInboundApply::ValidateObject */
    Result<void> ReplicationInboundApply::ValidateObject(const Object &object, const ReplicationWorldReadLease &world) const {
        if (object.retired || !object.role || !object.codec || world.IsRevoked())
            return Fail<void>(ReplicationStateErrors::Stale);
        const auto role = object.role->Snapshot();
        if (role.HasError())
            return Result<void>::Failure(role.ErrorValue());
        const auto &recipient = object.codec->Recipient();
        if (role.Value() != recipient || recipient.object != object.mapping.object || recipient.session != authority_.networkSession ||
            recipient.localPeer != authority_.localRecipient || recipient.role != world.Descriptor().role ||
            recipient.schema != object.mapping.provenance.schema || recipient.schemaVersion != object.mapping.provenance.schemaVersion)
            return Fail<void>(ReplicationStateErrors::Stale);
        if (std::ranges::find(world.Mapping().Entries(), object.mapping) == world.Mapping().Entries().end())
            return Fail<void>(ReplicationStateErrors::Stale);
        if ((object.baseline && !object.baseline->IsCurrent()) || (object.pending && !object.pending->IsCurrent()))
            return Fail<void>(ReplicationStateErrors::Closed);
        return Result<void>::Success();
    }

    /** @copydoc ReplicationInboundApply::RegisterObject */
    Result<void> ReplicationInboundApply::RegisterObject(const NetworkObjectMappingEntry &mapping,
                                                         std::shared_ptr<ReplicationRoleState> role,
                                                         std::shared_ptr<ReplicationStateCodec> codec) {
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (busy_ || !role || !codec)
            return Fail<void>(ReplicationStateErrors::Invalid);
        if (objects_.size() == limits_.maximumObjects)
            return Fail<void>(ReplicationStateErrors::Capacity);
        if (std::ranges::find(objects_, mapping.object, [](const Object &object) {
            return object.mapping.object;
        }) != objects_.end())
            return Fail<void>(ReplicationStateErrors::Stale);
        auto lease = world_.AcquireFor({authority_.scene, authority_.networkSession, Runtime::RuntimePhase::NetworkPoll, 1, {}},
                                       ReplicationWorldCapability::ApplyAuthoritativeState);
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        Object object{mapping, std::move(role), std::move(codec), {}, {}, {}, {}, {}, false};
        if (const auto valid = ValidateObject(object, lease.Value()); valid.HasError())
            return valid;
        objects_.push_back(std::move(object));
        return Result<void>::Success();
    }

    /** @copydoc ReplicationInboundApply::RevokeObject */
    Result<void> ReplicationInboundApply::RevokeObject(const NetworkObjectId object) {
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
            return Shutdown();
        ++revision_;
        const auto found = std::ranges::find(objects_, object, [](const Object &entry) {
            return entry.mapping.object;
        });
        if (found != objects_.end()) {
            found->pending.reset();
            found->baseline.reset();
            found->role.reset();
            found->codec.reset();
            found->retired = true;
        }
        return Result<void>::Success();
    }

    /** @copydoc ReplicationInboundApply::Handle */
    Result<void> ReplicationInboundApply::Handle(const MessageEnvelope &) {
        return Fail<void>(ReplicationStateErrors::Invalid);
    }

    /** @copydoc ReplicationInboundApply::HandleAdmitted */
    Result<void> ReplicationInboundApply::HandleAdmitted(const InboundMessageContext &context, const MessageEnvelope &message) {
        if (context.session != authority_.session || context.connection != authority_.connection ||
            context.generation != authority_.generation || message.protocol != authority_.protocol || message.message != authority_.message)
            return Fail<void>(ReplicationStateErrors::Stale);
        auto lease = Admit(Runtime::RuntimePhase::NetworkPoll, context.ownerTick, context.cancellation);
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        if (busy_ || message.payload.size() < 172 || message.payload.size() > limits_.maximumWireBytes)
            return Fail<void>(ReplicationStateErrors::Invalid);
        try {
            const auto wire = std::span{message.payload};
            auto index = ResolveRecord(wire, lease.Value());
            if (index.HasError())
                return Result<void>::Failure(index.ErrorValue());
            return DecodeAndStage(index.Value(), context, wire);
        } catch (const std::bad_alloc &) {
            return Fail<void>(ReplicationStateErrors::Capacity);
        }
    }

    /** @copydoc ReplicationInboundApply::ResolveRecord */
    Result<std::size_t> ReplicationInboundApply::ResolveRecord(const std::span<const std::byte> wire,
                                                               const ReplicationWorldReadLease &world) const {
        const auto epoch = ReplicationAuthorityEpoch::Create(ReadWord(wire, 40, 8));
        if (epoch.HasError())
            return Fail<std::size_t>(ReplicationStateErrors::Invalid);
        const auto identity =
            NetworkObjectId::Create(epoch.Value(), ReadWord(wire, 48, 8), static_cast<std::uint32_t>(ReadWord(wire, 56, 4)));
        if (identity.HasError())
            return Fail<std::size_t>(ReplicationStateErrors::Invalid);
        const auto found = std::ranges::find(objects_, identity.Value(), [](const Object &entry) {
            return entry.mapping.object;
        });
        if (found == objects_.end())
            return Fail<std::size_t>(ReplicationStateErrors::Stale);
        if (const auto valid = ValidateObject(*found, world); valid.HasError())
            return Result<std::size_t>::Failure(valid.ErrorValue());
        return Result<std::size_t>::Success(static_cast<std::size_t>(found - objects_.begin()));
    }

    /** @copydoc ReplicationInboundApply::DecodeAndStage */
    Result<void> ReplicationInboundApply::DecodeAndStage(const std::size_t index, const InboundMessageContext &context,
                                                         const std::span<const std::byte> wire) {
        const auto found = objects_.begin() + index;
        const auto digest = ComputeSha256(wire);
        if ((found->pending && digest == found->pendingWire) || (found->baseline && digest == found->baselineWire))
            return Result<void>::Success();
        const Object pinned = *found;
        const auto revision = revision_;
        [[maybe_unused]] const auto self = shared_from_this();
        const BusyGuard guard{busy_};
        const auto rootRevision = ReadWord(wire, 96, 8);
        const auto *baseline = pinned.baseline ? &*pinned.baseline : nullptr;
        if (pinned.pending && pinned.pending->PublicationRevision() == rootRevision)
            baseline = &*pinned.pending;
        auto decoded = pinned.codec->Decode(wire, pinned.mapping, baseline, context.cancellation);
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        if (stopped_ || revision != revision_ || context.cancellation.IsCancellationRequested())
            return Fail<void>(ReplicationStateErrors::Closed);
        auto current = Admit(Runtime::RuntimePhase::NetworkPoll, context.ownerTick, context.cancellation);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (const auto valid = ValidateObject(objects_[index], current.Value()); valid.HasError())
            return valid;
        const auto *latest = pinned.Latest();
        if (latest && (decoded.Value().PublicationRevision() <= latest->PublicationRevision() ||
                       decoded.Value().SimulationTick() < latest->SimulationTick()))
            return Fail<void>(ReplicationStateErrors::Stale);
        if (const auto retained = AdmitRetainedState(index, decoded.Value()); retained.HasError())
            return retained;
        objects_[index].pending = std::move(decoded).Value();
        objects_[index].pendingWire = digest;
        objects_[index].cancellation = context.cancellation;
        return Result<void>::Success();
    }

    /** @copydoc ReplicationInboundApply::AdmitRetainedState */
    Result<void> ReplicationInboundApply::AdmitRetainedState(const std::size_t index, const ReplicationDecodedState &state) const {
        std::size_t bytes{};
        std::size_t pending{};
        for (std::size_t objectIndex{}; objectIndex < objects_.size(); ++objectIndex) {
            const auto &object = objects_[objectIndex];
            if (object.pending)
                ++pending;
            const auto *pendingState = object.pending ? &*object.pending : nullptr;
            if (objectIndex == index)
                pendingState = &state;
            for (const auto *retained : {object.baseline ? &*object.baseline : nullptr, pendingState}) {
                if (!retained)
                    continue;
                const auto charge = Charge(*retained, limits_.maximumRetainedBytes - bytes);
                if (!charge)
                    return Fail<void>(ReplicationStateErrors::Capacity);
                bytes += *charge;
            }
        }
        if (!objects_[index].pending && pending == limits_.maximumPending)
            return Fail<void>(ReplicationStateErrors::Capacity);
        return Result<void>::Success();
    }

    /** @copydoc ReplicationInboundApply::ValidatePending */
    Result<void> ReplicationInboundApply::ValidatePending(const ReplicationWorldWorkRequest &request, const std::uint64_t now) const {
        auto current = Admit(request.phase, now, request.cancellation);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        for (const auto &object : objects_) {
            if (!object.pending)
                continue;
            if (object.cancellation.IsCancellationRequested())
                return Fail<void>(ReplicationStateErrors::Stale);
            if (const auto valid = ValidateObject(object, current.Value()); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc ReplicationInboundApply::DiscardPending */
    void ReplicationInboundApply::DiscardPending() noexcept {
        for (auto &object : objects_)
            object.pending.reset();
    }

    /** @copydoc ReplicationInboundApply::ApplyAtSafePoint */
    Result<ReplicationApplyReport> ReplicationInboundApply::ApplyAtSafePoint(const ReplicationWorldWorkRequest &request,
                                                                             const std::uint64_t now) {
        if (const auto owner = CheckOwner(); owner.HasError())
            return Result<ReplicationApplyReport>::Failure(owner.ErrorValue());
        if (request.scene != authority_.scene || request.session != authority_.networkSession ||
            request.phase != Runtime::RuntimePhase::CommitDeferredLifecycleChanges || request.simulationTick == 0 || busy_)
            return Fail<ReplicationApplyReport>(ReplicationStateErrors::Invalid);
        auto lease = Admit(request.phase, now, request.cancellation);
        if (lease.HasError())
            return Result<ReplicationApplyReport>::Failure(lease.ErrorValue());
        const auto owner = owner_.lock();
        if (!owner)
            return Fail<ReplicationApplyReport>(ReplicationStateErrors::Closed);
        [[maybe_unused]] const auto self = shared_from_this();
        const BusyGuard guard{busy_};
        try {
            auto result = ApplyPending(*owner, lease.Value(), request, now);
            if (result.HasError())
                DiscardPending();
            return result;
        } catch (const std::bad_alloc &) {
            DiscardPending();
            return Fail<ReplicationApplyReport>(ReplicationStateErrors::Capacity);
        } catch (...) {
            // Host-owned adapters may throw foreign exception types. Contain them before returning to
            // the scheduler; detached candidates are destroyed and no delta root is acknowledged.
            DiscardPending();
            return Fail<ReplicationApplyReport>(ReplicationStateErrors::CallbackFault);
        }
    }

    /** @copydoc ReplicationInboundApply::CollectPending */
    std::vector<ReplicationApplyUpdate> ReplicationInboundApply::CollectPending(const CancellationToken &worldCancellation) const {
        std::vector<ReplicationApplyUpdate> updates;
        updates.reserve(limits_.maximumPending);
        for (const auto &object : objects_)
            if (object.pending)
                updates.push_back({object.mapping, object.codec->Recipient(), *object.pending, object.cancellation, worldCancellation});
        return updates;
    }

    /** @copydoc ReplicationInboundApply::ApplyPending */
    Result<ReplicationApplyReport> ReplicationInboundApply::ApplyPending(IReplicationApplyOwner &owner,
                                                                         const ReplicationWorldReadLease &world,
                                                                         const ReplicationWorldWorkRequest &request,
                                                                         const std::uint64_t now) {
        const auto revision = revision_;
        if (const auto valid = ValidatePending(request, now); valid.HasError())
            return Result<ReplicationApplyReport>::Failure(valid.ErrorValue());
        const auto updates = CollectPending(world.Cancellation());
        if (updates.empty())
            return Result<ReplicationApplyReport>::Success({});
        auto candidate = owner.Prepare(updates);
        if (candidate.HasError())
            return Result<ReplicationApplyReport>::Failure(candidate.ErrorValue());
        if (!candidate.Value() || stopped_ || revision != revision_)
            return Fail<ReplicationApplyReport>(ReplicationStateErrors::Closed);
        if (const auto valid = ValidatePending(request, now); valid.HasError())
            return Result<ReplicationApplyReport>::Failure(valid.ErrorValue());
        if (const auto committed = candidate.Value()->Commit(request.cancellation); committed.HasError())
            return Result<ReplicationApplyReport>::Failure(committed.ErrorValue());
        for (auto &object : objects_)
            if (object.pending) {
                object.baseline = std::move(object.pending);
                object.pending.reset();
                object.baselineWire = object.pendingWire;
            }
        return Result<ReplicationApplyReport>::Success({updates.size()});
    }

    /** @copydoc ReplicationInboundApply::Shutdown */
    Result<void> ReplicationInboundApply::Shutdown() {
        if (thread_ != std::this_thread::get_id())
            return Fail<void>(ReplicationStateErrors::Invalid);
        if (stopped_)
            return Result<void>::Success();
        stopped_ = true;
        objects_.clear();
        return Result<void>::Success();
    }
}  // namespace Horo::Network
