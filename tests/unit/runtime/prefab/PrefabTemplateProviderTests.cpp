#include "Horo/Assets/AssetCook.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabTemplateProvider.h"
#include "PrefabTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Horo::Prefab {
    namespace {
        using namespace std::chrono_literals;

        PrefabLimitProfile Profile() {
            return PrefabLimitProfile::Create({}).Value();
        }

        AssetCookTargetId Target() {
            return AssetCookTargetId::Parse("linux-x64").Value();
        }

        Assets::AssetTypeId Type(std::string_view text) {
            return Assets::AssetTypeId::Parse(text).Value();
        }

        Sha256Digest Digest(std::span<const std::uint8_t> bytes) {
            return ComputeSha256(std::as_bytes(bytes));
        }

        std::vector<std::uint8_t> Envelope(Assets::AssetId id, std::string_view type, std::vector<std::uint8_t> payload) {
            auto artifact = Assets::EncodeCookedArtifact({id, Type(type), Target(), {}, {}, Digest(payload), std::move(payload)});
            REQUIRE(artifact.HasValue());
            return std::move(artifact).Value();
        }

        /** @brief Worker gate deliberately ignores cancellation until released, proving provider teardown never joins it. */
        class GatedProvider final : public Assets::IAssetProvider {
        public:
            Assets::MemoryAssetProvider memory;

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
                return memory.Exists(id, token);
            }

            Result<std::vector<std::uint8_t>> Load(Assets::AssetId id, const CancellationToken &token) const override {
                {
                    std::unique_lock lock{mutex_};
                    entered_ = true;
                    wake_.wait(lock, [&] {
                        return !blocked_;
                    });
                }
                return memory.Load(id, token);
            }

        private:
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

            explicit Fixture(PrefabTemplateProviderLimits limits = {},
                             std::unique_ptr<Runtime::SceneStructuralParticipant> participant = {})
                : provider(registry, loads, scenes, Profile(), limits) {
                for (std::uint16_t id = 1; id <= 3; ++id) {
                    const auto name = "assets/asset" + std::to_string(id) + (id == 2 ? ".obj" : ".prefab");
                    records.push_back({Test::Asset(id), Type(id == 2 ? "core.mesh" : "core.prefab"), ProjectPath::Parse(name).Value(),
                                       ProjectPath::Parse(name + ".horo").Value()});
                }
                const auto publication = registry.Publish(records);
                REQUIRE(publication.status == Assets::AssetRegistryBuildStatus::Complete);
                REQUIRE(publication.registeredAssets == records.size());
                REQUIRE(publication.publishedRevision.value != 0);
                dependency = Envelope(Test::Asset(2), "core.mesh", {1, 2, 3});
                bytes.memory.Insert(Test::Asset(2), dependency);
                root = Root(Test::Asset());
                bytes.memory.Insert(Test::Asset(), root);
                bytes.memory.Insert(Test::Asset(3), Root(Test::Asset(3)));
                if (participant)
                    REQUIRE(scenes.AddStructuralParticipant(std::move(participant)).HasValue());
                REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
                Activate();
                REQUIRE(provider.Startup(cancellation.Token()).HasValue());
            }

            // Always opens the worker gate before borrowed loader/job owners are destroyed, including fatal assertions.
            ~Fixture() {
                bytes.Release();
            }

            std::vector<std::uint8_t> Root(Assets::AssetId id) {
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
                auto cooked = CookedPrefab::Create(std::move(data), Profile());
                REQUIRE(cooked.HasValue());
                const auto payload = cooked.Value().Bytes();
                std::vector<std::uint8_t> encoded;
                encoded.reserve(payload.size());
                for (auto byte : payload)
                    encoded.push_back(std::to_integer<std::uint8_t>(byte));
                return Envelope(id, "core.prefab", std::move(encoded));
            }

            Runtime::FrameContext Context() {
                return {1, {}, 0.0, 0, {}, false, cancellation.Token()};
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
                PumpUntil([&] {
                    return handle.State() != PrefabTemplateLoadState::Loading;
                });
                auto lease = provider.TakeResult(handle);
                REQUIRE(lease.HasValue());
                return std::move(lease).Value();
            }
        };

        struct TransactionEvidence final {
            std::size_t prepared{};
            std::size_t rolledBack{};
            std::size_t published{};
            std::size_t notified{};
        };

        /** @brief Contract fault injector; real subsystem adapters are validated separately. */
        class TransactionParticipant final : public Runtime::SceneStructuralParticipant {
        public:
            TransactionParticipant(TransactionEvidence &evidence, CancellationSource *cancel = nullptr)
                : evidence_(evidence), cancel_(cancel) {}

            Runtime::SceneStructuralOwner Owner() const noexcept override {
                return Runtime::SceneStructuralOwner::Gameplay;
            }

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(Runtime::RuntimeSceneView,
                                                                               std::span<const Runtime::RuntimeEntityView> created,
                                                                               std::span<const Runtime::EntityRef> destroyed) override {
                REQUIRE((created.size() == 2 || destroyed.size() == 2));
                if (!created.empty())
                    REQUIRE(created[1].parent == created[0].entity);
                ++evidence_.prepared;
                if (cancel_)
                    cancel_->RequestCancellation();
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Success(std::make_unique<Candidate>(evidence_));
            }

        private:
            class Candidate final : public Runtime::SceneStructuralCandidate {
            public:
                explicit Candidate(TransactionEvidence &evidence) : evidence_(evidence) {}

                ~Candidate() override {
                    if (!published_)
                        ++evidence_.rolledBack;
                }

                Result<void> ValidatePublication() const override {
                    return Result<void>::Success();
                }

                void Publish() noexcept override {
                    published_ = true;
                    ++evidence_.published;
                }

                Result<void> AfterPublication() override {
                    ++evidence_.notified;
                    return Result<void>::Success();
                }

            private:
                TransactionEvidence &evidence_;
                bool published_{};
            };

            TransactionEvidence &evidence_;
            CancellationSource *cancel_{};
        };
    }  // namespace

    TEST_CASE("Runtime template retains exact canonical root and dependency allocations", "[prefab][provider]") {
        Fixture fixture;
        auto lease = fixture.Ready();
        REQUIRE(lease.Template());
        CHECK(lease.Template()->GetObjectCount() == 2);
        CHECK(lease.RegistryRevision() == fixture.registry.Snapshot().Revision());
        CHECK(lease.Scene() == fixture.scenes.ActiveScene()->RuntimeId());
        CHECK(lease.Artifact().Digest() == Digest(fixture.root));
        REQUIRE(lease.Dependencies().size() == 1);
        CHECK(lease.Dependencies()[0].artifact.Digest() == Digest(fixture.dependency));
        const auto retained = fixture.provider.PayloadSnapshot().retainedPayloadBytes;
        fixture.provider.Evict(Test::Asset());
        CHECK(fixture.provider.PayloadSnapshot().retainedPayloadBytes == retained);
        CHECK(fixture.provider.ValidateAdmission(lease).HasValue());
        fixture.provider.Shutdown();
        CHECK(fixture.provider.ValidateAdmission(lease).HasError());
        CHECK(lease.Template()->GetObjectCount() == 2);
        CHECK(!lease.Artifact().Bytes().empty());
    }

    TEST_CASE("Foreign and stale template requests cannot acquire scene admission", "[prefab][provider]") {
        Fixture fixture;
        auto lease = fixture.Ready();
        PrefabTemplateProvider other(fixture.registry, fixture.loads, fixture.scenes, Profile());
        CHECK(other.ValidateAdmission(lease).HasError());
        auto request = fixture.Load();
        CHECK(other.TakeResult(request).HasError());
        REQUIRE(fixture.registry.Publish(fixture.records).status == Assets::AssetRegistryBuildStatus::Complete);
        CHECK(fixture.provider.ValidateAdmission(lease).HasError());
        CHECK(fixture.provider.TakeResult(request).HasError());
        REQUIRE(fixture.scenes.QueueUnload().HasValue());
        fixture.Commit();
        CHECK(fixture.provider.ValidateAdmission(lease).HasError());
    }

    TEST_CASE("Malformed or wrong-generation canonical bytes never become a template lease", "[prefab][provider]") {
        Fixture fixture;
        auto corrupt = fixture.root;
        corrupt.back() ^= 1;
        fixture.bytes.memory.Insert(Test::Asset(), corrupt);
        auto handle = fixture.Load();
        fixture.PumpUntil([&] {
            return handle.State() != PrefabTemplateLoadState::Loading;
        });
        CHECK(fixture.provider.TakeResult(handle).HasError());
        CHECK(fixture.provider.PayloadSnapshot().retainedPayloadBytes == 0);
        auto mistyped = Envelope(Test::Asset(), "core.mesh", {1});
        fixture.bytes.memory.Insert(Test::Asset(), mistyped);
        auto wrong = fixture.provider.LoadAsync({Test::Asset(), Digest(mistyped), Target()});
        REQUIRE(wrong.HasValue());
        auto wrongHandle = std::move(wrong).Value();
        fixture.PumpUntil([&] {
            return wrongHandle.State() != PrefabTemplateLoadState::Loading;
        });
        CHECK(fixture.provider.TakeResult(wrongHandle).HasError());
    }

    TEST_CASE("Dropped observation cannot refund a still-running load slot", "[prefab][provider]") {
        Fixture fixture({.maximumOutstanding = 1, .maximumConcurrentLoads = 1});
        fixture.bytes.Block();
        {
            auto abandoned = fixture.Load();
            fixture.PumpUntil([&] {
                return fixture.bytes.Entered();
            });
        }
        REQUIRE(fixture.provider.Advance().HasValue());
        CHECK(fixture.provider.LoadAsync({Test::Asset(), Digest(fixture.root), Target()}).HasError());
        fixture.bytes.Release();
        fixture.PumpUntil([&] {
            return fixture.provider.LoadAsync({Test::Asset(), Digest(fixture.root), Target()}).HasValue();
        });
    }

    TEST_CASE("Evicted externally held templates still occupy finite allocation capacity", "[prefab][provider]") {
        Fixture fixture({.maximumTemplates = 1});
        auto lease = fixture.Ready();
        fixture.provider.Evict(Test::Asset());
        auto different = fixture.Load(Test::Asset(3));
        fixture.PumpUntil([&] {
            return different.State() != PrefabTemplateLoadState::Loading;
        });
        CHECK(fixture.provider.TakeResult(different).HasError());
        lease = {};
        auto retry = fixture.Load(Test::Asset(3));
        fixture.PumpUntil([&] {
            return retry.State() != PrefabTemplateLoadState::Loading;
        });
        CHECK(fixture.provider.TakeResult(retry).HasValue());
    }

    TEST_CASE("Provider shutdown does not join a blocked shared-loader worker", "[prefab][provider]") {
        Fixture fixture;
        fixture.bytes.Block();
        auto pending = fixture.Load();
        fixture.PumpUntil([&] {
            return fixture.bytes.Entered();
        });
        fixture.provider.Shutdown();
        CHECK(pending.State() == PrefabTemplateLoadState::Cancelled);
        CHECK(fixture.provider.TakeResult(pending).HasError());
        fixture.bytes.Release();
    }

    TEST_CASE("Move assignment cancels the replaced request without abandoning worker accounting", "[prefab][provider]") {
        Fixture fixture({.maximumOutstanding = 2, .maximumConcurrentLoads = 2});
        fixture.bytes.Block();
        auto replaced = fixture.Load();
        auto retained = fixture.Load(Test::Asset(3));
        fixture.PumpUntil([&] {
            return fixture.bytes.Entered();
        });
        replaced = std::move(retained);
        REQUIRE(fixture.provider.Advance().HasValue());
        CHECK(fixture.provider.LoadAsync({Test::Asset(), Digest(fixture.root), Target()}).HasError());
        fixture.bytes.Release();
        fixture.PumpUntil([&] {
            return replaced.State() != PrefabTemplateLoadState::Loading;
        });
        auto result = fixture.provider.TakeResult(replaced);
        REQUIRE(result.HasValue());
        CHECK(result.Value().Template()->GetAssetId() == Test::Asset(3));
    }

    TEST_CASE("Scene owns complete group topology and keeps resources until its final entity retires", "[prefab][provider][transaction]") {
        TransactionEvidence evidence;
        Fixture fixture({}, std::make_unique<TransactionParticipant>(evidence));
        auto lease = fixture.Ready();
        auto queued = fixture.provider.QueuePreparedGroup(lease, std::vector<Runtime::RuntimeComponentSet>(2));
        REQUIRE(queued.HasValue());
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == 0);
        fixture.Commit();
        auto committed = fixture.scenes.TakeStructuralCommitResult();
        REQUIRE(committed);
        REQUIRE(committed->created.size() == 2);
        const auto first = committed->created[0].entity;
        const auto second = committed->created[1].entity;
        CHECK(fixture.scenes.ActiveScene()->Get(second).Value().parent == first);
        CHECK(evidence.prepared == 1);
        CHECK(evidence.published == 1);
        CHECK(evidence.notified == 1);
        fixture.provider.Evict(Test::Asset());
        lease = {};
        CHECK(fixture.provider.PayloadSnapshot().retainedPayloadBytes > 0);
        Runtime::SceneCommandBuffer destruction;
        destruction.Destroy(second);
        destruction.Destroy(first);
        REQUIRE(fixture.scenes.QueueStructuralCommands(std::move(destruction)).HasValue());
        fixture.Commit();
        CHECK(fixture.provider.PayloadSnapshot().retainedPayloadBytes == 0);
    }

    TEST_CASE("Cancellation during owner preparation rejects the entire group at the final publication fence",
              "[prefab][provider][transaction]") {
        CancellationSource cancel;
        TransactionEvidence evidence;
        Fixture fixture({}, std::make_unique<TransactionParticipant>(evidence, &cancel));
        auto lease = fixture.Ready();
        REQUIRE(fixture.provider.QueuePreparedGroup(lease, std::vector<Runtime::RuntimeComponentSet>(2), cancel.Token()).HasValue());
        fixture.Commit();
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == 0);
        CHECK(!fixture.scenes.TakeStructuralCommitResult());
        CHECK(fixture.scenes.TakeOperationError().has_value());
        CHECK(evidence.prepared == 1);
        CHECK(evidence.rolledBack == 1);
        CHECK(evidence.published == 0);
        CHECK(evidence.notified == 0);
    }

    TEST_CASE("Queued group cannot publish under a replaced catalog or a closed provider", "[prefab][provider][transaction]") {
        Fixture fixture;
        auto lease = fixture.Ready();
        REQUIRE(fixture.provider.QueuePreparedGroup(lease, std::vector<Runtime::RuntimeComponentSet>(2)).HasValue());
        REQUIRE(fixture.registry.Publish(fixture.records).status == Assets::AssetRegistryBuildStatus::Complete);
        fixture.Commit();
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == 0);
        CHECK(fixture.scenes.TakeOperationError().has_value());
        lease = fixture.Ready();
        REQUIRE(fixture.provider.QueuePreparedGroup(lease, std::vector<Runtime::RuntimeComponentSet>(2)).HasValue());
        fixture.provider.Shutdown();
        fixture.Commit();
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == 0);
        CHECK(fixture.scenes.TakeOperationError().has_value());
    }
}  // namespace Horo::Prefab
