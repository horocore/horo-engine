#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/EditorInspectorAbi.h"
#include "Horo/Extensions/EditorInspectorHost.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "OwnedTestDirectory.h"
#include "SecurityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace Horo::Extensions::Tests {
    namespace {
        /** @brief Owns an actual isolated C11 module/package; native descriptors are admitted by the production loader. */
        struct InspectorPackage final {
            Horo::Tests::OwnedTestDirectory directory{"horo110 native inspector"};
            const std::filesystem::path root = directory.Path();

            InspectorPackage() {
                const std::filesystem::path library{HORO_EDITOR_INSPECTOR_FIXTURE};
                std::filesystem::copy_file(library, root / library.filename());
                std::ofstream manifest{root / "extension.json"};
                manifest
                    << R"({"id":"fixture.package","version":"1.0.0","modules":[{"id":"fixture.module","version":"1.0.0","kind":"native","roles":["editor-presentation"],"entry":")"
                    << library.filename().string()
                    << R"(","abi":{"major":1,"minimumMinor":5},"requiredCapabilities":["editor.inspector"]}],"contributions":[{"type":"editor.inspector","id":"fixture.inspector","module":"fixture.module"}]})";
            }
        };

        /** @brief Explicit host policy supplies the exact permission/capability; the module cannot self-approve document fields. */
        ExtensionAdmissionPolicy Policy() {
            return {.revision = 1,
                    .knownPermissions = {{"editor.inspector"}},
                    .approvedPermissions = {{"editor.inspector"}},
                    .availableCapabilities = {EditorInspectorCapability}};
        }

        /** @brief Keeps one real composition alive through native registration and whole-provider retirement. */
        struct Host final {
            JobSystem jobs;
            std::shared_ptr<EditorActivityHost> activities = std::make_shared<EditorActivityHost>(jobs);
            std::shared_ptr<EditorInspectorHost> inspectors = std::make_shared<EditorInspectorHost>(activities->Registry(), Policy());
            ExtensionManager manager{nullptr,
                                     ExtensionHostProfile::Interactive,
                                     {HORO_EDITOR_INSPECTOR_HOST_CAPABILITY},
                                     Horo::Tests::CreateAcceptingArtifactGate(),
                                     {},
                                     {},
                                     {.activities = activities, .inspectors = inspectors}};
        };
    }  // namespace

    TEST_CASE("ABI1.6 native declarations preserve exact integer bounds and revoke discovery without document authority",
              "[Extensions][Inspector][ABI]") {
        InspectorPackage package;
        Host host;
        const auto loaded = host.manager.LoadExtension(package.root.string());
        if (loaded.HasError())
            UNSCOPED_INFO(loaded.ErrorValue().message);
        REQUIRE(loaded.HasValue());
        auto available = host.inspectors->Contributions("game.fixture.counter", 3);
        REQUIRE(available.HasValue());
        REQUIRE(available.Value().size() == 1);
        const auto retained = available.Value().front();
        REQUIRE(retained.context.IsUsable());
        CHECK(retained.context.Provider().extensionId == "fixture.package");
        REQUIRE(retained.definition->fields.size() == 1);
        CHECK(retained.definition->fields.front().minimumInteger == INT64_MIN);
        CHECK(retained.definition->fields.front().maximumInteger == INT64_MAX);
        CHECK(host.inspectors->LocalizedText(retained, "fixture.inspector", "tr-TR") == "Deneme inceleyicisi");
        CHECK(host.activities->Registry().Save().surfaces.empty());
        CHECK(host.inspectors->Contributions("unapproved.other.type", 3).Value().empty());
        host.manager.UnloadExtension("fixture.package");
        CHECK_FALSE(retained.context.IsUsable());
        CHECK(host.inspectors->Contributions("game.fixture.counter", 3).Value().empty());
    }

    TEST_CASE("ABI1.5 negotiation cannot read or register the ABI1.6 inspector tail", "[Extensions][Inspector][ABI]") {
        InspectorPackage package;
        Host host;
        auto library =
            Platform::LoadDynamicLibrary((package.root / std::filesystem::path{HORO_EDITOR_INSPECTOR_FIXTURE}.filename()).string());
        REQUIRE(library.HasValue());
        const auto query = reinterpret_cast<void (*)(uint32_t)>(library.Value()->GetSymbol("horo_test_inspector_query_mode"));
        const auto prefix = reinterpret_cast<uint32_t (*)()>(library.Value()->GetSymbol("horo_test_inspector_observed_prefix"));
        REQUIRE(query);
        REQUIRE(prefix);
        query(1);
        CHECK(host.manager.LoadExtension(package.root.string()).HasError());
        CHECK(prefix() == offsetof(HoroExtensionHostApi, registerEditorInspector));
        CHECK(host.inspectors->Contributions("game.fixture.counter", 3).Value().empty());
        query(2);  // A1.6-only required bit is also refused during explicit1.5 negotiation, before native load.
        CHECK(host.manager.LoadExtension(package.root.string()).HasError());
        CHECK(host.inspectors->Contributions("game.fixture.counter", 3).Value().empty());
        query(0);
    }

    TEST_CASE("Native inspector declarations require an actual permissioned interactive composition", "[Extensions][Inspector][ABI]") {
        InspectorPackage package;
        ExtensionManager missing{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {HORO_EDITOR_INSPECTOR_HOST_CAPABILITY},
                                 Horo::Tests::CreateAcceptingArtifactGate()};
        CHECK(missing.LoadExtension(package.root.string()).HasError());
    }
}  // namespace Horo::Extensions::Tests
