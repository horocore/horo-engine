#include "../../support/AllocationProbe.h"
#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateHostPublicationFixture.h"

#include <algorithm>
#include <optional>
#include <thread>

namespace Horo::Application::TemplateHostTests {
    namespace {
        constexpr auto CommitPoint = Runtime::Ui::UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands;

        /** @brief Checks authoring and the actual provider-loaded runtime publish the same complete cooked revision. */
        void CheckPair(const UiTemplateHost &host, const UiTemplateHostCook &cook) {
            const auto runtime = host.Acquire();
            REQUIRE(runtime.HasValue());
            REQUIRE(host.IsCurrent(runtime.Value()).Value());
            const auto &actual = runtime.Value().Get()->Instance();
            CHECK(runtime.Value().CookedDocument().cacheIdentity == cook.Document().cacheIdentity);
            CHECK(actual.DocumentRevision() == host.Capture().Source().ordinary.Revision());
            CHECK(actual.DocumentRevision() == cook.Document().ordinary.SourceRevision());
            CHECK(std::ranges::equal(actual.Payload(), cook.Document().ordinary.Payload()));
        }
    }  // namespace

    TEST_CASE("Template host publishes detach and monotonic undo as complete runtime document pairs", "[template][host][publication]") {
        PublicationFixture fixture;
        auto host = fixture.Host();
        const auto old = host->Acquire();
        REQUIRE(old.HasValue());
        REQUIRE(host->IsCurrent(old.Value()).Value());
        const auto oldHandle = old.Value().Get()->Canvases().front().tree.Root().Value().handle;
        auto document = fixture.Detach(*host);
        const auto expected = fixture.Cook(document.Preview());
        auto prepared = fixture.Pair(*host, document);
        REQUIRE(host->Commit(prepared, CommitPoint).HasValue());
        CHECK(host->Capture().Source().instances.empty());
        CheckPair(*host, expected);
        CHECK(old.Value().Get()->Instance().State() == Runtime::Ui::UiRuntimeInstanceState::Retiring);
        CHECK_FALSE(host->IsCurrent(old.Value()).Value());
        CHECK(old.Value().CookedDocument().ordinary.SourceRevision().Value() == 1);
        CHECK(host->Acquire().Value().Get()->Canvases().front().tree.Get(oldHandle).HasError());
        CHECK(host->Commit(prepared, CommitPoint).HasError());
        auto undo = host->Authoring().PrepareUndo();
        REQUIRE(undo.HasValue());
        const auto restored = fixture.Cook(undo.Value().Preview());
        auto restore = fixture.Pair(*host, std::move(undo).Value());
        REQUIRE(host->Commit(restore, CommitPoint).HasValue());
        REQUIRE(host->Capture().Source().instances.size() == 1);
        const auto &accepted = host->Capture().Source().instances.front().accepted;
        CHECK(accepted.asset == fixture.source.reference.asset);
        CHECK(accepted.revision == fixture.source.reference.revision);
        CheckPair(*host, restored);
        CHECK(restored.Document().ordinary.SourceRevision().Value() == 3);
    }

    TEST_CASE("Template host cancellation and invalid cutoff preserve both publications and undo history",
              "[template][host][publication]") {
        PublicationFixture fixture;
        auto host = fixture.Host();
        const auto initial = fixture.Cook(host->Capture());
        CancellationSource cancellation;
        auto document = fixture.Detach(*host, cancellation.Token());
        auto paired = fixture.Pair(*host, document, cancellation.Token());
        SECTION("document cancellation") {
            cancellation.RequestCancellation();
        }
        SECTION("runtime cancellation") {
            CancellationSource runtimeCancellation;
            paired = fixture.Pair(*host, document, runtimeCancellation.Token());
            runtimeCancellation.RequestCancellation();
        }
        SECTION("invalid structural point") {
            CHECK(host->Commit(paired, static_cast<Runtime::Ui::UiStructuralCommitPoint>(255)).HasError());
            CheckPair(*host, initial);
            CHECK(host->Authoring().PrepareUndo().HasError());
            REQUIRE(host->Commit(paired, CommitPoint).HasValue());
            return;
        }
        CHECK(host->Commit(paired, CommitPoint).HasError());
        CheckPair(*host, initial);
        CHECK(host->Authoring().PrepareUndo().HasError());
    }

    TEST_CASE("Template source updates require explicit paired rebase and preserve valid authored arguments",
              "[template][host][publication][rebase]") {
        using namespace UiTemplates::TestSupport;
        PublicationFixture fixture;
        auto host = fixture.Host();
        const auto initial = fixture.Cook(host->Capture());
        const auto record = host->Capture().Source().instances.front();
        auto source = Source(2);
        SECTION("removed parameter blocks rebase without losing accepted data") {
            source.parameters.clear();
            const auto catalog = Catalog(std::move(source));
            CHECK(host->Authoring().PrepareRebase(record.id, Reference(2), catalog).HasError());
            CheckPair(*host, initial);
            CHECK(host->Capture().Source().instances.front().accepted.revision == record.accepted.revision);
            CHECK(std::get<std::string>(host->Capture().Source().instances.front().arguments.front().value) == "authored value");
            CHECK(host->Authoring().PrepareUndo().HasError());
        }
        SECTION("new content activates only after reviewed pair commit") {
            source.elements.push_back({Identity<UiTemplates::UiTemplateLocalElementId>(2), source.root, Kind(), {}});
            const auto provisional = Catalog(source);
            const auto bytes = UiTemplates::SerializeTemplateSource(*provisional, Reference(2));
            REQUIRE(bytes.HasValue());
            auto reference = Reference(2);
            reference.revision = ComputeSha256(std::as_bytes(std::span{bytes.Value()}));
            source.dependency.revision = reference.revision;
            const auto catalog = Catalog(std::move(source));
            REQUIRE(UiTemplates::SerializeTemplateSource(*catalog, reference).Value() == bytes.Value());
            fixture.source.WriteSource(bytes.Value());
            REQUIRE(host->Authoring().UpdateAvailable(record.id, reference).Value());
            CheckPair(*host, initial);
            CHECK(host->Capture().Source().instances.front().accepted.revision == record.accepted.revision);
            auto rebase = host->Authoring().PrepareRebase(record.id, reference, catalog);
            REQUIRE(rebase.HasValue());
            const auto expected = fixture.Cook(rebase.Value().Preview());
            auto paired = fixture.Pair(*host, std::move(rebase).Value());
            CheckPair(*host, initial);
            REQUIRE(host->Commit(paired, CommitPoint).HasValue());
            CHECK(host->Capture().Source().instances.front().accepted.revision == reference.revision);
            CHECK(std::get<std::string>(host->Capture().Source().instances.front().arguments.front().value) == "authored value");
            CHECK(host->Acquire().Value().Get()->Instance().Elements().size() == 3);
            CheckPair(*host, expected);
        }
    }

    TEST_CASE("Template host rejects foreign mismatched stale and moved candidates without split publication",
              "[template][host][publication]") {
        PublicationFixture fixture;
        auto host = fixture.Host();
        auto document = fixture.Detach(*host);
        const auto expected = fixture.Cook(document.Preview());
        SECTION("mismatched runtime bytes") {
            auto cook = fixture.Cook(host->Capture());
            auto runtime = fixture.Generation(cook);
            CHECK(host->Prepare(document, std::move(runtime), expected).HasError());
            CHECK(runtime.Instance().DocumentRevision().Value() == 1);
        }
        SECTION("foreign document owner") {
            PublicationFixture other;
            auto foreignHost = other.Host();
            CHECK_FALSE(foreignHost->IsCurrent(host->Acquire().Value()).Value());
            auto runtime = fixture.Generation(expected);
            CHECK(foreignHost->Prepare(document, std::move(runtime), expected).HasError());
        }
        auto first = fixture.Pair(*host, document);
        auto second = fixture.Pair(*host, document);
        auto moved = std::move(first);
        CHECK(host->Commit(first, CommitPoint).HasError());
        REQUIRE(host->Commit(moved, CommitPoint).HasValue());
        CHECK(host->Commit(second, CommitPoint).HasError());
        CheckPair(*host, expected);
    }

    TEST_CASE("Template host retirement backpressure permits retry only after actual leases drain", "[template][host][publication]") {
        PublicationFixture fixture;
        auto host = fixture.Host({1, 4});
        std::optional<UiTemplateRuntimeLease> old{std::move(host->Acquire()).Value()};
        {
            auto first = fixture.Pair(*host, fixture.Detach(*host));
            REQUIRE(host->Commit(first, CommitPoint).HasValue());
        }
        auto undo = host->Authoring().PrepareUndo();
        REQUIRE(undo.HasValue());
        auto second = fixture.Pair(*host, std::move(undo).Value());
        CHECK(host->Commit(second, CommitPoint).HasError());
        CHECK(host->Capture().Source().ordinary.Revision().Value() == 2);
        CHECK(host->CollectRetired().Value() == 0);
        old.reset();
        CHECK(host->CollectRetired().Value() == 1);
        REQUIRE(host->Commit(second, CommitPoint).HasValue());
        CHECK(host->Capture().Source().ordinary.Revision().Value() == 3);
    }

    TEST_CASE("Template host shutdown rejects late pairs and keeps external runtime leases readable", "[template][host][publication]") {
        PublicationFixture fixture;
        auto host = fixture.Host();
        const auto snapshot = host->Capture();
        const auto old = host->Acquire();
        REQUIRE(old.HasValue());
        auto candidate = fixture.Pair(*host, fixture.Detach(*host));
        host->Shutdown();
        host->Shutdown();
        CHECK(host->Commit(candidate, CommitPoint).HasError());
        CHECK(host->Acquire().HasError());
        CHECK_FALSE(host->IsCurrent(old.Value()).Value());
        CHECK(host->CollectRetired().Value() == 0);
        host.reset();
        CHECK(old.Value().Get()->Instance().State() == Runtime::Ui::UiRuntimeInstanceState::Retiring);
        CHECK_FALSE(old.Value().Get()->Instance().Payload().empty());
        CHECK(old.Value().CookedDocument().ordinary.SourceRevision().Value() == 1);
        CHECK(snapshot.Flatten().HasValue());
        CHECK(snapshot.Source().ordinary.Revision().Value() == 1);
    }

    TEST_CASE("Template host rejects wrong thread operations before either publisher changes", "[template][host][publication]") {
        PublicationFixture fixture;
        auto host = fixture.Host();
        auto document = fixture.Detach(*host);
        auto cook = fixture.Cook(document.Preview());
        auto runtime = fixture.Generation(cook);
        auto paired = fixture.Pair(*host, document);
        const auto lease = host->Acquire();
        REQUIRE(lease.HasValue());
        std::array<bool, 5> rejected{};
        const auto wrong = [](const auto &result) {
            return result.HasError() && result.ErrorValue().code.Value() == UiTemplates::UiErrors::TemplateDocumentWrongThread.code.Value();
        };
        std::jthread worker{[&rejected, &host, &lease, &paired, &document, &runtime, &cook, &wrong] {
            rejected = {wrong(host->Acquire()), wrong(host->IsCurrent(lease.Value())), wrong(host->CollectRetired()),
                        wrong(host->Commit(paired, CommitPoint)), wrong(host->Prepare(document, std::move(runtime), cook))};
        }};
        worker.join();
        CHECK(std::ranges::all_of(rejected, [](const bool value) {
            return value;
        }));
        CHECK(host->Capture().Source().ordinary.Revision().Value() == 1);
        REQUIRE(host->Commit(paired, CommitPoint).HasValue());
    }

    TEST_CASE("Template host failed initial admission preserves the sole document owner", "[template][host][publication]") {
        PublicationFixture fixture;
        auto cook = fixture.Cook(fixture.source.owner->Capture());
        auto runtime = fixture.Generation(cook);
        SECTION("invalid runtime retention bounds") {
            CHECK(UiTemplateHost::Create(std::move(fixture.source.owner), std::move(runtime), cook, {0, 4}).HasError());
        }
        SECTION("retirement storage allocation failure") {
            bool failed = false;
            {
                Tests::AllocationProbe::ScopedFailure failure;
                failed = UiTemplateHost::Create(std::move(fixture.source.owner), std::move(runtime), cook).HasError();
            }
            CHECK(failed);
        }
        REQUIRE(fixture.source.owner);
        CHECK(fixture.source.owner->Capture().Source().ordinary.Revision().Value() == 1);
        CHECK(runtime.Instance().State() == Runtime::Ui::UiRuntimeInstanceState::Prepared);
        REQUIRE(UiTemplateHost::Create(std::move(fixture.source.owner), std::move(runtime), cook).HasValue());
    }

    TEST_CASE("Template paired commit allocates and finally destroys no storage at the structural cutoff",
              "[template][host][publication][allocation]") {
        PublicationFixture fixture;
        auto host = fixture.Host();
        auto prepared = fixture.Pair(*host, fixture.Detach(*host));
        (void)CommitWithoutAllocation(*host, prepared);
    }

    TEST_CASE("Template paired lease replacement retains the matching cooked projection after host destruction",
              "[template][host][publication][lifetime]") {
        PublicationFixture fixture;
        auto host = fixture.Host();
        auto old = std::move(host->Acquire()).Value();
        {
            auto candidate = fixture.Pair(*host, fixture.Detach(*host));
            REQUIRE(host->Commit(candidate, CommitPoint).HasValue());
        }
        auto current = std::move(host->Acquire()).Value();
        host.reset();
        SECTION("copy assignment") {
            old = current;
            CHECK(current.Get()->Instance().DocumentRevision().Value() == 2);
        }
        SECTION("move assignment") {
            old = std::move(current);
            CHECK(current.Get() == nullptr);
        }
        CHECK(old.Get()->Instance().DocumentRevision().Value() == 2);
        CHECK(old.CookedDocument().ordinary.SourceRevision().Value() == 2);
        CHECK(std::ranges::equal(old.Get()->Instance().Payload(), old.CookedDocument().ordinary.Payload()));
    }

    TEST_CASE("Template candidate move assignment releases its prior runtime reservation exactly once",
              "[template][host][publication][lifetime]") {
        PublicationFixture fixture;
        auto host = fixture.Host({8, 2});
        const auto document = fixture.Detach(*host);
        auto first = fixture.Pair(*host, document);
        auto second = fixture.Pair(*host, document);
        first = std::move(second);
        auto third = fixture.Pair(*host, document);
        CHECK(host->Commit(second, CommitPoint).HasError());
        REQUIRE(host->Commit(first, CommitPoint).HasValue());
        CHECK(host->Commit(third, CommitPoint).HasError());
        CHECK(host->Acquire().Value().CookedDocument().ordinary.SourceRevision().Value() == 2);
    }
}  // namespace Horo::Application::TemplateHostTests
