#include "ShaderHotReloadInternal.h"

#include <limits>

namespace Horo::Application::ShaderHotReloadDetail {
    namespace {
        class Budget final {
        public:
            explicit Budget(std::size_t maximum) : remaining_(maximum) {}

            void Add(std::size_t bytes) noexcept {
                if (bytes > remaining_) {
                    valid_ = false;
                    return;
                }
                remaining_ -= bytes;
            }

            template <class T> void Rows(const std::vector<T> &rows) noexcept {
                if (rows.size() > remaining_ / sizeof(T)) {
                    valid_ = false;
                    return;
                }
                Add(rows.size() * sizeof(T));
            }

            void Product(std::size_t count, std::size_t bytes) noexcept {
                if (bytes != 0 && count > remaining_ / bytes) {
                    valid_ = false;
                    return;
                }
                Add(count * bytes);
            }

            [[nodiscard]] bool Valid() const noexcept {
                return valid_;
            }

        private:
            std::size_t remaining_;
            bool valid_{true};
        };

        void ManifestBytes(Budget &budget, const Render::ShaderManifest &manifest) {
            budget.Add(manifest.sourceIdentity.size());
            budget.Rows(manifest.entryPoints);
            for (const auto &entry : manifest.entryPoints)
                budget.Add(entry.name.size());
            budget.Rows(manifest.bindings);
            budget.Rows(manifest.parameters);
            budget.Rows(manifest.inlineConstants);
            budget.Rows(manifest.specializationInputs);
            budget.Rows(manifest.targets);
        }

        void ReflectionBytes(Budget &budget, const Render::ShaderCompilerReflectionRequest &reflection) {
            budget.Add(reflection.tool.release.size());
            budget.Rows(reflection.names);
            for (const auto &binding : reflection.names) {
                budget.Add(binding.name.size());
                budget.Rows(binding.parameters);
                for (const auto &parameter : binding.parameters)
                    budget.Add(parameter.name.size());
            }
        }

        void TargetBytes(Budget &budget, const Render::ShaderCompilerTargetDescriptor &target) {
            budget.Add(target.platformTriple.size());
            budget.Rows(target.tools);
            for (const auto &tool : target.tools)
                budget.Add(tool.release.size());
        }

        void SourceBytes(Budget &budget, const ShaderBuildRequest &build) {
            const auto &input = build.compilation;
            ManifestBytes(budget, input.manifest);
            budget.Add(input.source.size());
            budget.Rows(input.dependencies);
            for (const auto &dependency : input.dependencies) {
                budget.Add(dependency.logicalPath.size());
                budget.Add(dependency.content.size());
            }
            budget.Rows(input.defines);
            for (const auto &define : input.defines) {
                budget.Add(define.name.size());
                budget.Add(define.value.size());
            }
            budget.Rows(input.targets);
            for (const auto &target : input.targets)
                TargetBytes(budget, target);
            if (input.reflection)
                ReflectionBytes(budget, *input.reflection);
            budget.Rows(build.sources);
            for (const auto &mapping : build.sources) {
                budget.Add(mapping.logicalPath.size());
                budget.Product(mapping.absolutePath.native().size(), sizeof(std::filesystem::path::value_type));
            }
        }

        void OutputBytes(Budget &budget, const ShaderBuildRequest &build) {
            budget.Add(sizeof(Render::ShaderCompilationBatch));
            const auto &limits = build.limits;
            for (const auto &target : build.compilation.targets) {
                budget.Add(sizeof(Render::CompiledShaderArtifact));
                budget.Add(limits.maximumPayloadBytes);
                if (target.emitDebugInformation)
                    budget.Add(limits.maximumDebugPayloadBytes);
                budget.Product(limits.maximumDiagnosticsPerTarget, sizeof(Render::ShaderCompilerDiagnostic));
                budget.Product(limits.maximumDiagnosticsPerTarget, limits.maximumDiagnosticMessageBytes);
                // Source, entry, tool release and native code are independently bounded identities.
                budget.Product(limits.maximumDiagnosticsPerTarget, limits.maximumIdentityBytes);
                budget.Product(limits.maximumDiagnosticsPerTarget, limits.maximumIdentityBytes);
                budget.Product(limits.maximumDiagnosticsPerTarget, limits.maximumIdentityBytes);
                budget.Product(limits.maximumDiagnosticsPerTarget, limits.maximumIdentityBytes);
                // Canonical reflection limits bound its vectors and four source-map identity strings.
                // Eight MiB conservatively covers the complete normalized carrier, not just encoded metadata.
                if (build.compilation.reflection)
                    budget.Add(8U * 1024U * 1024U);
            }
            budget.Rows(build.compilation.dependencies);
            for (const auto &dependency : build.compilation.dependencies)
                budget.Add(dependency.logicalPath.size());
        }

        Result<void> ValidateProgram(const ShaderHotReloadProgram &program, Render::ShaderHotReloadTargetState target) {
            const auto &input = program.build.compilation;
            if (program.id.value == 0 || input.targets.size() != 1)
                return Result<void>::Failure(MakeError(ShaderHotReloadErrors::InvalidRequest));
            if (input.targets.front().requirement.backend != target.backend)
                return Result<void>::Failure(MakeError(ShaderHotReloadErrors::InvalidRequest));
            if (program.build.sources.size() > 257)
                return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Capacity));
            return Render::ValidateShaderCompilationRequest(input, program.build.limits);
        }

        Result<void> ValidatePrograms(const ShaderHotReloadRequest &request, Render::ShaderHotReloadTargetState target) {
            std::uint32_t previous{};
            for (const auto &program : request.programs) {
                if (program.id.value <= previous)
                    return Result<void>::Failure(MakeError(ShaderHotReloadErrors::InvalidRequest));
                previous = program.id.value;
                if (auto valid = ValidateProgram(program, target); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> ValidateConfiguration(const ShaderHotReloadConfiguration &configuration) {
        if (configuration.policy == ShaderHotReloadPolicy::Packaged)
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Packaged));
        if (configuration.policy != ShaderHotReloadPolicy::Development)
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Disabled));
        if (!configuration.context.IsValid())
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::InvalidRequest));
        if (configuration.maximumInputBytes == 0 || configuration.maximumInputBytes > 64U * 1024U * 1024U)
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Capacity));
        if (configuration.maximumCompiledBytes == 0 || configuration.maximumCompiledBytes > 128U * 1024U * 1024U)
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Capacity));
        return Result<void>::Success();
    }

    Result<void> ValidateInput(const ShaderHotReloadRequest &request, const ShaderHotReloadConfiguration &configuration,
                               Render::ShaderHotReloadTargetState target) {
        if (request.fence != configuration.context || target.deviceIncarnation != request.fence.deviceIncarnation)
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Stale));
        if (request.programs.empty() || request.programs.size() > MaximumPrograms || target.liveGeneration == 0)
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::InvalidRequest));
        // Reject the complete ownership envelope before canonical validation can allocate scratch data.
        Budget input(configuration.maximumInputBytes), output(configuration.maximumCompiledBytes);
        output.Add(sizeof(CompiledSet));
        input.Rows(request.programs);
        for (const auto &program : request.programs) {
            SourceBytes(input, program.build);
            OutputBytes(output, program.build);
        }
        if (!input.Valid() || !output.Valid())
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Capacity));
        return ValidatePrograms(request, target);
    }
}  // namespace Horo::Application::ShaderHotReloadDetail
