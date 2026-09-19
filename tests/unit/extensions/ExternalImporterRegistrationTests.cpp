#include "ExternalAssetImporter.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <span>

namespace Horo::Extensions::Tests {
    namespace {
        struct ImportInvocationState final {
            CancellationSource *cancellation{};
            int destroyed{};
            bool invoked{};
            bool cancelDuringCall{};
            bool rejectOutput{};
            bool emptyOutput{};
            bool inputMatched{};
            bool throwOnDestroy{};
        };

        struct ProgressCapture final {
            std::uint64_t completed{};
            std::uint64_t total{};
            std::string message;
        };

        void CaptureProgress(void *context,  // NOSONAR(cpp:S5008) The extension ABI requires an opaque callback context.
                             const std::uint64_t completed, const std::uint64_t total, const std::string_view message) {
            auto &capture = *static_cast<ProgressCapture *>(context);
            capture.completed = completed;
            capture.total = total;
            capture.message.assign(message);
        }

        void ThrowProgress(void *, std::uint64_t, std::uint64_t, std::string_view) {  // NOSONAR(cpp:S5008) Test ABI callback.
            throw std::runtime_error{"progress projection failed"};
        }

        [[nodiscard]] bool MatchesCompleteSettings(const HoroAssetImportRequest &request) {
            return request.settingCount == 3 && request.settings != nullptr &&
                   request.settings[0].kind == HORO_ASSET_IMPORT_SETTING_BOOLEAN && request.settings[0].booleanValue != 0 &&
                   request.settings[1].kind == HORO_ASSET_IMPORT_SETTING_INTEGER && request.settings[1].integerValue == 7 &&
                   request.settings[2].kind == HORO_ASSET_IMPORT_SETTING_TEXT &&
                   std::string_view{request.settings[2].textValue.data, request.settings[2].textValue.length} == "tag";
        }

        [[nodiscard]] bool MatchesCompleteRequest(const HoroAssetImportRequest &request) {
            const std::array<std::uint8_t, 2> expectedSource{1U, 2U};
            return request.structSize >= sizeof(HoroAssetImportRequest) && request.sourceByteCount == expectedSource.size() &&
                   std::equal(expectedSource.begin(), expectedSource.end(), request.sourceBytes) && request.sourceExtension.length == 3 &&
                   std::string_view{request.sourceExtension.data, request.sourceExtension.length} == "raw" &&
                   MatchesCompleteSettings(request);
        }

        HoroExtensionStatus InvokeCompleteImporter(
            void *context,  // NOSONAR(cpp:S5008) The extension ABI requires an opaque importer context.
            const HoroAssetImportRequest *request, HoroAssetImportResponse *response) {
            auto &state = *static_cast<ImportInvocationState *>(context);
            state.invoked = true;
            state.inputMatched = request != nullptr && MatchesCompleteRequest(*request);
            if (state.cancelDuringCall) {
                state.cancellation->RequestCancellation();
                return request->cancellation.isCancellationRequested(request->cancellation.context) != 0 ? HORO_EXTENSION_ERROR_CANCELLED
                                                                                                         : HORO_EXTENSION_ERROR_INIT_FAILED;
            }
            if (state.rejectOutput)
                static_cast<void>(response->progress.report(response->progress.context, 1, 0, {}));
            if (state.emptyOutput)
                return HORO_EXTENSION_SUCCESS;

            static constexpr char kType[] = "example.raw";
            static constexpr char kDependency[] = "12345678-1234-4234-8234-123456789abc";
            static constexpr char kProgress[] = "decode";
            static constexpr char kCode[] = "asset.import.note";
            static constexpr char kMessage[] = "decoded external source";
            response->assetType = {kType, sizeof(kType) - 1};
            std::uint8_t *payload{};
            if (response->editorPayload.resize(response->editorPayload.context, 3, &payload) != HORO_EXTENSION_SUCCESS)
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            const std::array<std::uint8_t, 3> expected{9U, 8U, 7U};
            std::copy(expected.begin(), expected.end(), payload);
            if (response->dependencies.append(response->dependencies.context, {kDependency, sizeof(kDependency) - 1}) !=
                HORO_EXTENSION_SUCCESS)
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            if (response->dependencies.append(response->dependencies.context, {kDependency, sizeof(kDependency) - 1}) !=
                HORO_EXTENSION_SUCCESS)
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            if (const HoroAssetImportDiagnostic diagnostic{
                    .severity = HORO_ASSET_IMPORT_DIAGNOSTIC_WARNING,
                    .code = {kCode, sizeof(kCode) - 1},
                    .message = {kMessage, sizeof(kMessage) - 1},
                    .line = 12,
                    .hasLine = 1,
                };
                response->diagnostics.append(response->diagnostics.context, &diagnostic) != HORO_EXTENSION_SUCCESS)
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            return response->progress.report(response->progress.context, 3, 4, {kProgress, sizeof(kProgress) - 1});
        }

        void DestroyCompleteImporter(void *context) {  // NOSONAR(cpp:S5008) The extension ABI requires an opaque importer context.
            auto &state = *static_cast<ImportInvocationState *>(context);
            ++state.destroyed;
            if (state.throwOnDestroy)
                throw std::runtime_error{"destroy failed"};
        }

        void CheckCompleteImport(const Assets::PreparedAssetImport &imported, const ImportInvocationState &invocation,
                                 const ProgressCapture &progress) {
            CHECK(invocation.invoked);
            CHECK(invocation.inputMatched);
            CHECK(imported.type.Value() == "example.raw");
            CHECK(imported.editorPayload == std::vector<std::uint8_t>{9U, 8U, 7U});
            REQUIRE(imported.dependencies.size() == 1);
            CHECK(imported.dependencies.front().ToString() == "12345678-1234-4234-8234-123456789abc");
            REQUIRE(imported.diagnostics.size() == 1);
            CHECK(imported.diagnostics.front().severity == Assets::ImportDiagnostic::Severity::Warning);
            CHECK(imported.diagnostics.front().code == "asset.import.note");
            CHECK(imported.diagnostics.front().line == 12);
            CHECK(progress.completed == 3);
            CHECK(progress.total == 4);
            CHECK(progress.message == "decode");
        }

        void CheckStickyOutputRejection(const Assets::IAssetImporter &importer, ImportInvocationState &invocation) {
            invocation.rejectOutput = true;
            const std::array<std::uint8_t, 2> source{1U, 2U};
            const CancellationToken cancellation;
            const auto rejected = importer.Import(Assets::AssetImportInput{.sourceBytes = source, .sourceExtension = "raw"}, cancellation);
            CHECK(rejected.HasError());
            invocation.rejectOutput = false;
        }

        void CheckCancellationAndTeardown(AssetImporterRegistrationSession &session, ImportInvocationState &invocation,
                                          CancellationSource &cancellation, const std::span<const std::uint8_t> source) {
            invocation.cancelDuringCall = true;
            invocation.invoked = false;
            const auto cancelled =
                session.contributions.front().strategy->Import(Assets::AssetImportInput{.sourceBytes = source, .sourceExtension = "raw"},
                                                               cancellation.Token());
            REQUIRE(cancelled.HasError());
            CHECK(invocation.invoked);
            CHECK(cancellation.Token().IsCancellationRequested());
            CHECK(invocation.destroyed == 0);
            session.contributions.clear();
            CHECK(invocation.destroyed == 1);
        }

        void CheckPreEntryCancellation(const Assets::IAssetImporter &importer, ImportInvocationState &invocation,
                                       const std::span<const std::uint8_t> source) {
            CancellationSource cancelled;
            cancelled.RequestCancellation();
            invocation.invoked = false;
            const auto result =
                importer.Import(Assets::AssetImportInput{.sourceBytes = source, .sourceExtension = "raw"}, cancelled.Token());
            CHECK(result.HasError());
            CHECK_FALSE(invocation.invoked);
        }

        void CheckProgressExceptionContainment(const Assets::IAssetImporter &importer, const std::span<const std::uint8_t> source) {
            const CancellationToken cancellation;
            const auto result = importer.Import(
                Assets::AssetImportInput{
                    .sourceBytes = source,
                    .sourceExtension = "raw",
                    .settings = {true, std::int64_t{7}, std::string{"tag"}},
                    .progress = {.context = nullptr, .report = ThrowProgress},
                },
                cancellation);
            CHECK(result.HasError());
        }

        void CheckBoundedInputAndEmptyOutput(const Assets::IAssetImporter &importer, ImportInvocationState &invocation,
                                             const std::span<const std::uint8_t> source) {
            const CancellationToken cancellation;
            invocation.invoked = false;
            const auto oversized =
                importer.Import(Assets::AssetImportInput{.sourceBytes = source, .sourceExtension = std::string(4097, 'x')}, cancellation);
            CHECK(oversized.HasError());
            CHECK_FALSE(invocation.invoked);

            invocation.emptyOutput = true;
            const auto empty = importer.Import(
                Assets::AssetImportInput{
                    .sourceBytes = source,
                    .sourceExtension = "raw",
                    .settings = {true, std::int64_t{7}, std::string{"tag"}},
                },
                cancellation);
            CHECK(empty.HasError());
            CHECK(invocation.invoked);
            invocation.emptyOutput = false;
        }

        [[nodiscard]] HoroAssetImporterDescriptor CompleteDescriptor(ImportInvocationState &invocation,
                                                                     const HoroExtensionStringView &extension,
                                                                     const HoroExtensionStringView &assetType) {
            return HoroAssetImporterDescriptor{
                .structSize = sizeof(HoroAssetImporterDescriptor),
                .abiVersion = HORO_ASSET_IMPORTER_ABI_VERSION,
                .contributionId = {"com.example.complete", 20},
                .contributionVersion = {"1.0.0", 5},
                .fileExtensions = &extension,
                .fileExtensionCount = 1,
                .assetTypes = &assetType,
                .assetTypeCount = 1,
                .targetExtension = {".horoasset", 10},
                .importerContext = &invocation,
                .importAsset = InvokeCompleteImporter,
                .destroyImporter = DestroyCompleteImporter,
            };
        }

        struct CompleteImporterFixture final {
            CompleteImporterFixture()
                : invocation{.cancellation = &cancellation}, session{.manifest = &manifest, .extensionModule = &owner},
                  descriptor{CompleteDescriptor(invocation, extension, assetType)} {
                manifest.id = "com.example.package";
                manifest.contributions.push_back({.type = "asset.importer", .id = "com.example.complete", .owningModule = owner.id});
            }

            ExtensionManifest manifest;
            ExtensionModuleManifest owner{.id = "com.example.native", .version = "1.0.0"};
            CancellationSource cancellation;
            ImportInvocationState invocation;
            HoroExtensionStringView extension{"raw", 3};
            HoroExtensionStringView assetType{"example.raw", 11};
            AssetImporterRegistrationSession session;
            HoroAssetImporterDescriptor descriptor;
        };
    }  // namespace

    struct RegistrationFixture {
        int destroyed{};
        ExtensionManifest manifest;
        ExtensionModuleManifest owner{.id = "com.example.native", .version = "1.0.0"};
        HoroExtensionStringView extension{"raw", 3};
        HoroExtensionStringView assetType{"example.raw", 11};
        AssetImporterRegistrationSession session{.manifest = &manifest, .extensionModule = &owner};
        HoroAssetImporterDescriptor descriptor{.structSize = sizeof(HoroAssetImporterDescriptor),
                                               .abiVersion = HORO_ASSET_IMPORTER_ABI_VERSION,
                                               .contributionId = {"com.example.raw", 15},
                                               .contributionVersion = {"1.0.0", 5},
                                               .fileExtensions = &extension,
                                               .fileExtensionCount = 1,
                                               .assetTypes = &assetType,
                                               .assetTypeCount = 1,
                                               .targetExtension = {".horoasset", 10},
                                               .importerContext = &destroyed,
                                               .importAsset =
                                                   [](void *,  // NOSONAR(cpp:S5008) The extension ABI requires an opaque importer context.
                                                      const HoroAssetImportRequest *, HoroAssetImportResponse *) -> HoroExtensionStatus {
            return HORO_EXTENSION_SUCCESS;
        },
                                               .destroyImporter = [](void *context) {  // NOSONAR(cpp:S5008) Required ABI callback context.
            ++*static_cast<int *>(context);
        }};

        RegistrationFixture() {
            manifest.id = "com.example.package";
            manifest.contributions.push_back({.type = "asset.importer", .id = "com.example.raw", .owningModule = owner.id});
        }
    };

    TEST_CASE_METHOD(RegistrationFixture, "Importer context transfers only after successful registration", "[Extensions][ABI]") {
        REQUIRE(RegisterExternalAssetImporter(&session, &descriptor) == HORO_EXTENSION_SUCCESS);
        REQUIRE(session.contributions.size() == 1);
        CHECK(destroyed == 0);
        descriptor.structSize = 1;
        CHECK(RegisterExternalAssetImporter(&session, &descriptor) == HORO_EXTENSION_ERROR_INVALID_ARGS);
        CHECK(session.failed);
        CHECK(destroyed == 0);
        session.contributions.clear();
        CHECK(destroyed == 1);

        using enum Assets::ImportSettingKind;
        const std::array kinds{std::pair{HORO_ASSET_IMPORT_SETTING_BOOLEAN, Boolean}, std::pair{HORO_ASSET_IMPORT_SETTING_INTEGER, Integer},
                               std::pair{HORO_ASSET_IMPORT_SETTING_FLOAT, Float}, std::pair{HORO_ASSET_IMPORT_SETTING_TEXT, Text},
                               std::pair{HORO_ASSET_IMPORT_SETTING_CHOICE, Choice}};
        for (const auto &[abiKind, expected] : kinds) {
            RegistrationFixture fixture;
            const HoroAssetImportSettingDescriptor setting{.id = {"setting", 7},
                                                           .labelKey = {"setting.label", 13},
                                                           .kind = static_cast<HoroAssetImportSettingKind>(abiKind),
                                                           .defaultValue = {.kind = static_cast<HoroAssetImportSettingKind>(abiKind)}};
            fixture.descriptor.settings = &setting;
            fixture.descriptor.settingCount = 1;
            REQUIRE(RegisterExternalAssetImporter(&fixture.session, &fixture.descriptor) == HORO_EXTENSION_SUCCESS);
            REQUIRE(fixture.session.contributions.front().settings.size() == 1);
            CHECK(fixture.session.contributions.front().settings.front().kind == expected);
        }

        const HoroAssetImportSettingDescriptor unknown{.id = {"setting", 7}, .labelKey = {"setting.label", 13}, .kind = 999};
        RegistrationFixture invalid;
        invalid.descriptor.settings = &unknown;
        invalid.descriptor.settingCount = 1;
        CHECK(RegisterExternalAssetImporter(&invalid.session, &invalid.descriptor) == HORO_EXTENSION_ERROR_INVALID_ARGS);
    }

    TEST_CASE("Malformed importer descriptors retain caller ownership", "[Extensions][ABI]") {
        using Mutate = void (*)(RegistrationFixture &);
        const std::array<Mutate, 13> invalid{[](RegistrationFixture &f) {
            f.descriptor.structSize = 1;
        }, [](RegistrationFixture &f) {
            f.descriptor.abiVersion = 99;
        }, [](RegistrationFixture &f) {
            f.descriptor.importAsset = nullptr;
        }, [](RegistrationFixture &f) {
            f.descriptor.destroyImporter = nullptr;
        }, [](RegistrationFixture &f) {
            f.descriptor.fileExtensionCount = 257;
        }, [](RegistrationFixture &f) {
            f.descriptor.contributionId = {};
        }, [](RegistrationFixture &f) {
            f.manifest.contributions.clear();
        }, [](RegistrationFixture &f) {
            f.descriptor.previewFallback = 255;
        }, [](RegistrationFixture &f) {
            f.extension = {"INVALID", 7};
        }, [](RegistrationFixture &f) {
            f.assetType = {};
        }, [](RegistrationFixture &f) {
            f.assetType = {"bad type", 8};
        }, [](RegistrationFixture &f) {
            f.descriptor.settingCount = 1;
        }, [](RegistrationFixture &f) {
            f.extension = {".raw", 4};
        }};
        for (const auto mutate : invalid) {
            RegistrationFixture fixture;
            mutate(fixture);
            CHECK(RegisterExternalAssetImporter(&fixture.session, &fixture.descriptor) == HORO_EXTENSION_ERROR_INVALID_ARGS);
            CHECK(fixture.session.failed);
            CHECK(fixture.session.contributions.empty());
            CHECK(fixture.destroyed == 0);
        }
        CHECK(RegisterExternalAssetImporter(nullptr, nullptr) == HORO_EXTENSION_ERROR_INVALID_ARGS);
    }

    TEST_CASE("External importer projects bounded output progress diagnostics cancellation and teardown", "[Extensions][ABI][Assets]") {
        CompleteImporterFixture fixture;

        REQUIRE(RegisterExternalAssetImporter(&fixture.session, &fixture.descriptor) == HORO_EXTENSION_SUCCESS);
        REQUIRE(fixture.session.contributions.size() == 1);
        ProgressCapture progress;
        const std::array<std::uint8_t, 2> source{1U, 2U};
        auto imported = fixture.session.contributions.front().strategy->Import(
            Assets::AssetImportInput{
                .sourceBytes = source,
                .sourceExtension = "raw",
                .settings = {true, std::int64_t{7}, std::string{"tag"}},
                .progress = {.context = &progress, .report = CaptureProgress},
            },
            fixture.cancellation.Token());
        REQUIRE(imported.HasValue());
        CheckCompleteImport(imported.Value(), fixture.invocation, progress);
        CheckPreEntryCancellation(*fixture.session.contributions.front().strategy, fixture.invocation, source);
        CheckStickyOutputRejection(*fixture.session.contributions.front().strategy, fixture.invocation);
        CheckProgressExceptionContainment(*fixture.session.contributions.front().strategy, source);
        CheckBoundedInputAndEmptyOutput(*fixture.session.contributions.front().strategy, fixture.invocation, source);
        CheckCancellationAndTeardown(fixture.session, fixture.invocation, fixture.cancellation, source);
    }

    TEST_CASE("External importer lifetime follows catalog snapshots and contains destroy exceptions", "[Extensions][ABI][Assets]") {
        CompleteImporterFixture fixture;
        REQUIRE(RegisterExternalAssetImporter(&fixture.session, &fixture.descriptor) == HORO_EXTENSION_SUCCESS);

        std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> snapshot;
        {
            Assets::AssetImporterCatalog catalog;
            REQUIRE(catalog.Register(std::move(fixture.session.contributions.front())).HasValue());
            fixture.session.contributions.clear();
            auto published = catalog.Publish();
            REQUIRE(published.HasValue());
            snapshot = std::move(published).Value();
            CHECK(fixture.invocation.destroyed == 0);
        }
        CHECK(fixture.invocation.destroyed == 0);
        fixture.invocation.throwOnDestroy = true;
        snapshot.reset();
        CHECK(fixture.invocation.destroyed == 1);
    }
}  // namespace Horo::Extensions::Tests
