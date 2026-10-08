#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectSourceDocument.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
    using namespace Horo;
    using namespace Horo::Application;

    class TemporaryProject {
    public:
        TemporaryProject() {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = std::filesystem::temp_directory_path() / std::filesystem::path(u8"horo compatibility İstanbul-");
            root_ += std::to_string(stamp);
            std::filesystem::create_directories(root_ / ".horo");
        }

        ~TemporaryProject() {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }

        void Write(const std::string &text) const {
            std::ofstream output(root_ / ".horo/project.json", std::ios::binary);
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
        }

        [[nodiscard]] const std::filesystem::path &Root() const noexcept {
            return root_;
        }

    private:
        std::filesystem::path root_;
    };

    class AcceptingVerifier final : public ICompatibilityProofVerifier {
    public:
        Result<void> Verify(const CompatibilityProof &, const PersistentContractHash &) const override {
            return Result<void>::Success();
        }
    };

    PersistentContractHash Hash(const char digit) {
        const auto parsed = ParsePersistentContractHash("sha256:" + std::string(64, digit));
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    CompatibilityDecisionHash DecisionHash(const char digit) {
        const auto parsed = ParseCompatibilityDecisionHash("sha256:" + std::string(64, digit));
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    HoroVersion Version(const std::string_view text) {
        const auto parsed = ParseHoroVersion(text);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    std::string Metadata(const std::string_view version, const PersistentContractHash &contract, const std::string_view proof = {}) {
        return "{\n"
               "  \"horoVersion\": \"" +
               std::string(version) +
               "\",\n"
               "  \"persistentContract\": \"" +
               FormatPersistentContractHash(contract) +
               "\",\n"
               "  \"projectId\": \"project-id\",\n"
               "  \"name\": \"Horo Project\",\n"
               "  \"projectVersion\": \"0.4.0\",\n"
               "  \"createdAt\": \"2026-07-18T00:00:00Z\",\n"
               "  \"settings\": { \"renderBackend\": \"metal\" }" +
               (proof.empty() ? std::string{} : ",\n  \"compatibilityProof\": " + std::string(proof)) + "\n}\n";
    }

    ReleaseCompatibilityRegistry Registry(const PersistentContractHash &contract) {
        const std::array decisions{ReleaseCompatibilityDecision{{Version("1.0.3")},
                                                                {Version("1.0.3")},
                                                                contract,
                                                                DecisionHash('1'),
                                                                CompatibilityDecisionKind::EstablishBaseline},
                                   ReleaseCompatibilityDecision{{Version("1.0.5")},
                                                                {Version("1.0.3")},
                                                                contract,
                                                                DecisionHash('2'),
                                                                CompatibilityDecisionKind::CompatibleReleaseLine}};
        auto registry = ReleaseCompatibilityRegistry::Create(decisions);
        REQUIRE((registry.HasValue()));
        return std::move(registry).Value();
    }

    std::string WithExtraKeys(std::string document, const std::size_t count) {
        std::string members;
        for (std::size_t index = 0; index < count; ++index)
            members += ",\"extra" + std::to_string(index) + "\":0";
        document.insert(document.rfind('}'), members);
        return document;
    }

    void RequireMetadataError(const std::string &document, const std::string_view code) {
        const auto decoded = DecodeProjectSourceDocument(document);
        REQUIRE(decoded.HasError());
        REQUIRE(decoded.ErrorValue().code.Value() == code);
    }

    TEST_CASE("Metadata profile requires exact registered release and persistent contract", "[unit][application][source-profile]") {
        const auto &registry = BuiltInReleaseCompatibilityRegistry();
        const auto *current = registry.Find({Version("0.2.0")});
        const auto *legacy = registry.Find({Version("0.1.0")});
        REQUIRE(current != nullptr);
        REQUIRE(legacy != nullptr);
        REQUIRE(DecodeProjectSourceDocument(WithExtraKeys(Metadata("0.2.0", current->persistentContract), 256)).HasValue());
        RequireMetadataError(WithExtraKeys(Metadata("0.2.0", legacy->persistentContract), 256), "project.metadata.limit_exceeded");
        RequireMetadataError(WithExtraKeys(Metadata("0.2.0", Hash('a')), 256), "project.metadata.limit_exceeded");
        for (const auto version : {"0.1.0", "0.2.1", "0.3.0", "forged"})
            RequireMetadataError(WithExtraKeys(Metadata(version, current->persistentContract), 256), "project.metadata.limit_exceeded");
    }

    TEST_CASE("Legacy metadata retains size key depth and duplicate diagnostic precedence", "[unit][application][source-profile]") {
        std::string oversized = Metadata("0.1.0", Hash('a'));
        oversized.insert(oversized.rfind('}'), ",\"projectId\":\"duplicate\"");
        oversized.append(64U * 1024U, ' ');
        RequireMetadataError(oversized, "project.metadata.size_invalid");
        RequireMetadataError(std::string(64U * 1024U + 1U, 'x'), "project.metadata.size_invalid");
        RequireMetadataError(WithExtraKeys(Metadata("0.1.0", Hash('a')), 256), "project.metadata.limit_exceeded");
        auto nested = Metadata("0.1.0", Hash('a'));
        nested.insert(nested.rfind('}'), ",\"nested\":" + std::string(17, '[') + "0" + std::string(17, ']'));
        RequireMetadataError(nested, "project.metadata.limit_exceeded");
        auto duplicate = Metadata("0.1.0", Hash('a'));
        duplicate.insert(duplicate.rfind('}'), ",\"projectId\":\"duplicate\"");
        RequireMetadataError(duplicate, "project.metadata.duplicate_key");
    }

    TEST_CASE("Registered metadata profile stays finite without changing value limits", "[unit][application][source-profile]") {
        const auto *current = BuiltInReleaseCompatibilityRegistry().Find({Version("0.2.0")});
        REQUIRE(current != nullptr);
        const auto metadata = Metadata("0.2.0", current->persistentContract);
        auto expanded = metadata;
        expanded.append(64U * 1024U, ' ');
        REQUIRE(DecodeProjectSourceDocument(expanded).HasValue());
        auto nested = metadata;
        nested.insert(nested.rfind('}'), ",\"nested\":" + std::string(17, '[') + "0" + std::string(17, ']'));
        REQUIRE(DecodeProjectSourceDocument(nested).HasValue());
        nested = metadata;
        nested.insert(nested.rfind('}'), ",\"nested\":" + std::string(33, '[') + "0" + std::string(33, ']'));
        RequireMetadataError(nested, "project.metadata.limit_exceeded");
        RequireMetadataError(WithExtraKeys(metadata, 32768), "project.metadata.limit_exceeded");
        RequireMetadataError(metadata + std::string(1024U * 1024U, ' '), "project.metadata.size_invalid");
        auto longValue = metadata;
        longValue.insert(longValue.rfind('}'), ",\"extra\":\"" + std::string(8193, 'x') + "\"");
        RequireMetadataError(longValue, "project.metadata.limit_exceeded");
    }

    TEST_CASE("Project source loader confines physical metadata to explicit project root", "[unit][application][source-profile]") {
        TemporaryProject project;
        project.Write(Metadata("0.1.0", Hash('a')));
        REQUIRE(LoadProjectSourceDocument(project.Root()).HasValue());
        TemporaryProject external;
        external.Write(Metadata("0.1.0", Hash('a')));
        std::filesystem::remove(project.Root() / ".horo/project.json");
        std::error_code error;
        std::filesystem::create_symlink(external.Root() / ".horo/project.json", project.Root() / ".horo/project.json", error);
        if (error) {
            SKIP("Host does not permit symlink creation: " << error.message());
        }
        const auto escaped = LoadProjectSourceDocument(project.Root());
        REQUIRE(escaped.HasError());
        REQUIRE(escaped.ErrorValue().code.Value() == "project.metadata.read_failed");
        REQUIRE(LoadProjectSourceDocument(external.Root()).HasValue());
    }

    TEST_CASE("Metadata profile authority is independent of root member ordering and rejects ambiguous markers",
              "[unit][application][source-profile]") {
        const auto *decision = BuiltInReleaseCompatibilityRegistry().Find({Version("0.2.0")});
        REQUIRE(decision != nullptr);
        const auto metadata = Metadata("0.2.0", decision->persistentContract);
        const auto withAuthorityLast = [&](const std::size_t count) {
            auto prefix = WithExtraKeys("{\"payload\":0}", count);
            prefix.back() = ',';
            return prefix + metadata.substr(1);
        };
        REQUIRE(DecodeProjectSourceDocument(withAuthorityLast(256)).HasValue());
        REQUIRE(DecodeProjectSourceDocument("{\"payload\":0," + std::string(64U * 1024U, ' ') + metadata.substr(1)).HasValue());
        RequireMetadataError(withAuthorityLast(32768), "project.metadata.limit_exceeded");
        for (const auto marker : {"\"horoVersion\":0", "\"persistentContract\":[]", "\"horoVersion\":\"0.1.0\""}) {
            auto duplicate = metadata;
            duplicate.insert(duplicate.rfind('}'), "," + std::string{marker});
            RequireMetadataError(duplicate, "project.metadata.duplicate_key");
        }
        auto wrongType = metadata;
        const auto release = wrongType.find("\"0.2.0\"");
        REQUIRE(release != std::string::npos);
        wrongType.replace(release, 7, "[\"0.2.0\"]");
        RequireMetadataError(WithExtraKeys(wrongType, 256), "project.metadata.limit_exceeded");
        wrongType = metadata;
        const auto contract = "\"" + FormatPersistentContractHash(decision->persistentContract) + "\"";
        const auto contractPosition = wrongType.find(contract);
        REQUIRE(contractPosition != std::string::npos);
        wrongType.replace(contractPosition, contract.size(), "{}");
        RequireMetadataError(WithExtraKeys(wrongType, 256), "project.metadata.limit_exceeded");
    }

    TEST_CASE("Sem Ver Is Canonical And Ordered", "[unit][application]") {
        const std::array valid{"0.0.1", "1.2.3-alpha", "1.2.3-alpha.1", "1.2.3-rc-1"};
        for (const auto text : valid) {
            const auto parsed = ParseHoroVersion(text);
            REQUIRE((parsed.HasValue()));
            REQUIRE((FormatHoroVersion(parsed.Value()) == text));
        }
        for (const auto invalid :
             {"", "1.0", "01.0.0", "1.00.0", "1.0.00", "1.0.0-01", "1.0.0+build", "1.0.0-", "1.0.0-a..b", "4294967296.0.0"})
            REQUIRE((ParseHoroVersion(invalid).HasError()));
        REQUIRE((CompareHoroVersions(Version("1.0.0-alpha"), Version("1.0.0-alpha.1")) < 0));
        REQUIRE((CompareHoroVersions(Version("1.0.0-rc.1"), Version("1.0.0")) < 0));
        REQUIRE((ParsePersistentContractHash("sha256:" + std::string(64, 'A')).HasError()));
        REQUIRE((FormatPersistentContractHash(Hash('a')) == "sha256:" + std::string(64, 'a')));
    }

    TEST_CASE("Registry Rejects Ambiguity And Patch Drift", "[unit][application]") {
        const auto contract = Hash('a');
        const ReleaseCompatibilityDecision first{{Version("1.0.0")},
                                                 {Version("1.0.0")},
                                                 contract,
                                                 DecisionHash('1'),
                                                 CompatibilityDecisionKind::EstablishBaseline};
        const std::array duplicate{first, first};
        REQUIRE((ReleaseCompatibilityRegistry::Create(duplicate).HasError()));

        const std::array drift{first, ReleaseCompatibilityDecision{{Version("1.0.1")},
                                                                   {Version("1.0.0")},
                                                                   Hash('b'),
                                                                   DecisionHash('2'),
                                                                   CompatibilityDecisionKind::CompatibleReleaseLine}};
        REQUIRE((ReleaseCompatibilityRegistry::Create(drift).HasError()));

        const std::array missingBaseline{ReleaseCompatibilityDecision{{Version("1.1.0")},
                                                                      {Version("1.0.0")},
                                                                      contract,
                                                                      DecisionHash('3'),
                                                                      CompatibilityDecisionKind::CompatibleReleaseLine}};
        REQUIRE((ReleaseCompatibilityRegistry::Create(missingBaseline).HasError()));
    }

    TEST_CASE("Inspector Classifies Known And Proven Future Patches", "[unit][application]") {
        const auto contract = Hash('a');
        const auto registry = Registry(contract);
        const EngineReleaseVersion current{Version("1.0.5")};
        RejectingCompatibilityProofVerifier rejecting;
        ProjectCompatibilityInspector inspector{registry, current, rejecting};
        TemporaryProject project;

        project.Write(Metadata("1.0.5", contract));
        REQUIRE((inspector.Inspect(project.Root()).status == ProjectCompatibilityStatus::Current));
        project.Write(Metadata("1.0.3", contract));
        const auto olderPatch = inspector.Inspect(project.Root());
        REQUIRE((olderPatch.status == ProjectCompatibilityStatus::CompatibleReleaseLine));
        REQUIRE((olderPatch.markerUpdateRequired));

        const std::string proof = "{\"release\":\"1.0.7\",\"contractBaseline\":\"1.0.3\","
                                  "\"decisionHash\":\"sha256:" +
                                  std::string(64, '3') + "\",\"signature\":\"fixture\"}";
        project.Write(Metadata("1.0.7", contract, proof));
        REQUIRE((inspector.Inspect(project.Root()).status == ProjectCompatibilityStatus::FutureVersion));

        AcceptingVerifier accepting;
        ProjectCompatibilityInspector trustedInspector{registry, current, accepting};
        REQUIRE((trustedInspector.Inspect(project.Root()).status == ProjectCompatibilityStatus::CompatibleReleaseLine));

        project.Write(Metadata("1.0.7", contract));
        REQUIRE((trustedInspector.Inspect(project.Root()).status == ProjectCompatibilityStatus::FutureVersion));

        project.Write(Metadata("1.1.0", contract));
        REQUIRE((trustedInspector.Inspect(project.Root()).status == ProjectCompatibilityStatus::FutureVersion));
        project.Write(Metadata("1.0.5", Hash('b')));
        REQUIRE((trustedInspector.Inspect(project.Root()).status == ProjectCompatibilityStatus::FutureVersion));
    }

    TEST_CASE("Bounded Metadata Rejects Legacy And Malformed Inputs", "[unit][application]") {
        TemporaryProject project;
        project.Write(R"({"formatVersion":1,"projectId":"old"})");
        REQUIRE((LoadProjectMetadata(project.Root()).HasError()));
        project.Write(R"({"horoVersion":"0.0.1","horoVersion":"0.0.1"})");
        REQUIRE((LoadProjectMetadata(project.Root()).HasError()));
        project.Write(std::string(64U * 1024U + 1U, 'x'));
        REQUIRE((LoadProjectMetadata(project.Root()).HasError()));

        project.Write(Metadata("0.0.1", Hash('a')));
        std::string invalidRenderer = Metadata("0.0.1", Hash('a'));
        invalidRenderer.replace(invalidRenderer.find("metal"), 5, "Metal");
        project.Write(invalidRenderer);
        REQUIRE((LoadProjectMetadata(project.Root()).HasError()));

        std::string unicode = Metadata("0.0.1", Hash('a'));
        unicode.replace(unicode.find("Horo Project"), 12, "Horo \\u0130stanbul");
        project.Write(unicode);
        const auto loadedUnicode = LoadProjectMetadata(project.Root());
        REQUIRE((loadedUnicode.HasValue()));
        REQUIRE((!loadedUnicode.Value().name.empty()));

        std::string nested = Metadata("0.0.1", Hash('a'));
        nested.insert(nested.rfind('}'), ",\"nested\":" + std::string(17, '[') + "0" + std::string(17, ']'));
        project.Write(nested);
        REQUIRE((LoadProjectMetadata(project.Root()).HasError()));

        TemporaryProject missing;
        std::filesystem::remove(missing.Root() / ".horo/project.json");
        const auto registry = Registry(Hash('a'));
        RejectingCompatibilityProofVerifier verifier;
        const ProjectCompatibilityInspector inspector{registry, {Version("1.0.5")}, verifier};
        REQUIRE((inspector.Inspect(missing.Root()).status == ProjectCompatibilityStatus::Inaccessible));
    }
}  // namespace
