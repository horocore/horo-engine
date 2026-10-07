#include "Horo/Extensions/ExtensionManifest.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>
#include <string_view>

namespace Horo::Extensions::Tests {
    namespace {
        constexpr std::string_view MinimalManifest = R"json({
            "schemaVersion": 1,
            "id": "com.example.minimum",
            "version": "1.0.0",
            "modules": [{
                "id": "com.example.minimum.native",
                "version": "1.0.0",
                "kind": "native"
            }]
        })json";

        void RequireError(const Result<ExtensionManifest> &result, const std::string_view path, const std::string_view diagnosticCode) {
            REQUIRE(result.HasError());
            const Error &error = result.ErrorValue();
            REQUIRE_THAT(error.message, Catch::Matchers::StartsWith(std::string{path} + ':'));
            REQUIRE(error.diagnostics.size() == 1);
            CHECK(error.diagnostics.front().code.Value() == diagnosticCode);
            CHECK(error.diagnostics.front().message == error.message);
            CHECK(error.diagnostics.front().path == path);
            CHECK(error.diagnostics.front().location.source == "extension.json");
        }
    }  // namespace

    TEST_CASE("Contribution point grammar admits normative underscore names without changing capability IDs",
              "[Extensions][Manifest][Activity]") {
        const auto package = [](const std::string &type) {
            return R"({"id":"fixture.package","version":"1.0.0","modules":[{"id":"fixture.module","version":"1.0.0","kind":"native"}],"contributions":[{"type":")" +
                   type + R"(","id":"fixture.activity","module":"fixture.module"}]})";
        };
        CHECK(ParseExtensionManifest(package("editor.activity_item")).HasValue());
        CHECK(ParseExtensionManifest(package("editor.status_item")).HasValue());
        for (const std::string type : {"", "Editor.activity_item", "editor. activity_item", "editor..activity_item", "editor._activity",
                                       "editor.activity_", "editor.activity_ ", ".editor.activity", "editor.activity."})
            CHECK(ParseExtensionManifest(package(type)).HasError());
        CHECK(ParseExtensionManifest(package(std::string(4096, 'a'))).HasError());
        RequireError(
            ParseExtensionManifest(
                R"({"id":"fixture.package","version":"1.0.0","modules":[{"id":"fixture.module","version":"1.0.0","kind":"native","requiredCapabilities":["editor.activity_item"]}]})"),
            "$.modules[0].requiredCapabilities[0]", "extension.manifest.invalid_identifier");
    }

    TEST_CASE("Extension manifest accepts bounded canonical and transitional package documents", "[Extensions][Manifest]") {
        SECTION("canonical document owns validated values") {
            auto result = ParseExtensionManifest(R"json({
                "schemaVersion": 1,
                "id": "com.example.importer",
                "version": "1.2.3-beta.1",
                "kind": "asset_importer",
                "displayName": "Example Importer",
                "description": "Imports example assets.",
                "author": "Example",
                "compatibility": {
                    "engineMin": "0.1.0",
                    "engineMax": "0.9.0",
                    "sdkAbi": "horo.extension-1",
                    "platforms": ["linux", "macos"]
                },
                "modules": [{
                    "id": "com.example.importer.native",
                    "version": "2.0.0",
                    "kind": "asset_importer",
                    "entry": "bin/importer",
                    "roles": ["backend-capability"]
                }],
                "contributions": [{
                    "type": "asset.importer",
                    "id": "com.example.importer.raw",
                    "module": "com.example.importer.native"
                }]
            })json");

            REQUIRE(result.HasValue());
            const ExtensionManifest &manifest = result.Value();
            CHECK(manifest.schemaVersion == 1);
            CHECK(manifest.id == "com.example.importer");
            CHECK(manifest.version == "1.2.3-beta.1");
            CHECK(manifest.engineMin == "0.1.0");
            CHECK(manifest.platforms.size() == 2);
            REQUIRE(manifest.modules.size() == 1);
            CHECK(manifest.modules.front().entry == "bin/importer");
            REQUIRE(manifest.contributions.size() == 1);
            CHECK(manifest.contributions.front().owningModule == manifest.modules.front().id);
        }

        SECTION("transitional package envelope remains explicit") {
            auto result = ParseExtensionManifest(R"json({
                "package": {"id": "com.example.legacy", "version": "1.0.0"},
                "modules": [{"id": "com.example.legacy.native", "version": "1.0.0", "kind": "native",
                             "roles": ["backend-capability"]}]
            })json");
            REQUIRE(result.HasValue());
            CHECK(result.Value().schemaVersion == 1);
            CHECK(result.Value().id == "com.example.legacy");
        }
    }

    TEST_CASE("Extension manifest rejects unsupported and ambiguous schemas", "[Extensions][Manifest]") {
        SECTION("unsupported version") {
            auto result = ParseExtensionManifest(R"json({"schemaVersion":2})json");
            RequireError(result, "$.schemaVersion", "extension.manifest.unsupported_schema");
        }

        SECTION("schema version has an exact integer type") {
            auto result = ParseExtensionManifest(R"json({"schemaVersion":"1"})json");
            RequireError(result, "$.schemaVersion", "extension.manifest.unsupported_schema");
        }

        SECTION("unknown root field") {
            auto result = ParseExtensionManifest(R"json({"futureAuthority":true})json");
            RequireError(result, "$.futureAuthority", "extension.manifest.unknown_field");
        }

        SECTION("separator keys use unambiguous bracket notation") {
            auto result = ParseExtensionManifest(R"json({"future.authority":true})json");
            RequireError(result, R"path($["future.authority"])path", "extension.manifest.unknown_field");
        }

        SECTION("unknown nested field") {
            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.test","version":"1.0.0",
                "modules":[{"id":"com.example.test.native","version":"1.0.0","kind":"native","autorun":true}]
            })json");
            RequireError(result, "$.modules[0].autorun", "extension.manifest.unknown_field");
        }

        SECTION("nested and root package fields cannot compete") {
            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.root",
                "package":{"id":"com.example.nested","version":"1.0.0"}
            })json");
            RequireError(result, "$.id", "extension.manifest.ambiguous_field");
        }

        SECTION("root type is exact") {
            auto result = ParseExtensionManifest("[]");
            RequireError(result, "$", "extension.manifest.invalid_type");
        }
    }

    TEST_CASE("Extension manifest owns explicit multi-module roles and service edges", "[Extensions][Manifest]") {
        auto result = ParseExtensionManifest(R"json({
            "schemaVersion": 1,
            "id": "com.example.hybrid",
            "version": "1.0.0",
            "modules": [{
                "id": "com.example.hybrid.backend",
                "version": "2.0.0",
                "kind": "native",
                "roles": ["backend-capability", "headless-tooling"],
                "abi": {"major": 1, "minimumMinor": 1},
                "requiredCapabilities": ["com.horo.assets"],
                "entries": [{"platform":"linux", "architecture":"x86_64", "buildProfile":"debug",
                             "entry":"bin/linux-x86_64-debug/module"}],
                "exports": [{"id": "com.example.hybrid.service", "contract": "com.horo.example", "version": "2.1.0"}]
            }, {
                "id": "com.example.hybrid.editor",
                "version": "1.0.0",
                "kind": "native",
                "roles": ["editor-presentation"],
                "dependencies": ["com.example.hybrid.backend"],
                "imports": [{"id": "com.example.hybrid.import", "service": "com.example.hybrid.service",
                             "contract": "com.horo.example", "minimumVersion": "2.0.0", "required": false}]
            }]
        })json");

        REQUIRE(result.HasValue());
        REQUIRE(result.Value().modules.size() == 2);
        CHECK(result.Value().modules.front().roles.size() == 2);
        REQUIRE(result.Value().modules.front().abi.has_value());
        CHECK(result.Value().modules.front().abi->minimumMinor == 1);
        CHECK(result.Value().modules.front().entries.front().entry == "bin/linux-x86_64-debug/module");
        CHECK(result.Value().modules.front().requiredCapabilities == std::vector<std::string>{"com.horo.assets"});
        CHECK(result.Value().modules.front().exports.front().id == "com.example.hybrid.service");
        CHECK(result.Value().modules.back().dependencies.front() == "com.example.hybrid.backend");
        CHECK(result.Value().modules.back().imports.front().minimumVersion == "2.0.0");
        CHECK_FALSE(result.Value().modules.back().imports.front().required);

        auto defaultRequired = ParseExtensionManifest(R"json({
            "id":"com.example.default-required","version":"1.0.0","modules":[{
                "id":"com.example.default-required.backend","version":"1.0.0","kind":"native",
                "imports":[{"id":"com.example.default-required.import","service":"com.example.service",
                            "contract":"com.example.contract","minimumVersion":"1.0.0"}]
            }]
        })json");
        REQUIRE(defaultRequired.HasValue());
        CHECK(defaultRequired.Value().modules.front().imports.front().required);

        auto nonBooleanRequired = ParseExtensionManifest(R"json({
            "id":"com.example.invalid-required","version":"1.0.0","modules":[{
                "id":"com.example.invalid-required.backend","version":"1.0.0","kind":"native",
                "imports":[{"id":"com.example.invalid-required.import","service":"com.example.service",
                            "contract":"com.example.contract","minimumVersion":"1.0.0","required":"no"}]
            }]
        })json");
        RequireError(nonBooleanRequired, "$.modules[0].imports[0].required", "extension.manifest.invalid_type");

        auto unknownRole = ParseExtensionManifest(R"json({
            "id":"com.example.test","version":"1.0.0",
            "modules":[{"id":"com.example.test.native","version":"1.0.0","kind":"native","roles":["gui-ish"]}]
        })json");
        RequireError(unknownRole, "$.modules[0].roles[0]", "extension.manifest.invalid_value");
    }

    TEST_CASE("Extension manifest rejects competing typed entry authority", "[Extensions][Manifest]") {
        auto competingEntries = ParseExtensionManifest(R"json({
            "id":"com.example.test","version":"1.0.0",
            "modules":[{"id":"com.example.test.native","version":"1.0.0","kind":"native","entry":"legacy",
                        "entries":[{"platform":"linux","architecture":"x86_64","buildProfile":"debug","entry":"typed"}]}]
        })json");
        RequireError(competingEntries, "$.modules[0].entries", "extension.manifest.ambiguous_field");

        auto duplicateSelectors = ParseExtensionManifest(R"json({
            "id":"com.example.test","version":"1.0.0",
            "modules":[{"id":"com.example.test.native","version":"1.0.0","kind":"native","entries":[
                {"platform":"linux","architecture":"x86_64","buildProfile":"debug","entry":"first"},
                {"platform":"linux","architecture":"x86_64","buildProfile":"debug","entry":"second"}]}]
        })json");
        RequireError(duplicateSelectors, "$.modules[0].entries[1]", "extension.manifest.duplicate_identifier");
    }

    TEST_CASE("Extension manifest enforces syntax resource limits before validation", "[Extensions][Manifest]") {
        SECTION("document bytes include an accepted boundary") {
            ExtensionManifestLimits limits;
            limits.maximumDocumentBytes = MinimalManifest.size();
            REQUIRE(ParseExtensionManifest(MinimalManifest, limits).HasValue());

            --limits.maximumDocumentBytes;
            RequireError(ParseExtensionManifest(MinimalManifest, limits), "$", "extension.manifest.document_limit");
        }

        SECTION("nesting includes an accepted boundary") {
            ExtensionManifestLimits limits;
            limits.maximumNestingDepth = 3;
            REQUIRE(ParseExtensionManifest(MinimalManifest, limits).HasValue());

            limits.maximumNestingDepth = 2;
            RequireError(ParseExtensionManifest(MinimalManifest, limits), "$", "extension.manifest.nesting_limit");
        }

        SECTION("object member budget") {
            ExtensionManifestLimits limits;
            limits.maximumObjectMembers = 7;
            REQUIRE(ParseExtensionManifest(MinimalManifest, limits).HasValue());

            limits.maximumObjectMembers = 6;
            RequireError(ParseExtensionManifest(MinimalManifest, limits), "$.modules[0].kind", "extension.manifest.object_limit");
        }
    }

    TEST_CASE("Extension manifest enforces collection and text parse budgets", "[Extensions][Manifest]") {
        SECTION("array element budget") {
            ExtensionManifestLimits limits;
            limits.maximumArrayElements = 1;
            REQUIRE(ParseExtensionManifest(MinimalManifest, limits).HasValue());

            limits.maximumArrayElements = 1;
            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.test","version":"1.0.0",
                "modules":[
                    {"id":"com.example.test.one","version":"1.0.0","kind":"native"},
                    {"id":"com.example.test.two","version":"1.0.0","kind":"native"}
                ]
            })json",
                                                 limits);
            RequireError(result, "$.modules[1]", "extension.manifest.array_limit");
        }

        SECTION("decoded string budget") {
            ExtensionManifestLimits limits;
            limits.maximumStringBytes = 5;
            auto result = ParseExtensionManifest(R"json({"description":"123456"})json", limits);
            RequireError(result, "$.description", "extension.manifest.string_limit");
        }

        SECTION("field-name budget") {
            ExtensionManifestLimits limits;
            limits.maximumIdentifierBytes = 4;
            auto result = ParseExtensionManifest(R"json({"longName":true})json", limits);
            RequireError(result, "$.longName", "extension.manifest.identifier_limit");
        }

        SECTION("zero parser limits are invalid configuration") {
            ExtensionManifestLimits limits;
            limits.maximumModules = 0;
            RequireError(ParseExtensionManifest(MinimalManifest, limits), "$", "extension.manifest.invalid_limits");
        }
    }

    TEST_CASE("Extension manifest reports duplicate fields and malformed JSON", "[Extensions][Manifest]") {
        SECTION("duplicate field includes its nested path") {
            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.test","version":"1.0.0",
                "modules":[{"id":"com.example.first","id":"com.example.second","version":"1.0.0","kind":"native"}]
            })json");
            RequireError(result, "$.modules[0].id", "extension.manifest.duplicate_field");
        }

        SECTION("malformed JSON includes source coordinates") {
            auto result = ParseExtensionManifest("{\n  \"id\": }");
            RequireError(result, "$", "extension.manifest.invalid_json");
            CHECK(result.ErrorValue().diagnostics.front().location.line == 2);
            CHECK(result.ErrorValue().diagnostics.front().location.column > 1);
        }
    }

    TEST_CASE("Extension manifest validates package, compatibility and entry values", "[Extensions][Manifest]") {
        SECTION("package identity") {
            auto result = ParseExtensionManifest(R"json({"id":"Com.Example","version":"1.0.0","modules":[]})json");
            RequireError(result, "$.id", "extension.manifest.invalid_identifier");
        }

        SECTION("package version") {
            auto result = ParseExtensionManifest(R"json({"id":"com.example.test","version":"01.0.0","modules":[]})json");
            RequireError(result, "$.version", "extension.manifest.invalid_version");
        }

        SECTION("compatibility field type") {
            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.test","version":"1.0.0","compatibility":[],"modules":[]
            })json");
            RequireError(result, "$.compatibility", "extension.manifest.invalid_type");
        }

        SECTION("compatibility version") {
            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.test","version":"1.0.0","compatibility":{"engineMin":"next"},"modules":[]
            })json");
            RequireError(result, "$.compatibility", "extension.manifest.invalid_version");
        }

        SECTION("module entry containment") {
            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.test","version":"1.0.0",
                "modules":[{"id":"com.example.test.native","version":"1.0.0","kind":"native","entry":"../escape"}]
            })json");
            RequireError(result, "$.modules[0].entry", "extension.manifest.invalid_path");
        }
    }

    TEST_CASE("Extension manifest validates collection bounds and identities", "[Extensions][Manifest]") {
        SECTION("module collection limit") {
            ExtensionManifestLimits limits;
            limits.maximumModules = 1;
            REQUIRE(ParseExtensionManifest(MinimalManifest, limits).HasValue());

            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.test","version":"1.0.0",
                "modules":[
                    {"id":"com.example.test.one","version":"1.0.0","kind":"native"},
                    {"id":"com.example.test.two","version":"1.0.0","kind":"native"}
                ]
            })json",
                                                 limits);
            RequireError(result, "$.modules", "extension.manifest.collection_limit");
        }

        SECTION("duplicate module identity") {
            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.test","version":"1.0.0",
                "modules":[
                    {"id":"com.example.test.native","version":"1.0.0","kind":"native"},
                    {"id":"com.example.test.native","version":"2.0.0","kind":"native"}
                ]
            })json");
            RequireError(result, "$.modules[1].id", "extension.manifest.duplicate_identifier");
        }

        SECTION("duplicate platform identity") {
            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.test","version":"1.0.0","compatibility":{"platforms":["linux","linux"]},
                "modules":[{"id":"com.example.test.native","version":"1.0.0","kind":"native"}]
            })json");
            RequireError(result, "$.compatibility.platforms[1]", "extension.manifest.duplicate_identifier");
        }

        SECTION("platform collection limit") {
            ExtensionManifestLimits limits;
            constexpr std::string_view manifest = R"json({
                "id":"com.example.test","version":"1.0.0","compatibility":{"platforms":["linux","macos"]},
                "modules":[{"id":"com.example.test.native","version":"1.0.0","kind":"native"}]
            })json";
            limits.maximumPlatforms = 2;
            REQUIRE(ParseExtensionManifest(manifest, limits).HasValue());

            limits.maximumPlatforms = 1;
            auto result = ParseExtensionManifest(manifest, limits);
            RequireError(result, "$.compatibility.platforms", "extension.manifest.collection_limit");
        }
    }

    TEST_CASE("Extension manifest validates contribution references before resolution", "[Extensions][Manifest]") {
        SECTION("unresolved module") {
            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.test","version":"1.0.0",
                "modules":[{"id":"com.example.test.native","version":"1.0.0","kind":"native"}],
                "contributions":[{"type":"asset.importer","id":"com.example.test.raw","module":"com.example.missing"}]
            })json");
            RequireError(result, "$.contributions[0].module", "extension.manifest.unresolved_reference");
        }

        SECTION("duplicate contribution identity") {
            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.test","version":"1.0.0",
                "modules":[{"id":"com.example.test.native","version":"1.0.0","kind":"native"}],
                "contributions":[
                    {"type":"asset.importer","id":"com.example.test.raw","module":"com.example.test.native"},
                    {"type":"asset.importer","id":"com.example.test.raw","module":"com.example.test.native"}
                ]
            })json");
            RequireError(result, "$.contributions[1].id", "extension.manifest.duplicate_identifier");
        }

        SECTION("contribution collection limit") {
            ExtensionManifestLimits limits;
            constexpr std::string_view oneContribution = R"json({
                "id":"com.example.test","version":"1.0.0",
                "modules":[{"id":"com.example.test.native","version":"1.0.0","kind":"native"}],
                "contributions":[
                    {"type":"asset.importer","id":"com.example.test.one","module":"com.example.test.native"}
                ]
            })json";
            limits.maximumContributions = 1;
            REQUIRE(ParseExtensionManifest(oneContribution, limits).HasValue());

            auto result = ParseExtensionManifest(R"json({
                "id":"com.example.test","version":"1.0.0",
                "modules":[{"id":"com.example.test.native","version":"1.0.0","kind":"native"}],
                "contributions":[
                    {"type":"asset.importer","id":"com.example.test.one","module":"com.example.test.native"},
                    {"type":"asset.importer","id":"com.example.test.two","module":"com.example.test.native"}
                ]
            })json",
                                                 limits);
            RequireError(result, "$.contributions", "extension.manifest.collection_limit");
        }
    }
}  // namespace Horo::Extensions::Tests
