#include "Horo/Assets/AssetCookTransaction.h"
#include "navigation/NavigationPublicationFixture.h"

#if !defined(_WIN32)
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Horo::Application {
    using namespace TestSupport;

    namespace {
        /** @brief Compares every serialized tile identity and topology with the worker-owned publication. */
        void CheckCompletePublication(const PublicationHarness &harness, const Assets::AssetCookGeneration &generation,
                                      const NavigationBakePublication &expected) {
            const auto decoded = DecodePublication(harness, generation);
            CHECK(decoded.inputFingerprint == expected.tiles.inputFingerprint);
            REQUIRE(decoded.tiles.size() == expected.tiles.tiles.size());
            for (std::size_t i = 0; i < decoded.tiles.size(); ++i) {
                CHECK(decoded.tiles[i]->Key() == expected.tiles.tiles[i]->Key());
                CHECK(decoded.tiles[i]->DependencyKey() == expected.tiles.tiles[i]->DependencyKey());
                CHECK(decoded.tiles[i]->ContentIdentity() == expected.tiles.tiles[i]->ContentIdentity());
                CHECK(std::ranges::equal(decoded.tiles[i]->Bytes(), expected.tiles.tiles[i]->Bytes()));
            }
        }

        /** @brief Publishes another real standard envelope through the sole AssetCook writer authority. */
        [[nodiscard]] std::vector<std::uint8_t> PublishUnrelated(PublicationHarness &harness) {
            const auto id = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000002").Value();
            const auto type = Assets::AssetTypeId::Parse("core.mesh").Value();
            const std::vector<std::uint8_t> payload{4, 5, 6, 7};
            auto encoded = Assets::EncodeCookedArtifact({.id = id,
                                                         .type = type,
                                                         .target = harness.config.target,
                                                         .cacheKeyDigest = Navigation::TestSupport::Digest(70),
                                                         .sourceDigest = Navigation::TestSupport::Digest(71),
                                                         .payloadDigest = ComputeSha256(std::as_bytes(std::span{payload})),
                                                         .payload = payload});
            REQUIRE(encoded.HasValue());
            auto bytes = std::move(encoded).Value();
            const Assets::AssetCookManifestEntry entry{.assetId = id,
                                                       .assetType = type,
                                                       .artifactFile = id.ToString() + ".cooked",
                                                       .artifactHash = ComputeSha256(std::as_bytes(std::span{bytes}))};
            REQUIRE(Assets::PublishCookArtifactReplacement(harness.config.targetRoot, harness.config.target, entry, bytes,
                                                           harness.config.maximumCandidateBytes, harness.config.cookLimits,
                                                           {.files = harness.files.get(), .newOperationId = harness.config.newOperationId})
                        .HasValue());
            return bytes;
        }

#if !defined(_WIN32)
        /** @brief Holds the writer lease in a distinct process created before any test scheduler threads. */
        class ChildWriterLease final {
        public:
            explicit ChildWriterLease(const std::filesystem::path &lockPath) {
                REQUIRE(pipe(ready_) == 0);
                REQUIRE(pipe(release_) == 0);
                child_ = fork();
                REQUIRE(child_ >= 0);
                if (child_ == 0) {
                    close(ready_[0]);
                    close(release_[1]);
                    NativeDurableFileSystem files;
                    auto lease = files.TryAcquireExclusive(lockPath, "navigation child publication owner");
                    const char acquired = lease.HasValue() ? '1' : '0';
                    if (write(ready_[1], &acquired, 1) != 1)
                        _exit(2);
                    char done{};
                    if (read(release_[0], &done, 1) != 1)
                        _exit(3);
                    _exit(lease.HasValue() ? 0 : 4);
                }
                close(ready_[1]);
                close(release_[0]);
                pollfd descriptor{.fd = ready_[0], .events = POLLIN, .revents = 0};
                const int signalled = poll(&descriptor, 1, 5000);
                char acquired{};
                const bool held = signalled == 1 && read(ready_[0], &acquired, 1) == 1 && acquired == '1';
                close(ready_[0]);
                if (!held)
                    Release();
                REQUIRE(held);
            }

            ChildWriterLease(const ChildWriterLease &) = delete;
            ChildWriterLease &operator=(const ChildWriterLease &) = delete;

            ~ChildWriterLease() {
                Release();
            }

            void Release() noexcept {
                if (child_ <= 0)
                    return;
                const char done = '1';
                const auto ignored = write(release_[1], &done, 1);
                (void)ignored;
                close(release_[1]);
                int status{};
                const auto waited = waitpid(child_, &status, 0);
                (void)waited;
                child_ = 0;
            }

        private:
            int ready_[2]{};
            int release_[2]{};
            pid_t child_{};
        };
#endif
    }  // namespace

    TEST_CASE("Disk readers pin complete old or new navigation closures across the actual pointer rename") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto prior = harness.service->Published();
        const auto pinned = harness.Current();
        const auto priorBytes = harness.Contents(pinned).artifacts;
        harness.fixture.ExcludeBorder();
        harness.files->beforeCurrentReached.store(false);
        harness.files->pauseBeforeCurrent.store(true);
        PublicationPause pause{harness.files};
        const auto replacement = harness.Submit();
        REQUIRE(WaitFor(harness.files->beforeCurrentReached));
        CHECK(harness.Current().manifestDigest == pinned.manifestDigest);
        CheckCompletePublication(harness, harness.Current(), *prior);
        harness.files->pauseBeforeCurrent.store(false);
        REQUIRE(harness.Terminal(replacement).state == OperationState::Succeeded);
        const auto after = harness.service->Published();
        CHECK(after->generation.manifestDigest != pinned.manifestDigest);
        CheckCompletePublication(harness, harness.Current(), *after);
        CheckCompletePublication(harness, pinned, *prior);
        CHECK(harness.Contents(pinned).artifacts == priorBytes);
        auto reader = ResolveNavigationBakePublication(harness.config);
        REQUIRE(reader.HasValue());
        CHECK(reader.Value()->generation.manifestDigest == after->generation.manifestDigest);
        CHECK(reader.Value()->tiles.inputFingerprint == after->tiles.inputFingerprint);
        CHECK(after->tiles.tiles[2]->ContentIdentity() == prior->tiles.tiles[2]->ContentIdentity());
    }

    TEST_CASE("Identical real navigation replay preserves deterministic manifest and envelope bytes") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto first = harness.Current();
        const auto bytes = harness.Contents(first).artifacts;
        harness.service->Close();
        harness.service = NavigationBakeService::Create(harness.config, harness.operations, harness.jobs).Value();
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        CHECK(harness.Current().manifestDigest == first.manifestDigest);
        CHECK(harness.Contents(harness.Current()).artifacts == bytes);
        CHECK(harness.service->Published()->reusedTiles == 4);
        CheckCompletePublication(harness, first, *harness.service->Published());
    }

    TEST_CASE("Atomic navigation closure carries the current unrelated artifact with its exact envelope") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto bytes = PublishUnrelated(harness);
        harness.fixture.ExcludeBorder();
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto contents = harness.Contents(harness.Current());
        REQUIRE(contents.entries.size() == 2);
        CHECK(contents.artifacts.back() == bytes);
        CHECK(contents.entries.back().artifactHash == ComputeSha256(std::as_bytes(std::span{bytes})));
        CheckCompletePublication(harness, harness.Current(), *harness.service->Published());
    }

    TEST_CASE("Navigation cancellation before current rename preserves disk and live publication") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto prior = harness.service->Published();
        const auto pointer = PublicationBytes(harness.config.targetRoot / "current.json");
        harness.fixture.ExcludeBorder();
        harness.files->beforeCurrentReached.store(false);
        harness.files->pauseBeforeCurrent.store(true);
        PublicationPause pause{harness.files};
        const auto operation = harness.Submit();
        REQUIRE(WaitFor(harness.files->beforeCurrentReached));
        SECTION("explicit cancellation before pointer rename") {
            REQUIRE(harness.operations.RequestCancel(operation));
        }
        SECTION("service shutdown before pointer rename") {
            harness.service->Close();
        }
        harness.files->pauseBeforeCurrent.store(false);
        CHECK(harness.Terminal(operation).state == OperationState::Cancelled);
        CHECK(harness.service->Published() == prior);
        CHECK(PublicationBytes(harness.config.targetRoot / "current.json") == pointer);
        CheckCompletePublication(harness, harness.Current(), *prior);
    }

    TEST_CASE("Cancellation after true current rename retains committed success and the same disk live receipt") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto prior = harness.Current();
        const auto priorPublication = harness.service->Published();
        harness.fixture.ExcludeBorder();
        harness.files->afterCurrentReached.store(false);
        harness.files->pauseAfterCurrent.store(true);
        PublicationPause pause{harness.files};
        const auto operation = harness.Submit();
        REQUIRE(WaitFor(harness.files->afterCurrentReached));
        CHECK(harness.Current().manifestDigest != prior.manifestDigest);
        CHECK(harness.files->native.TryAcquireExclusive(harness.config.targetRoot / ".cook-writer.lock", "reader adoption contender")
                  .HasError());
        SECTION("explicit cancellation after committed rename") {
            REQUIRE(harness.operations.RequestCancel(operation));
        }
        SECTION("service shutdown after committed rename") {
            harness.service->Close();
        }
        auto proposed = harness.fixture.revisions;
        proposed.scene = Navigation::TestSupport::Id<NavigationSceneDocumentRevision>(proposed.scene.Value() + 1);
        CHECK(harness.config.sourceAuthority->UpdateCurrent(proposed, harness.fixture.Observations()).HasError());
        harness.files->pauseAfterCurrent.store(false);
        const auto terminal = harness.Terminal(operation);
        CHECK(terminal.state == OperationState::Succeeded);
        CHECK_FALSE(terminal.error);
        CHECK(harness.Current().manifestDigest == harness.service->Published()->generation.manifestDigest);
        CheckCompletePublication(harness, prior, *priorPublication);
        CHECK(harness.config.sourceAuthority->UpdateCurrent(proposed, harness.fixture.Observations()).HasValue());
        CHECK(harness.files->native.TryAcquireExclusive(harness.config.targetRoot / ".cook-writer.lock", "after navigation adoption")
                  .HasValue());
    }

    TEST_CASE("Final navigation adoption rejects current source revisions changed after staging") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto prior = harness.service->Published();
        harness.fixture.ExcludeBorder();
        harness.files->beforeCurrentReached.store(false);
        harness.files->pauseBeforeCurrent.store(true);
        PublicationPause pause{harness.files};
        const auto operation = harness.Submit();
        REQUIRE(WaitFor(harness.files->beforeCurrentReached));
        auto changed = harness.fixture.revisions;
        changed.geometry = Navigation::TestSupport::Id<NavigationSourceSnapshotRevision>(changed.geometry.Value() + 1);
        REQUIRE(harness.config.sourceAuthority->UpdateCurrent(changed, harness.fixture.Observations()).HasValue());
        harness.files->pauseBeforeCurrent.store(false);
        const auto terminal = harness.Terminal(operation);
        CHECK(terminal.state == OperationState::Cancelled);
        REQUIRE(terminal.error);
        CHECK(terminal.error->code.Value() == NavigationErrors::BakeInputStale.code.Value());
        CHECK(harness.service->Published() == prior);
        CHECK(harness.Current().manifestDigest == prior->generation.manifestDigest);
    }

    TEST_CASE("Navigation revalidates source observations after waiting for the native writer lease") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto prior = harness.service->Published();
        auto acquired = harness.files->native.TryAcquireExclusive(harness.config.targetRoot / ".cook-writer.lock", "waiting source edit");
        REQUIRE(acquired.HasValue());
        auto owner = std::move(acquired).Value();
        harness.fixture.ExcludeBorder();
        harness.files->lockContended.store(false);
        const auto operation = harness.Submit();
        REQUIRE(WaitFor(harness.files->lockContended));
        auto sources = harness.fixture.Observations();
        sources.front().revision = Navigation::TestSupport::Id<NavigationSourceRevision>(2);
        REQUIRE(harness.config.sourceAuthority->UpdateCurrent(harness.fixture.revisions, sources).HasValue());
        owner = ExclusiveFileLock{};
        const auto terminal = harness.Terminal(operation);
        CHECK(terminal.state == OperationState::Cancelled);
        REQUIRE(terminal.error);
        CHECK(terminal.error->code.Value() == NavigationErrors::BakeInputStale.code.Value());
        CHECK(harness.service->Published() == prior);
        CHECK(harness.Current().manifestDigest == prior->generation.manifestDigest);
    }

    TEST_CASE("Navigation source authority rejects malformed evidence without replacing its valid adoption fence") {
        Navigation::TestSupport::IncrementalBakeFixture fixture;
        NavigationBakeSourceAuthority authority;
        REQUIRE(authority.UpdateCurrent(fixture.revisions, fixture.Observations()).HasValue());
        auto revisions = fixture.revisions;
        auto sources = fixture.Observations();
        SECTION("invalid revision") {
            revisions.scene = {};
        }
        SECTION("invalid producer identity") {
            sources.front().producer = {};
        }
        SECTION("invalid contribution revision") {
            sources.front().revision = {};
        }
        SECTION("unsupported producer kind") {
            sources.front().kind = NavigationSourceProducerKind::Count;
        }
        SECTION("duplicate observation identity") {
            sources.push_back(sources.front());
        }
        SECTION("bounded source observation count") {
            sources.resize(NavigationSourceGeometryLimits::MaximumContributions + 1, sources.front());
        }
        CHECK(authority.UpdateCurrent(revisions, std::move(sources)).HasError());
        CHECK(authority.TryAcquirePublication(*fixture.Input()).HasValue());
    }

    TEST_CASE("Navigation adoption guard excludes source mutation without blocking and releases with owned lifetime") {
        Navigation::TestSupport::IncrementalBakeFixture fixture;
        auto authority = std::make_shared<NavigationBakeSourceAuthority>();
        REQUIRE(authority->UpdateCurrent(fixture.revisions, fixture.Observations()).HasValue());
        {
            auto lease = authority->TryAcquirePublication(*fixture.Input());
            REQUIRE(lease.HasValue());
            auto updated = authority->UpdateCurrent(fixture.revisions, fixture.Observations());
            REQUIRE(updated.HasError());
            CHECK(updated.ErrorValue().code.Value() == NavigationErrors::BakeJobAdmissionRejected.code.Value());
            CHECK(authority->TryAcquirePublication(*fixture.Input()).HasError());
        }
        CHECK(authority->UpdateCurrent(fixture.revisions, fixture.Observations()).HasValue());
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CHECK(authority->TryAcquirePublication(*fixture.Input(), cancelled.Token()).HasError());
    }

    TEST_CASE("Native navigation publication failures before pointer commit retain the last valid serialized generation") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto prior = harness.service->Published();
        const auto pointer = PublicationBytes(harness.config.targetRoot / "current.json");
        harness.fixture.ExcludeBorder();
        SECTION("artifact durable write") {
            harness.files->fault.store(PublicationFault::ArtifactWrite);
        }
        SECTION("manifest durable write") {
            harness.files->fault.store(PublicationFault::ManifestWrite);
        }
        SECTION("current durable write") {
            harness.files->fault.store(PublicationFault::CurrentWrite);
        }
        SECTION("generation directory rename") {
            harness.files->fault.store(PublicationFault::GenerationRename);
        }
        SECTION("generation directory sync") {
            harness.files->fault.store(PublicationFault::DirectorySync);
        }
        SECTION("current atomic rename") {
            harness.files->fault.store(PublicationFault::CurrentRename);
        }
        const auto terminal = harness.Terminal(harness.Submit());
        CHECK(terminal.state == OperationState::Failed);
        REQUIRE(terminal.error);
        CHECK(harness.service->Published() == prior);
        CHECK(PublicationBytes(harness.config.targetRoot / "current.json") == pointer);
        CheckCompletePublication(harness, harness.Current(), *prior);
    }

    TEST_CASE("Postcommit durable confirmation failure remains a committed navigation receipt") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto prior = harness.Current();
        harness.fixture.ExcludeBorder();
        harness.files->afterCurrentReached.store(false);
        SECTION("filesystem returns failure after rename") {
            harness.files->fault.store(PublicationFault::AfterCurrentRename);
        }
        SECTION("filesystem throws a nonstandard exception after rename") {
            harness.files->fault.store(PublicationFault::AfterCurrentThrow);
        }
        CHECK(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto committed = harness.service->Published();
        REQUIRE(committed->generation.durabilityError);
        CHECK_FALSE(committed->generation.durabilityError->code.Value().empty());
        if (harness.files->fault.load() == PublicationFault::AfterCurrentRename)
            CHECK(committed->generation.durabilityError->code.Value() == NavigationErrors::BakeInputFailed.code.Value());
        CHECK(harness.Current().manifestDigest == committed->generation.manifestDigest);
        CHECK(committed->generation.manifestDigest != prior.manifestDigest);
        CheckCompletePublication(harness, harness.Current(), *committed);
    }

    TEST_CASE("Corrupt current navigation authority is rejected without selecting another generation") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto prior = harness.service->Published();
        const auto pointer = harness.config.targetRoot / "current.json";
        std::vector<std::uint8_t> malformed;
        SECTION("truncated pointer") {
            malformed = {'{', '"', 's'};
        }
        SECTION("non canonical malformed pointer") {
            malformed = {'{', '}'};
        }
        SECTION("oversized pointer") {
            malformed.resize(harness.config.cookLimits.maximumArtifactBytes + 1, 'x');
        }
        WritePublicationBytes(pointer, malformed);
        CHECK(Assets::ResolveCurrentCookGeneration(harness.config.targetRoot, harness.config.cookLimits).HasError());
        CHECK(ResolveNavigationBakePublication(harness.config).HasError());
        harness.fixture.ExcludeBorder();
        CHECK(harness.Terminal(harness.Submit()).state == OperationState::Failed);
        CHECK(harness.service->Published() == prior);
        CHECK(PublicationBytes(pointer) == malformed);
        CheckCompletePublication(harness, prior->generation, *prior);
    }

    TEST_CASE("Navigation reader rejects mutated manifest and truncated cooked envelope without accepting partial tiles") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto current = harness.Current();
        const auto contents = harness.Contents(current);
        SECTION("manifest digest mutation") {
            const std::vector<std::uint8_t> garbage{'{', '}'};
            WritePublicationBytes(current.generationRoot / "manifest.json", garbage);
        }
        SECTION("truncated serialized artifact") {
            auto bytes = contents.artifacts.front();
            bytes.resize(bytes.size() / 2);
            WritePublicationBytes(current.generationRoot / contents.entries.front().artifactFile, bytes);
        }
        CHECK(Assets::ReadCookGenerationContents(current, harness.config.maximumCandidateBytes, harness.config.cookLimits).HasError());
        CHECK(ResolveNavigationBakePublication(harness.config).HasError());
        harness.fixture.ExcludeBorder();
        CHECK(harness.Terminal(harness.Submit()).state == OperationState::Failed);
    }

    TEST_CASE("Production navigation reader enforces exact target type and lowered complete closure bounds") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        static_cast<void>(PublishUnrelated(harness));
        const auto prior = harness.Current();
        auto policy = harness.config;
        SECTION("wrong cook target") {
            policy.target = AssetCookTargetId::Parse("desktop-opengl").Value();
        }
        SECTION("wrong navigation envelope type") {
            policy.artifactType = Assets::AssetTypeId::Parse("core.mesh").Value();
        }
        SECTION("lowered complete encoded byte ceiling") {
            policy.maximumCandidateBytes = 16;
        }
        SECTION("lowered complete tile count ceiling") {
            policy.maximumTiles = 1;
        }
        SECTION("lowered retained tile byte ceiling") {
            policy.tileLimits.maximumOwnedBytes = 1;
        }
        SECTION("lowered generation artifact count ceiling") {
            policy.cookLimits.maximumAssets = 1;
        }
        CHECK(ResolveNavigationBakePublication(policy).HasError());
        CHECK(harness.Current().manifestDigest == prior.manifestDigest);
        CheckCompletePublication(harness, prior, *harness.service->Published());
    }

#if !defined(_WIN32)
    TEST_CASE("Native cross process writer ownership prevents navigation mutation and allows cancellation while waiting") {
        PublicationDirectory directory;
        ChildWriterLease owner(directory.path / "cooked" / ".cook-writer.lock");
        PublicationHarness harness(directory.path);
        CHECK(harness.files->native.TryAcquireExclusive(harness.config.targetRoot / ".cook-writer.lock", "competing parent").HasError());
        const auto operation = harness.Submit();
        REQUIRE(WaitFor(harness.files->lockContended));
        CHECK_FALSE(std::filesystem::exists(harness.config.targetRoot / "current.json"));
        REQUIRE(harness.operations.RequestCancel(operation));
        CHECK(harness.Terminal(operation).state == OperationState::Cancelled);
        CHECK_FALSE(std::filesystem::exists(harness.config.targetRoot / "current.json"));
        owner.Release();
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        CheckCompletePublication(harness, harness.Current(), *harness.service->Published());
    }

    TEST_CASE("Navigation reader rejects symlinked pointer and artifact without mutating the outside file") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto current = harness.Current();
        const auto contents = harness.Contents(current);
        const auto outside = directory.path / "outside retained bytes";
        const std::vector<std::uint8_t> outsideBytes{11, 22, 33};
        WritePublicationBytes(outside, outsideBytes);
        auto link = harness.config.targetRoot / "current.json";
        SECTION("current selector symlink") {}
        SECTION("generation artifact symlink") {
            link = current.generationRoot / contents.entries.front().artifactFile;
        }
        std::error_code error;
        REQUIRE(std::filesystem::remove(link, error));
        REQUIRE_FALSE(error);
        std::filesystem::create_symlink(outside, link, error);
        REQUIRE_FALSE(error);
        CHECK(ResolveNavigationBakePublication(harness.config).HasError());
        harness.fixture.ExcludeBorder();
        CHECK(harness.Terminal(harness.Submit()).state == OperationState::Failed);
        CHECK(PublicationBytes(outside) == outsideBytes);
    }

    TEST_CASE("Navigation publication rejects hardlinked authority files without mutating their aliases") {
        PublicationDirectory directory;
        PublicationHarness harness(directory.path);
        REQUIRE(harness.Terminal(harness.Submit()).state == OperationState::Succeeded);
        const auto current = harness.Current();
        const auto contents = harness.Contents(current);
        auto linked = harness.config.targetRoot / "current.json";
        SECTION("current selector hardlink") {}
        SECTION("generation artifact hardlink") {
            linked = current.generationRoot / contents.entries.front().artifactFile;
        }
        const auto alias = directory.path / "outside retained alias";
        std::error_code error;
        std::filesystem::create_hard_link(linked, alias, error);
        REQUIRE_FALSE(error);
        const auto before = PublicationBytes(alias);
        CHECK(ResolveNavigationBakePublication(harness.config).HasError());
        harness.fixture.ExcludeBorder();
        CHECK(harness.Terminal(harness.Submit()).state == OperationState::Failed);
        CHECK(PublicationBytes(alias) == before);
    }
#endif
}  // namespace Horo::Application
