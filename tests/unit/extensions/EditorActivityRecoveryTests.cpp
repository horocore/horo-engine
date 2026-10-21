#include "AllocationProbe.h"
#include "EditorActivityPackageSupport.h"
#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Extensions/ExtensionRetirement.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Platform.h"
#include "SecurityTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <fstream>

namespace Horo::Extensions::Tests {
    namespace {
        struct HostLoaderFailure final {};

        /** @brief Owns the real native fixture and its explicit host loader through recovery and teardown. */
        struct NativeRecoveryFixture final {
            bool rejectLoad_{};
            std::size_t loadAttempts_{};
            const Horo::Tests::AllocationProbe::ScopedMeasurement *measurement_{};
            std::size_t admissionRequests_{};

            Horo::Tests::EditorActivityPackage package;
            JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8}};
            std::shared_ptr<EditorActivityHost> host{std::make_shared<EditorActivityHost>(jobs, Horo::Tests::EditorCompositionPolicy())};
            ExtensionManager manager;

            NativeRecoveryFixture()
                : manager{nullptr,
                          ExtensionHostProfile::Interactive,
                          {HORO_EDITOR_ACTIVITY_HOST_CAPABILITY, HORO_EDITOR_CUSTOM_UI_HOST_CAPABILITY},
                          Horo::Tests::CreateAcceptingArtifactGate(),
                          [this](const std::string &path) {
                              if (measurement_)
                                  admissionRequests_ = measurement_->Snapshot().requests;
                              ++loadAttempts_;
                              if (rejectLoad_)
                                  throw HostLoaderFailure{};
                              return Platform::LoadDynamicLibrary(path);
                          },
                          {},
                          ExtensionEditorHosts{.activities = host}} {}

            void RejectNativeLoad(const bool reject = true) noexcept {
                rejectLoad_ = reject;
            }

            void MeasureAdmission(const Horo::Tests::AllocationProbe::ScopedMeasurement *measurement) noexcept {
                measurement_ = measurement;
            }

            [[nodiscard]] std::size_t AdmissionRequests() const noexcept {
                return admissionRequests_;
            }

            void RetainIndependentOwner() const {
                auto saved = host->Registry().Save();
                saved.surfaces.push_back(
                    {.surfaceId = "independent.drawer", .provider = {"fixture.package", "independent.module"}, .open = true});
                REQUIRE(host->Registry().Restore(saved).HasValue());
                REQUIRE(host->Registry()
                            .SetProviderStatus({"fixture.package", "independent.module"}, EditorSurfaceProviderStatus::Incompatible)
                            .HasValue());
            }

            void IncludeSecondSelectedModule() const {
                const auto manifestPath = package.root / "extension.json";
                std::ifstream input{manifestPath};
                std::string manifest{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
                input.close();
                const auto position = manifest.find("],\"contributions\"");
                REQUIRE(position != std::string::npos);
                manifest.insert(
                    position,
                    ",{" +
                        std::string{
                            R"("id":"fixture.second","version":"1.0.0","kind":"native","roles":["editor-presentation"],"entry":")"} +
                        std::filesystem::path{HORO_EDITOR_ACTIVITY_FIXTURE}.filename().string() +
                        R"(","abi":{"major":1,"minimumMinor":4},"requiredCapabilities":["editor.activity","editor.custom-composition"]})");
                std::ofstream output{manifestPath};
                output << manifest;
                output.close();
                REQUIRE(output.good());
                auto saved = host->Registry().Save();
                saved.surfaces.push_back({.surfaceId = "selected.first", .provider = {"fixture.package", "fixture.module"}, .open = true});
                saved.surfaces.push_back({.surfaceId = "selected.second", .provider = {"fixture.package", "fixture.second"}, .open = true});
                REQUIRE(host->Registry().Restore(saved).HasValue());
            }

            void RequireAtomicSelectedStatus() const {
                const auto entries = host->Registry().RecoverySnapshot();
                const auto first = std::ranges::find(entries, std::string{"selected.first"}, [](const auto &entry) {
                    return entry.intent.surfaceId;
                });
                const auto second = std::ranges::find(entries, std::string{"selected.second"}, [](const auto &entry) {
                    return entry.intent.surfaceId;
                });
                REQUIRE(first != entries.end());
                REQUIRE(second != entries.end());
                CHECK(first->status == second->status);
                CHECK(first->status != EditorSurfaceProviderStatus::Active);
            }

            void RequireIndependentOwner() const {
                const auto entries = host->Registry().RecoverySnapshot();
                const auto found = std::ranges::find(entries, std::string{"independent.drawer"}, [](const auto &entry) {
                    return entry.intent.surfaceId;
                });
                REQUIRE(found != entries.end());
                CHECK(found->status == EditorSurfaceProviderStatus::Incompatible);
                CHECK(found->intent.open);
            }

            [[nodiscard]] std::size_t LoadAttempts() const noexcept {
                return loadAttempts_;
            }

            void Open() {
                REQUIRE(manager.LoadExtension(package.root.string()).HasValue());
                host->Update();
                REQUIRE(host->Prepared().size() == 1);
                const auto provider = host->Prepared().front().surface.descriptor.provider;
                REQUIRE(host->Registry().ToggleActivity(provider, "fixture.activity").HasValue());
            }
        };

#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL == 0
        /** @brief Checks one injected admission failure while the caller retains the complete native fixture and selection. */
        void RequireRejectedAdmission(NativeRecoveryFixture &candidate, const std::string &directory,
                                      const std::array<std::string, 2> &selected, const std::size_t index) {
            namespace Probe = Horo::Tests::AllocationProbe;
            bool reachedNative{};
            bool rejected{};
            {
                const Probe::ScopedFailure failure{index};
                try {
                    const auto result = candidate.manager.LoadExtension(directory, {}, selected);
                    rejected = result.HasError();
                } catch (const std::bad_alloc &) {
                    // Either the original allocation exception or its typed admission error is permitted.
                    rejected = true;
                } catch (const HostLoaderFailure &) {
                    reachedNative = true;
                }
            }
            CHECK_FALSE(reachedNative);
            CHECK(rejected);
            CHECK(candidate.LoadAttempts() == 0);
            CHECK(candidate.manager.GetLoadedExtensionIds().empty());
            candidate.RequireIndependentOwner();
            candidate.RequireAtomicSelectedStatus();
        }
#endif
    }  // namespace

    TEST_CASE("Real native provider failure and reinstallation retain paired workspace state",
              "[Extensions][EditorSurface][Recovery][ABI]") {
        NativeRecoveryFixture fixture;
        fixture.Open();
        const auto &package = fixture.package;
        const auto &host = fixture.host;
        auto &manager = fixture.manager;
        const std::array<std::uint8_t, 3> bytes{0, 255, 9};
        REQUIRE(host->Registry().SetOpaqueState("fixture.drawer", bytes).HasValue());
        manager.UnloadExtension("fixture.package");
        host->Update();
        REQUIRE(host->Registry().RecoverySnapshot().size() == 2);
        CHECK(host->Prepared().empty());
        package.Icon("invalid svg");
        REQUIRE(manager.LoadExtension(package.root.string()).HasError());
        for (const auto &entry : host->Registry().RecoverySnapshot())
            CHECK(entry.status == EditorSurfaceProviderStatus::Failed);
        package.Icon(
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 48 48"><path fill="#ffffff" d="M4 4 H44 V44 H4 Z"/></svg>)");
        REQUIRE(manager.LoadExtension(package.root.string()).HasValue());
        host->Update();
        CHECK(host->Registry().RecoverySnapshot().empty());
        REQUIRE(host->Prepared().size() == 1);
        CHECK(host->Prepared().front().surface.open);
        const auto saved = host->Registry().Save();
        const auto drawer = std::ranges::find(saved.surfaces, std::string{"fixture.drawer"}, &EditorSurfaceWorkspaceEntry::surfaceId);
        REQUIRE(drawer != saved.surfaces.end());
        CHECK(drawer->opaqueState == std::vector<std::uint8_t>{0, 255, 9});
        manager.UnloadExtension("fixture.package");
        REQUIRE(host->Registry().CloseProviderWorkspace({"fixture.package", "fixture.module"}).HasValue());
        for (const auto &intent : host->Registry().Save().surfaces)
            CHECK_FALSE(intent.open);
        REQUIRE(manager.LoadExtension(package.root.string()).HasValue());
        host->Update();
        REQUIRE(host->Prepared().size() == 1);
        CHECK_FALSE(host->Prepared().front().surface.open);
    }

    TEST_CASE("Foreign native loader unwinding restores failure presentation without masking the original exception",
              "[Extensions][EditorSurface][Recovery][ABI]") {
        NativeRecoveryFixture fixture;
        fixture.Open();
        const std::array<std::uint8_t, 3> bytes{9, 0, 255};
        REQUIRE(fixture.host->Registry().SetOpaqueState("fixture.drawer", bytes).HasValue());
        fixture.manager.UnloadExtension("fixture.package");
        fixture.host->Update();
        const auto before = fixture.host->Registry().Save();
        fixture.RejectNativeLoad();
        REQUIRE_THROWS_AS(fixture.manager.LoadExtension(fixture.package.root.string()), HostLoaderFailure);
        CHECK(fixture.LoadAttempts() == 2);
        CHECK(fixture.manager.GetLoadedExtensionIds().empty());
        const auto retired = fixture.manager.FailedActivationRetirements();
        REQUIRE(retired.size() == 1);
        CHECK(retired.front()->IsDrained());
        const auto recovery = fixture.host->Registry().RecoverySnapshot();
        REQUIRE(recovery.size() == before.surfaces.size());
        for (const auto &entry : recovery) {
            CHECK(entry.status == EditorSurfaceProviderStatus::Failed);
            const auto saved = std::ranges::find(before.surfaces, entry.intent.surfaceId, &EditorSurfaceWorkspaceEntry::surfaceId);
            REQUIRE(saved != before.surfaces.end());
            CHECK(entry.intent.provider == saved->provider);
            CHECK(entry.intent.open == saved->open);
            CHECK(entry.intent.focused == saved->focused);
            CHECK(entry.intent.activityVisible == saved->activityVisible);
            CHECK(entry.intent.opaqueState == saved->opaqueState);
            CHECK(entry.intent.activityPlacement == saved->activityPlacement);
        }
    }

    TEST_CASE("Invalid manifest file bytes reject recovery before native loading and preserve independent ownership",
              "[Extensions][EditorSurface][Recovery][Manifest]") {
        for (const std::string_view content : {"", R"({"id":"fixture.package")", "{} trailing bytes"}) {
            NativeRecoveryFixture fixture;
            fixture.RetainIndependentOwner();
            fixture.RejectNativeLoad();
            {
                std::ofstream output{fixture.package.root / "extension.json", std::ios::binary | std::ios::trunc};
                output.write(content.data(), static_cast<std::streamsize>(content.size()));
                output.close();
                REQUIRE(output.good());
            }
            const auto loaded = fixture.manager.LoadExtension(fixture.package.root.string());
            REQUIRE(loaded.HasError());
            CHECK(loaded.ErrorValue().code.Value() == ExtensionErrors::InvalidManifest.code.Value());
            CHECK(fixture.LoadAttempts() == 0);
            CHECK(fixture.manager.GetLoadedExtensionIds().empty());
            fixture.RequireIndependentOwner();
        }
    }

    TEST_CASE("Selected provider failure and retry preserve independently owned recovery status",
              "[Extensions][EditorSurface][Recovery][ABI]") {
        NativeRecoveryFixture fixture;
        fixture.RetainIndependentOwner();
        const std::array<std::string, 1> selected{"fixture.module"};
        fixture.RejectNativeLoad();
        REQUIRE_THROWS_AS(fixture.manager.LoadExtension(fixture.package.root.string(), {}, selected), HostLoaderFailure);
        fixture.RequireIndependentOwner();
        fixture.RejectNativeLoad(false);
        REQUIRE(fixture.manager.LoadExtension(fixture.package.root.string(), {}, selected).HasValue());
        fixture.host->Update();
        REQUIRE(fixture.host->Prepared().size() == 1);
        fixture.RequireIndependentOwner();
        fixture.manager.UnloadExtension("fixture.package");
        fixture.RequireIndependentOwner();
    }

#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL == 0
    TEST_CASE("Every recovery admission allocation failure precedes native callbacks and preserves independent status",
              "[Extensions][EditorSurface][Recovery][Allocation]") {
        namespace Probe = Horo::Tests::AllocationProbe;
        NativeRecoveryFixture fixture;
        fixture.RetainIndependentOwner();
        fixture.IncludeSecondSelectedModule();
        const std::array<std::string, 2> selected{"fixture.module", "fixture.second"};
        const auto directory = fixture.package.root.string();
        fixture.RejectNativeLoad();
        bool loaderRejected{};
        {
            const Probe::ScopedMeasurement measurement;
            fixture.MeasureAdmission(&measurement);
            try {
                static_cast<void>(fixture.manager.LoadExtension(directory, {}, selected));
            } catch (const HostLoaderFailure &) {
                // The loader snapshots the exact preparatory allocation prefix before throwing without allocating.
                loaderRejected = true;
            }
            fixture.MeasureAdmission(nullptr);
        }
        REQUIRE(loaderRejected);
        const auto count = fixture.AdmissionRequests();
        REQUIRE(count > 0);
        std::size_t completed{};
        for (std::size_t index = 0; index < count; ++index) {
            INFO(index);
            NativeRecoveryFixture candidate;
            candidate.RetainIndependentOwner();
            candidate.IncludeSecondSelectedModule();
            candidate.RejectNativeLoad();
            const auto candidateDirectory = candidate.package.root.string();
            RequireRejectedAdmission(candidate, candidateDirectory, selected, index);
            ++completed;
        }
        std::fprintf(stderr, "[recovery-admission-allocation] measured=%zu completed=%zu\n", count, completed);
        CHECK(completed == count);
    }
#endif
}  // namespace Horo::Extensions::Tests
