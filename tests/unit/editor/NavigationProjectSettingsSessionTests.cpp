#include "AllocationProbe.h"
#include "Horo/Editor/NavigationProjectSettingsSession.h"
#include "NativePublicationFiles.h"
#include "NavigationProjectPolicyTestSupport.h"
#include "OwnedTestDirectory.h"

#include <fstream>
#include <thread>

using namespace Horo;
using namespace Horo::Tests::NavigationSettingsSupport;

namespace {
    /** @brief Injects uncertainty only after a genuine native replacement; source identities still advance. */
    class SessionFiles final : public TestSupport::NativePublicationFiles {
    public:
        bool uncertain{};

        Result<void> WritePrivateDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes, bool &created,
                                         const std::filesystem::perms mode) override {
            return native.WritePrivateDurable(path, bytes, created, mode);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            auto replaced = native.AtomicReplaceTracked(prepared, destination, receipt);
            if (replaced.HasValue() && uncertain)
                return Result<void>::Failure(MakeError(N::NavigationSettingsError(N::NavigationSettingsFailure::Invalid)));
            return replaced;
        }
    };

    /** @brief Real project sources, registered identities and canonical active Scene owner. */
    struct Project final {
        Tests::OwnedTestDirectory directory{"navigation-project"};
        std::filesystem::path root{directory.Path() / std::filesystem::path{u8"çalışma alanı"}};
        SessionFiles files;
        E::ProjectMutationCoordinator mutations{files};
        Assets::AssetRegistry registry;
        E::SceneDocumentTransactions scene = E::SceneDocumentTransactions::Create({Asset(2), 7}, Objects()).Value();

        Project() {
            REQUIRE(std::filesystem::create_directory(root));
            REQUIRE(
                registry.Publish({Record(1, "core.navmesh", "assets/nav.horoasset"), Record(2, "core.scene", "assets/a.horo")}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
            const auto record = N::EncodeNavigationDefinitionRecord(N::NavigationDefinition::Create(Definition()).Value(),
                                                                    Id<N::NavigationAuthoredRecordId>(1));
            REQUIRE(record.HasValue());
            const std::array support{N::NavigationDefinitionRecordSupport()};
            const N::NavigationSourceLoadContext context{.supportedRecords = support};
            const auto envelope = N::NavigationSourceRecords::Create(N::CurrentNavigationSourceSchemaVersion, {record.Value()}, context);
            REQUIRE(envelope.HasValue());
            const auto bytes = N::SerializeNavigationSourceRecords(envelope.Value());
            REQUIRE(bytes.HasValue());
            REQUIRE(files.WriteDurable(root / "assets/nav.horoasset", bytes.Value()).HasValue());
            const auto text = SceneSource::EncodeSceneSource({Objects(), {}});
            REQUIRE(files.WriteDurable(root / "assets/a.horo", std::as_bytes(std::span{text.data(), text.size()})).HasValue());
        }

        auto Open(const std::uint64_t generation = 7) {
            return E::NavigationProjectSettingsSession::Open(root, registry, files, mutations, generation, &scene);
        }
    };
}  // namespace

TEST_CASE("Navigation project session repairs canonical active Scene and saves actual independent source files",
          "[navigation][settings][session]") {
    Project project;
    auto opened = project.Open();
    REQUIRE(opened.HasValue());
    auto session = std::move(opened).Value();
    REQUIRE_FALSE(session->Policy());
    REQUIRE(session->TargetEvidence().empty());
    const auto savedPolicy = session->SavePolicy(Policy());
    REQUIRE(savedPolicy.HasValue());
    REQUIRE_FALSE(savedPolicy.Value().durabilityError);
    session->RequestOpenDefinition(Asset(1));
    REQUIRE(session->TakeOpenDefinitionRequest() == Asset(1));
    REQUIRE_FALSE(session->TakeOpenDefinitionRequest());
    const std::array<E::NavigationDefinitionEdit, 1> edits{
        E::RemoveNavigationProfile{Id<N::NavigationAgentProfileId>(1), Id<N::NavigationAgentProfileId>(2)}};
    auto review = session->Preview(Asset(1), edits);
    REQUIRE(review.HasValue());
    REQUIRE(review.Value().Impacts().size() == 2);
    REQUIRE(session->Commit(review.Value()).HasValue());
    REQUIRE(session->IsDirty());
    REQUIRE(project.scene.Snapshot().Value().objects[1].components.navigationAgent->profile == Id<N::NavigationAgentProfileId>(2));
    const auto published = session->SaveDocuments();
    REQUIRE(published.HasValue());
    REQUIRE(published.Value().size() == 2);
    for (const auto &outcome : published.Value()) {
        REQUIRE(outcome.durable);
        REQUIRE_FALSE(outcome.error);
    }
    REQUIRE_FALSE(session->IsDirty());
    session->Close();
    REQUIRE(session->Definitions().HasError());
    // The workspace owns this Scene; closing settings/tab presentation cannot revoke its canonical owner.
    REQUIRE(project.scene.Snapshot().HasValue());
    auto reopened = project.Open();
    REQUIRE(reopened.HasValue());
    REQUIRE(reopened.Value()->Definitions().Value().front().definition->Profiles().size() == 1);
    REQUIRE(reopened.Value()->Policy()->Input().definition == Asset(1));
}

TEST_CASE("Navigation project session refuses externally changed bytes and missing inventory", "[navigation][settings][freshness]") {
    Project project;
    auto opened = project.Open();
    REQUIRE(opened.HasValue());
    auto session = std::move(opened).Value();
    const std::array<E::NavigationDefinitionEdit, 1> edits{
        E::RemoveNavigationProfile{Id<N::NavigationAgentProfileId>(1), Id<N::NavigationAgentProfileId>(2)}};
    auto review = session->Preview(Asset(1), edits);
    REQUIRE(review.HasValue());
    SECTION("external source write after review") {
        std::ofstream file(project.root / "assets/a.horo", std::ios::app);
        file << '\n';
        file.close();
    }
    SECTION("closed active owner") {
        project.scene.Shutdown();
    }
    SECTION("registry generation") {
        REQUIRE(project.registry.Publish({Record(1, "core.navmesh", "assets/nav.horoasset"), Record(2, "core.scene", "assets/a.horo")})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
    }
    REQUIRE(session->Commit(review.Value()).HasError());
    REQUIRE(session->Definitions().Value().front().revision == 1);
}

TEST_CASE("Navigation definition save tickets cannot acknowledge another owner or clear later edits", "[navigation][settings][save]") {
    Fixture fixture;
    auto ticket = fixture.definition.CaptureSave(Id<N::NavigationAuthoredRecordId>(1));
    REQUIRE(ticket.HasValue());
    auto review = fixture.Removal();
    REQUIRE(fixture.workflow.Commit(review, true, {}).HasValue());
    REQUIRE(fixture.definition.AcknowledgeSave(ticket.Value(), 2).HasValue());
    REQUIRE(fixture.definition.IsDirty());
    auto replacement = E::NavigationDefinitionDocument::Open(Asset(1), 1, 1, N::NavigationDefinition::Create(Definition()).Value()).Value();
    REQUIRE(replacement.AcknowledgeSave(ticket.Value(), 2).HasError());
    fixture.definition.Close();
    REQUIRE(fixture.definition.AcknowledgeSave(ticket.Value(), 3).HasError());
}

TEST_CASE("Navigation source revision exhaustion rejects before a save ticket exists", "[navigation][settings][save]") {
    auto document =
        E::NavigationDefinitionDocument::Open(Asset(1), 1, UINT64_MAX, N::NavigationDefinition::Create(Definition()).Value()).Value();
    REQUIRE(document.CaptureSave(Id<N::NavigationAuthoredRecordId>(1)).HasError());
}

TEST_CASE("Navigation definition rendering projection reuses storage and never reads changed files",
          "[navigation][settings][projection][allocation]") {
    Project project;
    auto opened = project.Open();
    REQUIRE(opened.HasValue());
    auto session = std::move(opened).Value();
    const auto *slots = session->Definitions().Value().data();
    bool valid = true;
    std::size_t allocations{};
    // Changing the physical file cannot make a rendering query perform Fresh() or recapture source.
    {
        std::ofstream changed(project.root / "assets/a.horo", std::ios::app);
        changed << '\n';
    }
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        for (unsigned frame = 0; frame < 64; ++frame) {
            const auto projection = session->Definitions();
            valid &= projection.HasValue() && projection.Value().size() == 1 && projection.Value().data() == slots &&
                     projection.Value().front().revision == 1;
        }
        allocations = measurement.Snapshot().requests;
    }
    REQUIRE(valid);
    REQUIRE(allocations == 0);
    const std::array<E::NavigationDefinitionEdit, 1> edits{
        E::RemoveNavigationProfile{Id<N::NavigationAgentProfileId>(1), Id<N::NavigationAgentProfileId>(2)}};
    REQUIRE(session->Preview(Asset(1), edits).HasError());
    REQUIRE(session->Definitions().Value().front().revision == 1);
}

TEST_CASE("Navigation definition cached projection refreshes on commit replay and new opening",
          "[navigation][settings][projection][lifetime]") {
    Project project;
    auto opened = project.Open();
    REQUIRE(opened.HasValue());
    auto session = std::move(opened).Value();
    const auto before = session->Definitions().Value().front();
    const auto *slots = session->Definitions().Value().data();
    const std::array<E::NavigationDefinitionEdit, 1> edits{
        E::RemoveNavigationProfile{Id<N::NavigationAgentProfileId>(1), Id<N::NavigationAgentProfileId>(2)}};
    auto review = session->Preview(Asset(1), edits);
    REQUIRE(review.HasValue());
    REQUIRE(session->Commit(review.Value()).HasValue());
    REQUIRE(session->Definitions().Value().data() == slots);
    REQUIRE(session->Definitions().Value().front().revision == 2);
    REQUIRE(session->Definitions().Value().front().definition->Profiles().size() == 1);
    REQUIRE(before.revision == 1);
    REQUIRE(before.definition->Profiles().size() == 2);
    REQUIRE(session->Replay(false).HasValue());
    REQUIRE(session->Definitions().Value().front().revision == 3);
    REQUIRE(session->Definitions().Value().front().definition->Profiles().size() == 2);
    REQUIRE(session->Replay(true).HasValue());
    REQUIRE(session->Definitions().Value().front().revision == 4);
    REQUIRE(session->Definitions().Value().front().definition->Profiles().size() == 1);
    session->Close();
    REQUIRE(session->Definitions().HasError());
    REQUIRE(session->Commit(review.Value()).HasError());
    project.scene = E::SceneDocumentTransactions::Create({Asset(2), 8}, Objects()).Value();
    auto replacement = project.Open(8);
    REQUIRE(replacement.HasValue());
    REQUIRE(replacement.Value()->Definitions().Value().front().session == 8);
    REQUIRE(replacement.Value()->Definitions().Value().front().revision == 1);
    REQUIRE(replacement.Value()->Definitions().Value().front().definition->Profiles().size() == 2);
    REQUIRE(before.session == 7);
    REQUIRE(before.definition->Profiles().size() == 2);
}

TEST_CASE("Navigation presentation borrows revoke before close and never resolve another opening", "[navigation][settings][lifetime]") {
    Project project;
    auto opened = project.Open();
    REQUIRE(opened.HasValue());
    auto session = std::move(opened).Value();
    const auto borrowed = session->Borrow();
    REQUIRE(borrowed.Resolve() == session.get());
    std::size_t allocations{};
    bool same = true;
    {
        Tests::AllocationProbe::ScopedMeasurement measure;
        for (unsigned draw = 0; draw < 64; ++draw)
            same &= borrowed.Resolve() == session.get();
        allocations = measure.Snapshot().requests;
    }
    REQUIRE(same);
    REQUIRE(allocations == 0);
    bool foreignResolved = true;
    std::thread foreign([&] {
        foreignResolved = borrowed.Resolve() != nullptr;
    });
    foreign.join();
    REQUIRE_FALSE(foreignResolved);
    session->Close();
    REQUIRE(borrowed.Resolve() == nullptr);
    session.reset();
    REQUIRE(borrowed.Resolve() == nullptr);
    auto replacement = project.Open(8);
    REQUIRE(replacement.HasValue());
    REQUIRE(borrowed.Resolve() == nullptr);
    auto nextOwner = std::move(replacement).Value();
    const auto next = nextOwner->Borrow();
    REQUIRE(next.Resolve() == nextOwner.get());
    nextOwner.reset();
    REQUIRE(next.Resolve() == nullptr);
}

TEST_CASE("Navigation independent document saves retain dirty tickets after replacement uncertainty and can retry exact sources",
          "[navigation][settings][save][durability]") {
    Project project;
    auto opened = project.Open();
    REQUIRE(opened.HasValue());
    auto session = std::move(opened).Value();
    const std::array<E::NavigationDefinitionEdit, 1> edits{
        E::RemoveNavigationProfile{Id<N::NavigationAgentProfileId>(1), Id<N::NavigationAgentProfileId>(2)}};
    auto review = session->Preview(Asset(1), edits);
    REQUIRE(review.HasValue());
    REQUIRE(session->Commit(review.Value()).HasValue());
    project.files.uncertain = true;
    const auto uncertain = session->SaveDocuments();
    REQUIRE(uncertain.HasValue());
    REQUIRE(uncertain.Value().size() == 2);
    for (const auto &outcome : uncertain.Value()) {
        REQUIRE_FALSE(outcome.durable);
        REQUIRE(outcome.error);
    }
    REQUIRE(session->IsDirty());
    project.files.uncertain = false;
    // A retry must compare against the replaced bytes, not the pre-publication source identity.
    const auto retry = session->SaveDocuments();
    REQUIRE(retry.HasValue());
    REQUIRE(retry.Value().size() == 2);
    for (const auto &outcome : retry.Value()) {
        REQUIRE(outcome.durable);
        REQUIRE_FALSE(outcome.error);
    }
    REQUIRE_FALSE(session->IsDirty());
}
