#pragma once

#include "Horo/Network/SceneReplicationCommitSource.h"
#include "ReplicationDescriptorTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>

namespace Horo::Network::CaptureTestSupport {
    using TestSupport::Bytes;
    using TestSupport::Codec;
    using TestSupport::Connection;
    using TestSupport::Field;
    using TestSupport::FieldIdValue;
    using TestSupport::Id;
    using TestSupport::Limits;
    using TestSupport::RequireError;
    using TestSupport::Schema;
    using TestSupport::SchemaId;
    using TestSupport::Session;
    using TestSupport::ValueType;
    using TestSupport::WireIdentity;

    inline ReplicationWorldActivationDescriptor World(const std::uint64_t generation = 1,
                                                      const ReplicationExecutionRole role = ReplicationExecutionRole::AuthorityServer) {
        return {Runtime::SceneRuntimeId{10}, NetworkSessionGeneration::Create(generation).Value(),
                ReplicationAuthorityEpoch::Create(generation).Value(), role};
    }

    inline NetworkObjectMappingEntry Object(const std::uint64_t slot = 1, const std::uint32_t generation = 1) {
        return {NetworkObjectId::Create(World().authority, slot, generation).Value(),
                {Runtime::SceneRuntimeId{10}, {static_cast<std::uint32_t>(slot - 1), generation}},
                {SchemaId(10), {1, 0}, std::nullopt}};
    }

    inline ReplicationWorldCaptureRead Read(ReplicationWorldLifecycle &lifecycle, const std::uint64_t tick = 1) {
        const auto descriptor = lifecycle.ActiveDescriptor().Value();
        auto result = lifecycle.AcquireCaptureRead({descriptor.scene, descriptor.session, Runtime::RuntimePhase::NetworkFlush, tick, {}});
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline ReplicationWorldLifecycle Lifecycle() {
        auto result = std::move(ReplicationWorldLifecycle::Create()).Value();
        REQUIRE(result.Stage(World()).HasValue());
        REQUIRE(result.CommitAtSafePoint(World().scene, World().session).HasValue());
        return result;
    }

    class CountingCodec final : public IReplicationFieldSerializer {
    public:
        CountingCodec()
            : descriptor{ValueType(1), Codec(1), {.value = "game.replication"}, ReplicationValueKind::FloatingPoint, {}, 8, 1},
              scalar(CanonicalScalarReplicationSerializer::Create(descriptor).Value()) {}

        const ReplicationSerializerDescriptor &Descriptor() const noexcept override {
            return descriptor;
        }

        Result<std::vector<std::byte>> Encode(const ReplicationRuntimeValue &value) const override {
            ++encodeCalls;
            return scalar->Encode(value);
        }

        Result<ReplicationRuntimeValue> Decode(const std::span<const std::byte> bytes) const override {
            return scalar->Decode(bytes);
        }

        Result<bool> CanonicallyEqual(const ReplicationRuntimeValue &left, const ReplicationRuntimeValue &right) const override {
            ++compareCalls;
            if (onCompare)
                onCompare();
            return scalar->CanonicallyEqual(left, right);
        }

        ReplicationSerializerDescriptor descriptor;
        std::shared_ptr<const CanonicalScalarReplicationSerializer> scalar;
        mutable std::size_t encodeCalls{};
        mutable std::size_t compareCalls{};
        std::function<void()> onCompare;
    };

    inline std::shared_ptr<const ReplicationSerializerRegistry> Registry(const std::shared_ptr<CountingCodec> &codec,
                                                                         const std::uint32_t fieldCount = 1) {
        std::vector<ReplicationFieldDescriptor> fields;
        for (std::uint32_t index = 1; index <= fieldCount; ++index)
            fields.push_back(Field(index));
        const std::array schemas{Schema(10, std::move(fields))};
        const auto descriptors = BuildReplicationDescriptorSnapshot(schemas, Limits).Value();
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{codec};
        return std::make_shared<const ReplicationSerializerRegistry>(ReplicationSerializerRegistry::Create(descriptors, codecs).Value());
    }

    enum class WriterFault {
        None,
        Missing,
        Duplicate,
        Foreign,
        WrongKind
    };

    enum class CallbackThrow {
        None,
        Allocation,
        Unexpected,
        Foreign
    };

    /** @brief Models a foreign owner ABI exception without imposing the standard library's exception inheritance. */
    struct ForeignCallbackFault final {};

    class OwnerCallbackError final : public std::runtime_error {
    public:
        OwnerCallbackError() : std::runtime_error("owner fault") {}
    };

    inline void ThrowCallback(const CallbackThrow mode) {
        using enum CallbackThrow;
        if (mode == Allocation)
            throw std::bad_alloc{};
        if (mode == Unexpected)
            throw OwnerCallbackError{};
        if (mode == Foreign)
            throw ForeignCallbackFault{};
    }

    class Owner final : public ICommittedReplicationSource {
    public:
        void Commit(const std::uint64_t tick, const double value) {
            tick_ = tick;
            value_ = value;
            ++revision_;
        }

        Result<ReplicationCommittedRead> BeginRead(const NetworkObjectMappingEntry &, const std::uint64_t tick) const override {
            ThrowCallback(throwBegin);
            ++begins;
            if (!committed || reading || tick != tick_)
                return Result<ReplicationCommittedRead>::Failure(MakeError(ReplicationCaptureErrors::Uncommitted));
            reading = true;
            return Result<ReplicationCommittedRead>::Success({tick_, revision_, revision_});
        }

        Result<void> Capture(const ReplicationCommittedRead &, ReplicationCaptureWriter &writer) const override {
            ThrowCallback(throwCapture);
            ++captures;
            if (onCapture)
                onCapture();
            if (fail)
                return Result<void>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
            using enum WriterFault;
            switch (fault) {
                case Missing:
                    return Result<void>::Success();
                case Foreign:
                    return writer.Write(FieldIdValue(9), value_);
                case WrongKind:
                    return writer.Write(FieldIdValue(1), std::int64_t{1});
                case Duplicate: {
                    const auto first = writer.Write(FieldIdValue(1), value_);
                    if (first.HasError())
                        return first;
                    return writer.Write(FieldIdValue(1), value_);
                }
                case None:
                    return writer.Write(FieldIdValue(1), value_);
            }
            return Result<void>::Success();
        }

        bool IsCurrent(const ReplicationCommittedRead &read) const noexcept override {
            return reading && committed && read.simulationTick == tick_ && read.commitRevision == revision_;
        }

        void EndRead(const ReplicationCommittedRead &) const noexcept override {
            reading = false;
            ++ends;
        }

        WriterFault fault{WriterFault::None};
        CallbackThrow throwBegin{CallbackThrow::None};
        CallbackThrow throwCapture{CallbackThrow::None};
        bool committed{true};
        bool fail{};
        mutable bool reading{};
        mutable std::size_t begins{};
        mutable std::size_t captures{};
        mutable std::size_t ends{};
        std::function<void()> onCapture;

    private:
        std::uint64_t tick_{1};
        std::uint64_t revision_{1};
        double value_{};
    };

    struct Fixture final {
        ReplicationWorldLifecycle lifecycle{Lifecycle()};
        std::shared_ptr<CountingCodec> codec{std::make_shared<CountingCodec>()};
        std::shared_ptr<Owner> owner{std::make_shared<Owner>()};
        std::shared_ptr<const ReplicationSerializerRegistry> registry{Registry(codec)};
        std::unique_ptr<ReplicationStateCapture> capture;

        explicit Fixture(ReplicationCaptureLimits limits = {}) {
            REQUIRE(lifecycle.RegisterObject(World().scene, World().session, Object()).HasValue());
            const std::array targets{ReplicationCaptureTarget{Object(), owner}};
            auto result = ReplicationStateCapture::Prepare(Read(lifecycle), registry, targets, limits);
            REQUIRE(result.HasValue());
            capture = std::move(result).Value();
        }

        ReplicationCaptureReport Capture(const std::uint64_t tick, const double value = 0.0, const CancellationToken &cancellation = {}) {
            owner->Commit(tick, value);
            auto result = capture->CaptureAtCommit(Read(lifecycle, tick), tick, cancellation);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        ReplicationCapturedStatePin Pin() {
            return capture->Latest(Object().object).Value();
        }
    };
}  // namespace Horo::Network::CaptureTestSupport
