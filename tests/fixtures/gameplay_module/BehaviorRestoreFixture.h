#pragma once
/** @brief Real native module sources, values and optional capabilities; no host-only replacement source. */
#include "GameplayBehaviorRestoreFixture.h"
#include "GameplayRetirementFixture.h"
#include "Horo/Runtime/Save/SaveCanonicalCodec.h"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <tuple>

namespace Horo::Tests::BehaviorRestoreFixture {
    using namespace Gameplay;
    using namespace Runtime;

    inline Error Invalid() {
        return MakeError(SaveErrors::RestoreAdapterContractInvalid);
    }

    inline CanonicalCodecLimits Limits(std::uint64_t bytes = 65536) {
        CanonicalCodecLimits result;
        result.maximumBytes = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, 65536));
        result.maximumStringBytes = result.maximumBytes;
        result.maximumCollectionElements = 256;
        result.maximumNestingDepth = 4;
        result.maximumDecodedBytes = 131072;
        result.maximumReadWorkBytes = 524288;
        return result;
    }

    struct Entry final {
        BehaviorInstanceId instance;
        GameplayBehaviorDurableFields fields;
        PersistentEntityId entity;
        std::uint64_t generation{};
    };

    inline auto Key(const Entry &entry) {
        return std::tuple{entry.entity, entry.generation, entry.instance.value};
    }

    inline Result<void> WriteFields(CanonicalValueWriter &writer, const GameplayBehaviorDurableFields &fields) {
        if (auto r = writer.WriteUInt64(fields.value); r.HasError())
            return r;
        if (auto r = writer.WriteBool(fields.peer.has_value()); r.HasError())
            return r;
        if (!fields.peer)
            return Result<void>::Success();
        if (auto r = writer.WriteBytes(std::as_bytes(std::span{fields.peer->entity.world.Bytes()})); r.HasError())
            return r;
        if (auto r = writer.WriteBytes(std::as_bytes(std::span{fields.peer->entity.entity.Bytes()})); r.HasError())
            return r;
        return writer.WriteUInt64(fields.peer->generation);
    }

    inline Result<CanonicalEncodedValue> EncodeEntry(const Entry &entry) {
        CanonicalValueWriter writer{Limits()};
        if (auto r = writer.WriteBytes(std::as_bytes(std::span{entry.entity.Bytes()})); r.HasError())
            return Result<CanonicalEncodedValue>::Failure(r.ErrorValue());
        if (auto r = writer.WriteUInt64(entry.generation); r.HasError())
            return Result<CanonicalEncodedValue>::Failure(r.ErrorValue());
        if (auto r = writer.WriteUInt64(entry.instance.value); r.HasError())
            return Result<CanonicalEncodedValue>::Failure(r.ErrorValue());
        if (auto r = WriteFields(writer, entry.fields); r.HasError())
            return Result<CanonicalEncodedValue>::Failure(r.ErrorValue());
        return std::move(writer).Finalize();
    }

    template <class Id> Result<Id> ReadId(CanonicalValueReader &reader) {
        auto bytes = reader.ReadBytes(16);
        if (bytes.HasError())
            return Result<Id>::Failure(bytes.ErrorValue());
        if (bytes.Value().size() != 16)
            return Result<Id>::Failure(Invalid());
        SaveIdentityDetail::Bytes values{};
        std::ranges::transform(bytes.Value(), values.begin(), [](std::byte b) {
            return std::to_integer<std::uint8_t>(b);
        });
        return Id::FromBytes(values);
    }

    inline Result<BehaviorEntityRestoreTarget> ReadPeer(CanonicalValueReader &reader) {
        auto world = ReadId<SaveWorldId>(reader);
        if (world.HasError())
            return Result<BehaviorEntityRestoreTarget>::Failure(world.ErrorValue());
        auto entity = ReadId<PersistentEntityId>(reader);
        if (entity.HasError())
            return Result<BehaviorEntityRestoreTarget>::Failure(entity.ErrorValue());
        auto generation = reader.ReadUInt64();
        if (generation.HasError())
            return Result<BehaviorEntityRestoreTarget>::Failure(generation.ErrorValue());
        if (generation.Value() == 0)
            return Result<BehaviorEntityRestoreTarget>::Failure(Invalid());
        return Result<BehaviorEntityRestoreTarget>::Success({{world.Value(), entity.Value()}, generation.Value()});
    }

    inline Result<GameplayBehaviorDurableFields> ReadFields(CanonicalValueReader &reader) {
        using Outcome = Result<GameplayBehaviorDurableFields>;
        auto value = reader.ReadUInt64();
        if (value.HasError())
            return Outcome::Failure(value.ErrorValue());
        auto hasPeer = reader.ReadBool();
        if (hasPeer.HasError())
            return Outcome::Failure(hasPeer.ErrorValue());
        GameplayBehaviorDurableFields result{value.Value(), {}, {}};
        if (hasPeer.Value()) {
            auto peer = ReadPeer(reader);
            if (peer.HasError())
                return Outcome::Failure(peer.ErrorValue());
            result.peer = peer.Value();
        }
        return Outcome::Success(std::move(result));
    }

    inline Result<BehaviorRestoreAttachment> ReadAttachment(CanonicalValueReader &reader) {
        using Outcome = Result<BehaviorRestoreAttachment>;
        auto entity = ReadId<PersistentEntityId>(reader);
        if (entity.HasError())
            return Outcome::Failure(entity.ErrorValue());
        auto generation = reader.ReadUInt64();
        if (generation.HasError())
            return Outcome::Failure(generation.ErrorValue());
        auto instance = reader.ReadUInt64();
        if (instance.HasError())
            return Outcome::Failure(instance.ErrorValue());
        if (generation.Value() == 0 || instance.Value() == 0)
            return Outcome::Failure(Invalid());
        return Outcome::Success({entity.Value(), generation.Value(), {instance.Value()}});
    }

    inline Result<Entry> DecodeEntry(const CanonicalDecodedValue &value) {
        auto opened = value.OpenReader();
        if (opened.HasError())
            return Result<Entry>::Failure(opened.ErrorValue());
        auto reader = std::move(opened).Value();
        auto identity = ReadAttachment(reader);
        if (identity.HasError())
            return Result<Entry>::Failure(identity.ErrorValue());
        auto fields = ReadFields(reader);
        if (fields.HasError())
            return Result<Entry>::Failure(fields.ErrorValue());
        if (auto end = reader.RequireFinished(); end.HasError())
            return Result<Entry>::Failure(end.ErrorValue());
        return Result<Entry>::Success(
            {identity.Value().instance, std::move(fields).Value(), identity.Value().entity, identity.Value().generation});
    }

    inline bool NextEntry(std::span<const Entry> entries, const Entry &next) {
        return next.instance.IsValid() && next.entity.IsValid() && next.generation != 0 &&
               (entries.empty() || Key(entries.back()) < Key(next));
    }

    inline Result<std::vector<Entry>> Decode(std::span<const std::byte> bytes) {
        using Outcome = Result<std::vector<Entry>>;
        auto opened = CanonicalValueReader::Create(bytes, Limits());
        if (opened.HasError())
            return Outcome::Failure(opened.ErrorValue());
        auto reader = std::move(opened).Value();
        auto version = reader.ReadUInt32();
        if (version.HasError())
            return Outcome::Failure(version.ErrorValue());
        if (version.Value() != 1)
            return Outcome::Failure(Invalid());
        auto sequence = reader.ReadSequence();
        if (sequence.HasError())
            return Outcome::Failure(sequence.ErrorValue());
        std::vector<Entry> entries;
        entries.reserve(sequence.Value().size());
        for (const auto &value : sequence.Value()) {
            auto entry = DecodeEntry(value);
            if (entry.HasError())
                return Outcome::Failure(entry.ErrorValue());
            if (!NextEntry(entries, entry.Value()))
                return Outcome::Failure(Invalid());
            entries.push_back(std::move(entry).Value());
        }
        if (auto end = reader.RequireFinished(); end.HasError())
            return Outcome::Failure(end.ErrorValue());
        return Outcome::Success(std::move(entries));
    }

    class Collector final : public IBehaviorPersistenceVisitor {
    public:
        std::vector<Entry> entries;

        Result<void> Visit(std::span<const BehaviorRestoreInstance> instances) override {
            for (const auto &instance : instances) {
                auto *fields = dynamic_cast<IGameplayBehaviorDurableFixture *>(instance.implementation);
                if (!fields)
                    return Result<void>::Failure(Invalid());
                entries.push_back({instance.instance, fields->DurableFields(), instance.persistentEntity, instance.persistentGeneration});
            }
            std::ranges::sort(entries, [](const auto &a, const auto &b) {
                return Key(a) < Key(b);
            });
            return Result<void>::Success();
        }
    };

    inline Result<std::vector<std::byte>> Encode(std::span<const Entry> entries, std::uint64_t maximum) {
        using Outcome = Result<std::vector<std::byte>>;
        std::vector<CanonicalEncodedValue> values;
        values.reserve(entries.size());
        for (const auto &entry : entries) {
            auto value = EncodeEntry(entry);
            if (value.HasError())
                return Outcome::Failure(value.ErrorValue());
            values.push_back(std::move(value).Value());
        }
        CanonicalValueWriter writer{Limits(maximum)};
        if (auto r = writer.WriteUInt32(1); r.HasError())
            return Outcome::Failure(r.ErrorValue());
        if (auto r = writer.WriteSequence(values); r.HasError())
            return Outcome::Failure(r.ErrorValue());
        auto encoded = std::move(writer).Finalize();
        if (encoded.HasError())
            return Outcome::Failure(encoded.ErrorValue());
        return Outcome::Success({encoded.Value().Bytes().begin(), encoded.Value().Bytes().end()});
    }

    inline Result<std::vector<std::byte>> EncodeReload(const GameplayBehaviorDurableFields &fields) {
        using Outcome = Result<std::vector<std::byte>>;
        CanonicalValueWriter writer{Limits()};
        if (auto r = WriteFields(writer, fields); r.HasError())
            return Outcome::Failure(r.ErrorValue());
        if (auto r = writer.WriteBool(fields.resolvedPeer.has_value()); r.HasError())
            return Outcome::Failure(r.ErrorValue());
        if (fields.resolvedPeer) {
            if (auto r = writer.WriteBytes(std::as_bytes(std::span{fields.resolvedPeer->identity.Bytes()})); r.HasError())
                return Outcome::Failure(r.ErrorValue());
            if (auto r = writer.WriteUInt64(fields.resolvedPeer->incarnation); r.HasError())
                return Outcome::Failure(r.ErrorValue());
        }
        auto value = std::move(writer).Finalize();
        if (value.HasError())
            return Outcome::Failure(value.ErrorValue());
        return Outcome::Success({value.Value().Bytes().begin(), value.Value().Bytes().end()});
    }

    inline Result<void> ReadResolved(CanonicalValueReader &reader, GameplayBehaviorDurableFields &fields) {
        auto present = reader.ReadBool();
        if (present.HasError())
            return Result<void>::Failure(present.ErrorValue());
        if (!present.Value())
            return Result<void>::Success();
        auto identity = ReadId<PersistentEntityId>(reader);
        if (identity.HasError())
            return Result<void>::Failure(identity.ErrorValue());
        auto generation = reader.ReadUInt64();
        if (generation.HasError())
            return Result<void>::Failure(generation.ErrorValue());
        if (generation.Value() == 0)
            return Result<void>::Failure(Invalid());
        fields.resolvedPeer = SaveRestoreEntityTarget{identity.Value(), generation.Value()};
        return Result<void>::Success();
    }

    inline Result<GameplayBehaviorDurableFields> DecodeReload(std::span<const std::byte> bytes) {
        using Outcome = Result<GameplayBehaviorDurableFields>;
        auto opened = CanonicalValueReader::Create(bytes, Limits());
        if (opened.HasError())
            return Outcome::Failure(opened.ErrorValue());
        auto reader = std::move(opened).Value();
        auto fields = ReadFields(reader);
        if (fields.HasError())
            return fields;
        auto actual = std::move(fields).Value();
        if (auto r = ReadResolved(reader, actual); r.HasError())
            return Outcome::Failure(r.ErrorValue());
        if (auto r = reader.RequireFinished(); r.HasError())
            return Outcome::Failure(r.ErrorValue());
        return Outcome::Success(std::move(actual));
    }

    class ApplyVisitor final : public IBehaviorPersistenceVisitor {
    public:
        explicit ApplyVisitor(std::span<const Entry> entries) : entries_(entries) {}

        Result<void> Visit(std::span<const BehaviorRestoreInstance> instances) override {
            if (instances.size() != entries_.size())
                return Result<void>::Failure(Invalid());
            for (const auto &instance : instances) {
                const auto expected = std::tuple{instance.persistentEntity, instance.persistentGeneration, instance.instance.value};
                const auto found = std::ranges::lower_bound(entries_, expected, {}, Key);
                auto *fields = dynamic_cast<IGameplayBehaviorDurableFixture *>(instance.implementation);
                if (found == entries_.end() || Key(*found) != expected || !fields)
                    return Result<void>::Failure(Invalid());
                fields->SetDurableFields(found->fields);
            }
            return Result<void>::Success();
        }

    private:
        std::span<const Entry> entries_;
    };

    class Candidate final : public IPreparedPersistenceState,
                            public IInactiveBehaviorPersistenceState,
                            public IBehaviorPersistenceReferenceInventory,
                            public IPublishedGameplayPreparationRetirement {
    public:
        Candidate(BehaviorPersistenceTarget target, std::vector<Entry> entries, GameplayRetirementFixture *control)
            : target_(std::move(target)), entries_(std::move(entries)), control_(control) {
            const auto owner = SaveParticipantId::Parse("game.tests.durable0").Value();
            for (std::size_t index = 0; index < entries_.size(); ++index) {
                const auto &entry = entries_[index];
                if (entry.fields.peer)
                    references_.push_back(
                        {{index + 1}, owner, {entry.entity, entry.generation, entry.instance}, *entry.fields.peer, false, true});
            }
            if (control_ && !references_.empty()) {
                if (control_->restoreFailure == 12)
                    references_.front().owner = SaveParticipantId::Parse("game.foreign.owner").Value();
                if (control_->restoreFailure == 13)
                    references_.push_back(references_.front());
                if (control_->restoreFailure == 14)
                    references_.front().source.instance = {999};
                if (control_->restoreFailure == 15)
                    references_.front().target =
                        BehaviorComponentRestoreTarget{*entries_.front().fields.peer, ComponentTypeId::Parse("game.tests.absent").Value()};
            }
        }

        Result<void> MaterializeInactive(const BehaviorPersistenceTarget &target) override {
            if (!target.SameOwner(target_) || !target.InactiveRestore() || Fails(3))
                return Result<void>::Failure(Invalid());
            ApplyVisitor visitor{entries_};
            auto result = target.Visit(visitor);
            if (result.HasValue()) {
                materialized_ = true;
                if (control_)
                    ++control_->behaviorMaterialized;
            }
            return result;
        }

        Result<void> FixupRuntimeReferences(const SaveRestoreReferenceView &view) override {
            if (!materialized_ || Fails(4) || view.references.size() != references_.size())
                return Result<void>::Failure(Invalid());
            if (auto actual = ValidateMaterialization(); actual.HasError())
                return actual;
            if (auto fixed = Resolve(view); fixed.HasError())
                return fixed;
            ApplyVisitor visitor{entries_};
            if (auto applied = target_.Visit(visitor); applied.HasError())
                return applied;
            fixed_ = true;
            if (control_)
                ++control_->behaviorFixed;
            return Result<void>::Success();
        }

        std::span<const BehaviorRestoreReference> RestoreReferences() const noexcept override {
            return references_;
        }

        void Publish() noexcept override {
            if (!materialized_ || !fixed_)
                std::terminate();
            published_ = true;
        }

        Result<void> RetirePreparation() override {
            if (!published_ || Fails(5))
                return Result<void>::Failure(Invalid());
            if (Fails(10)) {
                if (control_->armAllocationFailure)
                    control_->armAllocationFailure(control_->allocationOwner);
                throw std::bad_alloc{};
            }
            if (Fails(11))
                throw std::runtime_error{"native retirement callback failure"};
            if (control_)
                ++control_->preparationRetired;
            return Result<void>::Success();
        }

    private:
        Result<void> ValidateMaterialization() {
            Collector actual;
            if (auto visited = target_.Visit(actual); visited.HasError())
                return visited;
            if (actual.entries.size() != entries_.size())
                return Result<void>::Failure(Invalid());
            for (std::size_t i = 0; i < entries_.size(); ++i)
                if (actual.entries[i].fields.value != entries_[i].fields.value)
                    return Result<void>::Failure(Invalid());
            return Result<void>::Success();
        }

        bool Fails(unsigned phase) const noexcept {
            return control_ && control_->restoreFailure == phase;
        }

        Result<void> Resolve(const SaveRestoreReferenceView &view) {
            for (std::size_t index = 0; index < entries_.size(); ++index) {
                auto &entry = entries_[index];
                if (entry.fields.peer) {
                    const auto *result = view.Find({index + 1});
                    if (!result || result->disposition != SaveRestoreReferenceDisposition::Resolved)
                        return Result<void>::Failure(Invalid());
                    const auto *target = std::get_if<SaveRestoreEntityTarget>(&result->target);
                    if (!target || target->identity != entry.fields.peer->entity.entity ||
                        target->incarnation != entry.fields.peer->generation)
                        return Result<void>::Failure(Invalid());
                    entry.fields.resolvedPeer = *target;
                }
            }
            return Result<void>::Success();
        }

        BehaviorPersistenceTarget target_;
        std::vector<Entry> entries_;
        std::vector<BehaviorRestoreReference> references_;
        GameplayRetirementFixture *control_{};
        bool materialized_{}, fixed_{}, published_{};
    };

    class WithoutRetirement final : public IPreparedPersistenceState,
                                    public IInactiveBehaviorPersistenceState,
                                    public IBehaviorPersistenceReferenceInventory {
    public:
        explicit WithoutRetirement(std::unique_ptr<Candidate> actual) : actual_(std::move(actual)) {}

        Result<void> MaterializeInactive(const BehaviorPersistenceTarget &target) override {
            return actual_->MaterializeInactive(target);
        }

        Result<void> FixupRuntimeReferences(const SaveRestoreReferenceView &view) override {
            return actual_->FixupRuntimeReferences(view);
        }

        std::span<const BehaviorRestoreReference> RestoreReferences() const noexcept override {
            return actual_->RestoreReferences();
        }

        void Publish() noexcept override {
            actual_->Publish();
        }

    private:
        std::unique_ptr<Candidate> actual_;
    };

    class LegacyPrepared final : public IPreparedPersistenceState, public IPublishedGameplayPreparationRetirement {
    public:
        Result<void> RetirePreparation() override {
            return Result<void>::Failure(Invalid());
        }

        void Publish() noexcept override {
            std::terminate();
        }
    };

    class BoundSource final : public IPersistenceSource {
    public:
        BoundSource(BehaviorPersistenceTarget target, GameplayRetirementFixture *control) : target_(std::move(target)), control_(control) {}

        Result<std::vector<std::byte>> CaptureRuntimeState(std::uint64_t maximum) const override {
            if (maximum == 0)
                return Result<std::vector<std::byte>>::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
            Collector visitor;
            if (auto result = target_.Visit(visitor); result.HasError())
                return Result<std::vector<std::byte>>::Failure(result.ErrorValue());
            if (control_)
                ++control_->behaviorCaptured;
            return Encode(visitor.entries, maximum);
        }

        Result<std::unique_ptr<IPreparedPersistenceState>> PrepareRuntimeState(std::span<const std::byte> bytes) override {
            using Outcome = Result<std::unique_ptr<IPreparedPersistenceState>>;
            if (control_ && control_->restoreFailure == 2)
                return Outcome::Failure(Invalid());
            auto entries = Decode(bytes);
            if (entries.HasError())
                return Outcome::Failure(entries.ErrorValue());
            if (control_ && control_->restoreFailure == 9)
                return Outcome::Success(std::make_unique<LegacyPrepared>());
            auto actual = std::make_unique<Candidate>(target_, std::move(entries).Value(), control_);
            if (control_ && control_->restoreFailure == 7)
                return Outcome::Success(std::make_unique<WithoutRetirement>(std::move(actual)));
            return Outcome::Success(std::move(actual));
        }

    private:
        BehaviorPersistenceTarget target_;
        GameplayRetirementFixture *control_{};
    };
}  // namespace Horo::Tests::BehaviorRestoreFixture
