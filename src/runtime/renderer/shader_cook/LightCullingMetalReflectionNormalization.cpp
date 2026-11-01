#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "LightCullingMetalReflectionInternal.h"

#include <algorithm>

namespace Horo::Render {
    namespace {
        using namespace LightCullingMetalCookDetail;

        std::array<Buffer, 5> NativeEvidence(std::string_view source, std::span<const std::uint8_t> spirv) {
            if (!spirv.empty())
                ProveLightCullingSpirv(spirv);
            return ParseNative(source, !spirv.empty());
        }

        Json Parse(std::string_view source) {
            if (source.empty() || source.size() > ShaderReflectionCookDetail::MaximumMetadataBytes)
                throw InvalidEvidence{};
            Foundation::JsonParseGuard guard{24};
            auto json = guard.Parse(source.begin(), source.end());
            if (guard.HasDuplicate() || guard.IsTooDeep() || json.is_discarded() || !json.is_object())
                throw InvalidEvidence{};
            return json;
        }

        void Specialization(const Json &entry) {
            for (const auto key : {"workgroup_size_is_spec_constant", "workgroup_size_is_spec_constant_id"})
                for (const auto &value : Rows(entry, key, 3))
                    if (value != false)
                        throw InvalidEvidence{};
        }

        void Entry(const Json &json) {
            const auto &entries = Rows(json, "entryPoints", 1);
            if (entries.size() != 1 || entries[0].at("name") != "CullLights" || entries[0].at("mode") != "comp")
                throw InvalidEvidence{};
            const auto &size = Rows(entries[0], "workgroup_size", 3);
            if (size.size() != 3 || Unsigned(size[0]) != 64 || Unsigned(size[1]) != 1 || Unsigned(size[2]) != 1)
                throw InvalidEvidence{};
            Specialization(entries[0]);
        }

        void ResourceEnvelope(const Json &json) {
            constexpr std::array
                banned{"inputs",   "outputs",        "images",         "separate_images",         "separate_samplers",
                       "textures", "push_constants", "subpass_inputs", "acceleration_structures", "specialization_constants"};
            for (const auto key : banned)
                if (!Rows(json, key, 0).empty())
                    throw InvalidEvidence{};
            if (!json.at("types").is_object() || json.at("types").size() > 32)
                throw InvalidEvidence{};
            if (Rows(json, "ssbos", 4).size() != 4 || Rows(json, "ubos", 1).size() != 1)
                throw InvalidEvidence{};
        }

        const Json &Named(const Json &entries, std::string_view name) {
            const Json *result = nullptr;
            for (const auto &entry : entries) {
                if (entry.at("name") != name)
                    continue;
                if (result)
                    throw InvalidEvidence{};
                result = &entry;
            }
            if (!result)
                throw InvalidEvidence{};
            return *result;
        }

        void UniqueBindings(const Json &json) {
            std::array<std::uint32_t, 5> bindings;
            std::size_t count = 0;
            for (const auto key : {"ssbos", "ubos"})
                for (const auto &entry : Rows(json, key, 5)) {
                    const auto binding = Unsigned(entry.at("binding"));
                    if (std::ranges::find(std::span{bindings}.first(count), binding) != std::span{bindings}.first(count).end())
                        throw InvalidEvidence{};
                    bindings[count++] = binding;
                }
        }

        ShaderReflectionCandidate Candidate(const ShaderManifest &manifest, const std::array<Buffer, 5> &native) {
            ShaderReflectionCandidate candidate{.backend = ShaderTargetBackend::Metal, .interfaceSchemaVersion = 1};
            for (std::size_t index = 0; index < manifest.bindings.size(); ++index) {
                const auto &declared = manifest.bindings[index];
                candidate.bindings.push_back(
                    {declared.id, declared.kind, declared.access, declared.arrayCount, ShaderStageVisibility::Compute, true});
                candidate.targetBindings.push_back(
                    {.id = declared.id, .nativeBinding = native[index].slot, .nativeName = native[index].name});
            }
            return candidate;
        }

        ShaderReflectionCandidate Normalize(const ShaderManifest &manifest, const Json &json, const std::array<Buffer, 5> &native,
                                            std::span<const ShaderReflectionBindingName> names) {
            Entry(json);
            ResourceEnvelope(json);
            UniqueBindings(json);
            auto candidate = Candidate(manifest, native);
            for (std::size_t index = 0; index < 4; ++index)
                ValidateStorage(json, Named(json.at("ssbos"), names[index].name), native[index], index);
            ValidateDispatch(json, Named(json.at("ubos"), names[4].name), native[4], candidate, names);
            return candidate;
        }
    }  // namespace

    Result<NormalizedShaderReflection> NormalizeLightCullingMetalReflection(const ShaderManifest &manifest,
                                                                            const ShaderTargetRequirement &target,
                                                                            std::span<const SpirvReflectionStage> stages,
                                                                            std::span<const std::string> translations,
                                                                            std::span<const ShaderReflectionBindingName> names,
                                                                            std::span<const std::uint8_t> validatedSpirv) {
        using Outcome = Result<NormalizedShaderReflection>;
        try {
            if (ValidateLightCullingMetalReflection(manifest, target, names).HasError() || stages.size() != 1 || translations.size() != 1)
                throw InvalidEvidence{};
            if (stages[0].stage != ShaderStage::Compute || stages[0].entryPoint != "CullLights")
                throw InvalidEvidence{};
            const auto native = NativeEvidence(translations[0], validatedSpirv);
            const auto json = Parse(stages[0].json);
            return NormalizeShaderReflection(manifest, target, Normalize(manifest, json, native, names));
        } catch (const Json::exception &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const InvalidEvidence &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const MetalReflectionCookDetail::InvalidEvidence &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
