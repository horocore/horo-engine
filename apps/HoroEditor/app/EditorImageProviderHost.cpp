#include "EditorImageProviderHost.h"

#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "HostSignatureDocument.h"

#include <algorithm>
#include <array>
#include <bit>
#include <new>

namespace Horo::Editor {
    namespace {
        using namespace Application::Internal;
        using namespace Render;
        using Bytes = std::vector<std::uint8_t>;
        using Entry = NativeReconstructionEntry;
        const ErrorDomainId Domain{"editor.image_provider"};
        const ErrorCodeDescriptor InvalidRequest{Domain, ErrorCode{"editor.image_provider.invalid_request"}, ErrorSeverity::Error,
                                                 "Image provider inspection inputs are incomplete or invalid.",
                                                 "Supply one explicit mode, product root and independent publisher file."};
        const ErrorCodeDescriptor InvalidInventory{Domain, ErrorCode{"editor.image_provider.invalid_inventory"}, ErrorSeverity::Error,
                                                   "The signed inventory does not describe this editor's complete requested files.",
                                                   "Use the signed release inventory for the active editor platform and version."};
        const ErrorCodeDescriptor ArtifactMismatch{Domain, ErrorCode{"editor.image_provider.artifact_mismatch"}, ErrorSeverity::Error,
                                                   "A provider file differs from its signed inventory record.",
                                                   "Restore authentic product files before inspection."};
        constexpr std::size_t MaximumManifestBytes = 16U * 1024U * 1024U;
        constexpr std::size_t MaximumPayloadBytes = 16U * 1024U * 1024U;
        constexpr std::size_t MaximumReflectionBytes = 1024U * 1024U;

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
            const auto signature =
                files.ReadFileBounded(request.productRoot / "manifest.signature.json", MaximumHostSignatureDocumentBytes);
            if (signature.HasError())
                return Outcome::Failure(signature.ErrorValue());
            const auto detached = DecodeDetachedSignatureDocument(std::as_bytes(std::span{signature.Value()}));
            if (detached.HasError())
                return Outcome::Failure(detached.ErrorValue());
            const auto bytes = files.ReadFileBounded(request.productRoot / "manifest.json", MaximumManifestBytes);
            if (bytes.HasError())
                return Outcome::Failure(bytes.ErrorValue());
            const auto verified = verifier.Verify(std::as_bytes(std::span{bytes.Value()}), detached.Value());
            if (verified.HasError())
                return Outcome::Failure(verified.ErrorValue());
            const auto json = std::string_view{std::bit_cast<const char *>(bytes.Value().data()), bytes.Value().size()};
            auto manifest = Release::ReleaseArtifactManifest::ParseCanonical(json);
            if (manifest.HasError())
                return manifest;
            if (!IsSelectedProduct(manifest.Value().Data(), expected) || manifest.Value().Digest() != verified.Value().ArtifactDigest())
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
            const auto prefix =
                std::string{"assets/shaders/runtime/native-reconstruction-vulkan/"} + std::string{NativeReconstructionEntryName(entry)};
            const auto payloadRecord = std::ranges::find(inventory, prefix + ".horoshdr", &Release::ReleaseArtifactRecord::path);
            const auto reflectionRecord = std::ranges::find(inventory, prefix + ".reflection.json", &Release::ReleaseArtifactRecord::path);
            if (payloadRecord == inventory.end() && reflectionRecord == inventory.end())
                return Outcome::Success(std::nullopt);
            if (payloadRecord == inventory.end() || reflectionRecord == inventory.end())
                return Outcome::Failure(MakeError(InvalidInventory));
            auto payload = ReadArtifact(files, root, *payloadRecord, MaximumPayloadBytes);
            if (payload.HasError())
                return Outcome::Failure(payload.ErrorValue());
            const auto reflection = ReadArtifact(files, root, *reflectionRecord, MaximumReflectionBytes);
            if (reflection.HasError())
                return Outcome::Failure(reflection.ErrorValue());

            const ShaderTargetRequirement target{.backend = ShaderTargetBackend::Vulkan,
                                                 .payloadFormat = ShaderPayloadFormat::SpirV16,
                                                 .descriptorVersion = 1,
                                                 .interfaceSchemaVersion = 1,
                                                 .maximumBindings = 16,
                                                 .maximumInlineConstantBytes = 128,
                                                 .supportsCompute = true,
                                                 .supportsStorageResources = true};
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

        /** @brief Retains in-memory stage bytes through existing registry admission and the selected frontend query. */
        Result<EditorImageProviderAvailability> QueryInstalledRoute(RenderFrontend &frontend, ReconstructionKind mode,
                                                                    const Release::ReleaseArtifactManifest &manifest,
                                                                    const DurableFileSystem &files, const std::filesystem::path &root) {
            using Outcome = Result<EditorImageProviderAvailability>;
            const bool temporal = mode == ReconstructionKind::Temporal;
            const std::array entries{temporal ? Entry::TemporalSeed : Entry::Spatial, Entry::TemporalHistory};
            std::array<CookedNativeReconstructionKernel, 2> cooked;
            std::array<Sha256Digest, 2> digests;
            std::size_t count{};
            for (const auto entry : std::span{entries}.first(temporal ? entries.size() : 1U)) {
                auto loaded = LoadEntry(files, root, manifest.Artifacts(), entry);
                if (loaded.HasError())
                    return Outcome::Failure(loaded.ErrorValue());
                if (auto candidate = std::move(loaded).Value()) {
                    digests[count] = candidate->artifact.artifactKey;
                    cooked[count++] = std::move(*candidate);
                }
            }
            auto descriptor = DescribeNativeReconstructionImageProvider(mode, std::span{cooked}.first(count));
            if (descriptor.HasError())
                return Outcome::Failure(descriptor.ErrorValue());
            auto route = descriptor.Value().route;
            ImageProviderRegistry providers;
            if (auto registered = providers.Register(std::move(descriptor).Value()); registered.HasError())
                return Outcome::Failure(registered.ErrorValue());
            providers.Seal();
            const auto supported =
                frontend.DiscoverImageProvider(providers, route,
                                               {.hostApi = {1, 0}, .admittedCookedArtifacts = std::span{digests}.first(count)});
            if (supported.HasError())
                return Outcome::Failure(supported.ErrorValue());
            return Outcome::Success({std::move(route), supported.Value(), manifest.Digest()});
        }
    }  // namespace

    /** @copydoc ParseEditorImageProviderInspection */
    Result<std::optional<EditorImageProviderInspection>> ParseEditorImageProviderInspection(std::span<char *const> args) {
        using Outcome = Result<std::optional<EditorImageProviderInspection>>;
        constexpr std::array<std::string_view, 3> flags{"--inspect-image-provider", "--image-provider-root", "--image-provider-trust"};
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
        if (supplied != flags.size() || (*values[0] != "spatial" && *values[0] != "taau"))
            return Outcome::Failure(MakeError(InvalidRequest));
        try {
            EditorImageProviderInspection request{*values[0] == "spatial" ? ReconstructionKind::Spatial : ReconstructionKind::Temporal,
                                                  std::filesystem::path{*values[1]}, std::filesystem::path{*values[2]}};
            if (!ValidHostPath(request.productRoot) || !ValidHostPath(request.trustedPublishers))
                return Outcome::Failure(MakeError(InvalidRequest));
            return Outcome::Success(std::move(request));
        } catch (const std::filesystem::filesystem_error &) {
            return Outcome::Failure(MakeError(InvalidRequest));
        }
    }

    /** @copydoc InspectEditorImageProvider */
    Result<EditorImageProviderAvailability> InspectEditorImageProvider(RenderFrontend &frontend,
                                                                       const EditorImageProviderInspection &request,
                                                                       const EditorProviderProductIdentity &expected,
                                                                       const DurableFileSystem &files,
                                                                       const Security::ArtifactVerifier &verifier) {
        using Outcome = Result<EditorImageProviderAvailability>;
        if (!ValidHostPath(request.productRoot) ||
            (request.mode != ReconstructionKind::Spatial && request.mode != ReconstructionKind::Temporal))
            return Outcome::Failure(MakeError(InvalidRequest));
        try {
            const auto root = files.InspectEntry(request.productRoot);
            if (root.HasError())
                return Outcome::Failure(root.ErrorValue());
            if (!root.Value() || root.Value()->kind != FileEntryKind::Directory)
                return Outcome::Failure(MakeError(InvalidRequest));
            const auto manifest = ReadManifest(request, expected, files, verifier);
            if (manifest.HasError())
                return Outcome::Failure(manifest.ErrorValue());
            return QueryInstalledRoute(frontend, request.mode, manifest.Value(), files, request.productRoot);
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
