#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/HeadlessExtensionHost.h"
#include "Horo/Foundation/ModuleDescriptor.h"
#include "SecurityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Horo::Extensions::Tests {
    namespace {
        const ErrorCodeDescriptor ValidationFinding{.domain = ErrorDomainId{"horo.test.headless"},
                                                    .code = ErrorCode{"validation.finding"},
                                                    .defaultSeverity = ErrorSeverity::Warning,
                                                    .summary = "Headless validation finding."};

        [[nodiscard]] Assets::AssetTypeId MeshType() {
            auto type = Assets::AssetTypeId::Parse("core.mesh");
            REQUIRE(type.HasValue());
            return std::move(type).Value();
        }

        [[nodiscard]] Assets::AssetId MeshAsset() {
            auto asset = Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc");
            REQUIRE(asset.HasValue());
            return std::move(asset).Value();
        }

        [[nodiscard]] AssetCookTargetId HeadlessTarget() {
            auto target = AssetCookTargetId::Parse("headless-null");
            REQUIRE(target.HasValue());
            return std::move(target).Value();
        }

        [[nodiscard]] ErrorCodeRegistry ValidationErrors() {
            ModuleDescriptor module{.id = ModuleId{"horo.test-headless"}, .version = {1U, 0U, 0U}};
            module.errorDomains.push_back({.id = ErrorDomainId{"horo.test.headless"}, .descriptors = {&ValidationFinding}});
            auto errors = BuildErrorCodeRegistry(std::span{&module, 1U});
            REQUIRE(errors.HasValue());
            return std::move(errors).Value();
        }

        class CopyingImporter final : public Assets::IAssetImporter {
        public:
            Result<Assets::PreparedAssetImport> Import(const Assets::AssetImportInput &input,
                                                       const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<Assets::PreparedAssetImport>::Failure(MakeError(ExtensionErrors::AssetCookCancelled));
                return Result<Assets::PreparedAssetImport>::Success(
                    {.type = MeshType(), .editorPayload = {input.sourceBytes.begin(), input.sourceBytes.end()}});
            }
        };

        class HeadlessCooker final : public IAssetCooker {
        public:
            Result<void> Cook(const AssetCookerInput &input, AssetCookerOutputSink &output, const CancellationToken &) const override {
                return output.WritePayload(input.sourceBytes);
            }
        };

        class HeadlessValidator final : public IProjectValidator {
        public:
            Result<void> Validate(const ProjectValidationSnapshot &snapshot, ProjectValidationFindingSink &findings,
                                  const CancellationToken &) const override {
                return findings.Add(ValidationFinding, "validated without project mutation",
                                    {.source = snapshot.resources.front().path.String()});
            }
        };

        class HeadlessPipelineStep final : public IPipelineStep {
        public:
            Result<void> Execute(const PipelineStepContext &context, PipelineOutputSink &outputs,
                                 const CancellationToken &) const override {
                const PipelineArtifactView *source = context.Find({"artifact.source"});
                if (source == nullptr)
                    return Result<void>::Failure(MakeError(ExtensionErrors::InvocationFailed, "source missing"));
                return outputs.Write({"artifact.output"}, source->bytes);
            }
        };

        class HeadlessToolchainPolicy final : public IToolchainInvocationPolicy {
        public:
            Result<ExternalProcessRequest> Resolve(const ToolchainProviderDescriptor &, const ToolchainInvocationIntent &) const override {
                return Result<ExternalProcessRequest>::Success(
                    {.executable = "/approved/tool", .arguments = {"--approved"}, .workingDirectory = "/workspace"});
            }
        };

        class RecordingProcessRunner final : public IExternalProcessRunner {
        public:
            Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &) override {
                ++calls;
                executable = request.executable;
                return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, 0});
            }

            std::size_t calls{};
            std::string executable;
        };

        struct HeadlessHostFixture {
            std::filesystem::path root =
                std::filesystem::temp_directory_path() /
                ("horo-headless-host-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            HeadlessToolchainPolicy toolchainPolicy;
            RecordingProcessRunner processRunner;

            HeadlessHostFixture() {
                REQUIRE(std::filesystem::create_directory(root));
            }

            ~HeadlessHostFixture() {
                std::error_code error;
                std::filesystem::remove_all(root, error);
            }

            [[nodiscard]] std::unique_ptr<HeadlessExtensionHost> CreateHost() {
                HeadlessExtensionHostConfiguration configuration;
                configuration.roots.push_back({.id = "test-root",
                                               .path = root,
                                               .kind = Discovery::RootKind::Development,
                                               .approval = Discovery::RootApproval::Approved,
                                               .configuration = Discovery::ConfigurationOrigin::UserLocal});
                configuration.discoveryPolicy = {.profile = Discovery::DiscoveryProfile::Development, .enableDevelopmentOverrides = true};
                configuration.validationErrors = ValidationErrors();
                configuration.artifactGate = Horo::Tests::CreateAcceptingArtifactGate();
                auto host = HeadlessExtensionHost::Create(std::move(configuration), toolchainPolicy, processRunner);
                REQUIRE(host.HasValue());
                return std::move(host).Value();
            }

            [[nodiscard]] Discovery::PackageLocation PrepareNativePackage() const {
                const std::filesystem::path package = root / "native-backend";
                REQUIRE(std::filesystem::create_directory(package));
                const std::filesystem::path source = std::filesystem::absolute(HORO_ABI_FIXTURE_0);
                const std::filesystem::path library = package / source.filename();
                std::filesystem::copy_file(source, library);
                std::ofstream manifest{package / "extension.json", std::ios::binary | std::ios::trunc};
                manifest
                    << R"({"id":"com.example.headless","version":"1.0.0","modules":[{"id":"com.example.headless.backend","version":"1.0.0","kind":"native","roles":["backend-capability","headless-tooling"],"entry":")"
                    << library.filename().generic_string() << R"("}]})";
                REQUIRE(manifest.good());
                return {.packageId = "com.example.headless", .rootId = "test-root", .relativePath = "native-backend"};
            }

            [[nodiscard]] Discovery::PackageLocation PrepareInvalidPackage() const {
                const std::filesystem::path package = root / "invalid-backend";
                REQUIRE(std::filesystem::create_directory(package));
                std::ofstream manifest{package / "extension.json", std::ios::binary | std::ios::trunc};
                manifest << R"({"id":"com.example.invalid","version":"not-semver","modules":[]})";
                REQUIRE(manifest.good());
                return {.packageId = "com.example.invalid", .rootId = "test-root", .relativePath = "invalid-backend"};
            }

            void RegisterProviders(HeadlessExtensionHost &host, ToolchainInvocationAuthority &toolchainAuthority) const {
                REQUIRE(host.RegisterCapability({.capability = {"horo.asset.pipeline"},
                                                 .version = {1U, 0U, 0U},
                                                 .provider = {.moduleId = "com.example.backend",
                                                              .providerId = "com.example.pipeline-provider",
                                                              .generation = 1U}})
                            .HasValue());
                REQUIRE(host.RegisterImporter({.contributionId = "com.example.importer",
                                               .packageId = "com.example.package",
                                               .moduleId = "com.example.backend",
                                               .moduleVersion = "1.0.0",
                                               .version = "1.0.0",
                                               .fileExtensions = {"mesh"},
                                               .assetTypes = {MeshType()},
                                               .strategy = std::make_shared<CopyingImporter>()})
                            .HasValue());
                REQUIRE(host.RegisterCooker({.cookerId = {"com.example.cooker"},
                                             .providerId = "com.example.backend",
                                             .providerGeneration = 1U,
                                             .assetType = MeshType(),
                                             .targets = {HeadlessTarget()},
                                             .cookerVersion = "1.0.0",
                                             .artifactFormatVersion = 1U},
                                            std::make_shared<HeadlessCooker>())
                            .HasValue());
                REQUIRE(host.RegisterValidator({.validatorId = {"com.example.validator"},
                                                .providerId = {"com.example.backend"},
                                                .providerGeneration = 1U},
                                               std::make_shared<HeadlessValidator>())
                            .HasValue());
                REQUIRE(host.RegisterPipelineStep({.stepId = {"com.example.pipeline-step"},
                                                   .providerId = "com.example.backend",
                                                   .providerGeneration = 1U,
                                                   .phase = PipelinePhase::Build,
                                                   .inputs = {{"artifact.source"}},
                                                   .outputs = {{"artifact.output"}}},
                                                  std::make_shared<HeadlessPipelineStep>())
                            .HasValue());
                auto toolchain = host.RegisterToolchain({.contributionId = "com.example.toolchain",
                                                         .providerId = "com.example.backend",
                                                         .providerGeneration = 1U,
                                                         .tools = {{"tool.compiler"}}});
                REQUIRE(toolchain.HasValue());
                toolchainAuthority = std::move(toolchain).Value();
            }
        };

        [[nodiscard]] AssetCookerRequest CookRequest() {
            static const std::array<std::uint8_t, 3> source{4U, 5U, 6U};
            const std::array<std::byte, 1> metadata{std::byte{7U}};
            const std::array<std::byte, 1> settings{std::byte{8U}};
            return {.input = {.assetId = MeshAsset(),
                              .assetType = MeshType(),
                              .target = HeadlessTarget(),
                              .sourceDigest = ComputeSha256(std::as_bytes(std::span{source})),
                              .metadataDigest = ComputeSha256(metadata),
                              .metadataSchemaVersion = 1U,
                              .settingsDigest = ComputeSha256(settings),
                              .settingsSchemaVersion = 1U,
                              .sourceBytes = source}};
        }
    }  // namespace

    TEST_CASE_METHOD(HeadlessHostFixture, "Headless host discovers activates and executes every backend capability without presentation",
                     "[integration][extensions][headless]") {
        std::unique_ptr<HeadlessExtensionHost> host = CreateHost();
        ToolchainInvocationAuthority toolchainAuthority;
        RegisterProviders(*host, toolchainAuthority);
        const Discovery::PackageLocation package = PrepareNativePackage();
        REQUIRE(host->Start(std::span{&package, 1U}).HasValue());

        const std::array<std::uint8_t, 3> source{1U, 2U, 3U};
        const auto imported = host->Import("com.example.importer", {.sourceBytes = source, .sourceExtension = "mesh"}, {});
        REQUIRE(imported.HasValue());
        CHECK(imported.Value().editorPayload == std::vector<std::uint8_t>{1U, 2U, 3U});

        const auto cooked = host->Cook(CookRequest(), {});
        REQUIRE(cooked.HasValue());
        CHECK(cooked.Value().payload == std::vector<std::uint8_t>{4U, 5U, 6U});

        auto path = ProjectPath::Parse(".horo/project.json");
        REQUIRE(path.HasValue());
        const std::array<std::byte, 1> projectBytes{std::byte{1U}};
        const std::array resources{ProjectValidationResourceView{std::move(path).Value(), projectBytes}};
        const auto validated = host->Validate({.projectId = "project", .resources = resources}, {});
        REQUIRE(validated.HasValue());
        REQUIRE(validated.Value().size() == 1U);
        CHECK(validated.Value().front().result.Diagnostics().front().code.Value() == "validation.finding");

        const std::array<std::byte, 2> pipelineBytes{std::byte{2U}, std::byte{3U}};
        const std::array pipelineInputs{PipelineArtifactView{"artifact.source", pipelineBytes}};
        const auto pipeline = host->ExecutePipeline(pipelineInputs, {});
        REQUIRE(pipeline.HasValue());
        REQUIRE(pipeline.Value().outputs.size() == 1U);
        CHECK(pipeline.Value().outputs.front().id.value == "artifact.output");

        const auto tool = host->InvokeToolchain(toolchainAuthority, {{"tool.compiler"}, {"source.cpp"}}, {});
        REQUIRE(tool.HasValue());
        CHECK(processRunner.calls == 1U);
        CHECK(processRunner.executable == "/approved/tool");

        const ExtensionAdmissionPolicy policy{.revision = 1U, .availableCapabilities = {{"horo.asset.pipeline"}}};
        const ExtensionAdmissionRequest request{.extensionId = "com.example.consumer",
                                                .moduleId = "com.example.consumer.backend",
                                                .activationGeneration = 7U,
                                                .capabilities = {{{"horo.asset.pipeline"}, {}}}};
        auto admission = ExtensionCapabilityAdmission::Evaluate(request, policy);
        REQUIRE(admission.HasValue());
        auto capabilityAuthority = admission.Value().Grant({"horo.asset.pipeline"});
        REQUIRE(capabilityAuthority.HasValue());
        const auto capability = host->ResolveCapability(capabilityAuthority.Value(), {{1U, 0U, 0U}, {1U, 0U, 0U}}, "com.example.consumer",
                                                        "com.example.consumer.backend", 7U);
        REQUIRE(capability.HasValue());
        CHECK(capability.Value().Descriptor().provider.providerId == "com.example.pipeline-provider");
        CHECK(capability.Value().IsUsable());

        const HeadlessExtensionHostSnapshot snapshot = host->Inspect();
        CHECK(snapshot.state == HeadlessExtensionHostState::Ready);
        CHECK(snapshot.discoveredPackages == std::vector<std::string>{"com.example.headless"});
        CHECK(snapshot.loadedExtensions == std::vector<std::string>{"com.example.headless"});
        CHECK(snapshot.diagnostics.empty());
    }

    TEST_CASE_METHOD(HeadlessHostFixture, "Headless host retains typed diagnostics and closes every work path on shutdown",
                     "[integration][extensions][headless]") {
        std::unique_ptr<HeadlessExtensionHost> host = CreateHost();
        ToolchainInvocationAuthority authority;
        RegisterProviders(*host, authority);
        REQUIRE(host->Start({}).HasValue());

        const auto unavailable = host->Import("com.example.missing", {}, {});
        REQUIRE(unavailable.HasError());
        CHECK(unavailable.ErrorValue().code.Value() == "headless_importer_unavailable");
        const HeadlessExtensionHostSnapshot beforeShutdown = host->Inspect();
        REQUIRE(beforeShutdown.diagnostics.size() == 1U);
        CHECK(beforeShutdown.diagnostics.front().stage == HeadlessExtensionHostStage::Import);
        CHECK(beforeShutdown.diagnostics.front().subject == "com.example.missing");

        host->Shutdown();
        host->Shutdown();
        CHECK(host->Inspect().state == HeadlessExtensionHostState::Shutdown);
        const auto rejected = host->Cook(CookRequest(), {});
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "headless_host_state_invalid");
        CHECK(host->RegisterToolchain({.contributionId = "com.example.late",
                                       .providerId = "com.example.backend",
                                       .providerGeneration = 2U,
                                       .tools = {{"tool.compiler"}}})
                  .ErrorValue()
                  .code.Value() == "headless_host_state_invalid");
    }

    TEST_CASE_METHOD(HeadlessHostFixture, "Headless host fails closed with an attributed activation diagnostic",
                     "[integration][extensions][headless]") {
        std::unique_ptr<HeadlessExtensionHost> host = CreateHost();
        const Discovery::PackageLocation package = PrepareInvalidPackage();

        const auto started = host->Start(std::span{&package, 1U});

        REQUIRE(started.HasError());
        CHECK(started.ErrorValue().code.Value() == "headless_host_activation_failed");
        const HeadlessExtensionHostSnapshot snapshot = host->Inspect();
        CHECK(snapshot.state == HeadlessExtensionHostState::Failed);
        CHECK(snapshot.discoveredPackages == std::vector<std::string>{"com.example.invalid"});
        CHECK(snapshot.loadedExtensions.empty());
        REQUIRE(snapshot.diagnostics.size() == 1U);
        CHECK(snapshot.diagnostics.front().stage == HeadlessExtensionHostStage::Activation);
        CHECK(snapshot.diagnostics.front().subject == "com.example.invalid");
        CHECK(static_cast<bool>(snapshot.diagnostics.front().error.cause));

        const auto rejected = host->ExecutePipeline({}, {});
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "headless_host_state_invalid");
    }
}  // namespace Horo::Extensions::Tests
