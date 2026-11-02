#include "BehaviorRuntimeRestoreInternal.h"

#include <algorithm>
#include <array>
#include <set>

namespace Horo::Gameplay {
    namespace {
        using namespace Runtime;
        constexpr std::size_t MaximumEntries = 16'384;

        bool Less(const BehaviorLifecycleEntry &a, const BehaviorLifecycleEntry &b) {
            return std::tie(a.entity, a.instance.value, a.type) < std::tie(b.entity, b.instance.value, b.type);
        }

        Result<void> Validate(const BehaviorLifecycleSnapshot &snapshot) {
            if (snapshot.instances.size() > MaximumEntries)
                return Result<void>::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
            std::set<std::pair<PersistentEntityId, std::uint64_t>> ids;
            const BehaviorLifecycleEntry *prior{};
            for (const auto &value : snapshot.instances) {
                const std::array valid{value.entity.IsValid(), value.generation.IsValid(), value.instance.IsValid(), value.type.IsValid(),
                                       ValidLifecycleFlags(value.flags)};
                if (!std::ranges::all_of(valid, [](bool v) {
                    return v;
                }))
                    return Result<void>::Failure(BehaviorRestoreError("Invalid lifecycle identity."));
                if (!ids.emplace(value.entity, value.instance.value).second || (prior && !Less(*prior, value)))
                    return Result<void>::Failure(BehaviorRestoreError("Duplicate or noncanonical lifecycle identity."));
                prior = &value;
            }
            return Result<void>::Success();
        }

        template <class Write> Result<CanonicalEncodedValue> Scalar(const CanonicalCodecLimits &limits, Write write) {
            CanonicalValueWriter writer{limits};
            if (auto value = write(writer); value.HasError())
                return Result<CanonicalEncodedValue>::Failure(value.ErrorValue());
            return std::move(writer).Finalize();
        }

        Result<CanonicalEncodedValue> Entry(const BehaviorLifecycleEntry &v, const CanonicalCodecLimits &limits) {
            std::array values{Scalar(limits,
                                     [&](auto &w) {
                return w.WriteBytes(std::as_bytes(std::span{v.entity.Bytes()}));
            }),
                              Scalar(limits,
                                     [&](auto &w) {
                return w.WriteUInt64(v.generation.value);
            }),
                              Scalar(limits,
                                     [&](auto &w) {
                return w.WriteUInt64(v.instance.value);
            }),
                              Scalar(limits,
                                     [&](auto &w) {
                return w.WriteUtf8(v.type.Value());
            }),
                              Scalar(limits,
                                     [&](auto &w) {
                return w.WriteBool(v.flags.created);
            }),
                              Scalar(limits,
                                     [&](auto &w) {
                return w.WriteBool(v.flags.enabledCallbackActive);
            }),
                              Scalar(limits,
                                     [&](auto &w) {
                return w.WriteBool(v.flags.started);
            }),
                              Scalar(limits, [&](auto &w) {
                return w.WriteBool(v.flags.enabled);
            })};
            std::vector<CanonicalRecordField> fields;
            fields.reserve(values.size());
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (values[i].HasError())
                    return Result<CanonicalEncodedValue>::Failure(values[i].ErrorValue());
                fields.push_back({CanonicalFieldId::Create(static_cast<std::uint32_t>(i + 1)).Value(), std::move(values[i]).Value()});
            }
            CanonicalValueWriter writer{limits};
            if (auto record = writer.WriteRecord(fields); record.HasError())
                return Result<CanonicalEncodedValue>::Failure(record.ErrorValue());
            return std::move(writer).Finalize();
        }

        template <class T, class Read> Result<T> Field(const CanonicalDecodedValue &field, Read read) {
            auto opened = field.OpenReader();
            if (opened.HasError())
                return Result<T>::Failure(opened.ErrorValue());
            auto reader = std::move(opened).Value();
            auto value = read(reader);
            if (value.HasError())
                return Result<T>::Failure(value.ErrorValue());
            if (auto end = reader.RequireFinished(); end.HasError())
                return Result<T>::Failure(end.ErrorValue());
            return value;
        }

        Result<PersistentEntityId> Identity(CanonicalValueReader &reader) {
            auto bytes = reader.ReadBytes(SaveIdentityDetail::Bytes{}.size());
            if (bytes.HasError())
                return Result<PersistentEntityId>::Failure(bytes.ErrorValue());
            SaveIdentityDetail::Bytes actual{};
            if (bytes.Value().size() != actual.size())
                return Result<PersistentEntityId>::Failure(BehaviorRestoreError("Invalid lifecycle entity bytes."));
            std::ranges::transform(bytes.Value(), actual.begin(), [](std::byte v) {
                return std::to_integer<std::uint8_t>(v);
            });
            return PersistentEntityId::FromBytes(actual);
        }

        Result<BehaviorLifecycleEntry> IdentityFields(std::span<const CanonicalDecodedField> f) {
            auto entity = Field<PersistentEntityId>(f[0].value, Identity);
            if (entity.HasError())
                return Result<BehaviorLifecycleEntry>::Failure(entity.ErrorValue());
            auto generation = Field<std::uint64_t>(f[1].value, [](auto &r) {
                return r.ReadUInt64();
            });
            if (generation.HasError())
                return Result<BehaviorLifecycleEntry>::Failure(generation.ErrorValue());
            auto instance = Field<std::uint64_t>(f[2].value, [](auto &r) {
                return r.ReadUInt64();
            });
            if (instance.HasError())
                return Result<BehaviorLifecycleEntry>::Failure(instance.ErrorValue());
            auto type = Field<std::string>(f[3].value, [](auto &r) {
                return r.ReadUtf8();
            });
            if (type.HasError())
                return Result<BehaviorLifecycleEntry>::Failure(type.ErrorValue());
            auto parsed = BehaviorTypeId::Parse(type.Value());
            if (parsed.HasError())
                return Result<BehaviorLifecycleEntry>::Failure(parsed.ErrorValue());
            return Result<BehaviorLifecycleEntry>::Success(
                {entity.Value(), {generation.Value()}, {instance.Value()}, std::move(parsed).Value(), {}});
        }

        Result<BehaviorLifecycleFlags> FlagFields(std::span<const CanonicalDecodedField> f) {
            std::array<bool, 4> flags{};
            for (std::size_t i = 0; i < flags.size(); ++i) {
                auto value = Field<bool>(f[i + 4].value, [](auto &r) {
                    return r.ReadBool();
                });
                if (value.HasError())
                    return Result<BehaviorLifecycleFlags>::Failure(value.ErrorValue());
                flags[i] = value.Value();
            }
            return Result<BehaviorLifecycleFlags>::Success({flags[0], flags[1], flags[2], flags[3]});
        }

        Result<BehaviorLifecycleEntry> ReadEntry(const CanonicalDecodedValue &encoded) {
            auto opened = encoded.OpenReader();
            if (opened.HasError())
                return Result<BehaviorLifecycleEntry>::Failure(opened.ErrorValue());
            auto reader = std::move(opened).Value();
            auto record = reader.ReadRecord();
            if (record.HasError())
                return Result<BehaviorLifecycleEntry>::Failure(record.ErrorValue());
            if (record.Value().size() != 8)
                return Result<BehaviorLifecycleEntry>::Failure(BehaviorRestoreError("Lifecycle field count differs from schema1."));
            for (std::size_t i = 0; i < 8; ++i)
                if (record.Value()[i].id.Value() != i + 1)
                    return Result<BehaviorLifecycleEntry>::Failure(BehaviorRestoreError("Unknown lifecycle field."));
            auto identity = IdentityFields(record.Value());
            if (identity.HasError())
                return identity;
            auto flags = FlagFields(record.Value());
            if (flags.HasError())
                return Result<BehaviorLifecycleEntry>::Failure(flags.ErrorValue());
            if (auto finished = reader.RequireFinished(); finished.HasError())
                return Result<BehaviorLifecycleEntry>::Failure(finished.ErrorValue());
            auto result = std::move(identity).Value();
            result.flags = flags.Value();
            return Result<BehaviorLifecycleEntry>::Success(std::move(result));
        }
    }  // namespace

    Result<CanonicalEncodedValue> EncodeBehaviorLifecycle(const BehaviorLifecycleSnapshot &snapshot, CanonicalCodecLimits limits) {
        if (auto valid = Validate(snapshot); valid.HasError())
            return Result<CanonicalEncodedValue>::Failure(valid.ErrorValue());
        std::vector<CanonicalEncodedValue> values;
        values.reserve(snapshot.instances.size());
        for (const auto &entry : snapshot.instances) {
            auto value = Entry(entry, limits);
            if (value.HasError())
                return value;
            values.push_back(std::move(value).Value());
        }
        CanonicalValueWriter writer{limits};
        if (auto version = writer.WriteUInt32(1); version.HasError())
            return Result<CanonicalEncodedValue>::Failure(version.ErrorValue());
        if (auto sequence = writer.WriteSequence(values); sequence.HasError())
            return Result<CanonicalEncodedValue>::Failure(sequence.ErrorValue());
        return std::move(writer).Finalize();
    }

    Result<BehaviorLifecycleSnapshot> DecodeBehaviorLifecycle(std::span<const std::byte> bytes, CanonicalCodecLimits limits) {
        limits.maximumCollectionElements = std::min(limits.maximumCollectionElements, MaximumEntries);
        limits.maximumStringBytes = std::min(limits.maximumStringBytes, MaximumBehaviorTypeIdBytes);
        limits.maximumFields = std::min<std::size_t>(limits.maximumFields, 8);
        auto opened = CanonicalValueReader::Create(bytes, limits);
        if (opened.HasError())
            return Result<BehaviorLifecycleSnapshot>::Failure(opened.ErrorValue());
        auto reader = std::move(opened).Value();
        auto version = reader.ReadUInt32();
        if (version.HasError())
            return Result<BehaviorLifecycleSnapshot>::Failure(version.ErrorValue());
        if (version.Value() != 1)
            return Result<BehaviorLifecycleSnapshot>::Failure(BehaviorRestoreError("Unsupported Gameplay lifecycle schema."));
        auto values = reader.ReadSequence();
        if (values.HasError())
            return Result<BehaviorLifecycleSnapshot>::Failure(values.ErrorValue());
        if (values.Value().size() > MaximumEntries)
            return Result<BehaviorLifecycleSnapshot>::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
        BehaviorLifecycleSnapshot result;
        result.instances.reserve(values.Value().size());
        for (const auto &value : values.Value()) {
            auto entry = ReadEntry(value);
            if (entry.HasError())
                return Result<BehaviorLifecycleSnapshot>::Failure(entry.ErrorValue());
            result.instances.push_back(std::move(entry).Value());
        }
        if (auto end = reader.RequireFinished(); end.HasError())
            return Result<BehaviorLifecycleSnapshot>::Failure(end.ErrorValue());
        if (auto valid = Validate(result); valid.HasError())
            return Result<BehaviorLifecycleSnapshot>::Failure(valid.ErrorValue());
        return Result<BehaviorLifecycleSnapshot>::Success(std::move(result));
    }
}  // namespace Horo::Gameplay
