#pragma once

#include "Horo/Editor/FractureAssetDocument.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Editor::FractureTest {
    template <typename T> T Id(const std::uint64_t value) {
        return T::Create(value).Value();
    }

    inline Assets::AssetId Asset(std::uint8_t value) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return Assets::AssetId::FromBytes(bytes);
    }

    inline Sha256Digest Digest(std::uint8_t value) {
        Sha256Digest digest;
        digest.bytes.back() = value;
        return digest;
    }

    inline FractureAssetSource Source() {
        using namespace Destruction;
        FractureAssetSource source;
        source.asset = FractureAssetId::Create(Asset(1)).Value();
        source.settings = {.sourceMesh = Asset(2),
                           .sourceRevision = 3,
                           .sourceDigest = Digest(4),
                           .recipe = 5,
                           .recipeRevision = 6,
                           .seed = 7,
                           .toolchainDigest = Digest(8),
                           .tier = DestructionFeatureTier::Standard,
                           .requiredFeatures = {.bits = DestructionFeatureBit<DestructionFeature::PreCookedFracture> |
                                                        DestructionFeatureBit<DestructionFeature::CookedSupport>}};
        source.chunks = {{Id<DestructionChunkId>(10), {}, 0, true, false},
                         {Id<DestructionChunkId>(20), Id<DestructionChunkId>(10), 0, false, true}};
        source.contacts = {{Id<DestructionChunkId>(10), Id<DestructionChunkId>(20), 1.5}};
        source.materials = {{0, Asset(9), Digest(10)}};
        return source;
    }

    inline FractureAssetDocument Document(FractureDocumentHistoryLimits limits = {}) {
        auto opened = FractureAssetDocument::Open(Source(), 11, Id<FractureDocumentSession>(12), limits);
        REQUIRE(opened.HasValue());
        return std::move(opened.Value());
    }

    inline FractureDocumentEditContext Context(const FractureAssetDocument &document) {
        const auto snapshot = document.Snapshot();
        return {.session = snapshot.session, .revision = snapshot.revision, .canEdit = true};
    }

    inline FractureSourcePatch SeedPatch(const FractureAssetDocument &document, std::uint64_t seed) {
        auto settings = document.Snapshot().source->settings;
        settings.seed = seed;
        return {{SetFractureSettings{std::move(settings)}}};
    }

    template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &error) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == error.code.Value());
    }
}  // namespace Horo::Editor::FractureTest
