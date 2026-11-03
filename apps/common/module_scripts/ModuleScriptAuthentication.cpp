#include "Horo/Extensions/ExtensionErrors.h"
#include "HostSignatureDocument.h"
#include "ModuleScriptPackage.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <fstream>
#include <set>

namespace Horo::Application::Internal {
    namespace {
        using Bytes = Result<std::vector<std::byte>>;

        template <typename T> Result<T> Denied() {
            return Result<T>::Failure(MakeError(Extensions::ExtensionErrors::CapabilityUnavailable,
                                                "Supply an independent approved publisher trust document, exact signed package inputs and "
                                                "explicit native/script consent."));
        }

        bool Outside(std::filesystem::path path, const std::filesystem::path &root) {
            for (;;) {
                if (path == root)
                    return false;
                const auto parent = path.parent_path();
                if (parent == path)
                    return true;
                path = parent;
            }
        }

        bool ActivePublisher(const auto &record, const std::string_view publisher, const std::uint64_t now) {
            return record.publisher.Value() == publisher && !record.publisherRevoked &&
                   (record.expiresAtUnixMilliseconds == 0 || now < record.expiresAtUnixMilliseconds);
        }

        Result<std::shared_ptr<const Security::ArtifactVerifier>> Verifier(const ExtensionScriptOptions &options) {
            std::error_code canonicalError;
            const auto trustPath = std::filesystem::canonical(options.hostTrust, canonicalError);
            if (canonicalError || !Outside(trustPath, options.packageRoot))
                return Denied<std::shared_ptr<const Security::ArtifactVerifier>>();
            auto bytes = ReadModuleScriptFile(trustPath, MaximumHostSignatureDocumentBytes);
            if (bytes.HasError())
                return Result<std::shared_ptr<const Security::ArtifactVerifier>>::Failure(bytes.ErrorValue());
            auto policy = DecodePublisherTrustDocument(bytes.Value(), Packages::PackagePublisherVerificationPolicy{});
            if (policy.HasError())
                return Result<std::shared_ptr<const Security::ArtifactVerifier>>::Failure(policy.ErrorValue());
            auto roots = std::make_shared<Security::TrustedRootStore>();
            const auto now = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            for (const auto &record : policy.Value().publishers) {
                if (!ActivePublisher(record, options.publisher, now))
                    continue;
                if (auto added = roots->Add({record.algorithm, record.publisher.Value(), record.keyId, record.publicKey}); added.HasError())
                    return Result<std::shared_ptr<const Security::ArtifactVerifier>>::Failure(added.ErrorValue());
            }
            return Result<std::shared_ptr<const Security::ArtifactVerifier>>::Success(
                std::make_shared<Security::ArtifactVerifier>(Security::CreateMbedTlsSignatureProvider(), roots));
        }

        Result<ExtensionScriptArtifact> Capture(const ExtensionScriptOptions &options, const Security::ArtifactVerifier &verifier,
                                                const std::filesystem::path &relative, const std::size_t maximum) {
            auto path = ModuleScriptRelative(options.packageRoot, relative);
            if (path.HasError())
                return Result<ExtensionScriptArtifact>::Failure(path.ErrorValue());
            auto bytes = ReadModuleScriptFile(path.Value(), maximum);
            if (bytes.HasError())
                return Result<ExtensionScriptArtifact>::Failure(bytes.ErrorValue());
            auto signaturePath = ModuleScriptRelative(options.packageRoot, std::filesystem::path{relative.string() + ".signature.json"});
            if (signaturePath.HasError())
                return Result<ExtensionScriptArtifact>::Failure(signaturePath.ErrorValue());
            auto encoded = ReadModuleScriptFile(signaturePath.Value(), MaximumHostSignatureDocumentBytes);
            if (encoded.HasError())
                return Result<ExtensionScriptArtifact>::Failure(encoded.ErrorValue());
            auto envelope = DecodeDetachedSignatureDocument(encoded.Value());
            if (envelope.HasError())
                return Result<ExtensionScriptArtifact>::Failure(envelope.ErrorValue());
            if (envelope.Value().publisherId != options.publisher)
                return Denied<ExtensionScriptArtifact>();
            auto evidence = verifier.Verify(bytes.Value(), envelope.Value());
            if (evidence.HasError())
                return Result<ExtensionScriptArtifact>::Failure(evidence.ErrorValue());
            ExtensionScriptArtifact artifact{relative, std::move(bytes).Value(), std::move(envelope).Value(), std::move(evidence).Value()};
            return Result<ExtensionScriptArtifact>::Success(std::move(artifact));
        }

        bool PresentationOnly(const Extensions::ExtensionModuleManifest &module) {
            return module.roles.size() == 1 && module.roles.front() == Extensions::ExtensionModuleRole::EditorPresentation;
        }

        Result<void> CaptureModules(AuthenticatedExtensionScript &packet, const ExtensionScriptOptions &options,
                                    const CancellationToken &cancellation, const ErrorCodeDescriptor &cancellationError) {
            if (packet.manifest.modules.size() > 16)
                return Denied<void>();
            std::size_t total{};
            std::set<std::string> paths;
            for (const auto &module : packet.manifest.modules) {
                if (PresentationOnly(module))
                    continue;
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(cancellationError));
                if (module.entry.empty() || !module.entries.empty() || !paths.insert(module.entry).second)
                    return Denied<void>();
                auto artifact = Capture(options, *packet.verifier, module.entry, 32U * 1024U * 1024U);
                if (artifact.HasError())
                    return Result<void>::Failure(artifact.ErrorValue());
                if (artifact.Value().bytes.size() > 64U * 1024U * 1024U - total)
                    return Denied<void>();
                total += artifact.Value().bytes.size();
                packet.artifacts.push_back(std::move(artifact).Value());
            }
            return Result<void>::Success();
        }

        Result<std::string> Service(const Extensions::ExtensionManifest &manifest, const ExtensionScriptOptions &options) {
            const auto module = std::ranges::find(manifest.modules, options.module, &Extensions::ExtensionModuleManifest::id);
            if (module == manifest.modules.end())
                return Denied<std::string>();
            if (std::ranges::find(module->roles, Extensions::ExtensionModuleRole::ScriptProvider) == module->roles.end())
                return Denied<std::string>();
            if (options.moduleOperation) {
                auto valid = Extensions::ValidateScriptModuleSelectedDeclaration(*module);
                if (valid.HasError())
                    return Result<std::string>::Failure(valid.ErrorValue());
                return Result<std::string>::Success({});
            }
            const auto import = std::ranges::find(module->imports, options.import, &Extensions::ExtensionServiceImportManifest::id);
            if (import == module->imports.end())
                return Denied<std::string>();
            return Result<std::string>::Success(import->service);
        }
    }  // namespace

    Result<std::shared_ptr<const Security::ArtifactVerifier>> RevalidateModuleScriptPackage(const AuthenticatedExtensionScript &packet) {
        auto current = Verifier(packet.trustOptions);
        if (current.HasError())
            return current;
        for (const auto &artifact : packet.artifacts) {
            auto verified = current.Value()->Verify(artifact.bytes, artifact.signature);
            if (verified.HasError())
                return Result<std::shared_ptr<const Security::ArtifactVerifier>>::Failure(verified.ErrorValue());
        }
        return current;
    }

    Bytes ReadModuleScriptFile(const std::filesystem::path &path, const std::size_t maximum) {
        std::error_code error;
        if (!path.is_absolute() || !std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error)
            return Denied<std::vector<std::byte>>();
        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0 || size > maximum)
            return Denied<std::vector<std::byte>>();
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        std::ifstream input{path, std::ios::binary};
        if (!input.read(std::bit_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
            input.peek() != std::char_traits<char>::eof())
            return Denied<std::vector<std::byte>>();
        return Bytes::Success(std::move(bytes));
    }

    Result<std::filesystem::path> ModuleScriptRelative(const std::filesystem::path &root, const std::filesystem::path &relative) {
        if (relative.empty() || relative.is_absolute() || relative != relative.lexically_normal())
            return Denied<std::filesystem::path>();
        for (const auto &part : relative)
            if (part == "." || part == "..")
                return Denied<std::filesystem::path>();
        std::error_code error;
        const auto path = root / relative;
        if (std::filesystem::canonical(path, error) != path || error)
            return Denied<std::filesystem::path>();
        return Result<std::filesystem::path>::Success(path);
    }

    Result<std::shared_ptr<const AuthenticatedExtensionScript>> AuthenticateModuleScriptPackage(
        const ExtensionScriptOptions &options, const CancellationToken &cancellation, const ErrorCodeDescriptor &cancellationError) {
        using Packet = Result<std::shared_ptr<const AuthenticatedExtensionScript>>;
        auto verifier = Verifier(options);
        if (verifier.HasError())
            return Packet::Failure(verifier.ErrorValue());
        auto manifest = Capture(options, *verifier.Value(), "extension.json", 1024U * 1024U);
        if (manifest.HasError())
            return Packet::Failure(manifest.ErrorValue());
        const auto &bytes = manifest.Value().bytes;
        auto parsed = Extensions::ParseExtensionManifest({std::bit_cast<const char *>(bytes.data()), bytes.size()});
        if (parsed.HasError())
            return Packet::Failure(parsed.ErrorValue());
        if (parsed.Value().id != options.package)
            return Denied<std::shared_ptr<const AuthenticatedExtensionScript>>();
        auto service = Service(parsed.Value(), options);
        if (service.HasError())
            return Packet::Failure(service.ErrorValue());
        auto packet = std::make_shared<AuthenticatedExtensionScript>();
        packet->trustOptions = options;
        packet->manifest = std::move(parsed).Value();
        packet->service = std::move(service).Value();
        packet->verifier = std::move(verifier).Value();
        packet->artifacts.push_back(std::move(manifest).Value());
        if (auto modules = CaptureModules(*packet, options, cancellation, cancellationError); modules.HasError())
            return Packet::Failure(modules.ErrorValue());
        auto script = Capture(options, *packet->verifier, options.script, 64U * 1024U);
        if (script.HasError())
            return Packet::Failure(script.ErrorValue());
        packet->source.assign(std::bit_cast<const char *>(script.Value().bytes.data()), script.Value().bytes.size());
        packet->artifacts.push_back(std::move(script).Value());
        return Packet::Success(std::move(packet));
    }
}  // namespace Horo::Application::Internal
