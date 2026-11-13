#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "PrefabPersistentCodecInternal.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Prefab {
    namespace {
        /** @brief Closed bounded data-only reader; it retains the original first cause and publishes no partial recipe. */
        struct Reader final {
            Runtime::CanonicalValueReader reader;
            const PrefabPersistenceLimits &limits;
            const PrefabLimitProfile &profile;
            std::size_t inputBytes{};
            std::optional<Error> failure;
            std::size_t members{};

            /** @brief Retains one canonical scalar/identity failure without replacing it with a later shape error. */
            template <typename T> T Read(Result<T> value) {
                if (value.HasError()) {
                    if (!failure)
                        failure = value.ErrorValue();
                    return {};
                }
                return std::move(value).Value();
            }

            /** @brief Records a structural failure at the exact closed Save record boundary. */
            void Reject(const ErrorCodeDescriptor &descriptor, std::string message) {
                if (!failure)
                    failure = MakeError(descriptor, std::move(message));
            }

            /** @brief Bounds all encoded collection counts before allocating or iterating. */
            std::uint32_t Count(std::size_t maximum, std::size_t minimumWireBytes = 1) {
                if (failure)
                    return 0;
                const auto count = Read(reader.ReadUInt32());
                if (count > maximum || reader.ByteOffset() > inputBytes || count > (inputBytes - reader.ByteOffset()) / minimumWireBytes) {
                    Reject(Runtime::SaveErrors::CanonicalCodecLimitExceeded, "Dynamic prefab record count exceeds its admission bound.");
                    return 0;
                }
                return count;
            }

            /** @brief Requires exact canonical identity byte width before constructing an existing strong Save identity. */
            template <typename Identity> Identity IdentityValue() {
                const auto bytes = Read(reader.ReadBytes(16));
                if (bytes.size() != 16) {
                    Reject(Runtime::SaveErrors::CanonicalCodecCorrupt, "Dynamic prefab identity has the wrong byte width.");
                    return {};
                }
                Runtime::SaveIdentityDetail::Bytes value{};
                for (std::size_t index = 0; index < value.size(); ++index)
                    value[index] = std::to_integer<std::uint8_t>(bytes[index]);
                return Read(Identity::FromBytes(value));
            }

            /** @brief Preserves source AssetId bytes directly; neither source paths nor runtime slots are accepted. */
            Assets::AssetId Asset() {
                const auto bytes = Read(reader.ReadBytes(16));
                if (bytes.size() != 16) {
                    Reject(Runtime::SaveErrors::CanonicalCodecCorrupt, "Dynamic prefab asset identity has the wrong byte width.");
                    return {};
                }
                std::array<std::uint8_t, 16> value{};
                for (std::size_t index = 0; index < value.size(); ++index)
                    value[index] = std::to_integer<std::uint8_t>(bytes[index]);
                return Assets::AssetId::FromBytes(value);
            }

            /** @brief Exact digest width prevents prefix/fallback identity admission. */
            Sha256Digest Digest() {
                const auto bytes = Read(reader.ReadBytes(32));
                Sha256Digest value{};
                if (bytes.size() != value.bytes.size()) {
                    Reject(Runtime::SaveErrors::CanonicalCodecCorrupt, "Dynamic prefab digest has the wrong byte width.");
                    return value;
                }
                for (std::size_t index = 0; index < value.bytes.size(); ++index)
                    value.bytes[index] = std::to_integer<std::uint8_t>(bytes[index]);
                return value;
            }

            /** @brief Reconstructs a relationship only within this exact record's world scope. */
            PrefabPersistentEntityReference Reference(Runtime::SaveWorldId world) {
                const auto entity = IdentityValue<Runtime::PersistentEntityId>();
                const auto generation = Read(reader.ReadUInt64());
                return {{world, entity}, {generation}};
            }

            /** @brief Reads only the explicitly tagged initialization algebra declared by the existing cooked format. */
            Gameplay::BehaviorFieldValue InitializationValue() {
                switch (Read(reader.ReadUInt8())) {
                    case 1:
                        return Read(reader.ReadBool());
                    case 2:
                        return Read(reader.ReadInt64());
                    case 3:
                        return Read(reader.ReadFloat64());
                    case 4:
                        return Read(reader.ReadUtf8());
                    case 5:
                        return Read(reader.ReadVec2());
                    case 6:
                        return Read(reader.ReadVec3());
                    case 7:
                        return Read(reader.ReadQuaternion());
                    default:
                        Reject(Runtime::SaveErrors::CanonicalCodecCorrupt, "Dynamic prefab initialization kind is unknown.");
                        return {};
                }
            }

            /** @brief Reads exact 64-bit interface identities and finite owned values under actual per-template count limits. */
            void Initializations(PrefabPersistentInstance &instance) {
                const auto count = Count(MaximumPrefabInitializationValues, 9);
                instance.initialization.reserve(count);
                for (std::uint32_t index = 0; index < count; ++index) {
                    const auto id = Read(reader.ReadUInt64());
                    auto value = InitializationValue();
                    if (failure)
                        return;
                    instance.initialization.push_back({{id}, std::move(value)});
                }
            }

            /** @brief Reads separate external relationship interfaces without interpreting a behavior's memory. */
            void Bindings(PrefabPersistentInstance &instance, Runtime::SaveWorldId world) {
                const auto count = Count(profile.Policy().maximumInstanceBindings, 32);
                instance.bindings.reserve(count);
                for (std::uint32_t index = 0; index < count; ++index) {
                    const auto id = Read(PrefabPropertyId::Create(Read(reader.ReadUInt64())));
                    auto target = Reference(world);
                    if (failure)
                        return;
                    instance.bindings.push_back({id, std::move(target)});
                }
            }

            /** @brief Reuses canonical stable-address construction; decoded arrival positions issue no identity. */
            PrefabObjectAddress Address() {
                const auto count = Count(MaximumPrefabObjectScopeDepth, 4);
                std::vector<LocalObjectId> scope;
                scope.reserve(count);
                for (std::uint32_t index = 0; index < count; ++index)
                    scope.push_back({Read(reader.ReadUInt32())});
                const LocalObjectId object{Read(reader.ReadUInt32())};
                return Read(PrefabObjectAddress::Create(scope, object));
            }

            /** @brief Reads per-entity source/runtime occurrence mappings; no native identity is allocated from record order. */
            std::vector<PrefabPersistentBehaviorIdentity> Behaviors() {
                const auto count = Count(profile.Policy().maximumComponentsPerObject, 20);
                std::vector<PrefabPersistentBehaviorIdentity> behaviors;
                behaviors.reserve(count);
                for (std::uint32_t index = 0; index < count; ++index) {
                    auto type = Read(Gameplay::BehaviorTypeId::Parse(Read(reader.ReadUtf8())));
                    const auto source = Read(reader.ReadUInt64());
                    const auto runtime = Read(reader.ReadUInt64());
                    const auto schema = Read(reader.ReadUInt32());
                    if (failure)
                        return {};
                    behaviors.push_back({std::move(type), {source}, {runtime}, schema});
                }
                return behaviors;
            }

            /** @brief Reads saved member identity/provenance; executable content admission is deliberately deferred to the real provider.
             */
            PrefabPersistentMember Member(const PrefabPersistentInstance &instance) {
                const auto identity = IdentityValue<Runtime::PersistentEntityId>();
                const auto generation = Read(reader.ReadUInt64());
                const auto disposition = static_cast<Runtime::PersistentEntityDisposition>(Read(reader.ReadUInt8()));
                const auto asset = Asset();
                auto address = Address();
                const auto digest = Digest();
                const auto definition = Read(Runtime::SaveAssetId::FromBytes(instance.source.asset.Bytes()));
                return {{identity, {generation}, Runtime::SpawnedPersistentEntityProvenance{definition, instance.identity}, disposition},
                        {asset, std::move(address), digest},
                        Behaviors()};
            }

            /** @brief Charges the aggregate member count before allocating per-instance storage. */
            void Members(PrefabPersistentInstance &instance) {
                const auto count = Count(std::min(limits.maximumMembers - members, profile.Policy().maximumObjectCount), 81);
                instance.members.reserve(count);
                members += count;
                for (std::uint32_t index = 0; index < count; ++index) {
                    auto member = Member(instance);
                    if (failure)
                        return;
                    instance.members.push_back(std::move(member));
                }
            }

            /** @brief Reads the exact genuine root revision without substituting the current engine/project version. */
            void Source(PrefabPersistentInstance &instance) {
                instance.identity = IdentityValue<Runtime::SavePrefabInstanceId>();
                instance.creator = Read(reader.ReadUtf8());
                instance.source.asset = Asset();
                instance.source.artifactDigest = Digest();
                instance.source.target = Read(AssetCookTargetId::Parse(Read(reader.ReadUtf8())));
                const auto version = Read(reader.ReadUtf8());
                instance.rootSourceRevision.projectVersion = Read(Application::ParseHoroVersion(version));
                if (!failure && Application::FormatHoroVersion(instance.rootSourceRevision.projectVersion) != version)
                    Reject(Runtime::SaveErrors::CanonicalCodecCorrupt, "Dynamic prefab source version is not canonically spelled.");
                instance.rootSourceRevision.contentDigest = Digest();
                instance.disposition = static_cast<Runtime::PersistentEntityDisposition>(Read(reader.ReadUInt8()));
            }

            /** @brief Reads root placement once, followed by an optional exact durable parent. */
            void Placement(PrefabPersistentInstance &instance, Runtime::SaveWorldId world) {
                const auto parent = Read(reader.ReadBool());
                instance.placement.translation = Read(reader.ReadVec3());
                instance.placement.rotation = Read(reader.ReadQuaternion());
                instance.placement.scale = Read(reader.ReadVec3());
                if (parent)
                    instance.parent = Reference(world);
            }

            /** @brief Delegates the typed override blob to the ONE existing canonical prefab delta parser. */
            void Overrides(PrefabPersistentInstance &instance) {
                const auto bytes = Read(reader.ReadBytes(limits.maximumBytes));
                const std::string_view json{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
                instance.overrides = Read(ParsePrefabValueOverrides(json, profile));
            }

            /** @brief Reads only bounded real creation ancestry; the same semantic validator rejects duplicate/cyclic policy evidence. */
            void Lineage(PrefabPersistentInstance &instance) {
                const auto count = Count(profile.Policy().maximumRuntimeSpawnDepth, 16);
                instance.creationLineage.reserve(count);
                for (std::uint32_t index = 0; index < count; ++index)
                    instance.creationLineage.push_back(Asset());
            }

            /** @brief Reads one complete data-only recipe in its declared fixed field order. */
            PrefabPersistentInstance Instance(Runtime::SaveWorldId world) {
                PrefabPersistentInstance instance;
                Source(instance);
                if (failure)
                    return instance;
                Placement(instance, world);
                Lineage(instance);
                Initializations(instance);
                Bindings(instance, world);
                Overrides(instance);
                Members(instance);
                return instance;
            }
        };

        /** @brief Requires the sole canonical encoder to reproduce every input byte, including stable record ordering. */
        Result<void> CanonicalBytes(std::span<const std::byte> bytes, const PrefabPersistentState &state,
                                    const PrefabPersistenceLimits &limits, const PrefabLimitProfile &profile) {
            auto encoded = EncodePrefabPersistentState(state, limits, profile);
            if (encoded.HasError())
                return Result<void>::Failure(encoded.ErrorValue());
            if (!std::ranges::equal(bytes, encoded.Value().Bytes()))
                return Result<void>::Failure(
                    MakeError(Runtime::SaveErrors::CanonicalCodecCorrupt,
                              "Dynamic prefab records must use canonical stable-identity order and scalar spelling."));
            return Result<void>::Success();
        }

        /** @brief Requires complete canonical bytes and the same whole-recipe semantic validation. */
        Result<void> FinishDecoded(Reader &reader, std::span<const std::byte> bytes, const PrefabPersistentState &state,
                                   const PrefabPersistenceLimits &limits, const PrefabLimitProfile &profile) {
            if (auto complete = reader.reader.RequireFinished(); complete.HasError())
                return Result<void>::Failure(complete.ErrorValue());
            if (auto valid = ValidatePrefabPersistentState(state, limits, profile); valid.HasError())
                return Result<void>::Failure(valid.ErrorValue());
            if (auto canonical = CanonicalBytes(bytes, state, limits, profile); canonical.HasError())
                return Result<void>::Failure(canonical.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Complete metadata is validated before it can be returned to a loader or unpublished Scene adapter. */
        Result<PrefabPersistentState> Decode(std::span<const std::byte> bytes, const PrefabPersistenceLimits &limits,
                                             const PrefabLimitProfile &profile) {
            using Outcome = Result<PrefabPersistentState>;
            if (!IsValidPrefabPersistenceLimits(limits))
                return Outcome::Failure(MakeError(Runtime::SaveErrors::CanonicalCodecConfigurationInvalid));
            auto input = Runtime::CanonicalValueReader::Create(bytes, Detail::PersistentCodecLimits(limits));
            if (input.HasError())
                return Outcome::Failure(input.ErrorValue());
            Reader reader{std::move(input).Value(), limits, profile, bytes.size()};
            const auto version = reader.Read(reader.reader.ReadUInt32());
            if (reader.failure)
                return Outcome::Failure(*reader.failure);
            if (version != 1)
                return Outcome::Failure(
                    MakeError(Runtime::SaveErrors::CanonicalCodecCorrupt, "Dynamic prefab record version is unsupported."));
            PrefabPersistentState state{reader.IdentityValue<Runtime::SaveWorldId>(), {}};
            const auto count = reader.Count(limits.maximumInstances);
            state.instances.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                auto instance = reader.Instance(state.world);
                if (reader.failure)
                    return Outcome::Failure(*reader.failure);
                state.instances.push_back(std::move(instance));
            }
            if (reader.failure)
                return Outcome::Failure(*reader.failure);
            if (auto valid = FinishDecoded(reader, bytes, state, limits, profile); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            return Outcome::Success(std::move(state));
        }
    }  // namespace

    /** @copydoc DecodePrefabPersistentState */
    Result<PrefabPersistentState> DecodePrefabPersistentState(std::span<const std::byte> bytes, const PrefabPersistenceLimits &limits,
                                                              const PrefabLimitProfile &profile) {
        try {
            return Decode(bytes, limits, profile);
        } catch (const std::bad_alloc &) {
            return Result<PrefabPersistentState>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }
}  // namespace Horo::Prefab
