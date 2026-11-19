#pragma once

#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Network/ReplicationDeclarationPolicy.h"
#include "Horo/Prefab/CookedPrefab.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Network::PrefabProductTestSupport {
    /** @brief The real native test module declares this component as its sole replicated entity owner. */
    inline ReplicationDeclaration NativePrefabDeclaration() {
        return {.schema = {.id = ReplicationSchemaId::Create(1001).Value(),
                           .version = {1, 0},
                           .compatibility = {{1, 0}, {1, 0}},
                           .owner = {.value = "game.tests"},
                           .fields = {{.id = FieldId::Create(1).Value(),
                                       .valueType = ReplicationValueTypeId::Create(1).Value(),
                                       .codec = ReplicationCodecId::Create(1).Value(),
                                       .introducedVersion = {1, 0},
                                       .limits = {8, 1}}}}};
    }

    /** @brief Publishes a cooked Prefab containing two replicated members and a local-only child. */
    inline Sha256Digest PutNetworkPrefab(Assets::MemoryAssetProvider &provider, Assets::AssetId asset, const Assets::AssetTypeId &type) {
        Prefab::RawComponentPayload component{.instance = Prefab::PrefabComponentInstanceId::Create(1).Value(),
                                              .component = {.typeId =
                                                                Gameplay::ComponentTypeId::Parse("game.tests.movement_settings").Value(),
                                                            .schemaVersion = 2,
                                                            .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson}};
        for (const unsigned char byte : std::string_view{R"({"speed":4.25})"})
            component.component.payload.push_back(static_cast<std::byte>(byte));
        Prefab::CookedPrefabEntity root;
        root.provenance = {asset, {}, ComputeSha256(std::as_bytes(std::span("source", 6)))};
        root.members.emplace_back(std::move(component));
        auto child = root;
        child.parent = Prefab::CookedPrefabEntitySlot{0};
        child.provenance.sourceObject = Prefab::PrefabObjectAddress::Create({}, {8}).Value();
        auto local = child;
        local.provenance.sourceObject = Prefab::PrefabObjectAddress::Create({}, {7}).Value();
        local.members.clear();
        auto cooked = Prefab::CookedPrefab::Create({.assetId = asset, .entities = {std::move(root), std::move(local), std::move(child)}},
                                                   Prefab::PrefabLimitProfile::Create({}).Value());
        REQUIRE(cooked.HasValue());
        std::vector<std::uint8_t> payload;
        for (const auto byte : cooked.Value().Bytes())
            payload.push_back(std::to_integer<std::uint8_t>(byte));
        const auto target = AssetCookTargetId::Parse("linux-x64").Value();
        const auto digest = ComputeSha256(std::as_bytes(std::span(payload)));
        auto bytes = Assets::EncodeCookedArtifact({asset, type, target, {}, {}, digest, std::move(payload)});
        REQUIRE(bytes.HasValue());
        const auto envelope = ComputeSha256(std::as_bytes(std::span(bytes.Value())));
        provider.Insert(asset, std::move(bytes).Value());
        return envelope;
    }
}  // namespace Horo::Network::PrefabProductTestSupport
