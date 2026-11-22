#include "Horo/Editor/SourceFileOpenService.h"
#include "LuaModuleHostApi.h"
#include "ScriptModuleProductRecoverySupport.h"
#include "editor/source_editor/HoroScriptModuleLanguage.h"

#include <algorithm>

namespace Horo::Extensions::Tests {
    using namespace ProductRecoverySupport;

    namespace {
        /** @brief Exercises editor queries on the same signed source and registered API used by the host. */
        void CheckSignedImportLanguageQueries(const SignedPackage &package, std::span<const ScriptModuleApiMetadata> imports) {
            using namespace Editor::Language;
            using enum Editor::SourceLanguageRequestKind;
            Editor::SourceFileOpenService sources{package.package};
            const auto opened = Take(sources.Open({.path = package.source}));
            REQUIRE(opened.sourceSnapshot);
            const auto &source = *opened.sourceSnapshot;
            const auto member = source.Text().find(".add");
            REQUIRE(member != std::string_view::npos);
            const auto name = member + 1;
            const auto completion = Take(QueryHoroScriptApi(source, Completion, name + 2, imports));
            const auto &choices = std::get<LanguageCompletions>(completion.payload).items;
            REQUIRE(choices.size() == 1);
            CHECK(choices.front().label == "add");
            CHECK(choices.front().primaryEdit == LanguageByteSpan{name, name + 3});
            const auto hover = Take(QueryHoroScriptApi(source, Hover, name + 1, imports));
            CHECK(std::get<LanguageHover>(hover.payload).text.starts_with("add("));
        }
    }  // namespace

    TEST_CASE("Script authoring metadata uses the actual provider generation and preserves denied imports as unavailable",
              "[Extensions][ScriptModule][Authoring]") {
        SignedPackage package{ScriptModuleRuntimeKind::Lua54, "error('metadata queries must never execute this script')\n"
                                                              "return horo.import('backend-import').com.horo.concurrent.api.add(20, 22)"};
        package.Capture();
        Headless host{package};
        const auto program = package.Program();
        const auto policy = host.Policy();
        const auto beforeInstall = Take(host.modules->DescribeApis(*program, policy));
        const auto initialRevision = host.modules->AuthoringApiRevision();
        CHECK(initialRevision != 0);
        REQUIRE(beforeInstall.size() == 2);
        CHECK(beforeInstall[0].alias == "backend-import");
        REQUIRE(beforeInstall[0].descriptor);
        CHECK(beforeInstall[0].descriptor->nameSpace == "com.horo.concurrent.api");
        CHECK(std::ranges::find(beforeInstall[0].descriptor->functions, "add", &ScriptExportFunctionDescriptor::id) !=
              beforeInstall[0].descriptor->functions.end());
        CHECK(beforeInstall[1].alias == "horo.reference");
        CHECK_FALSE(beforeInstall[1].descriptor);
        REQUIRE(beforeInstall[1].unavailable);
        CHECK(beforeInstall[1].unavailable->code.Value() == ExtensionErrors::ScriptInvocationUnavailable.code.Value());

        CheckSignedImportLanguageQueries(package, beforeInstall);
        const auto active = Take(host.modules->DescribeActiveModuleApis(package.id, package.module, host.project.Lease()));
        REQUIRE(active.size() == 2);
        REQUIRE(active[0].descriptor);
        CHECK(active[0].descriptor->nameSpace == beforeInstall[0].descriptor->nameSpace);
        CHECK(host.modules->DescribeActiveModuleApis(package.id, "not-loaded", host.project.Lease()).HasError());

        Accept(host.modules->PrepareReference(true));
        const auto installedRevision = host.modules->AuthoringApiRevision();
        CHECK(installedRevision > initialRevision);
        const auto retained = Take(host.modules->DescribeApis(*program, policy));
        REQUIRE(retained[1].descriptor);
        CHECK(retained[1].descriptor->version == ScriptExportVersion{1, 0, 0});
        CHECK(Take(host.modules->RefreshReference({1, 1, 0})).empty());
        CHECK(host.modules->AuthoringApiRevision() > installedRevision);
        const auto current = Take(host.modules->DescribeApis(*program, policy));
        REQUIRE(current[1].descriptor);
        CHECK(current[1].descriptor->version == ScriptExportVersion{1, 1, 0});
        CHECK(std::ranges::find(current[1].descriptor->functions, "apirevision", &ScriptExportFunctionDescriptor::id) !=
              current[1].descriptor->functions.end());
        CHECK(retained[1].descriptor->version == ScriptExportVersion{1, 0, 0});
        CHECK(Take(host.modules->PollProviderRetirement()));

        auto restricted = host.Policy();
        restricted.contextCapabilities = {{"horo.script.reference"}};
        const auto partial = Take(host.modules->DescribeApis(*program, restricted));
        CHECK_FALSE(partial[0].descriptor);
        REQUIRE(partial[0].unavailable);
        CHECK(partial[0].unavailable->code.Value() == ExtensionErrors::CapabilityUnavailable.code.Value());
        CHECK(partial[1].descriptor.has_value());
        host.project.Revoke();
        CHECK(host.modules->DescribeApis(*program, policy).HasError());
        CHECK(host.modules->DescribeActiveModuleApis(package.id, package.module, host.project.Lease()).HasError());
    }

    TEST_CASE("Unloading an actual native provider invalidates retained authoring metadata", "[Extensions][ScriptModule][Authoring]") {
        SignedPackage package{ScriptModuleRuntimeKind::Lua54, "return 42"};
        package.Capture();
        Interactive host{package};
        ScriptCapabilityScope project{ScriptCapabilityScopeKind::Project};
        const auto active = Take(host.modules->DescribeActiveModuleApis(package.id, package.module, project.Lease()));
        REQUIRE_FALSE(active.empty());
        const auto loaded = host.modules->AuthoringApiRevision();
        host.manager->UnloadAll();
        CHECK(host.modules->AuthoringApiRevision() > loaded);
        CHECK(host.modules->DescribeActiveModuleApis(package.id, package.module, project.Lease()).HasError());
        host.native->BeginShutdown();
        CHECK(host.modules->AuthoringApiRevision() == 0);
    }

    TEST_CASE("A signed Lua operation exposes the engine-owned authoring catalog", "[Extensions][ScriptModule][Authoring]") {
        std::string source = "local count = 0\nfor _ in pairs(horo) do count = count + 1 end\nassert(count == ";
        source += std::to_string(LuaModuleHostMembers.size()) + ")\n";
        for (const auto &member : LuaModuleHostMembers)
            source += std::string{"assert(horo."} + member.name + " ~= nil)\n";
        source += "assert(horo.u64('42') == horo.u64('42'))\n";
        source += "local imported = horo.import('backend-import')\nassert(imported.available and imported.code == nil)\n";
        source += "return imported.com.horo.concurrent.api.add(20, 22)";
        SignedPackage package{ScriptModuleRuntimeKind::Lua54, source};
        package.Capture();
        Headless host{package};
        auto operation = Take(host.modules->Begin(package.Program(), host.Policy()));
        Answer42(Await(*operation));
    }

    TEST_CASE("Actual signed Headless operations retain the same two VM states across a real additive provider update",
              "[Extensions][ScriptModule][Recovery][Headless][TwoVm]") {
        for (const auto vm : Vms) {
            SignedPackage package{vm, Source(vm), true, true};
            package.Capture();
            Headless host{package};
            auto operation = Take(host.modules->Begin(package.Program(), host.Policy()));
            REQUIRE_FALSE(operation->ResultSnapshot());
            auto reports = Take(host.modules->RefreshReference({1, 1, 0}));
            REQUIRE(reports.size() == 1);
            Compatible(reports.front());
            CHECK_FALSE(Take(host.modules->PollProviderRetirement()));
            Answer42(Await(*operation));
            REQUIRE(Take(host.modules->PollProviderRetirement()));
            CHECK(host.host->Inspect().state == HeadlessExtensionHostState::Ready);
        }
    }

    TEST_CASE("One rejected actual Headless operation prevents every prepared VM in the provider batch from publishing",
              "[Extensions][ScriptModule][Recovery][Headless][LastGood]") {
        for (const auto vm : Vms) {
            SignedPackage package{vm, Source(vm, false), true, true};
            package.Capture();
            Headless host{package};
            ScriptCapabilityScope secondScope{ScriptCapabilityScopeKind::Operation};
            auto secondPolicy = host.Policy();
            secondPolicy.scopes = {host.project.Lease(), secondScope.Lease()};
            auto first = Take(host.modules->Begin(package.Program(), host.Policy()));
            auto second = Take(host.modules->Begin(package.Program(), std::move(secondPolicy)));
            REQUIRE_FALSE(first->ResultSnapshot());
            REQUIRE_FALSE(second->ResultSnapshot());
            secondScope.Revoke();
            auto update = host.modules->RefreshReference({1, 1, 0});
            REQUIRE(update.HasError());
            CHECK_FALSE(update.ErrorValue().message.empty());
            Answer42(Await(*first));
            CHECK(host.host->Inspect().state == HeadlessExtensionHostState::Ready);
            second->Cancel();
        }
    }

    TEST_CASE("A real live native package update is classified RestartRequired without revoking its working provider",
              "[Extensions][ScriptModule][Recovery][Native]") {
        for (const auto vm : Vms) {
            const auto source = vm == ScriptModuleRuntimeKind::Lua54
                                    ? R"(return horo.import("backend-import").com.horo.concurrent.api.add(20,22))"
                                    : R"(return horo.import("backend-import").com.horo.concurrent.api.add(20n,22n);)";
            SignedPackage package{vm, source};
            package.Capture();
            Headless host{package};
            const auto program = package.Program();
            auto before = Take(host.modules->Begin(program, host.Policy()));
            Answer42(Await(*before));
            const auto suffix = std::filesystem::path{HORO_NATIVE_SERVICE_PROVIDER_FIXTURE}.extension().string();
            const auto changed = package.package / ("backend" + suffix);
            {
                std::ofstream file{changed, std::ios::binary | std::ios::app};
                REQUIRE(file);
                file.put('x');
                REQUIRE(file);
            }
            package.Sign(changed);
            package.Capture();
            const auto candidate = package.Program();
            const auto report = Take(host.modules->RequestNativeUpdate(*candidate));
            CHECK(report.kind == ScriptModuleUpdateKind::RestartRequired);
            REQUIRE(report.diagnostic);
            CHECK_FALSE(report.diagnostic->message.empty());
            CHECK(host.modules->Begin(candidate, host.Policy()).HasError());
            auto after = Take(host.modules->Begin(program, host.Policy()));
            Answer42(Await(*after));
            CHECK(host.host->Inspect().state == HeadlessExtensionHostState::Ready);
        }
    }

    TEST_CASE("Actual current trust denial rejects an update candidate while preserving the already admitted native provider",
              "[Extensions][ScriptModule][Recovery][Security]") {
        SignedPackage package{ScriptModuleRuntimeKind::Lua54, SumSource(ScriptModuleRuntimeKind::Lua54)};
        package.Capture();
        Headless host{package};
        const auto program = package.Program();
        auto operation = Take(host.modules->Begin(program, host.Policy()));
        Answer42(Await(*operation));
        package.Trust(true);
        CHECK(host.modules->RequestNativeUpdate(*program).HasError());
        CHECK(host.modules->RefreshReference({1, 1, 0}).HasValue());
        CHECK(host.host->Inspect().state == HeadlessExtensionHostState::Ready);
        CHECK(host.modules->Begin(program, host.Policy()).HasError());
    }
}  // namespace Horo::Extensions::Tests
