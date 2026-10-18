#include "EditorPageAbiConversion.h"
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
        /** @brief Copies one page snapshot so subsequent owner updates cannot invalidate its revision or surface identity. */
        EditorPageProjection Page(const EditorPageHost &host, std::string_view id) {
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
                if (const auto state = Page(host, id).state; state != EditorPageState::Validating && state != EditorPageState::Submitting)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("Native page operation did not finish in its bounded test deadline");
        }

        /** @brief Proves exact integer transport and durable application while the caller retains the module/code lease. */
        void CheckSettings(EditorPageHost &pages, const PageSupport::Store &store, int64_t (*integer)(), uint32_t (*calls)(uint32_t)) {
            const auto settings = Page(pages, "fixture.settings");
            CHECK(std::get<std::int64_t>(std::get<EditorUiNumberNode>(settings.form.nodes.front().payload).value) == 9007199254740993LL);
            REQUIRE(pages.Apply(settings.surface.provider, settings.surface.id, settings.revision).HasValue());
            Wait(pages, settings.surface.id);
            CHECK(integer() == 9007199254740993LL);
            CHECK(store.writes == 1);
            CHECK(calls(HORO_EDITOR_PAGE_VALIDATE) == 1);
        }

        /** @brief Native C11 localized validation prevents durable publication while retaining the exact editable draft. */
        void CheckNativeValidation(EditorPageHost &pages, const PageSupport::Store &store) {
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
        void CheckModals(EditorPageHost &pages, const PageSupport::Store &store, const EditorSurfaceRegistry &registry,
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
                                 ExtensionEditorHosts{.activities = activities, .pages = pages}};
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
        HoroExtensionModuleApi moduleApi{.structSize = sizeof(HoroExtensionModuleApi)};
        HoroExtensionHostApi shortHost{.structSize = offsetof(HoroExtensionHostApi, registerEditorPage),
                                       .abiVersion = 1,
                                       .abiMinorVersion = 4};
        CHECK(load(&shortHost, &moduleApi) == HORO_EXTENSION_ERROR_VERSION_MISMATCH);
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
                                 ExtensionEditorHosts{.activities = activities, .pages = pages}};
        CHECK(manager.LoadExtension(package.root.string()).HasError());
        pages->Update();
        CHECK(pages->Prepared().empty());
        CHECK(activities->Registry().Save().surfaces.empty());
        CHECK(store.writes == 0);
    }

    namespace {
        /** @brief Supplies an inert wire descriptor; copying must never invoke its endpoint. */
        HoroEditorPageDescriptor WirePage() {
            return {.structSize = sizeof(HoroEditorPageDescriptor),
                    .schemaVersion = HORO_EDITOR_PAGE_SCHEMA_VERSION,
                    .kind = HORO_EDITOR_PAGE_SETTINGS,
                    .scope = HORO_EDITOR_PAGE_PROJECT,
                    .invoke = +[](void *, const HoroEditorPageOperation *, const HoroEditorPageFindingSink *) -> HoroExtensionStatus {
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            }};
        }
    }  // namespace

    TEST_CASE("Native page copy owns boolean and text defaults in every uniform domain", "[Extensions][EditorPage][ABI]") {
        HoroEditorPageField field{.structSize = sizeof(HoroEditorPageField),
                                  .flags = HORO_EDITOR_PAGE_FIELD_READ_ONLY | HORO_EDITOR_PAGE_FIELD_DISABLED,
                                  .nodeId = {"node", 4},
                                  .labelKey = {"title", 5},
                                  .defaultValue = {.structSize = sizeof(HoroEditorPageValue),
                                                   .kind = HORO_EDITOR_PAGE_BOOLEAN,
                                                   .binding = {"value", 5},
                                                   .booleanValue = 1},
                                  .maximumStringBytes = 32};
        auto wire = WirePage();
        wire.fields = &field;
        wire.fieldCount = 1;
        const std::array scopes{HORO_EDITOR_PAGE_USER, HORO_EDITOR_PAGE_PROJECT, HORO_EDITOR_PAGE_WORKSPACE, HORO_EDITOR_PAGE_SESSION};
        const std::array expected{SettingScope::User, SettingScope::Project, SettingScope::Workspace, SettingScope::Session};
        for (std::size_t index = 0; index < scopes.size(); ++index) {
            wire.scope = scopes[index];
            auto copied = Detail::CopyPageDefinition(wire, "page", "title");
            REQUIRE(copied.HasValue());
            REQUIRE(copied.Value().fields.size() == 1);
            CHECK(copied.Value().fields.front().setting.scope == expected[index]);
            CHECK(std::get<bool>(copied.Value().fields.front().setting.defaultValue));
            const auto &node = std::get<EditorUiBooleanNode>(copied.Value().form.nodes.front().payload);
            CHECK(node.base.readOnly);
            CHECK_FALSE(node.base.enabled);
        }
        std::string text = "owned text";
        field.defaultValue.kind = HORO_EDITOR_PAGE_STRING;
        field.defaultValue.stringValue = {text.data(), static_cast<uint32_t>(text.size())};
        auto copied = Detail::CopyPageDefinition(wire, "page", "title");
        REQUIRE(copied.HasValue());
        text.assign(text.size(), 'x');
        CHECK(std::get<std::string>(copied.Value().fields.front().setting.defaultValue) == "owned text");
        CHECK(std::get<EditorUiTextFieldNode>(copied.Value().form.nodes.front().payload).value == "owned text");
    }

    TEST_CASE("Native page copy rejects malformed foreign strings before publication", "[Extensions][EditorPage][ABI]") {
        auto wire = WirePage();
        HoroEditorActivityMessage message{{"en-US", 5}, {"title", 5}, {"Page", 4}};
        wire.messages = &message;
        wire.messageCount = 1;
        REQUIRE(Detail::CopyPageDefinition(wire, "page", "title").HasValue());
        message.key = {"bad\0key", 7};
        CHECK(Detail::CopyPageDefinition(wire, "page", "title").HasError());
        wire.messageCount = 129;
        CHECK(Detail::CopyPageDefinition(wire, "page", "title").HasError());

        auto permissionWire = WirePage();
        HoroExtensionStringView permission{"editor.settings", 15};
        permissionWire.permissions = &permission;
        permissionWire.permissionCount = 1;
        HoroExtensionStringView diagnostic{"fixture.invalid", 15};
        permissionWire.diagnosticCodes = &diagnostic;
        permissionWire.diagnosticCodeCount = 1;
        EditorSurfaceContextDescriptor context;
        REQUIRE(Detail::CopyPagePermissions(permissionWire, context).HasValue());
        REQUIRE(context.surface.requiredPermissions.size() == 1);
        REQUIRE(context.diagnostics.size() == 1);
        CHECK(context.surface.requiredPermissions.front().value == "editor.settings");
        CHECK(context.diagnostics.front().value == "fixture.invalid");
        permission = {"bad\0permission", 14};
        CHECK(Detail::CopyPagePermissions(permissionWire, context).HasError());
        permission = {"editor.settings", 15};
        diagnostic = {"bad\0code", 8};
        CHECK(Detail::CopyPagePermissions(permissionWire, context).HasError());
        permissionWire.permissionCount = 33;
        CHECK(Detail::CopyPagePermissions(permissionWire, context).HasError());
    }

}  // namespace Horo::Extensions::Tests
