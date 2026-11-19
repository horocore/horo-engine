#include "HeadlessPrefabIntent.h"

#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Runtime/Save/SaveCanonicalCodec.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using Gameplay::BehaviorFieldValue;
        using Runtime::CanonicalValueReader;
        using Runtime::CanonicalValueWriter;
        constexpr std::uint16_t Version = 1;
        constexpr std::size_t MaximumInitializationStringBytes = 256;
        constexpr Runtime::CanonicalCodecLimits WireLimits{.maximumBytes = Network::MaximumNetworkPrefabIntentBytes,
                                                           .maximumDecodedBytes = Network::MaximumNetworkPrefabIntentBytes * 2,
                                                           .maximumStringBytes = Network::MaximumNetworkPrefabIntentBytes};
        enum class ReferenceKind : std::uint8_t {
            Authored = 1,
            Replicated = 2
        };

        template <typename T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected, "Invalid network Prefab spawn intent."));
        }

        /** @brief Writes a Scene-authored target or the live authority identity of a dynamic target. */
        Result<void> WriteReference(CanonicalValueWriter &writer, Runtime::EntityRef target, const Runtime::RuntimeSceneView &scene,
                                    const Network::NetworkAuthorityObjectPublication &authority) {
            const auto entity = scene.Get(target);
            if (entity.HasError())
                return Result<void>::Failure(entity.ErrorValue());
            if (entity.Value().authoredObject) {
                (void)writer.WriteUInt8(static_cast<std::uint8_t>(ReferenceKind::Authored));
                return writer.WriteUInt64(entity.Value().authoredObject->value);
            }
            const auto object = authority.Find(target);
            if (object.HasError())
                return Result<void>::Failure(object.ErrorValue());
            (void)writer.WriteUInt8(static_cast<std::uint8_t>(ReferenceKind::Replicated));
            (void)writer.WriteUInt64(object.Value().Epoch().Value());
            (void)writer.WriteUInt64(object.Value().Slot());
            return writer.WriteUInt32(object.Value().Generation());
        }

        /** @brief Writes the same exact closed value types as cooked Prefab initialization. */
        Result<void> WriteValue(CanonicalValueWriter &writer, const BehaviorFieldValue &value) {
            using Kind = Prefab::PrefabInitializationKind;
            const auto tag = [&](Kind kind) {
                (void)writer.WriteUInt8(static_cast<std::uint8_t>(kind));
            };
            return std::visit([&]<typename T>(const T &input) -> Result<void> {
                if constexpr (std::is_same_v<T, bool>) {
                    tag(Kind::Boolean);
                    return writer.WriteBool(input);
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    tag(Kind::Integer);
                    return writer.WriteInt64(input);
                } else if constexpr (std::is_same_v<T, double>) {
                    tag(Kind::Number);
                    return writer.WriteFloat64(input);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    tag(Kind::String);
                    return input.size() <= MaximumInitializationStringBytes ? writer.WriteUtf8(input) : Invalid<void>();
                } else if constexpr (std::is_same_v<T, Math::Vec2>) {
                    tag(Kind::Vec2);
                    return writer.WriteVec2(input);
                } else if constexpr (std::is_same_v<T, Math::Vec3>) {
                    tag(Kind::Vec3);
                    return writer.WriteVec3(input);
                } else if constexpr (std::is_same_v<T, Math::Quaternion>) {
                    tag(Kind::Quaternion);
                    return writer.WriteQuaternion(input);
                } else {
                    return Invalid<void>();
                }
            }, value);
        }

        /** @brief Orders supplied values by their template interface identity. */
        template <typename Entry, typename Identity> std::vector<const Entry *> Sorted(std::span<const Entry> values, Identity identity) {
            std::vector<const Entry *> ordered;
            ordered.reserve(values.size());
            for (const auto &value : values)
                ordered.push_back(&value);
            std::ranges::sort(ordered, {}, [&](const Entry *entry) {
                return identity(*entry);
            });
            return ordered;
        }

        /** @brief Emits unique, sorted typed initialization and binding identities. */
        Result<void> WriteInputs(CanonicalValueWriter &writer, const Prefab::PrefabSpawnRequest &request,
                                 const Runtime::RuntimeSceneView &scene, const Network::NetworkAuthorityObjectPublication &authority) {
            (void)writer.WriteUInt16(static_cast<std::uint16_t>(request.initialization.size()));
            std::uint64_t prior{};
            for (const auto *entry : Sorted(std::span{request.initialization}, [](const auto &value) {
                return value.id.value;
            })) {
                if (entry->id.value <= prior)
                    return Invalid<void>();
                prior = entry->id.value;
                (void)writer.WriteUInt64(prior);
                if (auto value = WriteValue(writer, entry->value); value.HasError())
                    return value;
            }
            (void)writer.WriteUInt16(static_cast<std::uint16_t>(request.bindings.size()));
            prior = 0;
            for (const auto *entry : Sorted(std::span{request.bindings}, [](const auto &binding) {
                return binding.id.Value();
            })) {
                if (entry->id.Value() <= prior)
                    return Invalid<void>();
                prior = entry->id.Value();
                (void)writer.WriteUInt64(prior);
                if (auto target = WriteReference(writer, entry->target, scene, authority); target.HasError())
                    return target;
            }
            return Result<void>::Success();
        }

        template <typename T> Result<BehaviorFieldValue> AsValue(Result<T> value) {
            return value.HasError() ? Result<BehaviorFieldValue>::Failure(value.ErrorValue())
                                    : Result<BehaviorFieldValue>::Success(std::move(value).Value());
        }

        /** @brief Enforces the cooked Prefab string limit after decoding. */
        Result<BehaviorFieldValue> ReadInitializationString(CanonicalValueReader &reader) {
            auto text = reader.ReadUtf8();
            if (text.HasValue() && text.Value().size() > MaximumInitializationStringBytes)
                return Invalid<BehaviorFieldValue>();
            return AsValue(std::move(text));
        }

        /** @brief Reads only the approved initialization types, including per-string bounds. */
        Result<BehaviorFieldValue> ReadValue(CanonicalValueReader &reader) {
            const auto kind = reader.ReadUInt8();
            if (kind.HasError())
                return Result<BehaviorFieldValue>::Failure(kind.ErrorValue());
            switch (static_cast<Prefab::PrefabInitializationKind>(kind.Value())) {
                case Prefab::PrefabInitializationKind::Boolean:
                    return AsValue(reader.ReadBool());
                case Prefab::PrefabInitializationKind::Integer:
                    return AsValue(reader.ReadInt64());
                case Prefab::PrefabInitializationKind::Number:
                    return AsValue(reader.ReadFloat64());
                case Prefab::PrefabInitializationKind::String:
                    return ReadInitializationString(reader);
                case Prefab::PrefabInitializationKind::Vec2:
                    return AsValue(reader.ReadVec2());
                case Prefab::PrefabInitializationKind::Vec3:
                    return AsValue(reader.ReadVec3());
                case Prefab::PrefabInitializationKind::Quaternion:
                    return AsValue(reader.ReadQuaternion());
                default:
                    return Invalid<BehaviorFieldValue>();
            }
        }

        /** @brief Resolves authority evidence only within this exact local Scene and world session. */
        Result<Runtime::EntityRef> ReadReference(CanonicalValueReader &reader, Network::NetworkSessionGeneration session,
                                                 const Runtime::RuntimeSceneView &scene, const Network::NetworkClientPrefabInbox &inbox) {
            const auto kind = reader.ReadUInt8();
            const auto identity = reader.ReadUInt64();
            if (kind.HasError() || identity.HasError())
                return Invalid<Runtime::EntityRef>();
            if (kind.Value() == static_cast<std::uint8_t>(ReferenceKind::Authored)) {
                const auto entity = scene.Find({identity.Value()});
                return entity ? Result<Runtime::EntityRef>::Success(*entity)
                              : Result<Runtime::EntityRef>::Failure(MakeError(Prefab::PrefabErrors::ReferenceRewriteInvalid));
            }
            if (kind.Value() != static_cast<std::uint8_t>(ReferenceKind::Replicated))
                return Invalid<Runtime::EntityRef>();
            const auto slot = reader.ReadUInt64();
            const auto generation = reader.ReadUInt32();
            const auto epoch = Network::ReplicationAuthorityEpoch::Create(identity.Value());
            if (slot.HasError() || generation.HasError() || epoch.HasError())
                return Invalid<Runtime::EntityRef>();
            const auto object = Network::NetworkObjectId::Create(epoch.Value(), slot.Value(), generation.Value());
            if (object.HasError())
                return Result<Runtime::EntityRef>::Failure(object.ErrorValue());
            return inbox.Resolve(session, object.Value());
        }

        /** @brief Reads a bounded array of exact ascending interface IDs and their typed values. */
        template <typename Entry, typename ReadEntry>
        Result<void> ReadInputs(CanonicalValueReader &reader, std::vector<Entry> &entries, std::size_t maximum, ReadEntry readEntry) {
            const auto count = reader.ReadUInt16();
            if (count.HasError() || count.Value() > maximum)
                return Invalid<void>();
            entries.reserve(count.Value());
            std::uint64_t prior{};
            for (std::uint16_t index = 0; index < count.Value(); ++index) {
                const auto identity = reader.ReadUInt64();
                if (identity.HasError() || identity.Value() <= prior)
                    return Invalid<void>();
                auto value = readEntry(identity.Value());
                if (value.HasError())
                    return Result<void>::Failure(value.ErrorValue());
                prior = identity.Value();
                entries.push_back(std::move(value).Value());
            }
            return Result<void>::Success();
        }

        /** @brief Writes the optional canonical override document. */
        Result<void> WriteOverrides(CanonicalValueWriter &writer, const Prefab::PrefabSpawnRequest &request,
                                    const Prefab::PrefabLimitProfile &limits) {
            (void)writer.WriteBool(request.overrides.has_value());
            if (!request.overrides)
                return Result<void>::Success();
            auto serialized = Prefab::SerializePrefabValueOverrides(*request.overrides, limits);
            if (serialized.HasError())
                return Result<void>::Failure(serialized.ErrorValue());
            return writer.WriteUtf8(serialized.Value());
        }

        /** @brief Reads the versioned placement prefix and validates the resulting transform. */
        Result<Math::Transform> ReadPlacement(CanonicalValueReader &reader) {
            const auto version = reader.ReadUInt16();
            if (version.HasError() || version.Value() != Version)
                return Invalid<Math::Transform>();
            const auto translation = reader.ReadVec3();
            const auto rotation = reader.ReadQuaternion();
            const auto scale = reader.ReadVec3();
            if (translation.HasError() || rotation.HasError() || scale.HasError())
                return Invalid<Math::Transform>();
            Math::Transform placement{.translation = translation.Value(), .rotation = rotation.Value(), .scale = scale.Value()};
            if (placement.TryToMatrix().HasError())
                return Invalid<Math::Transform>();
            return Result<Math::Transform>::Success(placement);
        }

        /** @brief Reads only the initialization fields declared by this wire format. */
        Result<void> ReadInitialization(CanonicalValueReader &reader, Prefab::PrefabSpawnRequest &request) {
            return ReadInputs(reader, request.initialization, Prefab::MaximumPrefabInitializationValues,
                              [&](std::uint64_t identity) -> Result<Prefab::PrefabInitializationValue> {
                auto value = ReadValue(reader);
                if (value.HasError())
                    return Result<Prefab::PrefabInitializationValue>::Failure(value.ErrorValue());
                return Result<Prefab::PrefabInitializationValue>::Success({{identity}, std::move(value).Value()});
            });
        }

        /** @brief Reads explicit optional override data through the cooked override contract. */
        Result<void> ReadOverrides(CanonicalValueReader &reader, Prefab::PrefabSpawnRequest &request,
                                   const Prefab::PrefabLimitProfile &limits) {
            const auto present = reader.ReadBool();
            if (present.HasError())
                return Result<void>::Failure(present.ErrorValue());
            if (!present.Value())
                return Result<void>::Success();
            auto serialized = reader.ReadUtf8();
            if (serialized.HasError())
                return Result<void>::Failure(serialized.ErrorValue());
            auto overrides = Prefab::ParsePrefabValueOverrides(serialized.Value(), limits);
            if (overrides.HasError())
                return Result<void>::Failure(overrides.ErrorValue());
            request.overrides.emplace(std::move(overrides).Value());
            return Result<void>::Success();
        }

        /** @brief Reads and resolves the optional parent using the same relationship rules as bindings. */
        Result<void> ReadParent(CanonicalValueReader &reader, Prefab::PrefabSpawnRequest &request,
                                Network::NetworkSessionGeneration session, const Runtime::RuntimeSceneView &scene,
                                const Network::NetworkClientPrefabInbox &inbox) {
            const auto present = reader.ReadBool();
            if (present.HasError())
                return Result<void>::Failure(present.ErrorValue());
            if (!present.Value())
                return Result<void>::Success();
            const auto parent = ReadReference(reader, session, scene, inbox);
            if (parent.HasError())
                return Result<void>::Failure(parent.ErrorValue());
            request.parent = parent.Value();
            return Result<void>::Success();
        }

        /** @brief Resolves external bindings after their exact interface IDs pass canonical-order validation. */
        Result<void> ReadBindings(CanonicalValueReader &reader, Prefab::PrefabSpawnRequest &request, std::size_t maximum,
                                  Network::NetworkSessionGeneration session, const Runtime::RuntimeSceneView &scene,
                                  const Network::NetworkClientPrefabInbox &inbox) {
            return ReadInputs(reader, request.bindings, maximum, [&](std::uint64_t identity) -> Result<Prefab::PrefabRuntimeBinding> {
                const auto id = Prefab::PrefabPropertyId::Create(identity);
                if (id.HasError())
                    return Result<Prefab::PrefabRuntimeBinding>::Failure(id.ErrorValue());
                const auto entity = ReadReference(reader, session, scene, inbox);
                if (entity.HasError())
                    return Result<Prefab::PrefabRuntimeBinding>::Failure(entity.ErrorValue());
                return Result<Prefab::PrefabRuntimeBinding>::Success({id.Value(), entity.Value()});
            });
        }

        /** @brief Reads the complete request body before any Scene operation can be queued. */
        Result<void> ReadSpawnRequest(CanonicalValueReader &reader, Prefab::PrefabSpawnRequest &request,
                                      const Prefab::PrefabLimitProfile &limits, Network::NetworkSessionGeneration session,
                                      const Runtime::RuntimeSceneView &scene, const Network::NetworkClientPrefabInbox &inbox) {
            auto placement = ReadPlacement(reader);
            if (placement.HasError())
                return Result<void>::Failure(placement.ErrorValue());
            request.placement = placement.Value();
            if (auto parent = ReadParent(reader, request, session, scene, inbox); parent.HasError())
                return parent;
            if (auto initialization = ReadInitialization(reader, request); initialization.HasError())
                return initialization;
            if (auto bindings = ReadBindings(reader, request, limits.Policy().maximumInstanceBindings, session, scene, inbox);
                bindings.HasError())
                return bindings;
            if (auto overrides = ReadOverrides(reader, request, limits); overrides.HasError())
                return overrides;
            return reader.RequireFinished();
        }

        /** @brief Checks request limits before narrowing counts or writing any bytes. */
        bool FitsIntent(const Prefab::PrefabSpawnRequest &request, const Prefab::PrefabLimitProfile &limits) {
            return request.placement.TryToMatrix().HasValue() &&
                   request.initialization.size() <= Prefab::MaximumPrefabInitializationValues &&
                   request.bindings.size() <= limits.Policy().maximumInstanceBindings &&
                   std::in_range<std::uint16_t>(request.bindings.size());
        }
    }  // namespace

    /** @copydoc EncodeHeadlessPrefabIntent */
    Result<std::vector<std::byte>> EncodeHeadlessPrefabIntent(const Prefab::PrefabSpawnRequest &request,
                                                              const Prefab::PrefabLimitProfile &limits,
                                                              const Runtime::RuntimeSceneView &scene,
                                                              const Network::NetworkAuthorityObjectPublication &authority) {
        using Outcome = Result<std::vector<std::byte>>;
        if (!FitsIntent(request, limits))
            return Invalid<std::vector<std::byte>>();
        try {
            CanonicalValueWriter writer{WireLimits};
            (void)writer.WriteUInt16(Version);
            (void)writer.WriteVec3(request.placement.translation);
            (void)writer.WriteQuaternion(request.placement.rotation);
            (void)writer.WriteVec3(request.placement.scale);
            (void)writer.WriteBool(request.parent.has_value());
            if (request.parent)
                if (auto parent = WriteReference(writer, *request.parent, scene, authority); parent.HasError())
                    return Outcome::Failure(parent.ErrorValue());
            if (auto values = WriteInputs(writer, request, scene, authority); values.HasError())
                return Outcome::Failure(values.ErrorValue());
            if (auto overrides = WriteOverrides(writer, request, limits); overrides.HasError())
                return Outcome::Failure(overrides.ErrorValue());
            auto encoded = std::move(writer).Finalize();
            if (encoded.HasError())
                return Outcome::Failure(encoded.ErrorValue());
            const auto bytes = encoded.Value().Bytes();
            return Outcome::Success({bytes.begin(), bytes.end()});
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Network::NetworkErrors::MessageEnvelopeCapacityExceeded));
        }
    }

    /** @copydoc DecodeHeadlessPrefabIntent */
    Result<Prefab::PrefabSpawnRequest> DecodeHeadlessPrefabIntent(const Network::NetworkPrefabLifecycleRecord &record,
                                                                  const AssetCookTargetId &target, const Prefab::PrefabLimitProfile &limits,
                                                                  const Runtime::RuntimeSceneView &scene,
                                                                  const Network::NetworkClientPrefabInbox &inbox) {
        using Outcome = Result<Prefab::PrefabSpawnRequest>;
        if (!record.spawn || record.spawn->intent.empty() || !target.IsValid())
            return Invalid<Prefab::PrefabSpawnRequest>();
        try {
            auto created = CanonicalValueReader::Create(record.spawn->intent, WireLimits);
            if (created.HasError())
                return Outcome::Failure(created.ErrorValue());
            auto reader = std::move(created).Value();
            Prefab::PrefabSpawnRequest request{.source = {record.spawn->recipe.asset, record.spawn->recipe.artifactDigest, target}};
            if (auto decoded = ReadSpawnRequest(reader, request, limits, record.session, scene, inbox); decoded.HasError())
                return Outcome::Failure(decoded.ErrorValue());
            return Outcome::Success(std::move(request));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Network::NetworkErrors::MessageEnvelopeCapacityExceeded));
        }
    }
}  // namespace Horo::Application::Internal
