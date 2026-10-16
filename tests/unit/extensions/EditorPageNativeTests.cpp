#include "EditorPagePackageSupport.h"
#include "EditorPageTestSupport.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/EditorPageAbi.h"
#include "Horo/Extensions/ExtensionAbiConformance.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "SecurityTestSupport.h"

#include <cstddef>

namespace Horo::Extensions::Tests {
    namespace {
        /** @brief Finds one exact native manifest-owned page from the real host projection. */
        const EditorPageProjection &Page(const EditorPageHost &host, std::string_view id) {
            const auto found = std::ranges::find_if(host.Prepared(), [&](const auto &p) {
                return p.surface.id == id;
            });
            REQUIRE(found != host.Prepared().end());
            return *found;
        }

        /** @brief Pumps only the owner lane while actual leased provider work completes. */
        void Wait(EditorPageHost &host, std::string_view id) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (std::chrono::steady_clock::now() < deadline) {
                host.Update();
                const auto state = Page(host, id).state;
                if (state != EditorPageState::Validating && state != EditorPageState::Submitting)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("Native page operation did not finish in its bounded test deadline");
        }

        /** @brief Proves exact integer transport and durable application while the caller retains the module/code lease. */
        void CheckSettings(EditorPageHost &pages, PageSupport::Store &store, int64_t (*integer)(), uint32_t (*calls)(uint32_t)) {
            const auto settings = Page(pages, "fixture.settings");
            CHECK(std::get<std::int64_t>(std::get<EditorUiNumberNode>(settings.form.nodes.front().payload).value) == 9007199254740993LL);
            REQUIRE(pages.Apply(settings.surface.provider, settings.surface.id, settings.revision).HasValue());
            Wait(pages, settings.surface.id);
            CHECK(integer() == 9007199254740993LL);
            CHECK(store.writes == 1);
            CHECK(calls(HORO_EDITOR_PAGE_VALIDATE) == 1);
        }

        /** @brief Native C11 localized validation prevents durable publication while retaining the exact editable draft. */
        void CheckNativeValidation(EditorPageHost &pages, PageSupport::Store &store) {
            const auto before = Page(pages, "fixture.settings");
            const auto writes = store.writes;
            REQUIRE(
                pages.SetValue(before.surface.provider, before.surface.id, before.revision, {{"page.count"}, std::int64_t{-1}}).HasValue());
            pages.Update();
            const auto candidate = Page(pages, before.surface.id);
            REQUIRE(pages.Apply(candidate.surface.provider, candidate.surface.id, candidate.revision).HasValue());
            Wait(pages, candidate.surface.id);
            const auto &after = Page(pages, candidate.surface.id);
            REQUIRE(after.findings.size() == 1);
            CHECK(after.findings.front().code.value == "fixture.negative");
            CHECK(after.findings.front().severity == EditorUiValidationSeverity::Error);
            CHECK(store.writes == writes);
        }

        /** @brief Exercises native root/child validation, submission and parent cancellation under caller-owned module lifetime. */
        void CheckModals(EditorPageHost &pages, PageSupport::Store &store, EditorSurfaceRegistry &registry,
                         HoroExtensionStatus (*open)(uint32_t), HoroExtensionStatus (*close)(uint32_t), uint32_t (*calls)(uint32_t)) {
            REQUIRE(open(1) == HORO_EXTENSION_SUCCESS);
            pages.Update();
            REQUIRE(open(2) == HORO_EXTENSION_SUCCESS);
            pages.Update();
            const auto child = Page(pages, "fixture.child");
            REQUIRE(pages.Apply(child.surface.provider, child.surface.id, child.revision).HasValue());
            Wait(pages, child.surface.id);
            CHECK(Page(pages, child.surface.id).state == EditorPageState::Completed);
            CHECK(calls(HORO_EDITOR_PAGE_SUBMIT) == 1);
            CHECK(store.writes == 1);
            CHECK(registry.Save().surfaces.empty());
            REQUIRE(open(2) == HORO_EXTENSION_SUCCESS);
            pages.Update();
            REQUIRE(close(1) == HORO_EXTENSION_SUCCESS);
            pages.Update();
            CHECK(Page(pages, "fixture.child").state == EditorPageState::Cancelled);
        }
    }  // namespace

    TEST_CASE("ABI1.5 C11 module stages actual settings and modal sessions and retires their exact provider",
              "[Extensions][EditorPage][ABI]") {
        Horo::Tests::EditorPagePackage package;
        JobSystem jobs;
        PageSupport::Store store;
        auto activities = std::make_shared<EditorActivityHost>(jobs);
        auto pages = std::make_shared<EditorPageHost>(jobs, activities->Registry(), store, PageSupport::Policy());
        ExtensionManager manager{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {HORO_EDITOR_PAGE_HOST_CAPABILITY},
                                 Horo::Tests::CreateAcceptingArtifactGate(),
                                 {},
                                 {},
                                 activities,
                                 pages};
        auto loaded = manager.LoadExtension(package.root.string());
        if (loaded.HasError())
            UNSCOPED_INFO(loaded.ErrorValue().message);
        REQUIRE(loaded.HasValue());
        pages->Update();
        REQUIRE(pages->Prepared().size() == 3);
        auto library = Platform::LoadDynamicLibrary((package.root / std::filesystem::path{HORO_EDITOR_PAGE_FIXTURE}.filename()).string());
        REQUIRE(library.HasValue());
        const auto open = reinterpret_cast<HoroExtensionStatus (*)(uint32_t)>(library.Value()->GetSymbol("horo_test_page_open"));
        const auto integer = reinterpret_cast<int64_t (*)()>(library.Value()->GetSymbol("horo_test_page_integer"));
        const auto calls = reinterpret_cast<uint32_t (*)(uint32_t)>(library.Value()->GetSymbol("horo_test_page_calls"));
        const auto close = reinterpret_cast<HoroExtensionStatus (*)(uint32_t)>(library.Value()->GetSymbol("horo_test_page_close"));
        REQUIRE(open);
        REQUIRE(close);
        REQUIRE(integer);
        REQUIRE(calls);
        CHECK(open(2) != HORO_EXTENSION_SUCCESS);  // A child cannot invent an active parent.
        REQUIRE(open(0) == HORO_EXTENSION_SUCCESS);
        pages->Update();
        CheckSettings(*pages, store, integer, calls);
        CheckModals(*pages, store, activities->Registry(), open, close, calls);
        CheckNativeValidation(*pages, store);
        manager.UnloadExtension("fixture.package");
        pages->Update();
        CHECK(pages->Prepared().empty());
        CHECK(open(0) !=
              HORO_EXTENSION_SUCCESS);  // Revocation rejects old opaque-session reentry, even if callback draining delays native unload.
    }

    TEST_CASE("ABI1.5 native pages reject missing capability composition and incomplete host prefixes", "[Extensions][EditorPage][ABI]") {
        Horo::Tests::EditorPagePackage package;
        ExtensionManager headless{nullptr, ExtensionHostProfile::Interactive, {}, Horo::Tests::CreateAcceptingArtifactGate()};
        CHECK(headless.LoadExtension(package.root.string()).HasError());
        auto library = Platform::LoadDynamicLibrary(HORO_EDITOR_PAGE_FIXTURE);
        REQUIRE(library.HasValue());
        const auto load = reinterpret_cast<HoroExtensionLoadFunc>(library.Value()->GetSymbol("horo_extension_load"));
        REQUIRE(load);
        HoroExtensionModuleApi module{.structSize = sizeof(module)};
        HoroExtensionHostApi shortHost{.structSize = offsetof(HoroExtensionHostApi, registerEditorPage),
                                       .abiVersion = 1,
                                       .abiMinorVersion = 4};
        CHECK(load(&shortHost, &module) == HORO_EXTENSION_ERROR_VERSION_MISMATCH);
        const auto report = RunExtensionAbiConformance(HORO_EDITOR_PAGE_FIXTURE);
        CHECK(report.code ==
              ExtensionAbiConformanceCode::NegotiationRejected);  // Importer-only tool does not counterfeit an interactive page host.
    }

    TEST_CASE("Explicit ABI1.4 negotiation cannot expose or activate settings and modal page transport", "[Extensions][EditorPage][ABI]") {
        Horo::Tests::EditorPagePackage package;
        JobSystem jobs;
        PageSupport::Store store;
        auto activities = std::make_shared<EditorActivityHost>(jobs);
        auto pages = std::make_shared<EditorPageHost>(jobs, activities->Registry(), store, PageSupport::Policy());
        auto library = Platform::LoadDynamicLibrary((package.root / std::filesystem::path{HORO_EDITOR_PAGE_FIXTURE}.filename()).string());
        REQUIRE(library.HasValue());
        const auto legacy = reinterpret_cast<void (*)()>(library.Value()->GetSymbol("horo_test_page_legacy_query"));
        REQUIRE(legacy);
        legacy();
        ExtensionManager manager{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {HORO_EDITOR_PAGE_HOST_CAPABILITY},
                                 Horo::Tests::CreateAcceptingArtifactGate(),
                                 {},
                                 {},
                                 activities,
                                 pages};
        CHECK(manager.LoadExtension(package.root.string()).HasError());
        pages->Update();
        CHECK(pages->Prepared().empty());
        CHECK(activities->Registry().Save().surfaces.empty());
        CHECK(store.writes == 0);
    }
}  // namespace Horo::Extensions::Tests
