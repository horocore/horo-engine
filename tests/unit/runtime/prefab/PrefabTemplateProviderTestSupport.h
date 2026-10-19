#pragma once

/** @file PrefabTemplateProviderTestSupport.h
 * @brief Shared real Assets/Scene fixture for provider lifetime and projection regressions.
 */
#include "Horo/Assets/AssetCook.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabTemplateCook.h"
#include "Horo/Prefab/PrefabTemplateProvider.h"
#include "PrefabTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>

namespace Horo::Prefab::TestSupport {
    using namespace std::chrono_literals;

    inline PrefabLimitProfile Profile() {
        return PrefabLimitProfile::Create({}).Value();
    }

    inline AssetCookTargetId Target() {
        return AssetCookTargetId::Parse("linux-x64").Value();
    }

    inline Assets::AssetTypeId Type(std::string_view text) {
        return Assets::AssetTypeId::Parse(text).Value();
    }

    inline Sha256Digest Digest(std::span<const std::uint8_t> bytes) {
        return ComputeSha256(std::as_bytes(bytes));
    }

    inline std::vector<std::uint8_t> Envelope(Assets::AssetId id, std::string_view type, std::vector<std::uint8_t> payload) {
        auto artifact = Assets::EncodeCookedArtifact({id, Type(type), Target(), {}, {}, Digest(payload), std::move(payload)});
        REQUIRE(artifact.HasValue());
        return std::move(artifact).Value();
    }

    /** @brief Worker gate deliberately ignores cancellation until released, proving provider teardown never joins it. */
    class GatedProvider final : public Assets::IAssetProvider {
    public:
        Assets::MemoryAssetProvider &Memory() noexcept {
            return memory_;
        }

        void Block() {
            std::scoped_lock lock{mutex_};
            blocked_ = true;
            entered_ = false;
        }

        void Release() noexcept {
            std::scoped_lock lock{mutex_};
            blocked_ = false;
            wake_.notify_all();
        }

        bool Entered() const {
            std::scoped_lock lock{mutex_};
            return entered_;
        }

        Result<bool> Exists(Assets::AssetId id, const CancellationToken &token) const override {
            return memory_.Exists(id, token);
        }

        Result<std::vector<std::uint8_t>> Load(Assets::AssetId id, const CancellationToken &token) const override {
            {
                std::unique_lock lock{mutex_};
                entered_ = true;
                wake_.wait(lock, [this] {
                    return !blocked_;
                });
            }
            return memory_.Load(id, token);
        }

    private:
        Assets::MemoryAssetProvider memory_;
        mutable std::mutex mutex_;
        mutable std::condition_variable wake_;
        mutable bool entered_{};
        bool blocked_{};
    };

    struct Fixture final {
        GatedProvider bytes;
        JobSystem jobs{{2, 16}};
        Assets::AssetLoadService loads{jobs, bytes};
        Assets::AssetRegistry registry;
        Runtime::RuntimeSceneService scenes{registry, loads};
        PrefabTemplateProvider provider;
        std::vector<Assets::AssetRecord> records;
        std::vector<std::uint8_t> root;
        std::vector<std::uint8_t> dependency;
        CancellationSource cancellation;
        const CancellationToken frameCancellation{cancellation.Token()};

        explicit Fixture(const PrefabTemplateProviderLimits &limits = {},
                         std::unique_ptr<Runtime::SceneStructuralParticipant> participant = {})
            : provider(registry, loads, scenes, Profile(), limits) {
            for (std::uint16_t id = 1; id <= 3; ++id) {
                const auto name = std::format("assets/asset{}{}", id, id == 2 ? ".obj" : ".prefab");
                records.emplace_back(Test::Asset(id), Type(id == 2 ? "core.mesh" : "core.prefab"), ProjectPath::Parse(name).Value(),
                                     ProjectPath::Parse(name + ".horo").Value());
            }
            const auto publication = registry.Publish(records);
            REQUIRE(publication.status == Assets::AssetRegistryBuildStatus::Complete);
            REQUIRE(publication.registeredAssets == records.size());
            REQUIRE(publication.publishedRevision.value != 0);
            dependency = Envelope(Test::Asset(2), "core.mesh", {1, 2, 3});
            bytes.Memory().Insert(Test::Asset(2), dependency);
            root = Root(Test::Asset());
            bytes.Memory().Insert(Test::Asset(), root);
            bytes.Memory().Insert(Test::Asset(3), Root(Test::Asset(3)));
            if (participant) {
                const auto added = scenes.AddStructuralParticipant(std::move(participant));
                REQUIRE(added.HasValue());
            }
            REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
            Activate();
            REQUIRE(provider.Startup(cancellation.Token()).HasValue());
        }

        // Always opens the worker gate before borrowed loader/job owners are destroyed, including fatal assertions.
        ~Fixture() {
            bytes.Release();
        }

        std::vector<std::uint8_t> Root(Assets::AssetId id, const bool interfaces = false) {
            CookedPrefabData data;
            data.assetId = id;
            CookedPrefabEntity first;
            first.provenance = {id, {}, Digest(dependency)};
            auto second = first;
            second.parent = CookedPrefabEntitySlot{0};
            second.localTransform.translation = {1, 2, 3};
            second.provenance.sourceObject = PrefabObjectAddress::Create({}, {7}).Value();
            data.entities = {first, second};
            data.dependencies = {{{Test::Asset(2), Type("core.mesh")}, Digest(dependency)}};
            if (interfaces) {
                Gameplay::BehaviorComponent behavior{{1}, Gameplay::BehaviorTypeId::Parse("game.tests.controller").Value()};
                data.entities[0].members.emplace_back(behavior);
                RawComponentPayload raw;
                raw.instance = PrefabComponentInstanceId::Create(7).Value();
                raw.component.typeId = Gameplay::ComponentTypeId::Parse("game.tests.health").Value();
                raw.component.payload = {std::byte{'{'}, std::byte{'}'}};
                data.entities[1].members.emplace_back(std::move(raw));
                data.bindings = {{PrefabPropertyId::Create(11).Value(), {}, true},
                                 {PrefabPropertyId::Create(12).Value(),
                                  std::get<RawComponentPayload>(data.entities[1].members[0]).component.typeId, false}};
                data.references = {{{{0}, 0}, PrefabPropertyId::Create(1).Value(), CookedPrefabEntitySlot{1}},
                                   {{{0}, 0}, PrefabPropertyId::Create(2).Value(), CookedPrefabMemberSlot{{1}, 0}},
                                   {{{0}, 0}, PrefabPropertyId::Create(3).Value(), CookedPrefabAssetSlot{0}},
                                   {{{0}, 0}, PrefabPropertyId::Create(4).Value(), CookedPrefabBindingSlot{0}},
                                   {{{0}, 0}, PrefabPropertyId::Create(5).Value(), CookedPrefabBindingSlot{1}}};
            }
            auto cooked = CookedPrefab::Create(std::move(data), Profile());
            REQUIRE(cooked.HasValue());
            const auto payload = cooked.Value().Bytes();
            std::vector<std::uint8_t> encoded;
            encoded.reserve(payload.size());
            for (auto byte : payload)
                encoded.push_back(std::to_integer<std::uint8_t>(byte));
            return Envelope(id, "core.prefab", std::move(encoded));
        }

        Runtime::FrameContext Context() const {
            return {1, {}, 0.0, 0, {}, false, frameCancellation};
        }

        void Commit() {
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context()).HasValue());
        }

        void Activate() {
            Runtime::SceneDefinitionBuilder builder{{7}, {1}};
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            REQUIRE(scenes.QueuePreparation(std::move(definition).Value()).HasValue());
            Commit();
            REQUIRE(scenes.ActiveScene().has_value());
        }

        PrefabTemplateLoadHandle Load(Assets::AssetId id = Test::Asset()) {
            const auto encoded = id == Test::Asset() ? root : Root(id);
            auto loaded = provider.LoadAsync({id, Digest(encoded), Target()});
            REQUIRE(loaded.HasValue());
            return std::move(loaded).Value();
        }

        template <typename Predicate> void PumpUntil(Predicate complete) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            bool finished = complete();
            while (!finished && std::chrono::steady_clock::now() < deadline) {
                REQUIRE(provider.Advance().HasValue());
                std::this_thread::yield();
                finished = complete();
            }
            REQUIRE(finished);
        }

        PrefabTemplateLease Ready() {
            auto handle = Load();
            PumpUntil([&handle] {
                return handle.State() != PrefabTemplateLoadState::Loading;
            });
            auto lease = provider.TakeResult(handle);
            REQUIRE(lease.HasValue());
            return std::move(lease).Value();
        }
    };

    /** @brief Complete cooked interface fixture; each malformed case changes one independent contract. */
    struct ProjectionFixture final {
        Fixture fixture;
        PrefabTemplateLease lease;
        std::vector<Runtime::RuntimeComponentSet> components{2};
        PrefabPreparedGroupOptions options;
        Runtime::EntityRef external{fixture.scenes.ActiveScene()->RuntimeId(), {0, 1}};

        ProjectionFixture() {
            fixture.root = fixture.Root(Test::Asset(), true);
            fixture.bytes.Memory().Insert(Test::Asset(), fixture.root);
            lease = fixture.Ready();
            auto behavior = std::get<Gameplay::BehaviorComponent>(lease.Template()->Data().entities[0].members[0]);
            behavior.instanceId = {42};
            const auto &raw = std::get<RawComponentPayload>(lease.Template()->Data().entities[1].members[0]);
            components[0].behaviors.push_back(behavior);
            components[1].gameplayComponents.push_back(raw.component);
            options.members = {{{behavior.typeId, behavior.instanceId.value}}, {{raw.component.typeId, raw.instance.Value()}}};
            options.references = {{{0, 1, Runtime::RuntimeGroupEntitySlot{1}},
                                   {0, 2, Runtime::RuntimeGroupMemberSlot{{1}, 0}},
                                   {0, 3, lease.Dependencies()[0].metadata},
                                   {0, 4, Runtime::RuntimeGroupExternalReference{external, {}}},
                                   {0, 5, std::monostate{}}},
                                  {}};
        }

        void RejectWithoutQueuing() {
            const auto queued = fixture.provider.QueuePreparedGroup(lease, std::move(components), {}, std::move(options));
            REQUIRE(queued.HasError());
            fixture.Commit();
            CHECK(fixture.scenes.ActiveScene()->SlotCount() == 0);
            CHECK_FALSE(fixture.scenes.TakeStructuralCommitResult());
        }
    };

}  // namespace Horo::Prefab::TestSupport
