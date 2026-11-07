#include "LightCullingMetalReflectionInternal.h"

#include <algorithm>

namespace Horo::Render::LightCullingMetalCookDetail {
    namespace {
        struct Expected {
            std::string_view type;
            std::uint32_t offset, count{1}, stride{};
        };

        constexpr std::array LightFields{Expected{"vec4", offsetof(PackedRenderLight, positionRange)},
                                         Expected{"vec3", offsetof(PackedRenderLight, direction)},
                                         Expected{"uint", offsetof(PackedRenderLight, kind)},
                                         Expected{"vec4", offsetof(PackedRenderLight, colorIntensity)},
                                         Expected{"vec2", offsetof(PackedRenderLight, cones)},
                                         Expected{"uvec2", offsetof(PackedRenderLight, identityLow)}};
        constexpr std::array ClusterFields{Expected{"vec4", offsetof(PackedLightCluster, planes), 6, sizeof(std::array<float, 4>)}};
        constexpr std::array DispatchFields{Expected{"uint", offsetof(LightCullingDispatch, lightCount)},
                                            Expected{"uint", offsetof(LightCullingDispatch, clusterCount)},
                                            Expected{"uint", offsetof(LightCullingDispatch, referencesPerCluster)},
                                            Expected{"uint", offsetof(LightCullingDispatch, reserved)}};
        static_assert(offsetof(PackedRenderLight, identityHigh) == offsetof(PackedRenderLight, identityLow) + sizeof(std::uint32_t));

        bool Plain(const Json &field) {
            return !field.contains("matrix_stride") && !field.contains("row_major") && !field.contains("physical_pointer");
        }

        void ArrayShape(const Json &json, const Expected &expected) {
            if (expected.count == 1) {
                if (json.contains("array") || json.contains("array_stride") || json.contains("array_size_is_literal"))
                    throw InvalidEvidence{};
                return;
            }
            const auto &array = Rows(json, "array", 1), &literal = Rows(json, "array_size_is_literal", 1);
            if (array.size() != 1 || literal.size() != 1 || literal[0] != true)
                throw InvalidEvidence{};
            if (Unsigned(array[0]) != expected.count || Unsigned(json.at("array_stride")) != expected.stride)
                throw InvalidEvidence{};
        }

        void FieldMatches(const Json &json, const Field &native, const Expected &expected) {
            if (json.at("name") != native.name || json.at("type").get_ref<const std::string &>() != expected.type ||
                native.jsonType != expected.type)
                throw InvalidEvidence{};
            if (Unsigned(json.at("offset")) != expected.offset || native.offset != expected.offset || !Plain(json))
                throw InvalidEvidence{};
            if (native.count != expected.count || native.stride != expected.stride)
                throw InvalidEvidence{};
            ArrayShape(json, expected);
        }

        void Fields(const Json &json, const Json &members, const Layout &native, std::span<const Expected> expected) {
            if (members.size() != expected.size() || native.fields.size() != expected.size() || !native.scalarType.empty())
                throw InvalidEvidence{};
            for (std::size_t index = 0; index < expected.size(); ++index)
                FieldMatches(members[index], native.fields[index], expected[index]);
            static_cast<void>(json);
        }

        void RuntimeShape(const Json &runtime) {
            const auto &array = Rows(runtime, "array", 1), &literal = Rows(runtime, "array_size_is_literal", 1);
            if (array.size() != 1 || literal.size() != 1 || literal[0] != true || Unsigned(array[0]) != 0)
                throw InvalidEvidence{};
            if (Unsigned(runtime.at("offset")) != 0 || !Plain(runtime))
                throw InvalidEvidence{};
        }

        const Json &Runtime(const Json &json, const Json &entry, const Buffer &native) {
            const auto &body = json.at("types").at(entry.at("type").get<std::string>());
            const auto &members = Rows(body, "members", 1);
            if (members.size() != 1)
                throw InvalidEvidence{};
            const auto &runtime = members[0];
            RuntimeShape(runtime);
            if (Unsigned(runtime.at("array_stride")) != native.layout.bytes)
                throw InvalidEvidence{};
            if (!native.wrapperMember.empty() && runtime.at("name") != native.wrapperMember)
                throw InvalidEvidence{};
            return runtime;
        }

        void RuntimeAccess(const Json &runtime, bool readonly) {
            if (readonly && runtime.value("writeonly", false))
                throw InvalidEvidence{};
            if (!readonly && runtime.value("readonly", false))
                throw InvalidEvidence{};
        }

        void Entry(const Json &entry, bool readonly) {
            if (Unsigned(entry.at("set")) != 0 || Unsigned(entry.at("block_size")) != 0)
                throw InvalidEvidence{};
            if (entry.value("readonly", false) != readonly || (readonly && entry.value("writeonly", false)))
                throw InvalidEvidence{};
            if (entry.contains("array") || entry.contains("physical_pointer") || entry.contains("buffer_reference"))
                throw InvalidEvidence{};
            static_cast<void>(Unsigned(entry.at("binding")));
        }
    }  // namespace

    std::uint32_t Unsigned(const Json &value) {
        if (!value.is_number_unsigned() || value.get<std::uint64_t>() > 0xffffffffULL)
            throw InvalidEvidence{};
        return value.get<std::uint32_t>();
    }

    const Json &Rows(const Json &value, const char *key, std::size_t maximum) {
        static const Json empty = Json::array();
        const auto &rows = value.contains(key) ? value.at(key) : empty;
        if (!rows.is_array() || rows.size() > maximum)
            throw InvalidEvidence{};
        return rows;
    }

    void ValidateStorage(const Json &json, const Json &entry, const Buffer &native, std::size_t index) {
        Entry(entry, index < 2);
        constexpr std::array<std::uint32_t, 4> sizes{sizeof(PackedRenderLight), sizeof(PackedLightCluster), sizeof(PackedLightMembership),
                                                     sizeof(std::uint32_t)};
        if (index >= sizes.size() || native.layout.bytes != sizes[index])
            throw InvalidEvidence{};
        const auto &runtime = Runtime(json, entry, native);
        RuntimeAccess(runtime, index < 2);
        if (index >= 2) {
            const auto expected = index == 2 ? "uvec4" : "uint";
            if (runtime.at("type") != expected || native.layout.scalarType != expected || !native.layout.fields.empty())
                throw InvalidEvidence{};
            return;
        }
        const auto &type = json.at("types").at(runtime.at("type").get<std::string>());
        const auto &members = Rows(type, "members", 8);
        const std::span<const Expected> expected =
            index == 0 ? std::span<const Expected>{LightFields} : std::span<const Expected>{ClusterFields};
        Fields(json, members, native.layout, expected);
    }

    void ValidateDispatch(const Json &json, const Json &entry, const Buffer &native, ShaderReflectionCandidate &candidate,
                          std::span<const ShaderReflectionBindingName> names) {
        if (Unsigned(entry.at("set")) != 0 || Unsigned(entry.at("block_size")) != sizeof(LightCullingDispatch))
            throw InvalidEvidence{};
        if (entry.contains("array") || entry.contains("physical_pointer") || native.layout.bytes != sizeof(LightCullingDispatch))
            throw InvalidEvidence{};
        const auto &type = json.at("types").at(entry.at("type").get<std::string>());
        const auto &members = Rows(type, "members", 4);
        Fields(json, members, native.layout, DispatchFields);
        for (std::size_t index = 0; index < DispatchFields.size(); ++index) {
            const auto &declared = names[4].parameters[index];
            if (members[index].at("name") != declared.name)
                throw InvalidEvidence{};
            candidate.parameters.push_back(
                {declared.id, {5}, ShaderValueType::Uint32, 1, 1, 1, Unsigned(members[index].at("offset")), 0, 0, true, true});
        }
    }
}  // namespace Horo::Render::LightCullingMetalCookDetail
