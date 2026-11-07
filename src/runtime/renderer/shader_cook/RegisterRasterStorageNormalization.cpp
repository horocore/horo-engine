#include "RegisterRasterReflectionInternal.h"

#include <algorithm>

namespace Horo::Render::RegisterRasterCookDetail {
    namespace {
        using Foundation::Json;

        std::uint32_t Unsigned(const Json &value) {
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > 0xffffffffULL)
                throw InvalidEvidence{};
            return value.get<std::uint32_t>();
        }

        const Json &Rows(const Json &json, const char *key, std::size_t maximum) {
            static const Json empty = Json::array();
            const auto &rows = json.contains(key) ? json.at(key) : empty;
            if (!rows.is_array() || rows.size() > maximum)
                throw InvalidEvidence{};
            return rows;
        }

        void Entry(const Json &entry) {
            if (entry.at("readonly") != true || entry.value("writeonly", false) || Unsigned(entry.at("set")) != 0)
                throw InvalidEvidence{};
            if (entry.contains("array") || entry.contains("physical_pointer") || entry.contains("buffer_reference"))
                throw InvalidEvidence{};
            if (Unsigned(entry.at("block_size")) != 0)
                throw InvalidEvidence{};
            static_cast<void>(Unsigned(entry.at("binding")));
        }

        void RuntimeMemberMetadata(const Json &member) {
            if (!Identifier(member.at("name").get<std::string>()) || member.value("writeonly", false))
                throw InvalidEvidence{};
            if (member.contains("readonly") && member.at("readonly") != true)
                throw InvalidEvidence{};
            if (Unsigned(member.at("offset")) != 0 || member.contains("physical_pointer"))
                throw InvalidEvidence{};
            if (member.contains("matrix_stride") || member.contains("row_major"))
                throw InvalidEvidence{};
        }

        const Json &RuntimeArray(const Json &json, const Json &entry) {
            const auto &block = json.at("types").at(entry.at("type").get<std::string>());
            const auto &members = Rows(block, "members", 1);
            if (members.size() != 1)
                throw InvalidEvidence{};
            const auto &member = members[0];
            const auto &array = Rows(member, "array", 1), &literal = Rows(member, "array_size_is_literal", 1);
            if (array.size() != 1 || literal.size() != 1 || literal[0] != true || Unsigned(array[0]) != 0)
                throw InvalidEvidence{};
            RuntimeMemberMetadata(member);
            return member;
        }

        const ShaderResourceBinding &Binding(const ShaderManifest &manifest, std::span<const ShaderReflectionBindingName> names,
                                             const std::string &name) {
            const auto authored = std::ranges::find(names, name, &ShaderReflectionBindingName::name);
            if (authored == names.end())
                throw InvalidEvidence{};
            const auto binding = std::ranges::find(manifest.bindings, authored->id, &ShaderResourceBinding::id);
            if (binding == manifest.bindings.end() || binding->kind != ShaderResourceKind::StorageBuffer)
                throw InvalidEvidence{};
            return *binding;
        }

        std::size_t ParameterIndex(const ShaderManifest &manifest, ShaderBindingId binding, ShaderParameterId id) {
            std::size_t index{};
            for (const auto &parameter : manifest.parameters) {
                if (parameter.binding != binding)
                    continue;
                if (parameter.id == id)
                    return index;
                ++index;
            }
            throw InvalidEvidence{};
        }

        bool PlainField(const Json &member) {
            return !member.contains("array") && !member.contains("array_stride") && !member.contains("array_size_is_literal") &&
                   !member.contains("matrix_stride") && !member.contains("row_major") && !member.contains("physical_pointer");
        }

        std::string_view JsonType(const StorageElementField &field) {
            if (field.type == ShaderValueType::Uint32)
                return "uint";
            switch (field.columns) {
                case 2:
                    return "vec2";
                case 3:
                    return "vec3";
                case 4:
                    return "vec4";
                default:
                    throw InvalidEvidence{};
            }
        }

        void RetainParameter(ShaderReflectionCandidate &candidate, const ShaderParameter &declared, std::uint32_t offset) {
            const auto retained = std::ranges::find(candidate.parameters, declared.id, &ShaderReflectedParameter::id);
            if (retained == candidate.parameters.end())
                throw InvalidEvidence{};
            if (retained->active && retained->byteOffset != offset)
                throw InvalidEvidence{};
            *retained =
                {declared.id, declared.binding, declared.type, declared.rows, declared.columns, declared.arrayCount, offset, 0, 0, true,
                 true};
        }

        const ShaderParameter &Declared(const ShaderManifest &manifest, const ShaderReflectionBindingName &names, const std::string &name) {
            const auto named = std::ranges::find(names.parameters, name, &ShaderReflectionParameterName::name);
            if (named == names.parameters.end())
                throw InvalidEvidence{};
            const auto declared = std::ranges::find(manifest.parameters, named->id, &ShaderParameter::id);
            if (declared == manifest.parameters.end() || declared->binding != names.id)
                throw InvalidEvidence{};
            return *declared;
        }

        void Field(const Json &member, const NativeStorageMember &native, const StorageElementField &expected) {
            if (member.at("name") != native.name || member.at("type").get_ref<const std::string &>() != JsonType(expected) ||
                !PlainField(member))
                throw InvalidEvidence{};
            if (native.type != expected.type || native.columns != expected.columns)
                throw InvalidEvidence{};
            if (Unsigned(member.at("offset")) != expected.byteOffset || native.byteOffset != expected.byteOffset)
                throw InvalidEvidence{};
        }

        void Members(const Json &json, const Json &runtime, const NativeStorage &native, const ShaderManifest &manifest,
                     const ShaderReflectionBindingName &names, StorageElementKind kind, ShaderReflectionCandidate &candidate) {
            const auto fields = StorageFields(kind);
            const auto &type = json.at("types").at(runtime.at("type").get<std::string>());
            const auto &members = Rows(type, "members", fields.size());
            if (native.scalarUint || members.size() != fields.size() || native.members.size() != fields.size())
                throw InvalidEvidence{};
            for (std::size_t index = 0; index < members.size(); ++index) {
                const auto &declared = Declared(manifest, names, members[index].at("name").get<std::string>());
                const auto parameterIndex = ParameterIndex(manifest, names.id, declared.id);
                if (parameterIndex >= fields.size())
                    throw InvalidEvidence{};
                Field(members[index], native.members[index], fields[parameterIndex]);
                RetainParameter(candidate, declared, fields[parameterIndex].byteOffset);
            }
        }

        void Scalar(const Json &runtime, const NativeStorage &native, const ShaderManifest &manifest,
                    const ShaderReflectionBindingName &names, ShaderReflectionCandidate &candidate) {
            if (!native.scalarUint || runtime.at("type") != "uint" || names.parameters.size() != 1)
                throw InvalidEvidence{};
            const auto &declared = Declared(manifest, names, runtime.at("name").get<std::string>());
            RetainParameter(candidate, declared, 0);
        }

        void Activate(const ShaderManifest &manifest, const ShaderResourceBinding &declared, const NativeStorage &native,
                      ShaderStageVisibility visibility, ShaderReflectionCandidate &candidate) {
            const auto index = static_cast<std::size_t>(&declared - manifest.bindings.data());
            auto &binding = candidate.bindings[index];
            auto &mapping = candidate.targetBindings[index];
            if (mapping.active && (mapping.nativeName != native.name || mapping.nativeBinding != native.binding))
                throw InvalidEvidence{};
            const auto previous = binding.active ? binding.stages : ShaderStageVisibility::None;
            binding.active = true;
            binding.stages = static_cast<ShaderStageVisibility>(static_cast<unsigned>(previous) | static_cast<unsigned>(visibility));
            mapping = {.id = binding.id, .nativeBinding = native.binding, .nativeName = native.name};
        }

        void Storage(const Json &json, const Json &entry, const NativeStage &native, const ShaderManifest &manifest,
                     std::span<const ShaderReflectionBindingName> names, ShaderStageVisibility visibility,
                     ShaderReflectionCandidate &candidate) {
            Entry(entry);
            const auto name = entry.at("name").get<std::string>();
            const auto &binding = Binding(manifest, names, name);
            const auto found = std::ranges::find(native.storage, name, &NativeStorage::name);
            if (found == native.storage.end())
                throw InvalidEvidence{};
            const auto &runtime = RuntimeArray(json, entry);
            const auto kind = StorageKind(manifest, binding.id);
            if (!kind || Unsigned(runtime.at("array_stride")) != StorageStride(*kind) || found->elementStride != StorageStride(*kind))
                throw InvalidEvidence{};
            if (!found->wrapperMember.empty() && runtime.at("name") != found->wrapperMember)
                throw InvalidEvidence{};
            const auto &authored = *FindName(names, binding.id);
            if (*kind == StorageElementKind::Reference)
                Scalar(runtime, *found, manifest, authored, candidate);
            else
                Members(json, runtime, *found, manifest, authored, *kind, candidate);
            Activate(manifest, binding, *found, visibility, candidate);
        }

        void UniqueEntries(const Json &entries) {
            for (std::size_t index = 0; index < entries.size(); ++index)
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (entries[index].at("name") == entries[previous].at("name") ||
                        entries[index].at("binding") == entries[previous].at("binding"))
                        throw InvalidEvidence{};
        }
    }  // namespace

    void NormalizeRegisterStorage(const Json &json, const NativeStage &native, const ShaderManifest &manifest,
                                  std::span<const ShaderReflectionBindingName> names, ShaderStageVisibility visibility,
                                  ShaderReflectionCandidate &candidate) {
        const auto &entries = Rows(json, "ssbos", MaximumStorageBuffers);
        if (entries.size() != native.storage.size())
            throw InvalidEvidence{};
        if (!entries.empty() && candidate.backend != ShaderTargetBackend::Metal)
            throw InvalidEvidence{};
        UniqueEntries(entries);
        for (const auto &entry : entries)
            Storage(json, entry, native, manifest, names, visibility, candidate);
    }
}  // namespace Horo::Render::RegisterRasterCookDetail
