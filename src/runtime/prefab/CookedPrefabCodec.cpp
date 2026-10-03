#include "CookedPrefabCodec.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <ranges>
#include <string_view>

namespace Horo::Prefab::Detail {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

    namespace {
        /** @brief Bounded network-order output; exhausted writers never allocate or append further bytes. */
        class Writer final {
        public:
            explicit Writer(const std::size_t maximumBytes) : maximumBytes_(maximumBytes) {}

            void Raw(const std::span<const std::byte> value) {
                if (!valid_ || value.size() > maximumBytes_ - bytes_.size()) {
                    valid_ = false;
                    return;
                }
                bytes_.insert(bytes_.end(), value.begin(), value.end());
            }

            template <typename T> void Integer(const T value) {
                std::array<std::byte, sizeof(T)> bytes{};
                T remaining = value;
                for (std::byte &byte : bytes | std::views::reverse) {
                    byte = static_cast<std::byte>(remaining & 0xffU);
                    remaining = static_cast<T>(remaining >> 8U);
                }
                Raw(bytes);
            }

            void Count(const std::size_t value) {
                Integer(static_cast<std::uint32_t>(value));
            }

            void Text(const std::string_view value) {
                Count(value.size());
                Raw(std::as_bytes(std::span(value.data(), value.size())));
            }

            void Float(const float value) {
                Integer(std::bit_cast<std::uint32_t>(value));
            }

            void Vector(const Math::Vec2 value) {
                Float(value.x);
                Float(value.y);
            }

            void Vector(const Math::Vec3 value) {
                Float(value.x);
                Float(value.y);
                Float(value.z);
            }

            void Vector(const Math::Quaternion value) {
                Float(value.x);
                Float(value.y);
                Float(value.z);
                Float(value.w);
            }

            void Asset(const Assets::AssetId &asset) {
                Raw(std::as_bytes(std::span(asset.Bytes())));
            }

            void Digest(const Sha256Digest &digest) {
                Raw(std::as_bytes(std::span(digest.bytes)));
            }

            [[nodiscard]] Result<std::vector<std::byte>> Finish() && {
                if (!valid_)
                    return Result<std::vector<std::byte>>::Failure(MakeError(PrefabErrors::CookPayloadTooLarge));
                return Result<std::vector<std::byte>>::Success(std::move(bytes_));
            }

        private:
            std::size_t maximumBytes_;
            bool valid_{true};
            std::vector<std::byte> bytes_;
        };

        /** @brief Fail-closed input cursor; lengths/counts are checked before allocation or traversal. */
        class Reader final {
        public:
            explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

            [[nodiscard]] std::span<const std::byte> Raw(const std::size_t size) {
                if (!valid_ || size > bytes_.size()) {
                    valid_ = false;
                    return {};
                }
                const auto result = bytes_.first(size);
                bytes_ = bytes_.subspan(size);
                return result;
            }

            template <typename T> [[nodiscard]] T Integer() {
                const auto bytes = Raw(sizeof(T));
                T result = 0;
                for (const auto byte : bytes)
                    result = static_cast<T>((result << 8U) | std::to_integer<std::uint8_t>(byte));
                return result;
            }

            [[nodiscard]] std::uint32_t Count(const std::size_t maximum, const std::size_t minimumRecordBytes = 1) {
                const auto count = Integer<std::uint32_t>();
                if (!valid_ || count > maximum || count > bytes_.size() / minimumRecordBytes) {
                    valid_ = false;
                    return 0;
                }
                return count;
            }

            [[nodiscard]] bool Boolean() {
                const auto value = Integer<std::uint8_t>();
                if (value > 1)
                    valid_ = false;
                return value == 1;
            }

            [[nodiscard]] std::string Text(const std::size_t maximum) {
                const auto count = Count(maximum);
                const auto bytes = Raw(count);
                std::string text(bytes.size(), '\0');
                std::ranges::transform(bytes, text.begin(), [](const std::byte byte) {
                    return std::bit_cast<char>(byte);
                });
                return text;
            }

            [[nodiscard]] float Float() {
                return std::bit_cast<float>(Integer<std::uint32_t>());
            }

            [[nodiscard]] Math::Vec2 Vec2() {
                return {Float(), Float()};
            }

            [[nodiscard]] Math::Vec3 Vec3() {
                return {Float(), Float(), Float()};
            }

            [[nodiscard]] Math::Quaternion Quaternion() {
                return {Float(), Float(), Float(), Float()};
            }

            [[nodiscard]] Assets::AssetId Asset() {
                std::array<std::uint8_t, 16> value{};
                const auto bytes = Raw(value.size());
                for (std::size_t index = 0; index < bytes.size(); ++index)
                    value[index] = std::to_integer<std::uint8_t>(bytes[index]);
                return Assets::AssetId::FromBytes(value);
            }

            [[nodiscard]] Sha256Digest Digest() {
                Sha256Digest value;
                const auto bytes = Raw(value.bytes.size());
                for (std::size_t index = 0; index < bytes.size(); ++index)
                    value.bytes[index] = std::to_integer<std::uint8_t>(bytes[index]);
                return value;
            }

            template <typename T> [[nodiscard]] T Parsed(Result<T> result) {
                if (result.HasError()) {
                    valid_ = false;
                    return {};
                }
                return std::move(result).Value();
            }

            void Reject() noexcept {
                valid_ = false;
            }

            [[nodiscard]] bool Complete() const noexcept {
                return valid_ && bytes_.empty();
            }

        private:
            std::span<const std::byte> bytes_;
            bool valid_{true};
        };

        /** @brief Writes one closed behavioral value, including its explicit portable type tag. */
        void WriteField(Writer &writer, const Gameplay::BehaviorField &field) {
            writer.Text(field.name);
            writer.Integer(static_cast<std::uint8_t>(field.value.index()));
            std::visit([&writer]<typename Value>(const Value &value) {
                if constexpr (std::is_same_v<Value, bool>)
                    writer.Integer(static_cast<std::uint8_t>(value));
                else if constexpr (std::is_same_v<Value, std::int64_t>)
                    writer.Integer(std::bit_cast<std::uint64_t>(value));
                else if constexpr (std::is_same_v<Value, double>)
                    writer.Integer(std::bit_cast<std::uint64_t>(value));
                else if constexpr (std::is_same_v<Value, std::string>)
                    writer.Text(value);
                else if constexpr (!std::is_same_v<Value, std::monostate>)
                    writer.Vector(value);
            }, field.value);
        }

        /** @brief Reads one closed behavioral value; unknown tags fail rather than guessing a default. */
        [[nodiscard]] Gameplay::BehaviorField ReadField(Reader &reader) {
            Gameplay::BehaviorField field;
            field.name = reader.Text(Gameplay::MaximumBehaviorFieldNameBytes);
            switch (reader.Integer<std::uint8_t>()) {
                case 0:
                    field.value = std::monostate{};
                    break;
                case 1:
                    field.value = reader.Boolean();
                    break;
                case 2:
                    field.value = std::bit_cast<std::int64_t>(reader.Integer<std::uint64_t>());
                    break;
                case 3:
                    field.value = std::bit_cast<double>(reader.Integer<std::uint64_t>());
                    break;
                case 4:
                    field.value = reader.Text(4096);
                    break;
                case 5:
                    field.value = reader.Vec2();
                    break;
                case 6:
                    field.value = reader.Vec3();
                    break;
                case 7:
                    field.value = reader.Quaternion();
                    break;
                default:
                    reader.Reject();
                    break;
            }
            return field;
        }

        /** @brief Encodes a complete member envelope without native layout or pointers. */
        void WriteMember(Writer &writer, const CookedPrefabMember &member) {
            writer.Integer(static_cast<std::uint8_t>(member.index()));
            std::visit([&writer]<typename Member>(const Member &value) {
                if constexpr (std::is_same_v<Member, RawComponentPayload>) {
                    writer.Integer(value.instance.Value());
                    writer.Text(value.component.typeId.Value());
                    writer.Integer(value.component.schemaVersion);
                    writer.Integer(static_cast<std::uint8_t>(value.component.encoding));
                    writer.Count(value.component.payload.size());
                    writer.Raw(value.component.payload);
                } else {
                    writer.Integer(value.instanceId.value);
                    writer.Text(value.typeId.Value());
                    writer.Integer(value.schemaVersion);
                    writer.Integer(static_cast<std::uint8_t>(value.enabled));
                    writer.Count(value.fields.size());
                    for (const auto &field : value.fields)
                        WriteField(writer, field);
                }
            }, member);
        }

        /** @brief Decodes one member within both provider-specific and cooked-byte bounds. */
        [[nodiscard]] CookedPrefabMember ReadMember(Reader &reader) {
            const auto kind = reader.Integer<std::uint8_t>();
            const auto instance = reader.Integer<std::uint64_t>();
            if (kind == 0) {
                RawComponentPayload member;
                member.instance = reader.Parsed(PrefabComponentInstanceId::Create(instance));
                member.component.typeId =
                    reader.Parsed(Gameplay::ComponentTypeId::Parse(reader.Text(Gameplay::MaximumComponentTypeIdBytes)));
                member.component.schemaVersion = reader.Integer<std::uint32_t>();
                if (const auto encoding = reader.Integer<std::uint8_t>();
                    encoding != static_cast<std::uint8_t>(Gameplay::ComponentPayloadEncoding::CanonicalJson))
                    reader.Reject();
                const auto size = reader.Count(Gameplay::MaximumSerializedComponentBytes);
                const auto payload = reader.Raw(size);
                member.component.payload.assign(payload.begin(), payload.end());
                return member;
            }
            if (kind != 1) {
                reader.Reject();
                return RawComponentPayload{};
            }
            Gameplay::BehaviorComponent member;
            member.instanceId.value = instance;
            member.typeId = reader.Parsed(Gameplay::BehaviorTypeId::Parse(reader.Text(Gameplay::MaximumBehaviorTypeIdBytes)));
            member.schemaVersion = reader.Integer<std::uint32_t>();
            member.enabled = reader.Boolean();
            const auto fields = reader.Count(Gameplay::MaximumBehaviorFields, 5);
            for (std::uint32_t index = 0; index < fields; ++index)
                member.fields.push_back(ReadField(reader));
            return member;
        }

        /** @brief Writes flattened hierarchy and path-free source evidence for one entity. */
        void WriteEntity(Writer &writer, const CookedPrefabEntity &entity) {
            writer.Integer(entity.parent ? entity.parent->value : std::numeric_limits<std::uint32_t>::max());
            writer.Vector(entity.localTransform.translation);
            writer.Vector(entity.localTransform.rotation);
            writer.Vector(entity.localTransform.scale);
            writer.Asset(entity.provenance.sourceAsset);
            writer.Digest(entity.provenance.sourceDigest);
            writer.Count(entity.provenance.sourceObject.NestedInstanceScope().size());
            for (const auto placement : entity.provenance.sourceObject.NestedInstanceScope())
                writer.Integer(placement.value);
            writer.Integer(entity.provenance.sourceObject.SourceObject().value);
            writer.Count(entity.members.size());
            for (const auto &member : entity.members)
                WriteMember(writer, member);
        }

        /** @brief Reads bounded hierarchy/member data without resolving source assets. */
        [[nodiscard]] CookedPrefabEntity ReadEntity(Reader &reader, const PrefabProjectPolicy &policy) {
            CookedPrefabEntity entity;
            if (const auto parent = reader.Integer<std::uint32_t>(); parent != std::numeric_limits<std::uint32_t>::max())
                entity.parent = CookedPrefabEntitySlot{parent};
            entity.localTransform.translation = reader.Vec3();
            entity.localTransform.rotation = reader.Quaternion();
            entity.localTransform.scale = reader.Vec3();
            entity.provenance.sourceAsset = reader.Asset();
            entity.provenance.sourceDigest = reader.Digest();
            const auto count = reader.Count(policy.maximumNestedPrefabDepth, 4);
            std::array<LocalObjectId, MaximumPrefabObjectScopeDepth> scope{};
            for (std::uint32_t index = 0; index < count; ++index)
                scope[index].value = reader.Integer<std::uint32_t>();
            const LocalObjectId local{reader.Integer<std::uint32_t>()};
            entity.provenance.sourceObject = reader.Parsed(PrefabObjectAddress::Create(std::span(scope).first(count), local));
            const auto members = reader.Count(policy.maximumComponentsPerObject, 18);
            for (std::uint32_t index = 0; index < members; ++index)
                entity.members.push_back(ReadMember(reader));
            return entity;
        }

        /** @brief Encodes one property fixup and its closed target class. */
        void WriteReference(Writer &writer, const CookedPrefabReference &reference) {
            writer.Integer(reference.owner.entity.value);
            writer.Integer(reference.owner.member);
            writer.Integer(reference.property.Value());
            writer.Integer(static_cast<std::uint8_t>(reference.target.index()));
            std::visit([&writer]<typename Target>(const Target &target) {
                if constexpr (std::is_same_v<Target, CookedPrefabMemberSlot>) {
                    writer.Integer(target.entity.value);
                    writer.Integer(target.member);
                } else
                    writer.Integer(target.value);
            }, reference.target);
        }

        /** @brief Decodes one property fixup, rejecting every unrecognized reference class. */
        [[nodiscard]] CookedPrefabReference ReadReference(Reader &reader) {
            CookedPrefabReference reference;
            reference.owner.entity.value = reader.Integer<std::uint32_t>();
            reference.owner.member = reader.Integer<std::uint32_t>();
            reference.property = reader.Parsed(PrefabPropertyId::Create(reader.Integer<std::uint64_t>()));
            switch (reader.Integer<std::uint8_t>()) {
                case 0:
                    reference.target = CookedPrefabEntitySlot{reader.Integer<std::uint32_t>()};
                    break;
                case 1:
                    reference.target = CookedPrefabMemberSlot{{reader.Integer<std::uint32_t>()}, reader.Integer<std::uint32_t>()};
                    break;
                case 2:
                    reference.target = CookedPrefabAssetSlot{reader.Integer<std::uint32_t>()};
                    break;
                case 3:
                    reference.target = CookedPrefabBindingSlot{reader.Integer<std::uint32_t>()};
                    break;
                default:
                    reader.Reject();
                    break;
            }
            return reference;
        }
    }  // namespace

    /** @copydoc EncodeCookedPrefabPayload */
    Result<std::vector<std::byte>> EncodeCookedPrefabPayload(const CookedPrefabData &data, const std::size_t maximumBytes) {
        Writer writer(maximumBytes);
        writer.Count(data.entities.size());
        for (const auto &entity : data.entities)
            WriteEntity(writer, entity);
        writer.Count(data.dependencies.size());
        for (const auto &dependency : data.dependencies) {
            writer.Asset(dependency.asset.id);
            writer.Text(dependency.asset.expectedType.Value());
            writer.Digest(dependency.artifactDigest);
        }
        writer.Count(data.bindings.size());
        for (const auto &binding : data.bindings) {
            writer.Integer(binding.id.Value());
            writer.Integer(static_cast<std::uint8_t>(binding.required));
            writer.Integer(static_cast<std::uint8_t>(binding.componentType.has_value()));
            if (binding.componentType)
                writer.Text(binding.componentType->Value());
        }
        writer.Count(data.references.size());
        for (const auto &reference : data.references)
            WriteReference(writer, reference);
        return std::move(writer).Finish();
    }

    /** @copydoc DecodeCookedPrefabPayload */
    Result<CookedPrefabData> DecodeCookedPrefabPayload(const std::span<const std::byte> payload, Assets::AssetId asset,
                                                       const std::uint32_t objectCount, const PrefabProjectPolicy &policy) {
        Reader reader(payload);
        CookedPrefabData data;
        data.assetId = std::move(asset);
        const auto entities = reader.Count(policy.maximumObjectCount, 104);
        if (entities != objectCount)
            return Result<CookedPrefabData>::Failure(MakeError(PrefabErrors::CorruptedPayload));
        for (std::uint32_t index = 0; index < entities; ++index)
            data.entities.push_back(ReadEntity(reader, policy));
        const auto dependencies = reader.Count(policy.maximumReferencedAssets, 53);
        for (std::uint32_t index = 0; index < dependencies; ++index) {
            CookedPrefabDependency dependency;
            dependency.asset.id = reader.Asset();
            dependency.asset.expectedType = reader.Parsed(Assets::AssetTypeId::Parse(reader.Text(96)));
            dependency.artifactDigest = reader.Digest();
            data.dependencies.push_back(std::move(dependency));
        }
        const auto bindings = reader.Count(policy.maximumBindingSlots, 10);
        for (std::uint32_t index = 0; index < bindings; ++index) {
            CookedPrefabBindingDeclaration binding;
            binding.id = reader.Parsed(PrefabPropertyId::Create(reader.Integer<std::uint64_t>()));
            binding.required = reader.Boolean();
            if (reader.Boolean())
                binding.componentType = reader.Parsed(Gameplay::ComponentTypeId::Parse(reader.Text(Gameplay::MaximumComponentTypeIdBytes)));
            data.bindings.push_back(std::move(binding));
        }
        const auto references = reader.Count(policy.maximumBindingUses, 21);
        for (std::uint32_t index = 0; index < references; ++index)
            data.references.push_back(ReadReference(reader));
        if (!reader.Complete())
            return Result<CookedPrefabData>::Failure(MakeError(PrefabErrors::CorruptedPayload));
        return Result<CookedPrefabData>::Success(std::move(data));
    }
}  // namespace Horo::Prefab::Detail
