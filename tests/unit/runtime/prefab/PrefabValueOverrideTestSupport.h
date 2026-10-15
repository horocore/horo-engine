#pragma once
#include "Horo/Prefab/PrefabValueOverrides.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Prefab::ValueTestSupport {
    inline PrefabLimitProfile Limits() {
        return PrefabLimitProfile::Create({}).Value();
    }

    inline Assets::AssetId Asset() {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = 91;
        return Assets::AssetId::FromBytes(bytes);
    }

    inline Gameplay::ComponentTypeId Type() {
        return Gameplay::ComponentTypeId::Parse("game.override.values").Value();
    }

    inline PrefabObjectAddress Object() {
        return PrefabObjectAddress::Create({}, LocalObjectId{0}).Value();
    }

    inline PrefabPropertyAddress Property() {
        return PrefabPropertyAddress::Create(Object(), Type(), PrefabComponentInstanceId::Create(1).Value(),
                                             PrefabPropertyId::Create(7).Value())
            .Value();
    }

    inline PrefabValueSchemaSnapshot Schemas() {
        return PrefabValueSchemaSnapshot::Create({{Type(), PrefabPropertyId::Create(7).Value(),
                                                   Gameplay::ComponentPropertyId::Parse("speed").Value(), 1,
                                                   Gameplay::ComponentPropertyKind::Number, 1.0, 0.0, 10.0}},
                                                 Limits())
            .Value();
    }

    inline PrefabSourceResolverSnapshot Sources(const std::string_view payload = "{\"speed\":2.0}", const std::uint8_t revision = 1) {
        Assets::AssetRegistry registry;
        REQUIRE(registry
                    .Publish({{Asset(), Assets::AssetTypeId::Parse("core.prefab").Value(),
                               ProjectPath::Parse("assets/value.prefab").Value(), ProjectPath::Parse("assets/value.prefab.horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
        Gameplay::SerializedComponent component;
        component.typeId = Type();
        component.payload.assign(reinterpret_cast<const std::byte *>(payload.data()),
                                 reinterpret_cast<const std::byte *>(payload.data() + payload.size()));
        PrefabObjectNode object{.localId = {0}, .name = "Root"};
        object.components.push_back({PrefabComponentInstanceId::Create(1).Value(), std::move(component)});
        auto document = PrefabDocument::Create({.projectVersion = Application::ParseHoroVersion("1.2.3").Value(),
                                                .assetId = Asset(),
                                                .objects = {std::move(object)}},
                                               Limits());
        REQUIRE(document.HasValue());
        Sha256Digest digest{};
        digest.bytes.back() = revision;
        auto snapshot =
            BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                              {{std::move(document).Value(), {Application::ParseHoroVersion("1.2.3").Value(), digest}}},
                                              Limits());
        REQUIRE(snapshot.HasValue());
        return std::move(snapshot).Value();
    }

    inline EffectivePrefabCandidate Source(const std::string_view payload = "{\"speed\":2.0}", const std::uint8_t revision = 1) {
        return Sources(payload, revision).Resolve(Asset(), PrefabInstanceId::Create(9).Value(), Limits()).Value();
    }
}  // namespace Horo::Prefab::ValueTestSupport
