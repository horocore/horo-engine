#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Editor/ProposedChangeSet.h"
#include "Horo/Security/AgentPolicy.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::Proposals;
    using Json = nlohmann::json;

    SourceDocumentId Path(std::string_view value) {
        return SourceDocumentId::Parse(value).Value();
    }

    struct Fixture final {
        Horo::TestSupport::SourceProject project;
        SourceDocumentService documents{project.directory};
        ProjectPolicy policy =
            ProjectPolicy::Capture("project.one", 3,
                                   {{Path("code.cpp"), false}, {Path("old.cpp"), false}, {Path("remove.cpp"), false}, {Path("src"), true}})
                .Value();
        SourceDocumentSnapshot code, old, removed;

        Fixture() {
            project.Put("int x;\n");
            project.Put("old\n", "old.cpp");
            project.Put("remove\n", "remove.cpp");
            code = documents.Open(project.Identity(), project.directory / "code.cpp").Value();
            old = documents.Open(project.Identity("old.cpp", 2), project.directory / "old.cpp").Value();
            removed = documents.Open(project.Identity("remove.cpp", 3), project.directory / "remove.cpp").Value();
        }

        Draft Values() const {
            return {"proposal.one",
                    1,
                    {"provider.one", "model.one", "request.one"},
                    {CreateFile{Path("src/new.cpp"), "hello\n"}, EditFile{FileBase::Capture(code).Value(), {{4, "x", "y"}}},
                     DeleteFile{FileBase::Capture(removed).Value()},
                     RenameFile{FileBase::Capture(old).Value(), Path("src/moved.cpp"), {{0, "old", "new"}}}}};
        }

        ChangeSet Proposal() const {
            return ChangeSet::Capture(policy, Values()).Value();
        }

        std::string Disk(const char *name) const {
            std::ifstream input(project.directory / name, std::ios::binary);
            return {std::istreambuf_iterator<char>{input}, {}};
        }
    };

    void Seal(Json &root) {
        const auto body = root.at("payload").dump();
        root["commitment"] = FormatSha256(ComputeSha256(std::as_bytes(std::span{body})));
    }
}  // namespace

TEST_CASE("Untrusted source proposal review changes neither actual buffers history nor disk", "[editor][proposal]") {
    Fixture fixture;
    const auto proposal = fixture.Proposal();
    const auto before = fixture.documents.Snapshot(fixture.code.Identity().instance).Value();
    const auto disk = fixture.Disk("code.cpp");
    const auto history = fixture.documents.History(before.Identity().instance).Value();
    REQUIRE(proposal.Files().size() == 4);
    for (const auto choice : {ReviewChoice::Accept, ReviewChoice::Reject}) {
        const auto decision = Review(proposal, choice, "local.reviewer");
        REQUIRE(decision.HasValue());
        CHECK(decision.Value().digest == proposal.Digest());
        const auto after = fixture.documents.Snapshot(before.Identity().instance).Value();
        CHECK(after.SharesStateWith(before));
        CHECK(after.Revision() == before.Revision());
        CHECK(after.BaseRevision() == before.BaseRevision());
        CHECK(after.Dirty() == before.Dirty());
        CHECK(after.Text() == before.Text());
        CHECK(after.DiskBase() == before.DiskBase());
        CHECK(fixture.documents.History(before.Identity().instance).Value().undoEntries == history.undoEntries);
        CHECK(fixture.Disk("code.cpp") == disk);
        CHECK(fixture.Disk("old.cpp") == "old\n");
        CHECK(fixture.Disk("remove.cpp") == "remove\n");
        CHECK_FALSE(std::filesystem::exists(fixture.project.directory / "src/new.cpp"));
        CHECK_FALSE(std::filesystem::exists(fixture.project.directory / "src/moved.cpp"));
        Security::AgentPolicy security;
        REQUIRE(security.SetProjectTrust(true, 1));
        CHECK(security
                  .EvaluateTool(Security::AgentProviderResidence::Local, {7, proposal.Values().revision, Security::AgentToolRisk::Mutation},
                                1)
                  .reason == Security::AgentPolicyReason::ApprovalRequired);
    }
    const auto encoded = EncodeRecovery(proposal);
    REQUIRE(encoded.HasValue());
    const auto restored = DecodeRecovery(encoded.Value(), fixture.policy);
    REQUIRE(restored.HasValue());
    CHECK(restored.Value().Digest() == proposal.Digest());
    CHECK(fixture.documents.Snapshot(before.Identity().instance).Value().SharesStateWith(before));
}

TEST_CASE("Source proposal bases use actual immutable revisions and exact UTF8 hunk boundaries", "[editor][proposal]") {
    Fixture fixture;
    const auto base = FileBase::Capture(fixture.code).Value();
    const std::array hunks{TextHunk{4, "x", "y"}};
    REQUIRE(ValidateAgainstSnapshot(base, hunks, fixture.code).HasValue());
    const auto changed = fixture.documents.Edit(fixture.code.Identity().instance, {fixture.code.Revision(), 4, 1, "z"});
    REQUIRE(changed.HasValue());
    CHECK(ValidateAgainstSnapshot(base, hunks, changed.Value()).ErrorValue().code.Value() == Errors::StaleBase.code.Value());
    CHECK(ValidateAgainstSnapshot(base, hunks, fixture.code).HasValue());
    CHECK(ValidateAgainstSnapshot(base, std::array{TextHunk{4, "q", "y"}}, fixture.code).HasError());
    CHECK(ValidateAgainstSnapshot(base, std::array{TextHunk{0, "int", "z"}, TextHunk{2, "t", "q"}}, fixture.code).HasError());
    fixture.project.Put("a\xc3\xa9\n", "unicode.cpp");
    const auto unicode =
        fixture.documents.Open(fixture.project.Identity("unicode.cpp", 4), fixture.project.directory / "unicode.cpp").Value();
    CHECK(ValidateAgainstSnapshot(FileBase::Capture(unicode).Value(), std::array{TextHunk{2, "", "x"}}, unicode).HasError());
    CHECK(FileBase::Capture({}).HasError());
}

TEST_CASE("Source proposal policy admits only canonical portable paths and collision-free operations", "[editor][proposal]") {
    Fixture fixture;
    for (const auto text : {"../escape.cpp", "/tmp/outside.cpp", "C:/outside.cpp", "\\server\\outside.cpp", "src/../code.cpp"})
        CHECK(SourceDocumentId::Parse(text).HasError());
    CHECK_FALSE(fixture.policy.Allows(Path("src")));
    for (const auto text : {"src-other/a.cpp", ".git/config", "src/CON", "src/a.", "src/trailing "}) {
        auto draft = fixture.Values();
        draft.files = {CreateFile{Path(text), "x"}};
        CHECK(ChangeSet::Capture(fixture.policy, std::move(draft)).HasError());
    }
    SECTION("case collision") {
        auto draft = fixture.Values();
        draft.files = {CreateFile{Path("src/a.cpp"), "a"}, CreateFile{Path("src/A.cpp"), "b"}};
        CHECK(ChangeSet::Capture(fixture.policy, std::move(draft)).HasError());
    }
    SECTION("rename target collides with another source") {
        auto draft = fixture.Values();
        std::get<RenameFile>(draft.files.back()).destination = Path("code.cpp");
        CHECK(ChangeSet::Capture(fixture.policy, std::move(draft)).HasError());
    }
    SECTION("rename escapes allowed roots") {
        auto draft = fixture.Values();
        std::get<RenameFile>(draft.files.back()).destination = Path("outside.cpp");
        CHECK(ChangeSet::Capture(fixture.policy, std::move(draft)).HasError());
    }
    CHECK(ProjectPolicy::Capture("p", 1, {}).HasError());
    CHECK(ProjectPolicy::Capture("p", 0, {{Path("src"), true}}).HasError());
}

TEST_CASE("Source proposal structure text and arithmetic obey finite exact boundaries", "[editor][proposal]") {
    Fixture fixture;
    auto limits = Limits{};
    limits.maximumFiles = 4;
    limits.maximumTotalTextBytes = 14;
    REQUIRE(ChangeSet::Capture(fixture.policy, fixture.Values(), limits).HasValue());
    --limits.maximumTotalTextBytes;
    CHECK(ChangeSet::Capture(fixture.policy, fixture.Values(), limits).HasError());
    limits = {};
    limits.maximumFiles = 3;
    CHECK(ChangeSet::Capture(fixture.policy, fixture.Values(), limits).HasError());
    auto draft = fixture.Values();
    SECTION("unknown empty edit") {
        std::get<EditFile>(draft.files[1]).hunks.clear();
    }
    SECTION("binary create") {
        std::get<CreateFile>(draft.files[0]).text = std::string(1, '\0');
    }
    SECTION("malformed UTF8") {
        std::get<CreateFile>(draft.files[0]).text = "\xc0\xaf";
    }
    SECTION("offset overflow") {
        std::get<EditFile>(draft.files[1]).hunks[0].offset = std::numeric_limits<std::uint64_t>::max();
    }
    SECTION("overlap") {
        std::get<EditFile>(draft.files[1]).hunks = {{0, "int", "a"}, {2, "t", "b"}};
    }
    SECTION("duplicate insertion") {
        std::get<EditFile>(draft.files[1]).hunks = {{0, "", "a"}, {0, "", "b"}};
    }
    SECTION("missing base") {
        std::get<EditFile>(draft.files[1]).base.revision = 0;
    }
    SECTION("oversized provenance") {
        draft.provenance.provider.assign(129, 'a');
    }
    CHECK(ChangeSet::Capture(fixture.policy, std::move(draft)).HasError());
}

TEST_CASE("Proposal recovery is canonical checked versioned and non-authorizing", "[editor][proposal]") {
    Fixture fixture;
    const auto proposal = fixture.Proposal();
    auto reversed = fixture.Values();
    std::ranges::reverse(reversed.files);
    const auto ordered = ChangeSet::Capture(fixture.policy, std::move(reversed));
    REQUIRE(ordered.HasValue());
    CHECK(ordered.Value().Digest() == proposal.Digest());
    const auto encoded = EncodeRecovery(proposal).Value();
    CHECK(EncodeRecovery(DecodeRecovery(encoded, fixture.policy).Value()).Value() == encoded);
    const auto changedPolicy = ProjectPolicy::Capture("project.one", 4, {{Path("code.cpp"), false}, {Path("src"), true}}).Value();
    CHECK(DecodeRecovery(encoded, changedPolicy).ErrorValue().code.Value() == Errors::PolicyDenied.code.Value());
    auto exact = Limits{};
    exact.maximumWireBytes = encoded.size();
    CHECK(EncodeRecovery(proposal, exact).HasValue());
    --exact.maximumWireBytes;
    CHECK(EncodeRecovery(proposal, exact).HasError());
    CHECK(DecodeRecovery(encoded.substr(0, encoded.size() - 1), fixture.policy).HasError());
    auto root = Json::parse(encoded);
    SECTION("future version") {
        root["version"] = std::uint64_t{2};
    }
    SECTION("unknown operation") {
        root["payload"]["files"][0]["kind"] = "provider-unified-diff";
        Seal(root);
    }
    SECTION("inconsistent commitment") {
        root["payload"]["id"] = "tampered";
    }
    SECTION("unknown payload field") {
        root["payload"]["rawPatch"] = "untrusted provider syntax";
        Seal(root);
    }
    SECTION("negative offset") {
        root["payload"]["files"][0]["hunks"][0]["offset"] = -1;
        Seal(root);
    }
    CHECK(DecodeRecovery(root.dump(), fixture.policy).HasError());
    std::string duplicate = encoded;
    const auto at = duplicate.find("\"version\":1");
    REQUIRE(at != std::string::npos);
    duplicate.replace(at, 11, "\"version\":1,\"version\":1");
    CHECK(DecodeRecovery(duplicate, fixture.policy).HasError());
    CHECK(DecodeRecovery(std::string(100, '[') + "0" + std::string(100, ']'), fixture.policy).HasError());
}

TEST_CASE("Review application and durable save receipts remain distinct with redacted audit", "[editor][proposal]") {
    Fixture fixture;
    const auto proposal = fixture.Proposal();
    const auto review = Review(proposal, ReviewChoice::Accept, "local.reviewer").Value();
    ApplicationReceipt application{proposal.Values().id, proposal.Values().revision, proposal.Digest()};
    DiskSaveReceipt save{application.proposal, application.revision, application.digest, DiskSaveState::Durable};
    CHECK(ValidateReceipts(proposal, application, save).HasError());
    application.state = ApplicationState::AppliedToWorkspace;
    application.appliedOperations = 4;
    REQUIRE(ValidateReceipts(proposal, application, save).HasValue());
    const auto record = Audit(proposal, review, application, save);
    REQUIRE(record.HasValue());
    const auto encoded = EncodeAudit(record.Value());
    REQUIRE(encoded.HasValue());
    CHECK(encoded.Value().find("code.cpp") == std::string::npos);
    CHECK(encoded.Value().find("hello") == std::string::npos);
    CHECK(encoded.Value().find("provider.one") == std::string::npos);
    CHECK(encoded.Value().find("proposal.one") == std::string::npos);
    const auto decoded = DecodeAudit(encoded.Value());
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value().operationCounts == std::array<std::uint32_t, 4>{1, 1, 1, 1});
    CHECK(decoded.Value().review == ReviewChoice::Accept);
    CHECK(decoded.Value().application == ApplicationState::AppliedToWorkspace);
    CHECK(decoded.Value().save == DiskSaveState::Durable);
    application.state = ApplicationState::PartiallyApplied;
    application.appliedOperations = 1;
    CHECK(ValidateReceipts(proposal, application).HasValue());
    CHECK(ValidateReceipts(proposal, application, save).HasError());
    auto stale = review;
    ++stale.revision;
    CHECK(Audit(proposal, stale).HasError());
    CHECK(Review(proposal, static_cast<ReviewChoice>(255), "local").HasError());
    auto wire = Json::parse(encoded.Value());
    wire["version"] = std::uint64_t{2};
    CHECK(DecodeAudit(wire.dump()).ErrorValue().code.Value() == Errors::UnsupportedVersion.code.Value());
    wire = Json::parse(encoded.Value());
    wire["hunks"] = "secret";
    CHECK(DecodeAudit(wire.dump()).HasError());
}

TEST_CASE("Proposal hunks check final simultaneous size and protect the source BOM", "[editor][proposal]") {
    Fixture fixture;
    auto draft = fixture.Values();
    draft.files = {EditFile{FileBase::Capture(fixture.code).Value(), {{0, "", "abcdef"}, {1, "nt x;\n", ""}}}};
    auto limits = Limits{};
    limits.maximumTextBytes = 7;
    const auto proposal = ChangeSet::Capture(fixture.policy, draft, limits);
    REQUIRE(proposal.HasValue());
    const auto &edit = std::get<EditFile>(proposal.Value().Files().front());
    CHECK(ValidateAgainstSnapshot(edit.base, edit.hunks, fixture.code).HasValue());
    fixture.project.Put("\xef\xbb\xbfint x;\n", "bom.cpp");
    const auto bom = fixture.documents.Open(fixture.project.Identity("bom.cpp", 4), fixture.project.directory / "bom.cpp").Value();
    const auto policy = ProjectPolicy::Capture("project.one", 3, {{Path("bom.cpp"), false}}).Value();
    draft.files = {EditFile{FileBase::Capture(bom).Value(), {{0, "", "x"}}}};
    CHECK(ChangeSet::Capture(policy, draft).HasError());
    auto rejected = Review(fixture.Proposal(), ReviewChoice::Reject, "local").Value();
    const auto original = fixture.Proposal();
    ApplicationReceipt applied{original.Values().id, original.Values().revision, original.Digest(), ApplicationState::AppliedToWorkspace,
                               4};
    CHECK(Audit(original, rejected, applied).HasError());
}
