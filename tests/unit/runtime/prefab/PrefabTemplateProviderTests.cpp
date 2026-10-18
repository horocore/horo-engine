#include "PrefabTemplateProviderTestSupport.h"

namespace Horo::Prefab {
    namespace {
        using namespace TestSupport;

        struct TransactionEvidence final {
            std::size_t prepared{};
            std::size_t rolledBack{};
            std::size_t published{};
            std::size_t notified{};
        };

        /** @brief Contract fault injector; real subsystem adapters are validated separately. */
        class TransactionParticipant final : public Runtime::SceneStructuralParticipant {
        public:
            explicit TransactionParticipant(TransactionEvidence &evidence, CancellationSource *cancel = nullptr)
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

                Candidate(const Candidate &) = delete;
                Candidate &operator=(const Candidate &) = delete;

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
        std::as_writable_bytes(std::span{corrupt}).back() ^= std::byte{1};
        fixture.bytes.Memory().Insert(Test::Asset(), corrupt);
        auto handle = fixture.Load();
        fixture.PumpUntil([&handle] {
            return handle.State() != PrefabTemplateLoadState::Loading;
        });
        CHECK(fixture.provider.TakeResult(handle).HasError());
        CHECK(fixture.provider.PayloadSnapshot().retainedPayloadBytes == 0);
        auto mistyped = Envelope(Test::Asset(), "core.mesh", {1});
        fixture.bytes.Memory().Insert(Test::Asset(), mistyped);
        auto wrong = fixture.provider.LoadAsync({Test::Asset(), Digest(mistyped), Target()});
        REQUIRE(wrong.HasValue());
        auto wrongHandle = std::move(wrong).Value();
        fixture.PumpUntil([&wrongHandle] {
            return wrongHandle.State() != PrefabTemplateLoadState::Loading;
        });
        CHECK(fixture.provider.TakeResult(wrongHandle).HasError());
    }

    TEST_CASE("Dropped observation cannot refund a still-running load slot", "[prefab][provider]") {
        Fixture fixture({.maximumOutstanding = 1, .maximumConcurrentLoads = 1});
        fixture.bytes.Block();
        {
            auto abandoned = fixture.Load();
            fixture.PumpUntil([&fixture] {
                return fixture.bytes.Entered();
            });
        }
        REQUIRE(fixture.provider.Advance().HasValue());
        CHECK(fixture.provider.LoadAsync({Test::Asset(), Digest(fixture.root), Target()}).HasError());
        fixture.bytes.Release();
        fixture.PumpUntil([&fixture] {
            return fixture.provider.LoadAsync({Test::Asset(), Digest(fixture.root), Target()}).HasValue();
        });
    }

    TEST_CASE("Evicted externally held templates still occupy finite allocation capacity", "[prefab][provider]") {
        Fixture fixture({.maximumTemplates = 1});
        auto lease = fixture.Ready();
        fixture.provider.Evict(Test::Asset());
        auto different = fixture.Load(Test::Asset(3));
        fixture.PumpUntil([&different] {
            return different.State() != PrefabTemplateLoadState::Loading;
        });
        CHECK(fixture.provider.TakeResult(different).HasError());
        lease = {};
        auto retry = fixture.Load(Test::Asset(3));
        fixture.PumpUntil([&retry] {
            return retry.State() != PrefabTemplateLoadState::Loading;
        });
        CHECK(fixture.provider.TakeResult(retry).HasValue());
    }

    TEST_CASE("Provider shutdown does not join a blocked shared-loader worker", "[prefab][provider]") {
        Fixture fixture;
        fixture.bytes.Block();
        auto pending = fixture.Load();
        fixture.PumpUntil([&fixture] {
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
        fixture.PumpUntil([&fixture] {
            return fixture.bytes.Entered();
        });
        replaced = std::move(retained);
        REQUIRE(fixture.provider.Advance().HasValue());
        CHECK(fixture.provider.LoadAsync({Test::Asset(), Digest(fixture.root), Target()}).HasError());
        fixture.bytes.Release();
        fixture.PumpUntil([&replaced] {
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
        const auto destroyed = fixture.scenes.QueueStructuralCommands(std::move(destruction));
        REQUIRE(destroyed.HasValue());
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

    TEST_CASE("Source cooked template prepares through the existing immutable provider", "[prefab][provider][template-cook]") {
        Fixture fixture;
        PrefabDocumentData source;
        source.projectVersion = Application::ParseHoroVersion("1.2.3").Value();
        source.assetId = Test::Asset();
        source.objects = {{.localId = {0}, .name = "Root"}, {.localId = {7}, .parentLocalId = LocalObjectId{0}, .name = "Child"}};
        source.objects[1].localTransform.translation = {4, 5, 6};
        source.referencedAssets = {Test::Asset(2)};
        auto document = PrefabDocument::Create(std::move(source), Profile());
        REQUIRE(document.HasValue());
        const auto canonical = document.Value().SerializeCanonical().Value();
        const PrefabSourceRevision revision{document.Value().Data().projectVersion,
                                            ComputeSha256(std::as_bytes(std::span(canonical.data(), canonical.size())))};
        auto sources = BuildPrefabSourceResolverSnapshot(fixture.registry.Snapshot(), {{std::move(document).Value(), revision}}, Profile());
        REQUIRE(sources.HasValue());
        const PrefabTemplateCookResource resource{Test::Asset(2), fixture.dependency};
        auto cooked =
            CookPrefabTemplate(sources.Value(), fixture.registry.Snapshot(), Test::Asset(), std::span{&resource, 1}, Target(), Profile());
        REQUIRE(cooked.HasValue());
        std::vector<std::uint8_t> payload;
        for (const auto byte : cooked.Value().Bytes())
            payload.push_back(std::to_integer<std::uint8_t>(byte));
        fixture.root = Envelope(Test::Asset(), "core.prefab", std::move(payload));
        fixture.bytes.Memory().Insert(Test::Asset(), fixture.root);
        auto lease = fixture.Ready();
        REQUIRE(lease.Template() != nullptr);
        CHECK(lease.Template()->Data() == cooked.Value().Data());
        REQUIRE(lease.Dependencies().size() == 1);
        CHECK(lease.Dependencies()[0].metadata.id == Test::Asset(2));
        REQUIRE(fixture.provider.QueuePreparedGroup(lease, std::vector<Runtime::RuntimeComponentSet>(2)).HasValue());
        fixture.Commit();
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == 2);
        fixture.provider.Shutdown();
        CHECK(lease.Template()->Data() == cooked.Value().Data());
    }
}  // namespace Horo::Prefab
