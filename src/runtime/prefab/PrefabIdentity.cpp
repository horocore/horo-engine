#include "Horo/Prefab/PrefabIdentity.h"

#include <algorithm>

namespace Horo::Prefab {
    /** @copydoc PrefabAssetReference::Create */
    Result<PrefabAssetReference> PrefabAssetReference::Create(const Assets::AssetId &assetId) {
        if (!assetId.IsValid())
            return Result<PrefabAssetReference>::Failure(MakeError(PrefabErrors::ReferenceInvalid));
        return Result<PrefabAssetReference>::Success(PrefabAssetReference{assetId});
    }

    /** @copydoc PrefabAssetReference::Asset */
    const Assets::AssetId &PrefabAssetReference::Asset() const noexcept {
        return assetId_;
    }

    /** @copydoc PrefabAssetReference::IsValid */
    bool PrefabAssetReference::IsValid() const noexcept {
        return assetId_.IsValid();
    }

    /** @copydoc PrefabObjectAddress::Create */
    Result<PrefabObjectAddress> PrefabObjectAddress::Create(const std::span<const LocalObjectId> nestedInstanceScope,
                                                            const LocalObjectId sourceObject) {
        if (nestedInstanceScope.size() > MaximumPrefabObjectScopeDepth || std::ranges::any_of(nestedInstanceScope, &LocalObjectId::IsRoot))
            return Result<PrefabObjectAddress>::Failure(MakeError(PrefabErrors::AddressInvalid));

        std::array<LocalObjectId, MaximumPrefabObjectScopeDepth> scope{};
        std::ranges::copy(nestedInstanceScope, scope.begin());
        return Result<PrefabObjectAddress>::Success(
            PrefabObjectAddress{scope, static_cast<std::uint8_t>(nestedInstanceScope.size()), sourceObject});
    }

    /** @copydoc PrefabObjectAddress::NestedInstanceScope */
    std::span<const LocalObjectId> PrefabObjectAddress::NestedInstanceScope() const noexcept {
        return std::span{scope_}.first(scopeSize_);
    }

    /** @copydoc PrefabPropertyAddress::Create */
    Result<PrefabPropertyAddress> PrefabPropertyAddress::Create(PrefabObjectAddress object, Gameplay::ComponentTypeId componentType,
                                                                const PrefabComponentInstanceId componentInstance,
                                                                const PrefabPropertyId property) {
        if (!componentType.IsValid() || !componentInstance.IsValid() || !property.IsValid())
            return Result<PrefabPropertyAddress>::Failure(MakeError(PrefabErrors::AddressInvalid));
        return Result<PrefabPropertyAddress>::Success(
            PrefabPropertyAddress{std::move(object), std::move(componentType), componentInstance, property});
    }

    /** @copydoc PrefabPropertyAddress::Object */
    const PrefabObjectAddress &PrefabPropertyAddress::Object() const noexcept {
        return object_;
    }

    /** @copydoc PrefabPropertyAddress::ComponentType */
    const Gameplay::ComponentTypeId &PrefabPropertyAddress::ComponentType() const noexcept {
        return componentType_;
    }

    /** @copydoc ValidateRawComponentPayload */
    Result<void> ValidateRawComponentPayload(const RawComponentPayload &payload) {
        if (!payload.instance.IsValid())
            return Result<void>::Failure(MakeError(PrefabErrors::IdentityInvalid));
        if (payload.component.encoding != Gameplay::ComponentPayloadEncoding::CanonicalJson)
            return Result<void>::Failure(MakeError(PrefabErrors::DocumentInvalid));
        return Gameplay::ValidateSerializedComponent(payload.component);
    }
}  // namespace Horo::Prefab
