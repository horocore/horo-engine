#include "ExposureGpuReflectionInternal.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"

#include <algorithm>

namespace Horo::Render::ExposureGpuCookDetail {
    namespace {
        /** @brief Requires a bounded duplicate-free JSON object from actual Cross output. */
        Json Parse(std::string_view source) {
            if (source.empty() || source.size() > ShaderReflectionCookDetail::MaximumMetadataBytes)
                throw EvidenceFailure{__func__, __LINE__};
            Foundation::JsonParseGuard guard{24};
            auto result = guard.Parse(source.begin(), source.end());
            if (guard.HasDuplicate() || guard.IsTooDeep() || result.is_discarded() || !result.is_object())
                throw EvidenceFailure{__func__, __LINE__};
            return result;
        }

        /** @brief Requires the exact fixed local group shape without specialization. */
        void LocalSize(const Json &entry, ExposureGpuStage stage) {
            const auto &size = Rows(entry, "workgroup_size", 3);
            const bool build = stage == ExposureGpuStage::BuildHistogram;
            if (size.size() != 3 || Unsigned(size[0]) != (build ? 8u : 256u) || Unsigned(size[1]) != (build ? 8u : 1u) ||
                Unsigned(size[2]) != 1)
                throw EvidenceFailure{__func__, __LINE__};
            for (const auto key : {"workgroup_size_is_spec_constant", "workgroup_size_is_spec_constant_id"})
                for (const auto &value : Rows(entry, key, 3))
                    if (value != false)
                        throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Rejects foreign entrypoints and validates their actual local-size evidence. */
        void Entry(const Json &json, ExposureGpuStage stage) {
            const auto &entries = Rows(json, "entryPoints", 1);
            if (entries.size() != 1 || entries[0].at("name").get_ref<const std::string &>() != EntryName(stage) ||
                entries[0].at("mode") != "comp")
                throw EvidenceFailure{__func__, __LINE__};
            LocalSize(entries[0], stage);
        }

        /** @brief Proves each actual field's primitive ABI against the independently parsed native declaration. */
        void FieldMatches(const Json &json, const Field &native) {
            if (json.at("name") != native.name || json.at("type") != native.jsonType || Unsigned(json.at("offset")) != native.offset)
                throw EvidenceFailure{__func__, __LINE__};
            for (const auto key : {"array", "array_stride", "array_size_is_literal", "matrix_stride", "row_major", "physical_pointer"})
                if (json.contains(key))
                    throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Requires the complete exact ordered member set. */
        void Fields(const Json &json, const Layout &native) {
            const auto &members = Rows(json, "members", 8);
            if (members.size() != native.fields.size())
                throw EvidenceFailure{__func__, __LINE__};
            for (std::size_t i = 0; i < members.size(); ++i)
                FieldMatches(members[i], native.fields[i]);
        }

        /** @brief Rejects descriptor arrays, indirect resources and noncanonical register spaces. */
        void Resource(const Json &entry, const Buffer &native) {
            if (Unsigned(entry.at("set")) != 0 || Unsigned(entry.at("binding")) != native.slot)
                throw EvidenceFailure{__func__, __LINE__};
            for (const auto key : {"array", "physical_pointer", "buffer_reference"})
                if (entry.contains(key))
                    throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Resolves one exact named resource without accepting duplicate evidence. */
        const Json &Named(const Json &rows, std::string_view name) {
            const Json *found = nullptr;
            for (const auto &row : rows) {
                if (row.at("name").get_ref<const std::string &>() != name)
                    continue;
                if (found)
                    throw EvidenceFailure{__func__, __LINE__};
                found = &row;
            }
            if (!found)
                throw EvidenceFailure{__func__, __LINE__};
            return *found;
        }

        /** @brief Requires storage access to agree with the independently parsed native declaration. */
        void StorageAccess(const Json &entry, const Buffer &native) {
            Resource(entry, native);
            if (Unsigned(entry.at("block_size")) != 0 || entry.value("readonly", false) != native.readonly ||
                entry.value("writeonly", false))
                throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Proves the single unsized array and its exact actual element stride. */
        void RuntimeShape(const Json &runtime, const Buffer &native) {
            const auto &shape = Rows(runtime, "array", 1), &literal = Rows(runtime, "array_size_is_literal", 1);
            if (shape.size() != 1 || literal.size() != 1 || literal[0] != true || Unsigned(shape[0]) != 0)
                throw EvidenceFailure{__func__, __LINE__};
            if (Unsigned(runtime.at("offset")) != 0 || Unsigned(runtime.at("array_stride")) != native.layout.bytes)
                throw EvidenceFailure{__func__, __LINE__};
            if (!native.wrapperMember.empty() && runtime.at("name") != native.wrapperMember)
                throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Proves runtime-array stride and storage layout using actual Cross declarations. */
        void Storage(const Json &json, const Json &entry, const Buffer &native) {
            StorageAccess(entry, native);
            const auto &type = json.at("types").at(entry.at("type").get<std::string>());
            const auto &members = Rows(type, "members", 1);
            if (members.size() != 1)
                throw EvidenceFailure{__func__, __LINE__};
            const auto &runtime = members[0];
            RuntimeShape(runtime, native);
            if (native.layout.fields.empty()) {
                if (runtime.at("type") != native.layout.scalarType)
                    throw EvidenceFailure{__func__, __LINE__};
            } else
                Fields(json.at("types").at(runtime.at("type").get<std::string>()), native.layout);
        }

        /** @brief Validates the complete dispatch ABI and retains actual field offsets in the candidate. */
        void Dispatch(const Json &json, const Json &entry, const Buffer &native, const ShaderReflectionBindingName &names,
                      ShaderReflectionCandidate &candidate) {
            Resource(entry, native);
            if (Unsigned(entry.at("block_size")) != 96)
                throw EvidenceFailure{__func__, __LINE__};
            const auto &type = json.at("types").at(entry.at("type").get<std::string>());
            Fields(type, native.layout);
            const auto &members = Rows(type, "members", 6);
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (members[i].at("name") != names.parameters[i].name)
                    throw EvidenceFailure{__func__, __LINE__};
                const auto scalar = i == 0 || i >= 4 ? ShaderValueType::Uint32 : ShaderValueType::Float32;
                candidate.parameters.push_back(
                    {names.parameters[i].id, {5}, scalar, 1, 4, 1, Unsigned(members[i].at("offset")), 0, 0, true, true});
            }
        }

        /** @brief Proves a real nonsampling two-dimensional float image. */
        void Texture(const Json &entry, const Buffer &native) {
            Resource(entry, native);
            if (entry.at("type") != "texture2D" || entry.value("writeonly", false))
                throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Rejects every unsupported resource category before normalization. */
        void Envelope(const Json &json, std::size_t storage, std::size_t images) {
            constexpr std::array banned{"inputs",
                                        "outputs",
                                        "images",
                                        "separate_samplers",
                                        "textures",
                                        "push_constants",
                                        "subpass_inputs",
                                        "acceleration_structures",
                                        "specialization_constants"};
            for (const auto key : banned)
                if (!Rows(json, key, 0).empty())
                    throw EvidenceFailure{__func__, __LINE__};
            if (!json.at("types").is_object() || json.at("types").size() > 32)
                throw EvidenceFailure{__func__, __LINE__};
            if (Rows(json, "ssbos", 3).size() != storage || Rows(json, "separate_images", 1).size() != images ||
                Rows(json, "ubos", 1).size() != 1)
                throw EvidenceFailure{__func__, __LINE__};
        }
    }  // namespace

    /** @copydoc Normalize */
    ShaderReflectionCandidate Normalize(const ShaderManifest &manifest, const Json &json,
                                        const std::array<std::optional<Buffer>, 5> &native,
                                        std::span<const ShaderReflectionBindingName> names) {
        const auto stage = Stage(manifest);
        Entry(json, stage);
        const auto images = stage == ExposureGpuStage::BuildHistogram ? 1u : 0u;
        Envelope(json, manifest.bindings.size() - images - 1, images);
        ShaderReflectionCandidate candidate{.backend = ShaderTargetBackend::Metal, .interfaceSchemaVersion = 1};
        for (std::size_t i = 0; i < manifest.bindings.size(); ++i) {
            const auto &declared = manifest.bindings[i];
            const auto &actual = *native[declared.id.value - 1];
            candidate.bindings.push_back(
                {declared.id, declared.kind, declared.access, declared.arrayCount, ShaderStageVisibility::Compute, true});
            candidate.targetBindings.push_back({.id = declared.id, .nativeBinding = actual.slot, .nativeName = actual.name});
            if (actual.constant)
                Dispatch(json, Named(json.at("ubos"), names[i].name), actual, names[i], candidate);
            else if (actual.texture)
                Texture(Named(json.at("separate_images"), names[i].name), actual);
            else
                Storage(json, Named(json.at("ssbos"), names[i].name), actual);
        }
        return candidate;
    }
}  // namespace Horo::Render::ExposureGpuCookDetail

namespace Horo::Render {
    namespace {
        /** @brief Validates the exact canonical capture envelope before any borrowed stage is indexed. */
        void ValidateCaptureEnvelope(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                     std::span<const SpirvReflectionStage> stages, std::span<const std::string> translations,
                                     std::span<const ShaderReflectionBindingName> names) {
            if (ValidateExposureGpuMetalReflection(manifest, target, names).HasError() || stages.size() != 1 || translations.size() != 1)
                throw ExposureGpuCookDetail::EvidenceFailure{__func__, __LINE__};
        }
    }  // namespace

    /** @copydoc NormalizeExposureGpuMetalReflection */
    Result<NormalizedShaderReflection> NormalizeExposureGpuMetalReflection(const ShaderManifest &manifest,
                                                                           const ShaderTargetRequirement &target,
                                                                           std::span<const SpirvReflectionStage> stages,
                                                                           std::span<const std::string> translations,
                                                                           std::span<const ShaderReflectionBindingName> names,
                                                                           std::span<const std::uint8_t> validatedSpirv) {
        using namespace ExposureGpuCookDetail;
        using Return = Result<NormalizedShaderReflection>;
        try {
            ValidateCaptureEnvelope(manifest, target, stages, translations, names);
            const auto stage = Stage(manifest);
            if (stages[0].stage != ShaderStage::Compute || stages[0].entryPoint != EntryName(stage))
                throw EvidenceFailure{__func__, __LINE__};
            const auto native = ParseNative(translations[0], stage);
            auto candidate = Normalize(manifest, Parse(stages[0].json), native, names);
            // Only actual native+JSON float2D evidence can resolve a binary Depth=2 classification.
            ProveSpirv(validatedSpirv, stage);
            return NormalizeShaderReflection(manifest, target, std::move(candidate));
        } catch (const EvidenceFailure &failure) {
            return Return::Failure(MakeError(ShaderReflectionErrors::InvalidReflection, EvidenceMessage(failure)));
        } catch (const Json::exception &failure) {
            return Return::Failure(MakeError(ShaderReflectionErrors::InvalidReflection,
                                             std::string{"Exposure Cross JSON evidence: "} + std::string{failure.what()}.substr(0, 256)));
        } catch (const InvalidEvidence &) {
            return Return::Failure(
                MakeError(ShaderReflectionErrors::InvalidReflection, "Exposure shared structure/layout parser rejected evidence."));
        } catch (const MetalReflectionCookDetail::InvalidEvidence &) {
            return Return::Failure(
                MakeError(ShaderReflectionErrors::InvalidReflection, "Exposure native lexical tokenizer rejected evidence."));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
