#include "ScriptModuleProgram.h"

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <array>
#include <bit>

namespace Horo::Extensions {
    namespace {
        Result<void> Invalid(std::string message) {
            return Result<void>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable, std::move(message)));
        }

        bool Relative(const std::filesystem::path &path) {
            if (path.empty() || path.is_absolute() || path != path.lexically_normal())
                return false;
            return std::ranges::none_of(path, [](const auto &part) {
                return part == "." || part == "..";
            });
        }

        Result<void> Shape(const ScriptModuleProgramInput &input) {
            if (!input.currentTrust || !input.root.is_absolute() || !Relative(input.source) || input.artifacts.empty() ||
                input.artifacts.size() > 18)
                return Invalid("Module capture root/gate/artifact bounds are invalid.");
            if (const auto extension = input.runtime == ScriptModuleRuntimeKind::Lua54 ? ".lua" : ".js";
                input.runtime > ScriptModuleRuntimeKind::JavaScriptNg || input.source.extension() != extension)
                return Invalid("Module source runtime and extension disagree.");
            return Result<void>::Success();
        }

        Result<void> ValidateArtifacts(const ScriptModuleProgramInput &input) {
            std::size_t total{};
            for (std::size_t index = 0; index < input.artifacts.size(); ++index) {
                const auto &artifact = input.artifacts[index];
                if (!Relative(artifact.relative) || !artifact.bytes || artifact.bytes->empty() ||
                    artifact.bytes->size() > 32U * 1024U * 1024U)
                    return Invalid("Module artifact is not a bounded immutable capture.");
                if (artifact.bytes->size() > 66U * 1024U * 1024U - total)
                    return Invalid("Module artifact aggregate exceeds its budget.");
                total += artifact.bytes->size();
                if (std::ranges::any_of(std::span{input.artifacts}.first(index), [&](const auto &other) {
                    return other.relative == artifact.relative;
                }))
                    return Invalid("Module artifact paths are duplicated.");
                if (artifact.evidence.PublisherId() != input.artifacts.front().evidence.PublisherId())
                    return Invalid("Module capture mixes publisher identities.");
                if (ComputeSha256(*artifact.bytes) != artifact.evidence.ArtifactDigest())
                    return Invalid("Captured bytes differ from their actual verified evidence.");
            }
            return Result<void>::Success();
        }

        void IdentityText(Sha256Builder &hash, std::string_view text) {
            std::array<std::byte, 8> size{};
            for (unsigned index = 0; index < 8; ++index)
                size[index] = static_cast<std::byte>((static_cast<std::uint64_t>(text.size()) >> (index * 8)) & 255U);
            static_cast<void>(hash.Update(size));
            static_cast<void>(hash.Update(std::as_bytes(std::span{text.data(), text.size()})));
        }

        std::string ScriptIdentity(const ScriptModuleProgramInput &input, const Security::VerifiedArtifactEvidence &source) {
            Sha256Builder hash;
            IdentityText(hash, "horo.module.program.v1");
            IdentityText(hash, input.package);
            IdentityText(hash, input.moduleId);
            IdentityText(hash, input.source.generic_string());
            IdentityText(hash, input.runtime == ScriptModuleRuntimeKind::Lua54 ? "lua" : "javascript");
            static_cast<void>(hash.Update(std::as_bytes(std::span{source.ArtifactDigest().bytes})));
            return "horo.module.source-" + FormatSha256(hash.Finalize()).substr(7);
        }

        const ScriptModuleCapturedArtifact *Find(const ScriptModuleProgramInput &input, const std::filesystem::path &path) {
            const auto found = std::ranges::find(input.artifacts, path, &ScriptModuleCapturedArtifact::relative);
            return found == input.artifacts.end() ? nullptr : &*found;
        }
    }  // namespace

    Result<void> ValidateScriptModuleSelectedDeclaration(const ExtensionModuleManifest &moduleDeclaration) {
        if (std::ranges::find(moduleDeclaration.roles, ExtensionModuleRole::ScriptProvider) == moduleDeclaration.roles.end())
            return Invalid("The signed module does not declare ScriptProvider ownership.");
        if (std::ranges::any_of(moduleDeclaration.imports, [](const auto &serviceImport) {
            return serviceImport.id == "horo.reference";
        }))
            return Invalid("The builtin alias horo.reference is reserved; rename the declared native import.");
        return Result<void>::Success();
    }

    bool SameScriptModuleEvidence(const Security::VerifiedArtifactEvidence &a, const Security::VerifiedArtifactEvidence &b) noexcept {
        return a.ArtifactDigest() == b.ArtifactDigest() && a.PublisherId() == b.PublisherId() && a.KeyId() == b.KeyId();
    }

    Result<void> ScriptModuleProgram::VerifyCurrent() const {
        for (const auto &artifact : input_.artifacts) {
            auto current = input_.currentTrust->Verify(input_.root / artifact.relative);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (!SameScriptModuleEvidence(artifact.evidence, current.Value()))
                return Invalid("Current artifact policy/evidence differs from the frozen signed capture.");
        }
        return Result<void>::Success();
    }

    Result<std::shared_ptr<const ScriptModuleProgram>> ScriptModuleProgram::Capture(ScriptModuleProgramInput input) {
        using Output = Result<std::shared_ptr<const ScriptModuleProgram>>;
        if (auto valid = Shape(input); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        if (auto valid = ValidateArtifacts(input); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        auto program = std::shared_ptr<ScriptModuleProgram>{new ScriptModuleProgram{std::move(input)}};
        if (auto current = program->VerifyCurrent(); current.HasError())
            return Output::Failure(current.ErrorValue());
        if (auto decoded = program->DecodeCapture(); decoded.HasError())
            return Output::Failure(decoded.ErrorValue());
        return Output::Success(std::move(program));
    }

    Result<void> ScriptModuleProgram::SelectModule() {
        if (manifest_.id != input_.package || manifest_.modules.size() > 16)
            return Invalid("Signed module package identity/count is invalid.");
        const auto found = std::ranges::find(manifest_.modules, input_.moduleId, &ExtensionModuleManifest::id);
        if (found == manifest_.modules.end())
            return Invalid("Selected module is absent from the signed manifest.");
        if (auto valid = ValidateScriptModuleSelectedDeclaration(*found); valid.HasError())
            return valid;
        module_ = &*found;
        return Result<void>::Success();
    }

    Result<void> ScriptModuleProgram::DecodeCapture() {
        const auto *manifest = Find(input_, "extension.json");
        const auto *source = Find(input_, input_.source);
        if (!manifest || !source || manifest->bytes->size() > 1024U * 1024U || source->bytes->size() > 64U * 1024U)
            return Invalid("Signed manifest/source capture bounds are invalid.");
        auto decoded = ParseExtensionManifest({std::bit_cast<const char *>(manifest->bytes->data()), manifest->bytes->size()});
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        manifest_ = std::move(decoded).Value();
        if (auto selected = SelectModule(); selected.HasError())
            return selected;
        source_.assign(std::bit_cast<const char *>(source->bytes->data()), source->bytes->size());
        if (source_.find('\0') != std::string::npos || !IsValidUtf8ScalarSequence(source_))
            return Invalid("Module program must be complete UTF-8 text without embedded NUL.");
        scriptId_ = ScriptIdentity(input_, source->evidence);
        return Result<void>::Success();
    }

    bool ScriptModuleProgram::RequestsReference() const noexcept {
        return std::ranges::find(module_->requiredCapabilities, "horo.script.reference") != module_->requiredCapabilities.end();
    }
}  // namespace Horo::Extensions
