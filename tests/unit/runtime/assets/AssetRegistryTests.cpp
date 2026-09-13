#include "Horo/Assets/AssetRegistry.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <thread>

namespace {
    std::atomic<std::size_t> gAllocations{};

    void *Allocate(const std::size_t size) {
        gAllocations.fetch_add(1, std::memory_order_relaxed);
        if (void *memory = std::malloc(size))
            return memory;
        throw std::bad_alloc{};
    }
}  // namespace

void *operator new(const std::size_t size) {
    return Allocate(size);
}

void *operator new[](const std::size_t size) {
    return Allocate(size);
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete[](void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    AssetId Id(const std::string_view value) {
        auto parsed = AssetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetTypeId MeshType() {
        auto parsed = AssetTypeId::Parse("core.mesh");
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    ProjectPath Path(const std::string_view value) {
        auto parsed = ProjectPath::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetRecord Record(const AssetId id, const std::string_view source) {
        return {id, MeshType(), Path(source), Path(std::string{source} + ".horo")};
    }

    void Write(const std::filesystem::path &path, const std::string_view contents) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << contents;
        REQUIRE((output.good()));
    }

    std::filesystem::path TemporaryProject() {
        static std::atomic<unsigned> sequence{};
        const auto root = std::filesystem::temp_directory_path() / ("horo-assets-registry-" + std::to_string(sequence.fetch_add(1)));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / ".horo");
        std::filesystem::create_directories(root / "assets/models");
        return root;
    }

    TEST_CASE("Stable Identities Are Canonical", "[unit][runtime][assets]") {
        constexpr std::string_view canonical = "00112233-4455-6677-8899-aabbccddeeff";
        const AssetId id = Id(canonical);
        REQUIRE((id.IsValid()));
        REQUIRE((id.ToString() == canonical));
        REQUIRE((AssetId::Parse("00112233-4455-6677-8899-AABBCCDDEEFF").HasError()));
        REQUIRE((AssetId::Parse("00000000-0000-0000-0000-000000000000").HasError()));
        REQUIRE((AssetId::Parse("not-an-id").HasError()));
        REQUIRE((AssetTypeId::Parse("core.mesh").HasValue()));
        REQUIRE((AssetTypeId::Parse("Mesh").HasError()));
    }

    TEST_CASE("Snapshot Publish Is Atomic Pinned And Allocation Free", "[unit][runtime][assets]") {
        AssetRegistry registry;
        const AssetId firstId = Id("00112233-4455-6677-8899-aabbccddeeff");
        auto first = registry.Publish({Record(firstId, "assets/models/first.obj")});
        REQUIRE((first.status == AssetRegistryBuildStatus::Complete));
        const AssetRegistrySnapshot pinned = registry.Snapshot();

        const std::size_t before = gAllocations.load(std::memory_order_relaxed);
        for (int iteration = 0; iteration < 1000; ++iteration) {
            REQUIRE((pinned.Find(firstId) != nullptr));
            REQUIRE((pinned.FindByPath("assets/models/first.obj") != nullptr));
        }
        REQUIRE((gAllocations.load(std::memory_order_relaxed) == before));

        const AssetId secondId = Id("11112233-4455-6677-8899-aabbccddeeff");
        auto second = registry.Publish({Record(secondId, "assets/models/second.obj")});
        REQUIRE((second.status == AssetRegistryBuildStatus::Complete));
        REQUIRE((pinned.Find(firstId) != nullptr));
        REQUIRE((registry.Snapshot().Find(firstId) == nullptr));

        const auto preserved = registry.Snapshot();
        auto duplicate = registry.Publish({Record(secondId, "assets/models/a.obj"), Record(secondId, "assets/models/b.obj")});
        REQUIRE((duplicate.status == AssetRegistryBuildStatus::Failed));
        REQUIRE((registry.Snapshot().Revision() == preserved.Revision()));
        auto collision = registry.Publish({Record(firstId, "assets/models/Thing.obj"), Record(secondId, "assets/models/thing.obj")});
        REQUIRE((collision.status == AssetRegistryBuildStatus::Failed));
        REQUIRE((registry.Snapshot().Revision() == preserved.Revision()));
        auto duplicatePath = registry.Publish({Record(firstId, "assets/models/same.obj"), Record(secondId, "assets/models/same.obj")});
        REQUIRE((duplicatePath.status == AssetRegistryBuildStatus::Failed));
        REQUIRE((registry.Snapshot().Revision() == preserved.Revision()));
    }

    TEST_CASE("Concurrent Readers Observe Complete Snapshots", "[unit][runtime][assets]") {
        AssetRegistry registry;
        const AssetId firstId = Id("00112233-4455-6677-8899-aabbccddeeff");
        const AssetId secondId = Id("11112233-4455-6677-8899-aabbccddeeff");
        REQUIRE((registry.Publish({Record(firstId, "assets/models/a.obj")}).status == AssetRegistryBuildStatus::Complete));
        std::atomic<bool> stop{};
        std::atomic<bool> invalid{};
        std::thread reader([&] {
            while (!stop.load(std::memory_order_acquire)) {
                const AssetRegistrySnapshot snapshot = registry.Snapshot();
                if (snapshot.Records().size() != 1 || (snapshot.Find(firstId) == nullptr && snapshot.Find(secondId) == nullptr))
                    invalid.store(true, std::memory_order_release);
            }
        });
        for (int index = 0; index < 500; ++index) {
            const bool first = index % 2 == 0;
            const auto published =
                registry.Publish({Record(first ? firstId : secondId, first ? "assets/models/a.obj" : "assets/models/b.obj")});
            REQUIRE((published.status == AssetRegistryBuildStatus::Complete));
        }
        stop.store(true, std::memory_order_release);
        reader.join();
        REQUIRE((!invalid.load(std::memory_order_acquire)));
    }

    TEST_CASE("Rebuild Classifies Invalid Assets And Preserves Moved Identity", "[unit][runtime][assets]") {
        const std::filesystem::path root = TemporaryProject();
        Write(root / "assets/models/good.obj", "mesh");
        Write(root / "assets/models/good.obj.horo",
              R"({"schemaVersion":1,"assetId":"00112233-4455-6677-8899-aabbccddeeff","assetType":"core.mesh"})");
        Write(root / "assets/models/untracked.obj", "mesh");
        Write(root / "assets/models/missing-id.obj", "mesh");
        Write(root / "assets/models/missing-id.obj.horo", R"({"schemaVersion":1,"assetType":"core.mesh"})");
        Write(root / "assets/models/invalid-id.obj", "mesh");
        Write(root / "assets/models/invalid-id.obj.horo", R"({"schemaVersion":1,"assetId":"invalid","assetType":"core.mesh"})");
        Write(root / "assets/models/malformed.obj", "mesh");
        Write(root / "assets/models/malformed.obj.horo", "{");
        Write(root / "assets/models/future.obj", "mesh");
        Write(root / "assets/models/future.obj.horo",
              R"({"schemaVersion":2,"assetId":"11112233-4455-6677-8899-aabbccddeeff","assetType":"core.mesh"})");
        Write(root / "assets/models/orphan.fbx.horo",
              R"({"schemaVersion":1,"assetId":"21112233-4455-6677-8899-aabbccddeeff","assetType":"core.mesh"})");
        std::error_code symlinkError;
        std::filesystem::create_symlink(root / "assets/models/good.obj", root / "assets/models/link.obj", symlinkError);

        AssetRegistry registry;
        auto rebuilt = RebuildAssetRegistry(registry, root, AssetRegistryOpenMode::ReadOnly);
        REQUIRE((rebuilt.HasValue()));
        REQUIRE((rebuilt.Value().status == AssetRegistryBuildStatus::Degraded));
        REQUIRE((rebuilt.Value().registeredAssets == 1));
        REQUIRE((!std::filesystem::exists(root / ".horo/asset_index.json")));
        std::vector<std::string> codes;
        for (const auto &diagnostic : rebuilt.Value().diagnostics)
            codes.push_back(diagnostic.error.code.Value());
        REQUIRE((std::ranges::find(codes, "asset.registry.sidecar_missing") != codes.end()));
        REQUIRE((std::ranges::find(codes, "asset.registry.identity_missing") != codes.end()));
        REQUIRE((std::ranges::find(codes, "asset.registry.identity_invalid") != codes.end()));
        REQUIRE((std::ranges::find(codes, "asset.registry.sidecar_malformed") != codes.end()));
        REQUIRE((std::ranges::find(codes, "asset.registry.schema_unsupported") != codes.end()));
        REQUIRE((std::ranges::find(codes, "asset.registry.source_missing") != codes.end()));
        if (!symlinkError)
            REQUIRE((std::ranges::find(codes, "asset.registry.symlink_ambiguous") != codes.end()));

        AssetRegistry deterministicRegistry;
        auto deterministic = RebuildAssetRegistry(deterministicRegistry, root, AssetRegistryOpenMode::ReadOnly);
        REQUIRE((deterministic.HasValue()));
        std::vector<std::string> repeatedCodes;
        for (const auto &diagnostic : deterministic.Value().diagnostics)
            repeatedCodes.push_back(diagnostic.error.code.Value());
        REQUIRE((repeatedCodes == codes));

        std::filesystem::rename(root / "assets/models/good.obj", root / "assets/models/moved.obj");
        std::filesystem::rename(root / "assets/models/good.obj.horo", root / "assets/models/moved.obj.horo");
        auto moved = RebuildAssetRegistry(registry, root, AssetRegistryOpenMode::Edit);
        REQUIRE((moved.HasValue()));
        const AssetId stable = Id("00112233-4455-6677-8899-aabbccddeeff");
        REQUIRE((registry.Snapshot().Find(stable) != nullptr));
        REQUIRE((registry.Snapshot().Find(stable)->sourcePath.String() == "assets/models/moved.obj"));
        REQUIRE((std::filesystem::exists(root / ".horo/asset_index.json")));

        Write(root / ".horo/asset_index.json", "{");
        AssetRegistry fallback;
        auto loaded = LoadAssetRegistry(fallback, root, AssetRegistryOpenMode::ReadOnly);
        REQUIRE((loaded.HasValue()));
        REQUIRE((loaded.Value().status == AssetRegistryBuildStatus::Degraded));
        REQUIRE((loaded.Value().diagnostics.front().error.code.Value() == "asset.registry.index_malformed"));
        REQUIRE((fallback.Snapshot().Find(stable) != nullptr));
        std::filesystem::remove_all(root);
    }

    TEST_CASE("Prepared Candidate Does Not Publish Until Owner Install", "[unit][runtime][assets]") {
        const std::filesystem::path root = TemporaryProject();
        Write(root / "assets/models/prepared.obj", "mesh");
        Write(root / "assets/models/prepared.obj.horo",
              R"({"schemaVersion":1,"assetId":"00112233-4455-6677-8899-aabbccddeeff","assetType":"core.mesh"})");
        AssetRegistry authoritative;
        const AssetRegistryRevision before = authoritative.Snapshot().Revision();
        auto prepared = PrepareAssetRegistryCandidate(root);
        REQUIRE((prepared.HasValue()));
        REQUIRE((authoritative.Snapshot().Revision() == before));
        const auto published = authoritative.Publish(std::move(prepared).Value());
        REQUIRE((published.status == AssetRegistryBuildStatus::Complete));
        REQUIRE((authoritative.Snapshot().Records().size() == 1));
        std::filesystem::remove_all(root);
    }
}  // namespace
