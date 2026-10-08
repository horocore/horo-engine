#include "Horo/Prefab/CookedPrefab.h"

#include "CookedPrefabCodec.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace Horo::Prefab {
    /** @copydoc ValidatePrefabInitializationValue */
    Result<void> ValidatePrefabInitializationValue(const CookedPrefabInitialization &declaration,
                                                   const Gameplay::BehaviorFieldValue &value) {
        if (const auto kind = static_cast<std::size_t>(declaration.kind);
            declaration.id.value == 0 || kind == 0 || kind > 7 || value.index() != kind || !std::isfinite(declaration.minimum) ||
            !std::isfinite(declaration.maximum) || declaration.minimum > declaration.maximum)
            return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Invalid typed initialization interface."));
        const bool valid = std::visit([&declaration]<typename Value>(const Value &input) {
            if constexpr (std::is_same_v<Value, double> || std::is_same_v<Value, std::int64_t>) {
                const auto number = static_cast<long double>(input);
                return std::isfinite(number) && number >= declaration.minimum && number <= declaration.maximum;
            } else if constexpr (std::is_same_v<Value, std::string>)
                return input.size() <= 256;
            else if constexpr (std::is_same_v<Value, Math::Vec2> || std::is_same_v<Value, Math::Vec3>)
                return Math::IsFinite(input);
            else if constexpr (std::is_same_v<Value, Math::Quaternion>)
                return Math::IsFinite(input) && input.TryNormalized().HasValue();
            else
                return std::is_same_v<Value, bool>;
        }, value);
        return valid ? Result<void>::Success()
                     : Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Initialization value exceeds its typed bounds."));
    }

    namespace {
        /** @brief Tests that a digest carries explicit revision evidence. */
        [[nodiscard]] bool HasDigest(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](const auto value) {
                return value != 0;
            });
        }

        /** @brief Resolves a member occurrence without returning mutable storage. */
        [[nodiscard]] bool HasMember(const CookedPrefabData &data, const CookedPrefabMemberSlot slot) noexcept {
            return slot.entity.value < data.entities.size() && slot.member < data.entities[slot.entity.value].members.size();
        }

        /** @brief Validates every cook-owned target and its immutable default before publishing the artifact. */
        [[nodiscard]] Result<void> ValidateInitialization(const CookedPrefabData &data) {
            if (data.initialization.size() > MaximumPrefabInitializationValues)
                return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected));
            PrefabInitializationId previous;
            for (std::size_t index = 0; index < data.initialization.size(); ++index) {
                const auto &declaration = data.initialization[index];
                if (declaration.id <= previous || !HasMember(data, declaration.owner))
                    return Result<void>::Failure(MakeError(PrefabErrors::CookArtifactInvalid));
                const auto *behavior = std::get_if<Gameplay::BehaviorComponent>(
                    &data.entities[declaration.owner.entity.value].members[declaration.owner.member]);
                if (!behavior || declaration.field >= behavior->fields.size())
                    return Result<void>::Failure(MakeError(PrefabErrors::CookArtifactInvalid));
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (data.initialization[prior].owner == declaration.owner && data.initialization[prior].field == declaration.field)
                        return Result<void>::Failure(MakeError(PrefabErrors::CookArtifactInvalid));
                }
                if (auto valid = ValidatePrefabInitializationValue(declaration, behavior->fields[declaration.field].value);
                    valid.HasError())
                    return valid;
                previous = declaration.id;
            }
            return Result<void>::Success();
        }

        /** @brief Rejects nonfinite behavioral values even when provider descriptors are unavailable. */
        [[nodiscard]] bool IsFiniteField(const Gameplay::BehaviorField &field) noexcept {
            return std::visit([]<typename Value>(const Value &value) {
                if constexpr (std::is_same_v<Value, double>)
                    return std::isfinite(value);
                else if constexpr (std::is_same_v<Value, Math::Vec2> || std::is_same_v<Value, Math::Vec3> ||
                                   std::is_same_v<Value, Math::Quaternion>)
                    return Math::IsFinite(value);
                else
                    return true;
            }, field.value);
        }

        /** @brief Validates portable member envelopes and per-kind stable occurrence uniqueness. */
        [[nodiscard]] Result<void> ValidateMembers(const CookedPrefabEntity &entity) {
            std::vector<std::pair<std::size_t, std::uint64_t>> identities;
            for (const auto &member : entity.members) {
                if (const auto result = std::visit(
                        []<typename Member>(const Member &value) -> Result<void> {
                    if constexpr (std::is_same_v<Member, RawComponentPayload>)
                        return ValidateRawComponentPayload(value);
                    else {
                        if (const auto valid = Gameplay::ValidateBehaviorComponent(value); valid.HasError())
                            return valid;
                        if (!std::ranges::all_of(value.fields, IsFiniteField))
                            return Result<void>::Failure(MakeError(PrefabErrors::CookArtifactInvalid));
                        return Result<void>::Success();
                    }
                }, member);
                    result.HasError())
                    return result;
                const auto id = std::visit([]<typename Member>(const Member &value) {
                    if constexpr (std::is_same_v<Member, RawComponentPayload>)
                        return value.instance.Value();
                    else
                        return value.instanceId.value;
                }, member);
                const std::pair identity{member.index(), id};
                if (std::ranges::find(identities, identity) != identities.end())
                    return Result<void>::Failure(MakeError(PrefabErrors::CookArtifactInvalid));
                identities.push_back(identity);
            }
            return Result<void>::Success();
        }

        /** @brief Checks finite entity state and unique path-free source evidence independently from hierarchy traversal. */
        [[nodiscard]] Result<void> ValidateEntityProvenance(const CookedPrefabData &data, const std::size_t index,
                                                            const PrefabProjectPolicy &policy) {
            const auto &entity = data.entities[index];
            if (!Math::IsFinite(entity.localTransform.translation) || !Math::IsFinite(entity.localTransform.scale) ||
                entity.localTransform.rotation.TryNormalized().HasError())
                return Result<void>::Failure(MakeError(PrefabErrors::CookArtifactInvalid));
            if (!entity.provenance.sourceAsset.IsValid() || !HasDigest(entity.provenance.sourceDigest) ||
                entity.provenance.sourceObject.NestedInstanceScope().size() > policy.maximumNestedPrefabDepth)
                return Result<void>::Failure(MakeError(PrefabErrors::CookArtifactInvalid));
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (entity.provenance.sourceObject == data.entities[prior].provenance.sourceObject)
                    return Result<void>::Failure(MakeError(PrefabErrors::CookArtifactInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Checks flattened hierarchy, bounded source evidence and member envelopes before encoding. */
        [[nodiscard]] Result<void> ValidateEntities(const CookedPrefabData &data, const PrefabProjectPolicy &policy) {
            if (data.entities.empty() || data.entities.size() > policy.maximumObjectCount)
                return Result<void>::Failure(MakeError(PrefabErrors::ObjectCountExceeded));
            std::vector<std::size_t> depths;
            for (std::size_t index = 0; index < data.entities.size(); ++index) {
                const auto &entity = data.entities[index];
                if ((index == 0 && entity.parent) || (index != 0 && (!entity.parent || entity.parent->value >= index)))
                    return Result<void>::Failure(MakeError(PrefabErrors::HierarchyInvalid));
                const auto depth = entity.parent ? depths[entity.parent->value] + 1 : 1;
                if (depth > policy.maximumHierarchyDepth)
                    return Result<void>::Failure(MakeError(PrefabErrors::HierarchyDepthExceeded));
                depths.push_back(depth);
                if (const auto provenance = ValidateEntityProvenance(data, index, policy); provenance.HasError())
                    return provenance;
                if (entity.members.size() > policy.maximumComponentsPerObject)
                    return Result<void>::Failure(MakeError(PrefabErrors::ComponentCountExceeded));
                auto members = ValidateMembers(entity);
                if (members.HasError())
                    return members;
            }
            return Result<void>::Success();
        }

        /** @brief Validates canonical complete dependencies and explicit external binding declarations. */
        [[nodiscard]] Result<void> ValidateInterfaces(const CookedPrefabData &data, const PrefabProjectPolicy &policy) {
            if (data.dependencies.size() > policy.maximumReferencedAssets)
                return Result<void>::Failure(MakeError(PrefabErrors::ReferenceCountExceeded));
            for (std::size_t index = 0; index < data.dependencies.size(); ++index) {
                const auto &dependency = data.dependencies[index];
                if (!dependency.asset.id.IsValid() || dependency.asset.id == data.assetId ||
                    dependency.asset.expectedType.Value().empty() || !HasDigest(dependency.artifactDigest) ||
                    (index != 0 && !(data.dependencies[index - 1].asset.id < dependency.asset.id)))
                    return Result<void>::Failure(MakeError(PrefabErrors::CookArtifactInvalid));
            }
            if (data.bindings.size() > policy.maximumBindingSlots || data.references.size() > policy.maximumBindingUses)
                return Result<void>::Failure(MakeError(PrefabErrors::ReferenceCountExceeded));
            for (std::size_t index = 0; index < data.bindings.size(); ++index) {
                const auto &binding = data.bindings[index];
                if (!binding.id.IsValid() || (binding.componentType && !binding.componentType->IsValid()) ||
                    (index != 0 && !(data.bindings[index - 1].id < binding.id)))
                    return Result<void>::Failure(MakeError(PrefabErrors::CookArtifactInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Checks that every fixup has one owner/property and a valid closed target. */
        [[nodiscard]] Result<void> ValidateReferences(const CookedPrefabData &data) {
            for (std::size_t index = 0; index < data.references.size(); ++index) {
                const auto &reference = data.references[index];
                if (!HasMember(data, reference.owner) || !reference.property.IsValid())
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                if (index != 0) {
                    const auto &prior = data.references[index - 1];
                    if (!(std::tie(prior.owner, prior.property) < std::tie(reference.owner, reference.property)))
                        return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                }
                const bool valid = std::visit([&data]<typename Target>(const Target &target) {
                    if constexpr (std::is_same_v<Target, CookedPrefabEntitySlot>)
                        return target.value < data.entities.size();
                    else if constexpr (std::is_same_v<Target, CookedPrefabMemberSlot>)
                        return HasMember(data, target);
                    else if constexpr (std::is_same_v<Target, CookedPrefabAssetSlot>)
                        return target.value < data.dependencies.size();
                    else
                        return target.value < data.bindings.size();
                }, reference.target);
                if (!valid)
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Emits a fixed-width network-order envelope word. */
        void AppendWord(std::vector<std::byte> &bytes, const std::uint32_t value) {
            for (const unsigned shift : {24U, 16U, 8U, 0U})
                bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
        }

        /** @brief Reads one envelope word after the fixed header size has been admitted. */
        [[nodiscard]] std::uint32_t ReadWord(const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
            std::uint32_t value = 0;
            for (std::size_t index = offset; index < offset + 4; ++index)
                value = (value << 8U) | std::to_integer<std::uint8_t>(bytes[index]);
            return value;
        }
    }  // namespace

    /** @copydoc CookedPrefab::Create */
    Result<CookedPrefab> CookedPrefab::Create(CookedPrefabData candidate, const PrefabLimitProfile &limits) {
        if (!candidate.assetId.IsValid())
            return Result<CookedPrefab>::Failure(MakeError(PrefabErrors::IdentityInvalid));
        auto valid = ValidateInterfaces(candidate, limits.Policy());
        if (valid.HasError())
            return Result<CookedPrefab>::Failure(valid.ErrorValue());
        valid = ValidateEntities(candidate, limits.Policy());
        if (valid.HasError())
            return Result<CookedPrefab>::Failure(valid.ErrorValue());
        valid = ValidateReferences(candidate);
        if (valid.HasError())
            return Result<CookedPrefab>::Failure(valid.ErrorValue());
        valid = ValidateInitialization(candidate);
        if (valid.HasError())
            return Result<CookedPrefab>::Failure(valid.ErrorValue());
        auto payload = Detail::EncodeCookedPrefabPayload(candidate, limits.Policy().maximumCookedPayloadBytes);
        if (payload.HasError())
            return Result<CookedPrefab>::Failure(payload.ErrorValue());
        const auto digest = ComputeSha256(payload.Value());
        std::vector bytes{std::byte{'H'}, std::byte{'P'}, std::byte{'F'}, std::byte{'B'}};
        AppendWord(bytes, CurrentCookedPrefabVersion);
        for (const auto byte : candidate.assetId.Bytes())
            bytes.push_back(static_cast<std::byte>(byte));
        AppendWord(bytes, static_cast<std::uint32_t>(candidate.entities.size()));
        AppendWord(bytes, static_cast<std::uint32_t>(payload.Value().size()));
        for (const auto byte : digest.bytes)
            bytes.push_back(static_cast<std::byte>(byte));
        bytes.insert(bytes.end(), payload.Value().begin(), payload.Value().end());
        return Result<CookedPrefab>::Success(CookedPrefab(std::move(candidate), std::move(bytes), digest));
    }

    /** @copydoc CookedPrefab::Parse */
    Result<CookedPrefab> CookedPrefab::Parse(const std::span<const std::byte> bytes, const Assets::AssetId &expectedAsset,
                                             const PrefabLimitProfile &limits) {
        const auto corrupt = [] {
            return Result<CookedPrefab>::Failure(MakeError(PrefabErrors::CorruptedPayload));
        };
        if (constexpr std::array magic{std::byte{'H'}, std::byte{'P'}, std::byte{'F'}, std::byte{'B'}};
            bytes.size() < CookedPrefabHeaderBytes || !std::ranges::equal(bytes.first(4), magic) || !expectedAsset.IsValid())
            return corrupt();
        if (ReadWord(bytes, 4) != CurrentCookedPrefabVersion)
            return Result<CookedPrefab>::Failure(MakeError(PrefabErrors::UnsupportedCookedVersion));
        if (bytes.size() - CookedPrefabHeaderBytes > limits.Policy().maximumCookedPayloadBytes)
            return Result<CookedPrefab>::Failure(MakeError(PrefabErrors::CookPayloadTooLarge));
        if (ReadWord(bytes, 28) != bytes.size() - CookedPrefabHeaderBytes)
            return corrupt();
        for (std::size_t index = 0; index < expectedAsset.Bytes().size(); ++index) {
            if (bytes[8 + index] != static_cast<std::byte>(expectedAsset.Bytes()[index]))
                return corrupt();
        }
        const auto objectCount = ReadWord(bytes, 24);
        if (objectCount == 0 || objectCount > limits.Policy().maximumObjectCount)
            return Result<CookedPrefab>::Failure(MakeError(PrefabErrors::ObjectCountExceeded));
        const auto payload = bytes.subspan(CookedPrefabHeaderBytes);
        const auto digest = ComputeSha256(payload);
        for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
            if (bytes[32 + index] != static_cast<std::byte>(digest.bytes[index]))
                return corrupt();
        }
        auto data = Detail::DecodeCookedPrefabPayload(payload, expectedAsset, objectCount, limits.Policy());
        if (data.HasError())
            return Result<CookedPrefab>::Failure(data.ErrorValue());
        auto artifact = Create(std::move(data).Value(), limits);
        if (artifact.HasError())
            return artifact;
        if (!std::ranges::equal(artifact.Value().Bytes(), bytes))
            return corrupt();
        return artifact;
    }

    /** @copydoc CookedPrefab::CookedPrefab */
    CookedPrefab::CookedPrefab(CookedPrefabData data, std::vector<std::byte> bytes, const Sha256Digest &digest) noexcept
        : data_(std::move(data)), bytes_(std::move(bytes)), digest_(digest) {}

    /** @copydoc CookedPrefab::GetAssetId */
    const Assets::AssetId &CookedPrefab::GetAssetId() const noexcept {
        return data_.assetId;
    }

    /** @copydoc CookedPrefab::GetObjectCount */
    std::uint32_t CookedPrefab::GetObjectCount() const noexcept {
        return static_cast<std::uint32_t>(data_.entities.size());
    }

    /** @copydoc CookedPrefab::Data */
    const CookedPrefabData &CookedPrefab::Data() const noexcept {
        return data_;
    }

    /** @copydoc CookedPrefab::Bytes */
    std::span<const std::byte> CookedPrefab::Bytes() const noexcept {
        return bytes_;
    }

    /** @copydoc CookedPrefab::PayloadDigest */
    Sha256Digest CookedPrefab::PayloadDigest() const noexcept {
        return digest_;
    }
}  // namespace Horo::Prefab
