#include "EditorImageProviderHost.h"

#include "EditorNativeReconstructionReleaseLayout.h"
#include "EditorVulkanViewportShader.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "HostReleaseManifestSignatureVerifier.h"
#include "HostSignatureDocument.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <new>

namespace Horo::Editor {
    namespace {
        using namespace Application::Internal;
        using namespace Render;
        using Bytes = std::vector<std::uint8_t>;
        using Entry = NativeReconstructionEntry;
        const ErrorDomainId Domain{"editor.image_provider"};
        const ErrorCodeDescriptor InvalidRequest{Domain, ErrorCode{"editor.image_provider.invalid_request"}, ErrorSeverity::Error,
                                                 "Image provider invocation inputs are incomplete or invalid.",
                                                 "Supply one explicit mode, product root and independent publisher file."};
        const ErrorCodeDescriptor InvalidInventory{Domain, ErrorCode{"editor.image_provider.invalid_inventory"}, ErrorSeverity::Error,
                                                   "The signed inventory does not describe this editor's complete requested files.",
                                                   "Use the signed release inventory for the active editor platform and version."};
        const ErrorCodeDescriptor ArtifactMismatch{Domain, ErrorCode{"editor.image_provider.artifact_mismatch"}, ErrorSeverity::Error,
                                                   "A provider file differs from its signed inventory record.",
                                                   "Restore authentic product files before provider admission."};
        constexpr std::size_t MaximumManifestBytes = 16U * 1024U * 1024U;

        /** @brief Accepts a normalized absolute host path, never a path provided by the signed document. */
        bool ValidHostPath(const std::filesystem::path &path) {
            return path.is_absolute() && path == path.lexically_normal() && path.filename() != "." && path.filename() != "..";
        }

        /** @brief Tests the actual editor release tuple before accessing any requested artifact. */
        bool IsSelectedProduct(const Release::ReleaseArtifactManifestData &data, const EditorProviderProductIdentity &expected) {
            return data.product.kind == Release::DistributionProductKind::Editor &&
                   data.version == Release::ReleaseProductVersion{expected.version} && data.platform == expected.platform &&
                   data.architecture == expected.architecture;
        }

        /** @brief Reads the detached bytes and verifies exactly the bytes passed to the existing canonical inventory parser. */
        Result<Release::ReleaseArtifactManifest> ReadManifest(const EditorImageProviderInspection &request,
                                                              const EditorProviderProductIdentity &expected, const DurableFileSystem &files,
                                                              const Security::ArtifactVerifier &verifier) {
            using Outcome = Result<Release::ReleaseArtifactManifest>;
            const auto signature = files.ReadFileBounded(request.productRoot / Release::ReleaseManifestSignatureFilename,
                                                         Release::MaximumReleaseManifestSignatureBytes);
            if (signature.HasError())
                return Outcome::Failure(signature.ErrorValue());
            const auto bytes = files.ReadFileBounded(request.productRoot / Release::ReleaseManifestFilename, MaximumManifestBytes);
            if (bytes.HasError())
                return Outcome::Failure(bytes.ErrorValue());
            const HostReleaseManifestSignatureVerifier manifestVerifier{verifier};
            const auto verified =
                manifestVerifier.Verify(std::as_bytes(std::span{bytes.Value()}), std::as_bytes(std::span{signature.Value()}));
            if (verified.HasError())
                return Outcome::Failure(verified.ErrorValue());
            const auto json = std::string_view{std::bit_cast<const char *>(bytes.Value().data()), bytes.Value().size()};
            auto manifest = Release::ReleaseArtifactManifest::ParseCanonical(json);
            if (manifest.HasError())
                return manifest;
            if (!IsSelectedProduct(manifest.Value().Data(), expected))
                return Outcome::Failure(MakeError(InvalidInventory));
            return manifest;
        }

        /** @brief Checks exact signed size and digest on the same bounded bytes subsequently decoded. */
        Result<Bytes> ReadArtifact(const DurableFileSystem &files, const std::filesystem::path &root,
                                   const Release::ReleaseArtifactRecord &record, std::size_t maximum) {
            if (record.role != Release::ReleaseArtifactRole::AssetArchive || record.size == 0 || record.size > maximum)
                return Result<Bytes>::Failure(MakeError(InvalidInventory));
            auto bytes = files.ReadFileBounded(root / record.path, static_cast<std::size_t>(record.size));
            if (bytes.HasError())
                return bytes;
            if (bytes.Value().size() != record.size || ComputeSha256(std::as_bytes(std::span{bytes.Value()})) != record.digest)
                return Result<Bytes>::Failure(MakeError(ArtifactMismatch));
            return bytes;
        }

        /** @brief Loads only the already requested canonical shader stage and its matching cook reflection. */
        Result<std::optional<CookedNativeReconstructionKernel>> LoadEntry(const DurableFileSystem &files, const std::filesystem::path &root,
                                                                          std::span<const Release::ReleaseArtifactRecord> inventory,
                                                                          Entry entry) {
            using Outcome = Result<std::optional<CookedNativeReconstructionKernel>>;
            const auto prefix = NativeReconstructionRelease::Prefix(entry);
            if (prefix.empty())
                return Outcome::Failure(MakeError(InvalidInventory));
            const auto payloadRecord = std::ranges::find(inventory, prefix + ".horoshdr", &Release::ReleaseArtifactRecord::path);
            const auto reflectionRecord = std::ranges::find(inventory, prefix + ".reflection.json", &Release::ReleaseArtifactRecord::path);
            if (payloadRecord == inventory.end() && reflectionRecord == inventory.end())
                return Outcome::Success(std::nullopt);
            if (payloadRecord == inventory.end() || reflectionRecord == inventory.end())
                return Outcome::Failure(MakeError(InvalidInventory));
            auto payload = ReadArtifact(files, root, *payloadRecord, NativeReconstructionRelease::MaximumPayloadBytes);
            if (payload.HasError())
                return Outcome::Failure(payload.ErrorValue());
            const auto reflection = ReadArtifact(files, root, *reflectionRecord, NativeReconstructionRelease::MaximumReflectionBytes);
            if (reflection.HasError())
                return Outcome::Failure(reflection.ErrorValue());

            constexpr auto target = NativeReconstructionRelease::Target;
            const auto expected = DescribeNativeReconstructionShader(entry, target);
            if (expected.HasError())
                return Outcome::Failure(expected.ErrorValue());
            auto final = DecodeReflectedShaderArtifact(expected.Value().manifest, target, payload.Value(), reflection.Value());
            if (final.HasError())
                return Outcome::Failure(final.ErrorValue());
            CompiledShaderArtifact artifact{.backend = target.backend,
                                            .payloadFormat = target.payloadFormat,
                                            .artifactKey = final.Value().compiledArtifactKey,
                                            .payload = std::move(payload).Value(),
                                            .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(final).Value())};
            return Outcome::Success(CookedNativeReconstructionKernel{entry, target, std::move(artifact)});
        }

        /** @brief Stores the available cooked stages and their immutable registry identities in route order. */
        struct InstalledKernels final {
            std::array<CookedNativeReconstructionKernel, 2> cooked;
            std::array<Sha256Digest, 2> digests;
            std::size_t count{};
        };

        /** @brief Collects the signed kernels declared for the requested route. */
        Result<InstalledKernels> LoadInstalledKernels(ReconstructionKind mode, const Release::ReleaseArtifactManifest &manifest,
                                                      const DurableFileSystem &files, const std::filesystem::path &root) {
            using Outcome = Result<InstalledKernels>;
            using enum Entry;
            const bool temporal = mode == ReconstructionKind::Temporal;
            const std::array entries{temporal ? TemporalSeed : Spatial, TemporalHistory};
            InstalledKernels installed;
            for (const auto entry : std::span{entries}.first(temporal ? entries.size() : 1U)) {
                auto loaded = LoadEntry(files, root, manifest.Artifacts(), entry);
                if (loaded.HasError())
                    return Outcome::Failure(loaded.ErrorValue());
                if (auto candidate = std::move(loaded).Value()) {
                    installed.digests[installed.count] = candidate->artifact.artifactKey;
                    installed.cooked[installed.count++] = std::move(*candidate);
                }
            }
            return Outcome::Success(std::move(installed));
        }

        /** @brief Retains in-memory stage bytes through existing registry admission and the selected frontend query. */
        Result<EditorImageProviderAvailability> QueryInstalledRoute(const RenderFrontend &frontend, ReconstructionKind mode,
                                                                    const Release::ReleaseArtifactManifest &manifest,
                                                                    const DurableFileSystem &files, const std::filesystem::path &root,
                                                                    std::span<CookedNativeReconstructionKernel> retained = {}) {
            using Outcome = Result<EditorImageProviderAvailability>;
            auto loaded = LoadInstalledKernels(mode, manifest, files, root);
            if (loaded.HasError())
                return Outcome::Failure(loaded.ErrorValue());
            auto installed = std::move(loaded).Value();
            const auto kernels = std::span{installed.cooked}.first(installed.count);
            auto descriptor = DescribeNativeReconstructionImageProvider(mode, kernels);
            if (descriptor.HasError())
                return Outcome::Failure(descriptor.ErrorValue());
            auto route = descriptor.Value().route;
            ImageProviderRegistry providers;
            if (auto registered = providers.Register(std::move(descriptor).Value()); registered.HasError())
                return Outcome::Failure(registered.ErrorValue());
            providers.Seal();
            const auto supported =
                frontend.DiscoverImageProvider(providers, route,
                                               {.hostApi = {1, 0},
                                                .admittedCookedArtifacts = std::span{installed.digests}.first(installed.count)});
            if (supported.HasError())
                return Outcome::Failure(supported.ErrorValue());
            if (!retained.empty()) {
                if (kernels.size() != retained.size())
                    return Outcome::Failure(MakeError(NativeReconstructionErrors::Unsupported));
                std::ranges::move(kernels, retained.begin());
            }
            return Outcome::Success({std::move(route), supported.Value(), manifest.Digest()});
        }

        /** @brief Performs the shared root, signature and product-identity fence before reading artifact bytes. */
        Result<Release::ReleaseArtifactManifest> ReadCheckedManifest(const EditorImageProviderInspection &request,
                                                                     const EditorProviderProductIdentity &expected,
                                                                     const DurableFileSystem &files,
                                                                     const Security::ArtifactVerifier &verifier) {
            using Outcome = Result<Release::ReleaseArtifactManifest>;
            if (!ValidHostPath(request.productRoot))
                return Outcome::Failure(MakeError(InvalidRequest));
            const auto root = files.InspectEntry(request.productRoot);
            if (root.HasError())
                return Outcome::Failure(root.ErrorValue());
            if (!root.Value() || root.Value()->kind != FileEntryKind::Directory)
                return Outcome::Failure(MakeError(InvalidRequest));
            return ReadManifest(request, expected, files, verifier);
        }

        /** @brief Reads an exact editor artifact from the already verified provider inventory. */
        Result<Bytes> ReadSignedAsset(const DurableFileSystem &files, const std::filesystem::path &root,
                                      const Release::ReleaseArtifactManifest &manifest, std::string_view path, std::size_t maximum) {
            const auto records = manifest.Artifacts();
            const auto found = std::ranges::find(records, path, &Release::ReleaseArtifactRecord::path);
            if (found == records.end())
                return Result<Bytes>::Failure(MakeError(InvalidInventory));
            return ReadArtifact(files, root, *found, maximum);
        }

        /** @brief Loads a same-operation shader pair through the exact signed length and digest checks. */
        Result<EditorSignedProviderShader> LoadSignedProviderShader(const DurableFileSystem &files, const std::filesystem::path &root,
                                                                    const Release::ReleaseArtifactManifest &manifest,
                                                                    std::string_view payloadPath, std::string_view reflectionPath) {
            using Outcome = Result<EditorSignedProviderShader>;
            using namespace NativeReconstructionRelease;
            auto payload = ReadSignedAsset(files, root, manifest, payloadPath, MaximumPayloadBytes);
            if (payload.HasError())
                return Outcome::Failure(payload.ErrorValue());
            auto reflection = ReadSignedAsset(files, root, manifest, reflectionPath, MaximumReflectionBytes);
            if (reflection.HasError())
                return Outcome::Failure(reflection.ErrorValue());
            return Outcome::Success({std::move(payload).Value(), std::move(reflection).Value()});
        }

        /** @brief Returns one exact signed temporal guide after validating its shader and reflected binding contract. */
        Result<EditorSignedProviderShader> LoadTemporalGuide(const DurableFileSystem &files, const std::filesystem::path &root,
                                                             const Release::ReleaseArtifactManifest &manifest,
                                                             EditorVulkanTemporalInput input, std::string_view name) {
            const std::string prefix{name};
            auto guide = LoadSignedProviderShader(files, root, manifest, prefix + ".horoshdr", prefix + ".reflection.json");
            if (guide.HasError())
                return guide;
            if (const auto validated = ValidateEditorVulkanTemporalInputCook(input, guide.Value().payload, guide.Value().reflection);
                validated.HasError())
                return Result<EditorSignedProviderShader>::Failure(validated.ErrorValue());
            return guide;
        }

        struct TemporalGuides final {
            EditorSignedProviderShader linearDepth;
            EditorSignedProviderShader motion;
        };

        /** @brief Returns both authenticated guides together so callers never retain an incomplete pair. */
        Result<TemporalGuides> LoadTemporalGuides(const DurableFileSystem &files, const std::filesystem::path &root,
                                                  const Release::ReleaseArtifactManifest &manifest) {
            using Outcome = Result<TemporalGuides>;
            using namespace NativeReconstructionRelease;
            auto depth = LoadTemporalGuide(files, root, manifest, EditorVulkanTemporalInput::LinearDepth, TemporalDepthPrefix);
            if (depth.HasError())
                return Outcome::Failure(depth.ErrorValue());
            auto motion = LoadTemporalGuide(files, root, manifest, EditorVulkanTemporalInput::Motion, TemporalMotionPrefix);
            if (motion.HasError())
                return Outcome::Failure(motion.ErrorValue());
            return Outcome::Success({std::move(depth).Value(), std::move(motion).Value()});
        }

        /** @brief Detects a guide declaration without treating an entirely absent optional family as broken. */
        bool DeclaresTemporalGuides(const Release::ReleaseArtifactManifest &manifest) {
            using namespace NativeReconstructionRelease;
            const auto inventory = manifest.Artifacts();
            const auto declares = [&inventory](std::string_view name) {
                const std::string prefix{name};
                return std::ranges::find(inventory, prefix + ".horoshdr", &Release::ReleaseArtifactRecord::path) != inventory.end() ||
                       std::ranges::find(inventory, prefix + ".reflection.json", &Release::ReleaseArtifactRecord::path) != inventory.end();
            };
            return declares(TemporalDepthPrefix) || declares(TemporalMotionPrefix);
        }
    }  // namespace

    /** @copydoc ParseEditorImageProviderInspection */
    Result<std::optional<EditorImageProviderInspection>> ParseEditorImageProviderInspection(std::span<char *const> args) {
        using Outcome = Result<std::optional<EditorImageProviderInspection>>;
        constexpr std::array<std::string_view, 5> flags{"--inspect-image-provider", "--image-provider-root", "--image-provider-trust",
                                                        "--activate-image-provider", "--image-provider-gpu-budget-ms"};
        std::array<std::optional<std::string_view>, flags.size()> values;
        std::size_t supplied{};
        for (const auto *raw : args.subspan(std::min<std::size_t>(1, args.size()))) {
            if (raw == nullptr)
                continue;
            const std::string_view arg{raw};
            const auto split = arg.find('=');
            const auto option = std::ranges::find(flags, arg.substr(0, split));
            if (option == flags.end())
                continue;
            auto &value = values[static_cast<std::size_t>(option - flags.begin())];
            if (value || split == arg.npos || split + 1 == arg.size())
                return Outcome::Failure(MakeError(InvalidRequest));
            value = arg.substr(split + 1);
            ++supplied;
        }
        if (supplied == 0)
            return Outcome::Success(std::nullopt);
        const bool activate = values[3].has_value();
        const auto &mode = activate ? values[3] : values[0];
        const auto budget = values.back();
        if (supplied != 3 + std::size_t{budget.has_value()} || !values[1] || !values[2] || !mode || (activate && values[0]) ||
            (*mode != "spatial" && *mode != "taau" && !(activate && *mode == "taa")))
            return Outcome::Failure(MakeError(InvalidRequest));
        std::optional<std::chrono::milliseconds> gpuBudget;
        if (budget) {
            constexpr std::uint32_t maximumGpuBudgetMs = 1000;
            std::uint32_t milliseconds{};
            const auto end = budget->data() + budget->size();
            const auto value = std::from_chars(budget->data(), end, milliseconds);
            if (!activate || *mode != "taau" || value.ec != std::errc{} || value.ptr != end || milliseconds == 0 ||
                milliseconds > maximumGpuBudgetMs)
                return Outcome::Failure(
                    MakeError(InvalidRequest, "The GPU budget requires TAAU activation and 1 to 1000 whole milliseconds."));
            gpuBudget = std::chrono::milliseconds{milliseconds};
        }
        try {
            EditorImageProviderInspection request{*mode == "spatial" ? ReconstructionKind::Spatial : ReconstructionKind::Temporal,
                                                  std::filesystem::path{*values[1]},
                                                  std::filesystem::path{*values[2]},
                                                  activate,
                                                  activate && *mode == "taau",
                                                  gpuBudget};
            if (!ValidHostPath(request.productRoot) || !ValidHostPath(request.trustedPublishers))
                return Outcome::Failure(MakeError(InvalidRequest));
            return Outcome::Success(std::move(request));
        } catch (const std::filesystem::filesystem_error &) {
            return Outcome::Failure(MakeError(InvalidRequest));
        }
    }

    /** @copydoc InspectEditorImageProvider */
    Result<EditorImageProviderAvailability> InspectEditorImageProvider(const RenderFrontend &frontend,
                                                                       const EditorImageProviderInspection &request,
                                                                       const EditorProviderProductIdentity &expected,
                                                                       const DurableFileSystem &files,
                                                                       const Security::ArtifactVerifier &verifier) {
        using Outcome = Result<EditorImageProviderAvailability>;
        if (request.activateViewport || (request.mode != ReconstructionKind::Spatial && request.mode != ReconstructionKind::Temporal))
            return Outcome::Failure(MakeError(InvalidRequest));
        try {
            const auto manifest = ReadCheckedManifest(request, expected, files, verifier);
            if (manifest.HasError())
                return Outcome::Failure(manifest.ErrorValue());
            auto route = QueryInstalledRoute(frontend, request.mode, manifest.Value(), files, request.productRoot);
            if (route.HasError() || request.mode != ReconstructionKind::Temporal || !DeclaresTemporalGuides(manifest.Value()))
                return route;
            if (const auto guides = LoadTemporalGuides(files, request.productRoot, manifest.Value()); guides.HasError())
                return Outcome::Failure(guides.ErrorValue());
            auto report = std::move(route).Value();
            report.temporalGuidesVerified = true;
            return Outcome::Success(std::move(report));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(ImageProviderRegistryErrors::AllocationFailed));
        }
    }

    /** @copydoc LoadEditorSignedSpatialProvider */
    Result<EditorSignedSpatialProvider> LoadEditorSignedSpatialProvider(const RenderFrontend &frontend,
                                                                        const EditorImageProviderInspection &request,
                                                                        const EditorProviderProductIdentity &expected,
                                                                        const DurableFileSystem &files,
                                                                        const Security::ArtifactVerifier &verifier) {
        using Outcome = Result<EditorSignedSpatialProvider>;
        if (!request.activateViewport || request.mode != ReconstructionKind::Spatial)
            return Outcome::Failure(MakeError(InvalidRequest));
        try {
            const auto manifest = ReadCheckedManifest(request, expected, files, verifier);
            if (manifest.HasError())
                return Outcome::Failure(manifest.ErrorValue());
            auto scene =
                LoadSignedProviderShader(files, request.productRoot, manifest.Value(), NativeReconstructionRelease::SceneLinearPayloadPath,
                                         NativeReconstructionRelease::SceneLinearReflectionPath);
            if (scene.HasError())
                return Outcome::Failure(scene.ErrorValue());
            EditorSignedSpatialProvider admitted{};
            const auto route =
                QueryInstalledRoute(frontend, request.mode, manifest.Value(), files, request.productRoot, std::span{&admitted.kernel, 1});
            if (route.HasError())
                return Outcome::Failure(route.ErrorValue());
            if (route.Value().support.readiness != ImageProviderReadiness::Ready)
                return Outcome::Failure(MakeError(NativeReconstructionErrors::Unsupported));
            auto shader = std::move(scene).Value();
            admitted.scenePayload = std::move(shader.payload);
            admitted.sceneReflection = std::move(shader.reflection);
            return Outcome::Success(std::move(admitted));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(ImageProviderRegistryErrors::AllocationFailed));
        }
    }

    /** @copydoc LoadEditorSignedTemporalProvider */
    Result<EditorSignedTemporalProvider> LoadEditorSignedTemporalProvider(const RenderFrontend &frontend,
                                                                          const EditorImageProviderInspection &request,
                                                                          const EditorProviderProductIdentity &expected,
                                                                          const DurableFileSystem &files,
                                                                          const Security::ArtifactVerifier &verifier) {
        using Outcome = Result<EditorSignedTemporalProvider>;
        using namespace NativeReconstructionRelease;
        if (request.mode != ReconstructionKind::Temporal)
            return Outcome::Failure(MakeError(InvalidRequest));
        try {
            const auto manifest = ReadCheckedManifest(request, expected, files, verifier);
            if (manifest.HasError())
                return Outcome::Failure(manifest.ErrorValue());
            auto scene =
                LoadSignedProviderShader(files, request.productRoot, manifest.Value(), SceneLinearPayloadPath, SceneLinearReflectionPath);
            if (scene.HasError())
                return Outcome::Failure(scene.ErrorValue());
            if (const auto shader = LoadEditorVulkanSceneLinearCook(scene.Value().payload, scene.Value().reflection); shader.HasError())
                return Outcome::Failure(shader.ErrorValue());
            auto guides = LoadTemporalGuides(files, request.productRoot, manifest.Value());
            if (guides.HasError())
                return Outcome::Failure(guides.ErrorValue());
            std::array<CookedNativeReconstructionKernel, 2> kernels;
            auto route = QueryInstalledRoute(frontend, request.mode, manifest.Value(), files, request.productRoot, kernels);
            if (route.HasError())
                return Outcome::Failure(route.ErrorValue());
            auto report = std::move(route).Value();
            report.temporalGuidesVerified = true;
            auto shaders = std::move(guides).Value();
            std::optional<CookedNativeReconstructionKernel> fallback;
            if (request.scaleViewport) {
                auto loaded = LoadEntry(files, request.productRoot, manifest.Value().Artifacts(), Entry::Spatial);
                if (loaded.HasError())
                    return Outcome::Failure(loaded.ErrorValue());
                if (!loaded.Value())
                    return Outcome::Failure(MakeError(InvalidInventory));
                fallback = std::move(loaded).Value();
            }
            return Outcome::Success({std::move(report), std::move(kernels), std::move(scene).Value(), std::move(shaders.linearDepth),
                                     std::move(shaders.motion), std::move(fallback)});
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(ImageProviderRegistryErrors::AllocationFailed));
        }
    }

    /** @copydoc ReadEditorImageProviderTrust */
    Result<Security::ArtifactVerifier> ReadEditorImageProviderTrust(const DurableFileSystem &files, const std::filesystem::path &trust,
                                                                    std::uint64_t nowMilliseconds) {
        using Outcome = Result<Security::ArtifactVerifier>;
        const auto raw = files.ReadFileBounded(trust, MaximumHostSignatureDocumentBytes);
        if (raw.HasError())
            return Outcome::Failure(raw.ErrorValue());
        const auto policy =
            DecodePublisherTrustDocument(std::as_bytes(std::span{raw.Value()}), Packages::PackagePublisherVerificationPolicy{});
        if (policy.HasError())
            return Outcome::Failure(policy.ErrorValue());
        if (policy.Value().allowUnsigned)
            return Outcome::Failure(MakeError(InvalidRequest));
        auto roots = std::make_shared<Security::TrustedRootStore>();
        std::size_t admitted{};
        for (const auto &record : policy.Value().publishers) {
            if (record.publisherRevoked || (record.expiresAtUnixMilliseconds != 0 && record.expiresAtUnixMilliseconds <= nowMilliseconds))
                continue;
            if (const auto added = roots->Add({record.algorithm, record.publisher.Value(), record.keyId, record.publicKey});
                added.HasError())
                return Outcome::Failure(added.ErrorValue());
            ++admitted;
        }
        if (admitted == 0)
            return Outcome::Failure(MakeError(InvalidRequest));
        return Outcome::Success(Security::ArtifactVerifier{Security::CreateMbedTlsSignatureProvider(), std::move(roots)});
    }

    /** @copydoc EditorImageProviderReadinessName */
    const char *EditorImageProviderReadinessName(ImageProviderReadiness value) noexcept {
        constexpr std::array
            names{"ready",           "api-mismatch",           "component-unavailable", "sdk-unavailable",        "backend-mismatch",
                  "device-mismatch", "capability-unavailable", "texture-unsupported",   "cooked-artifact-missing"};
        const auto index = static_cast<std::size_t>(value);
        return index < names.size() ? names[index] : "unknown";
    }
}  // namespace Horo::Editor
