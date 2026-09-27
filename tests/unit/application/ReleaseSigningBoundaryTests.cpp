#include "Horo/Release/ReleaseSigningBoundary.h"
#include "ReleaseTestFixtures.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>

using namespace Horo;
using namespace Horo::Release;
using namespace ReleaseTestFixtures;

namespace {
    class TemporaryStage final {
    public:
        TemporaryStage()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-signing-boundary-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(root / "bin");
        }

        ~TemporaryStage() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path root;
    };

    void WriteFile(const std::filesystem::path &path, const std::string_view bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    ReleaseExecutionPlan SigningPlan() {
        auto request = Request();
        request.profile = Profile(ReleaseSigningPolicy::Required);
        request.signingSelected = true;
        auto outcome = PreflightRelease(request, Facts(request));
        REQUIRE(outcome.plan.has_value());
        return std::move(*outcome.plan);
    }

    ReleasePreSignInventory Inventory() {
        auto inventory =
            ReleasePreSignInventory::Create(ReleaseCandidateId{42U}, {{"bin/game", ReleaseArtifactRole::Binary, 4U, Digest("game")}});
        REQUIRE(inventory.HasValue());
        return std::move(inventory).Value();
    }

    class RecordingSigner final : public IReleaseSigningBackend {
    public:
        [[nodiscard]] Result<void> Sign(const std::filesystem::path &root, const ReleaseCredentialHandle handle) override {
            ++calls;
            seenRoot = root;
            seenHandle = handle;
            return Result<void>::Success();
        }

        int calls{};
        std::filesystem::path seenRoot;
        ReleaseCredentialHandle seenHandle;
    };
}  // namespace

TEST_CASE("Release signing handoff requires exact unsigned bytes and an authorized opaque credential", "[release][signing]") {
    TemporaryStage stage;
    WriteFile(stage.root / "bin/game", "game");
    const auto plan = SigningPlan();
    const auto inventory = Inventory();
    RecordingSigner signer;
    ReleaseSigningRequest request{plan, inventory, stage.root, ReleaseCredentialHandle{42U}};

    REQUIRE(SignVerifiedReleaseStage(request, signer).HasValue());
    CHECK(signer.calls == 1);
    CHECK(signer.seenRoot == stage.root);
    CHECK(signer.seenHandle == ReleaseCredentialHandle{42U});

    request.credential = ReleaseCredentialHandle{43U};
    CHECK(SignVerifiedReleaseStage(request, signer).HasError());
    CHECK(signer.calls == 1);

    request.credential = ReleaseCredentialHandle{42U};
    WriteFile(stage.root / "bin/game", "evil");
    CHECK(SignVerifiedReleaseStage(request, signer).HasError());
    CHECK(signer.calls == 1);

    WriteFile(stage.root / "bin/game", "game");
    WriteFile(stage.root / "extra", "undeclared");
    CHECK(SignVerifiedReleaseStage(request, signer).HasError());
    CHECK(signer.calls == 1);
}
