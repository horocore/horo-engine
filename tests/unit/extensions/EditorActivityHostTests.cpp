#include "EditorActivityPackageSupport.h"
#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "SecurityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <thread>
#include <type_traits>

namespace Horo::Extensions::Tests {
    namespace {
        /** @brief Releases the fixture's held provider before manager/job teardown; never owns or outlives the loaded library. */
        class ReleaseBarrier final {
        public:
            explicit ReleaseBarrier(void (*callback)(std::uint32_t)) : release_(callback) {}

            ReleaseBarrier(const ReleaseBarrier &) = delete;
            ReleaseBarrier &operator=(const ReleaseBarrier &) = delete;
            ReleaseBarrier(ReleaseBarrier &&) = delete;
            ReleaseBarrier &operator=(ReleaseBarrier &&) = delete;

            ~ReleaseBarrier() {
                release_(0);
            }

        private:
            void (*release_)(std::uint32_t);
        };

        static_assert(!std::is_copy_constructible_v<ReleaseBarrier> && !std::is_move_constructible_v<ReleaseBarrier>);

        void RequirePackageLoad(ExtensionManager &manager, const std::filesystem::path &path) {
            const auto loaded = manager.LoadExtension(path.string());
            if (loaded.HasError()) {
                for (const Error *error = &loaded.ErrorValue(); error; error = error->cause.Get())
                    UNSCOPED_INFO("Package load error domain=" << error->domain.Value() << " code=" << error->code.Value()
                                                               << " detail=" << error->message);
            }
            REQUIRE(loaded.HasValue());
        }

        /** @brief Pumps the owner lane until the admitted worker publishes a newer revision or the bounded deadline expires. */
        void WaitForActivityRevision(EditorActivityHost &host, const std::uint64_t previousRevision) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (host.Prepared().front().revision == previousRevision && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                host.Update();
            }
        }
    }  // namespace

    TEST_CASE("External ABI activity package publishes real copied SVG/form data and retires its exact generation",
              "[Extensions][EditorSurface][Activity][ABI]") {
        Horo::Tests::EditorActivityPackage package;
        JobSystem jobs;
        auto host = std::make_shared<EditorActivityHost>(jobs);
        const EditorActivityHost &commands = *host;
        ExtensionManager manager{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {HORO_EDITOR_ACTIVITY_HOST_CAPABILITY},
                                 Horo::Tests::CreateAcceptingArtifactGate(),
                                 {},
                                 {},
                                 host};
        RequirePackageLoad(manager, package.root);
        host->Update();
        REQUIRE(host->Prepared().size() == 1);
        const auto initial = host->Prepared().front();
        REQUIRE(initial.icon);
        CHECK(initial.icon->pixels.size() == 48U * 48U * 4U);
        REQUIRE(initial.surface.form);
        CHECK(initial.surface.form->nodes.size() == 2);
        CHECK(host->LocalizedText(initial.surface.descriptor.provider, "fixture.label", "tr-TR") == "Deneme cekmecesi");
        REQUIRE(host->Registry().ToggleActivity(initial.surface.descriptor.provider, "fixture.activity").HasValue());
        host->Update();
        CHECK(host->Prepared().front().surface.open);
        REQUIRE(commands.QueueAction(initial.surface.descriptor.provider, "fixture.activity", "fixture.run", "fixture.run", 1).HasValue());
        CHECK(commands.QueueAction(initial.surface.descriptor.provider, "fixture.activity", "fixture.run", "fixture.run", 1).HasError());
        host->Update();
        WaitForActivityRevision(*host, 1);
        CHECK(host->Prepared().front().revision == 2);
        CHECK(host->Prepared().front().surface.activity.badgeCount == 1);
        CHECK(commands.QueueAction(initial.surface.descriptor.provider, "fixture.activity", "fixture.run", "fixture.run", 1).HasError());
        REQUIRE(host->Registry().SetActivityVisibility("fixture.activity", false).HasValue());
        const auto saved = host->Registry().Save();
        manager.UnloadExtension("fixture.package");
        host->Update();
        CHECK(host->Prepared().empty());
        CHECK_FALSE(host->IsLive(initial.surface.descriptor.provider));
        CHECK(commands.QueueAction(initial.surface.descriptor.provider, "fixture.activity", "fixture.run", "fixture.run", 2).HasError());
        RequirePackageLoad(manager, package.root);
        host->Update();
        REQUIRE(host->Prepared().size() == 1);
        CHECK(host->Prepared().front().surface.descriptor.provider.activationGeneration !=
              initial.surface.descriptor.provider.activationGeneration);
        REQUIRE(host->Registry().Restore(saved).HasValue());
        host->Update();
        CHECK_FALSE(host->Prepared().front().surface.activity.visible);
    }

    TEST_CASE("External activity callback rejects missing and short action prefixes before reading members",
              "[Extensions][Activity][ABI]") {
        Horo::Tests::EditorActivityPackage package;
        const auto library =
            Platform::LoadDynamicLibrary((package.root / std::filesystem::path{HORO_EDITOR_ACTIVITY_FIXTURE}.filename()).string());
        REQUIRE(library.HasValue());
        const auto invoke = reinterpret_cast<HoroExtensionStatus (*)(const HoroEditorActivityAction *)>(
            library.Value()->GetSymbol("horo_test_activity_invoke_prefix"));
        REQUIRE(invoke);
        CHECK(invoke(nullptr) == HORO_EXTENSION_ERROR_VERSION_MISMATCH);
        const HoroEditorActivityAction shortPrefix{.structSize = offsetof(HoroEditorActivityAction, revision)};
        CHECK(invoke(&shortPrefix) == HORO_EXTENSION_ERROR_VERSION_MISMATCH);
    }

    TEST_CASE("Cancelled action results cannot publish after the provider successfully completes", "[Extensions][Activity][ABI]") {
        Horo::Tests::EditorActivityPackage package;
        const auto library =
            Platform::LoadDynamicLibrary((package.root / std::filesystem::path{HORO_EDITOR_ACTIVITY_FIXTURE}.filename()).string());
        REQUIRE(library.HasValue());
        const auto hold = reinterpret_cast<void (*)(std::uint32_t)>(library.Value()->GetSymbol("horo_test_activity_hold_action"));
        const auto published = reinterpret_cast<std::uint32_t (*)()>(library.Value()->GetSymbol("horo_test_activity_result_published"));
        REQUIRE(hold);
        REQUIRE(published);
        JobSystem jobs;
        auto host = std::make_shared<EditorActivityHost>(jobs);
        ExtensionManager manager{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {HORO_EDITOR_ACTIVITY_HOST_CAPABILITY},
                                 Horo::Tests::CreateAcceptingArtifactGate(),
                                 {},
                                 {},
                                 host};
        RequirePackageLoad(manager, package.root);
        host->Update();
        const auto provider = host->Prepared().front().surface.descriptor.provider;

        hold(1);
        ReleaseBarrier release{hold};
        REQUIRE(host->QueueAction(provider, "fixture.activity", "fixture.run", "fixture.run", 1).HasValue());
        host->Update();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!published() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE(published());
        const auto snapshot = jobs.SnapshotIfChanged(0);
        REQUIRE(snapshot);
        REQUIRE(snapshot->jobs.size() == 1);
        REQUIRE(jobs.RequestCancel(snapshot->jobs.front().id).HasValue());
        hold(0);
        jobs.Shutdown(ShutdownPolicy::Drain);
        host->Update();
        REQUIRE(host->Prepared().size() == 1);
        CHECK(host->Prepared().front().revision == 1);
        CHECK(host->Prepared().front().surface.activity.badgeCount == 0);
    }

    void RewriteFixtureManifest(const Horo::Tests::EditorActivityPackage &package, const std::string_view expected,
                                const std::string_view replacement) {
        const auto path = package.root / "extension.json";
        std::ifstream input{path};
        REQUIRE(input);
        std::string manifest{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        const auto position = manifest.find(expected);
        REQUIRE(position != std::string::npos);
        manifest.replace(position, expected.size(), replacement);
        input.close();
        std::ofstream output{path};
        output << manifest;
        REQUIRE(output);
    }

    void CheckInvalidCapability(const Horo::Tests::EditorActivityPackage &package, const std::shared_ptr<EditorActivityHost> &host) {
        RewriteFixtureManifest(package, R"("requiredCapabilities":["editor.activity"])",
                               R"("requiredCapabilities":["editor.activity_item"])");
        ExtensionManager manager{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {HORO_EDITOR_ACTIVITY_HOST_CAPABILITY},
                                 Horo::Tests::CreateAcceptingArtifactGate(),
                                 {},
                                 {},
                                 host};
        const auto rejected = manager.LoadExtension(package.root.string());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "invalid_manifest");
        CHECK(host->Prepared().empty());
    }

    void CheckMissingCapability(const Horo::Tests::EditorActivityPackage &package, const std::shared_ptr<EditorActivityHost> &host) {
        ExtensionManager manager{nullptr, ExtensionHostProfile::Interactive, {}, Horo::Tests::CreateAcceptingArtifactGate(), {}, {}, host};
        CHECK(manager.LoadExtension(package.root.string()).HasError());
        host->Update();
        CHECK(host->Prepared().empty());
    }

    void CheckHeadlessCapability(const Horo::Tests::EditorActivityPackage &package, const std::shared_ptr<EditorActivityHost> &host) {
        // A mixed-role module is selected by headless resolution, so its explicit ABI requirement must reject.
        RewriteFixtureManifest(package, R"("roles":["editor-presentation"])", R"("roles":["backend-capability","editor-presentation"])");
        ExtensionManager manager{nullptr,
                                 ExtensionHostProfile::Headless,
                                 {HORO_EDITOR_ACTIVITY_HOST_CAPABILITY},
                                 Horo::Tests::CreateAcceptingArtifactGate(),
                                 {},
                                 {},
                                 host};
        CHECK(manager.LoadExtension(package.root.string()).HasError());
        host->Update();
        CHECK(host->Prepared().empty());
    }

    void CheckMissingTrust(const Horo::Tests::EditorActivityPackage &package, const std::shared_ptr<EditorActivityHost> &host) {
        ExtensionManager manager{nullptr, ExtensionHostProfile::Interactive, {HORO_EDITOR_ACTIVITY_HOST_CAPABILITY}, {}, {}, {}, host};
        CHECK(manager.LoadExtension(package.root.string()).HasError());
        host->Update();
        CHECK(host->Prepared().empty());
    }

    void CheckExternalResource(const Horo::Tests::EditorActivityPackage &package, const std::shared_ptr<EditorActivityHost> &host) {
        package.Icon(R"(<svg xmlns="http://www.w3.org/2000/svg"><image href="file:///etc/passwd"/></svg>)");
        ExtensionManager manager{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {HORO_EDITOR_ACTIVITY_HOST_CAPABILITY},
                                 Horo::Tests::CreateAcceptingArtifactGate(),
                                 {},
                                 {},
                                 host};
        CHECK(manager.LoadExtension(package.root.string()).HasError());
        host->Update();
        CHECK(host->Prepared().empty());
    }

    void CheckMissingHost(const Horo::Tests::EditorActivityPackage &package) {
        ExtensionManager manager{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {HORO_EDITOR_ACTIVITY_HOST_CAPABILITY},
                                 Horo::Tests::CreateAcceptingArtifactGate()};
        CHECK(manager.LoadExtension(package.root.string()).HasError());
    }

    TEST_CASE("ABI activity package rejects unsupported resources and absent application trust composition",
              "[Extensions][EditorSurface][Activity][ABI]") {
        Horo::Tests::EditorActivityPackage package;
        JobSystem jobs;
        auto host = std::make_shared<EditorActivityHost>(jobs);
        SECTION("contribution-point spelling is not a canonical host capability") {
            CheckInvalidCapability(package, host);
        }
        SECTION("canonical transport capability is rejected when not advertised") {
            CheckMissingCapability(package, host);
        }
        SECTION("headless composition cannot advertise the graphical transport") {
            CheckHeadlessCapability(package, host);
        }
        SECTION("missing trust remains fail closed") {
            CheckMissingTrust(package, host);
        }
        SECTION("active or external SVG resources never reach the live registry") {
            CheckExternalResource(package, host);
        }
        SECTION("an uncomposed surface host rejects explicit ABI 1.4 requirements") {
            CheckMissingHost(package);
        }
    }
}  // namespace Horo::Extensions::Tests
