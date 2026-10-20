#include "Horo/Application/NavigationProjectSettingsStore.h"
#include "NavigationProjectPolicyTestSupport.h"
#include "OwnedTestDirectory.h"

#include <fstream>
#include <nlohmann/json.hpp>

using namespace Horo;
using namespace Horo::Tests::NavigationSettingsSupport;

namespace {
    /** @brief Injects at the real native replacement boundary; all storage and ownership remain native. */
    class FaultFiles final : public DurableFileSystem {
    public:
        enum class Fault {
            None,
            BeforeReplace,
            AfterReplace
        } fault{};
        NativeDurableFileSystem native;

        Horo::Result<bool> CreateDirectoryPath(const std::filesystem::path &path, Horo::DirectoryCreationMode mode) override {
            return native.CreateDirectoryPath(path, mode);
        }

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &p, std::string_view owner) override {
            return native.TryAcquireExclusive(p, owner);
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &p) const override {
            return native.AvailableBytes(p);
        }

        Result<void> WriteDurable(const std::filesystem::path &p, std::span<const std::byte> b) override {
            return native.WriteDurable(p, b);
        }

        Result<void> WritePrivateDurable(const std::filesystem::path &p, std::span<const std::byte> b, bool &created,
                                         std::filesystem::perms mode) override {
            return native.WritePrivateDurable(p, b, created, mode);
        }

        Result<void> CopyDurable(const std::filesystem::path &a, const std::filesystem::path &b) override {
            return native.CopyDurable(a, b);
        }

        Result<void> AtomicReplace(const std::filesystem::path &a, const std::filesystem::path &b) override {
            return native.AtomicReplace(a, b);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &a, const std::filesystem::path &b,
                                          AtomicFileReplacementReceipt &receipt) override {
            if (fault == Fault::BeforeReplace)
                return Failure();
            auto result = native.AtomicReplaceTracked(a, b, receipt);
            return result.HasValue() && fault == Fault::AfterReplace ? Failure() : result;
        }

        Result<void> RemoveDurable(const std::filesystem::path &p) override {
            return native.RemoveDurable(p);
        }

        Result<void> SyncDirectory(const std::filesystem::path &p) override {
            return native.SyncDirectory(p);
        }

    private:
        static Result<void> Failure() {
            return Result<void>::Failure(MakeError(N::NavigationSettingsError(N::NavigationSettingsFailure::Invalid)));
        }
    };

    std::string Read(const std::filesystem::path &p) {
        std::ifstream file(p, std::ios::binary);
        return {std::istreambuf_iterator<char>{file}, {}};
    }
}  // namespace

TEST_CASE("Navigation project policy roundtrips exact versioned authority and migrates schema one",
          "[navigation][settings][serialization]") {
    const auto settings = Application::NavigationProjectSettings::Create(Policy());
    REQUIRE(settings.HasValue());
    const auto text = Application::EncodeNavigationProjectSettings(settings.Value());
    const auto decoded = Application::DecodeNavigationProjectSettings(text);
    REQUIRE(decoded.HasValue());
    REQUIRE(Application::EncodeNavigationProjectSettings(decoded.Value()) == text);
    auto legacy = nlohmann::json::parse(text);
    legacy["schema"] = 1;
    legacy["provider"] = legacy["defaultProvider"];
    legacy.erase("defaultProvider");
    const auto migrated = Application::DecodeNavigationProjectSettings(legacy.dump());
    REQUIRE(migrated.HasValue());
    REQUIRE(Application::EncodeNavigationProjectSettings(migrated.Value()) == text);
    REQUIRE(text.find("overlay") == std::string::npos);
    REQUIRE(text.find("profiles") == std::string::npos);
    REQUIRE(text.find("areas") == std::string::npos);
    REQUIRE(text.find("filters") == std::string::npos);
}

TEST_CASE("Navigation project policy rejects malformed version skew duplicate and overflowing input",
          "[navigation][settings][serialization]") {
    const auto settings = Application::NavigationProjectSettings::Create(Policy()).Value();
    auto json = nlohmann::json::parse(Application::EncodeNavigationProjectSettings(settings));
    SECTION("future schema") {
        json["schema"] = 3;
    }
    SECTION("negative revision") {
        json["revision"] = -1;
    }
    SECTION("fractional capacity") {
        json["runtimeProfile"]["capacities"][0] = 1.5;
    }
    SECTION("narrowing capacity") {
        json["runtimeProfile"]["capacities"][0] = std::uint64_t{UINT32_MAX} + 1;
    }
    SECTION("unknown overlay field") {
        json["overlay"] = true;
    }
    SECTION("invalid enum") {
        json["runtimeProfile"]["query"][0] = 255;
    }
    SECTION("missing definition") {
        json.erase("definition");
    }
    SECTION("invalid envelope") {
        json["runtimeProfile"]["capacities"][6] = 1;
    }
    REQUIRE(Application::DecodeNavigationProjectSettings(json.dump()).HasError());
    REQUIRE(Application::DecodeNavigationProjectSettings("{\"schema\":2,\"schema\":1}").HasError());
    REQUIRE(Application::DecodeNavigationProjectSettings(std::string(65537, ' ')).HasError());
}

TEST_CASE("Navigation project policy target admission never silently switches provider", "[navigation][settings][compatibility]") {
    const auto settings = Application::NavigationProjectSettings::Create(Policy()).Value();
    Application::NavigationTargetProviderEvidence evidence{"headless-test", "recast_detour",
                                                           N::MakeAvailablePathQueryCapabilities(1, {1024, 128, 1000}, 8)};
    REQUIRE(Application::CheckNavigationProjectCompatibility(settings, evidence).HasValue());
    evidence.provider = "another_provider";
    REQUIRE(Application::CheckNavigationProjectCompatibility(settings, evidence).HasError());
    evidence.provider = "recast_detour";
    evidence.capabilities.availability = N::NavigationProviderAvailability::Unavailable;
    REQUIRE(Application::CheckNavigationProjectCompatibility(settings, evidence).HasError());
    REQUIRE(settings.Input().defaultProvider == "recast_detour");
}

TEST_CASE("Navigation policy save uses real atomic CAS and preserves old bytes before commit", "[navigation][settings][filesystem]") {
    Tests::OwnedTestDirectory directory{"navigation-settings"};
    const auto root = directory.Path() / std::filesystem::path{u8"ayarlar ü"};
    REQUIRE(std::filesystem::create_directory(root));
    FaultFiles files;
    const auto path = root / ".horo/navigation.json";
    Application::NavigationProjectSettingsStore store{files, root, path};
    auto initial = Application::NavigationProjectSettings::Create(Policy()).Value();
    const auto saved = store.Save({}, initial);
    REQUIRE(saved.HasValue());
    REQUIRE_FALSE(saved.Value().durabilityError);
    const auto old = Read(path);
    auto input = Policy();
    input.revision = 2;
    input.runtimeProfile.revision = Id<N::NavigationProjectProfileRevision>(2);
    input.runtimeProfile.capacities.maximumAgents = 96;
    const auto next = Application::NavigationProjectSettings::Create(input).Value();
    SECTION("precommit native failure") {
        files.fault = FaultFiles::Fault::BeforeReplace;
        REQUIRE(store.Save(saved.Value().snapshot, next).HasError());
        REQUIRE(Read(path) == old);
    }
    SECTION("late durability failure is a committed receipt") {
        files.fault = FaultFiles::Fault::AfterReplace;
        const auto publication = store.Save(saved.Value().snapshot, next);
        REQUIRE(publication.HasValue());
        REQUIRE(publication.Value().durabilityError);
        REQUIRE(Read(path) != old);
        REQUIRE(store.Load().Value().settings->Input().revision == 2);
    }
    SECTION("external bytes changed") {
        std::ofstream out(path, std::ios::app);
        out << ' ';
        out.close();
        const auto changed = Read(path);
        REQUIRE(store.Save(saved.Value().snapshot, next).HasError());
        REQUIRE(Read(path) == changed);
    }
    SECTION("stale revision") {
        REQUIRE(store.Save(saved.Value().snapshot, initial).HasError());
        REQUIRE(Read(path) == old);
    }
    SECTION("fresh load successor") {
        REQUIRE(store.Save(store.Load().Value(), next).HasValue());
        REQUIRE(store.Load().Value().settings->Input().runtimeProfile.capacities.maximumAgents == 96);
    }
}

TEST_CASE("Navigation settings store cannot publish to another project source", "[navigation][settings][containment]") {
    Tests::OwnedTestDirectory directory{"navigation-policy-containment"};
    NativeDurableFileSystem files;
    Application::NavigationProjectSettingsStore wrong{files, directory.Path(), directory.Path() / "scene.json"};
    REQUIRE(wrong.Save({}, Application::NavigationProjectSettings::Create(Policy()).Value()).HasError());
    REQUIRE_FALSE(std::filesystem::exists(directory.Path() / "scene.json"));
}
