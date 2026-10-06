#include "ReplicationStateCaptureInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Network {
    namespace {
        /** @brief Constructs each field's sole variant alternative and reserves container backing once. */
        ReplicationRuntimeValue PrepareValue(const ReplicationValueKind kind, const std::size_t maximumElements) {
            using enum ReplicationValueKind;
            switch (kind) {
                case Boolean:
                    return false;
                case SignedInteger:
                    return std::int64_t{};
                case UnsignedInteger:
                    return std::uint64_t{};
                case FloatingPoint:
                    return double{};
                case Utf8Text: {
                    std::string value;
                    value.reserve(maximumElements);
                    return value;
                }
                case ByteSequence: {
                    std::vector<std::byte> value;
                    value.reserve(maximumElements);
                    return value;
                }
                case Count:
                    return false;
            }
            return false;
        }

        /** @brief Rejects a zero, unbounded or internally starving setup before owner reads can open. */
        bool ValidLimits(const ReplicationCaptureLimits &limits, const std::size_t targetCount) noexcept {
            return limits.maximumTargets > 0 && limits.maximumTargets <= 4096 && targetCount > 0 && targetCount <= limits.maximumTargets &&
                   limits.maximumFieldsPerTarget > 0 && limits.maximumFieldsPerTarget <= 1024 && limits.snapshotSlotsPerTarget >= 2 &&
                   limits.snapshotSlotsPerTarget <= 64 && limits.maximumPreparedBytes > 0 && limits.maximumTargetsPerTick > 0 &&
                   limits.maximumFieldsPerTick > 0 && limits.maximumBytesPerTick > 0;
        }
    }  // namespace

    /** @brief Validates a complete owner binding and prepares every immutable slot before publication. */
    Result<void> ReplicationStateCapture::Impl::PrepareTarget(const ReplicationWorldCaptureRead &read,
                                                              const ReplicationCaptureTarget &binding, std::size_t &preparedBytes) {
        if (const auto mapped = read.Resolve(binding.object.object);
            !binding.source || mapped.HasError() || mapped.Value() != binding.object)
            return Result<void>::Failure(MakeError(ReplicationCaptureErrors::Stale));
        const auto schema = serializers->Schemas()->Find(binding.object.provenance.schema);
        if (schema.HasError())
            return Result<void>::Failure(schema.ErrorValue());
        if (schema.Value()->version != binding.object.provenance.schemaVersion || schema.Value()->fields.empty() ||
            schema.Value()->fields.size() > limits.maximumFieldsPerTarget || schema.Value()->fields.size() > limits.maximumFieldsPerTick)
            return Result<void>::Failure(MakeError(ReplicationCaptureErrors::Capacity));
        Target target{.binding = binding};
        if (const auto fields = PrepareFields(target, *schema.Value()); fields.HasError())
            return fields;
        if (const auto slots = PrepareSlots(target, preparedBytes); slots.HasError())
            return slots;
        targets.push_back(std::move(target));
        return Result<void>::Success();
    }

    /** @brief Prepares exact typed field envelopes and rejects targets exceeding one tick's byte budget. */
    Result<void> ReplicationStateCapture::Impl::PrepareFields(Target &target, const ReplicationSchemaDescriptor &schema) const {
        target.fields.reserve(schema.fields.size());
        for (const auto &field : schema.fields) {
            const auto metadata = serializers->DescriptorFor(schema.id, field.id);
            if (metadata.HasError())
                return Result<void>::Failure(metadata.ErrorValue());
            const auto kind = metadata.Value()->valueKind;
            const bool container = kind == ReplicationValueKind::Utf8Text || kind == ReplicationValueKind::ByteSequence;
            const std::size_t elements =
                container ? std::min<std::size_t>(field.limits.maximumElementCount, field.limits.maximumEncodedBytes) : 1;
            const std::size_t byteBound = container ? elements : sizeof(std::uint64_t);
            if (byteBound > limits.maximumBytesPerTick - target.byteBound)
                return Result<void>::Failure(MakeError(ReplicationCaptureErrors::Capacity));
            target.byteBound += byteBound;
            target.fields.emplace_back(field.id, kind, elements, false);
        }
        return Result<void>::Success();
    }

    /** @brief Allocates all immutable slots transactionally inside the finite preparation budget. */
    Result<void> ReplicationStateCapture::Impl::PrepareSlots(Target &target, std::size_t &preparedBytes) const {
        const std::size_t slotBytes =
            target.byteBound + target.fields.size() * sizeof(ReplicationCapturedField) + sizeof(ReplicationCapturedState);
        if (slotBytes > (limits.maximumPreparedBytes - preparedBytes) / limits.snapshotSlotsPerTarget)
            return Result<void>::Failure(MakeError(ReplicationCaptureErrors::Capacity));
        preparedBytes += slotBytes * limits.snapshotSlotsPerTarget;
        target.pool.reserve(limits.snapshotSlotsPerTarget);
        for (std::size_t slot{}; slot < limits.snapshotSlotsPerTarget; ++slot) {
            auto state = std::make_shared<ReplicationCapturedState>();
            state->descriptors_ = serializers->Schemas();
            state->admission_ = admission;
            state->fields_.reserve(target.fields.size());
            for (const auto &field : target.fields)
                state->fields_.emplace_back(field.field, PrepareValue(field.kind, field.maximumElements));
            target.pool.push_back(std::move(state));
        }
        return Result<void>::Success();
    }

    /** @copydoc ReplicationStateCapture::Prepare */
    Result<std::unique_ptr<ReplicationStateCapture>> ReplicationStateCapture::Prepare(
        const ReplicationWorldCaptureRead &world, std::shared_ptr<const ReplicationSerializerRegistry> serializers,
        const std::span<const ReplicationCaptureTarget> targets, const ReplicationCaptureLimits &limits) {
        if (!world.IsCurrent() || !serializers || !serializers->Schemas() || !ValidLimits(limits, targets.size()))
            return Result<std::unique_ptr<ReplicationStateCapture>>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
        try {
            auto impl = std::make_unique<Impl>();
            impl->world = world.Descriptor();
            impl->limits = limits;
            impl->serializers = std::move(serializers);
            impl->targets.reserve(targets.size());
            std::size_t preparedBytes{};
            for (const auto &target : targets) {
                if (const auto prepared = impl->PrepareTarget(world, target, preparedBytes); prepared.HasError())
                    return Result<std::unique_ptr<ReplicationStateCapture>>::Failure(prepared.ErrorValue());
            }
            std::ranges::sort(impl->targets, {}, [](const Impl::Target &target) {
                return target.binding.object.object;
            });
            impl->hints.resize(targets.size());
            for (std::size_t index = 1; index < impl->targets.size(); ++index)
                if (impl->targets[index - 1].binding.object.object == impl->targets[index].binding.object.object)
                    return Result<std::unique_ptr<ReplicationStateCapture>>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
            return Result<std::unique_ptr<ReplicationStateCapture>>::Success(
                std::make_unique<ReplicationStateCapture>(ConstructionKey{}, std::move(impl)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<ReplicationStateCapture>>::Failure(MakeError(ReplicationCaptureErrors::Capacity));
        }
    }
}  // namespace Horo::Network
