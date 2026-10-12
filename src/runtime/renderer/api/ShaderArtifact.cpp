#include "Horo/Runtime/Render/ShaderArtifact.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>

namespace Horo::Render {
    namespace {
        constexpr std::array<std::uint8_t, 8> Magic{'H', 'O', 'R', 'O', 'S', 'H', 'D', 'R'};
        constexpr std::uint32_t Version = 1;
        constexpr std::size_t HeaderBytes = 18;
        constexpr std::size_t RecordBytes = 13;
        constexpr std::size_t MaximumPackageBytes = 64U * 1024U * 1024U;
        constexpr std::size_t MaximumEntryBytes = 128;

        /** @brief Accepts only explicit family/format pairs understood by the version-one codec. */
        [[nodiscard]] bool TargetValid(const ShaderTargetBackend backend, const ShaderPayloadFormat format) noexcept {
            using enum ShaderTargetBackend;
            using enum ShaderPayloadFormat;
            switch (backend) {
                case Null:
                    return format == ValidationFixture;
                case OpenGL:
                    return format == Glsl410;
                case Vulkan:
                    return format == SpirV16;
                case Metal:
                    return format == MetalLibrary24;
                case D3D12:
                    return format == Dxil60;
            }
            return false;
        }

        /** @brief Bounds names and accepts canonical portable entry-point identifiers. */
        [[nodiscard]] bool EntryValid(const std::string_view name) noexcept {
            if (name.empty() || name.size() > MaximumEntryBytes)
                return false;
            const auto letter = [](const char value) {
                return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
            };
            return letter(name.front()) && std::ranges::all_of(name, [&](const char value) {
                return letter(value) || (value >= '0' && value <= '9');
            });
        }

        /** @brief Writes the fixed little-endian wire representation. */
        template <typename T> void Append(std::vector<std::uint8_t> &output, const T value) {
            static_assert(std::is_unsigned_v<T>);
            for (std::size_t index = 0; index < sizeof(T); ++index)
                output.push_back(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
        }

        /** @brief Checked allocation-free cursor over untrusted package bytes. */
        class Cursor final {
        public:
            explicit Cursor(const std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

            template <typename T> bool Read(T &value) noexcept {
                static_assert(std::is_unsigned_v<T>);
                if (sizeof(T) > bytes_.size())
                    return false;
                value = 0;
                for (std::size_t index = 0; index < sizeof(T); ++index)
                    value |= static_cast<T>(static_cast<T>(bytes_[index]) << (index * 8U));
                bytes_ = bytes_.subspan(sizeof(T));
                return true;
            }

            bool Take(const std::uint64_t count, std::span<const std::uint8_t> &out) noexcept {
                if (count > bytes_.size())
                    return false;
                out = bytes_.first(static_cast<std::size_t>(count));
                bytes_ = bytes_.subspan(static_cast<std::size_t>(count));
                return true;
            }

            [[nodiscard]] bool Empty() const noexcept {
                return bytes_.empty();
            }

        private:
            std::span<const std::uint8_t> bytes_;
        };

        /** @brief Rejects malformed stages and duplicate stage/entry identities. */
        [[nodiscard]] bool RecordValid(const ShaderArtifactStageView &record,
                                       const std::span<const ShaderArtifactStageView> previous) noexcept {
            return static_cast<std::uint8_t>(record.stage) <= static_cast<std::uint8_t>(ShaderStage::Compute) &&
                   EntryValid(record.entryPoint) && !record.bytes.empty() &&
                   std::ranges::none_of(previous, [&](const ShaderArtifactStageView &other) {
                return other.stage == record.stage && other.entryPoint == record.entryPoint;
            });
        }
    }  // namespace

    /** @copydoc DecodeShaderArtifactStages */
    Result<ShaderArtifactPackageView> DecodeShaderArtifactStages(const std::span<const std::uint8_t> bytes,
                                                                 const ShaderTargetBackend backend, const ShaderPayloadFormat format,
                                                                 const std::size_t maximumBytes) {
        const auto failure = [] {
            return Result<ShaderArtifactPackageView>::Failure(
                MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid,
                          "Cooked shader stage package is malformed or targets a different backend."));
        };
        if (maximumBytes < HeaderBytes || maximumBytes > MaximumPackageBytes || bytes.size() < HeaderBytes || bytes.size() > maximumBytes ||
            !TargetValid(backend, format) || !std::ranges::equal(bytes.first(Magic.size()), Magic))
            return failure();
        Cursor cursor{bytes.subspan(Magic.size())};
        std::uint32_t version = 0;
        std::uint8_t family = 0;
        std::uint8_t encoding = 0;
        std::uint32_t count = 0;
        if (!cursor.Read(version) || !cursor.Read(family) || !cursor.Read(encoding) || !cursor.Read(count) || version != Version ||
            family != static_cast<std::uint8_t>(backend) || encoding != static_cast<std::uint8_t>(format) || count == 0 ||
            count > ShaderArtifactPackageView::MaximumStages)
            return failure();
        ShaderArtifactPackageView result;
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint8_t stage = 0;
            std::uint32_t nameBytes = 0;
            std::uint64_t codeBytes = 0;
            std::span<const std::uint8_t> name;
            std::span<const std::uint8_t> code;
            if (!cursor.Read(stage) || !cursor.Read(nameBytes) || nameBytes == 0 || nameBytes > MaximumEntryBytes ||
                !cursor.Take(nameBytes, name) || !cursor.Read(codeBytes) || !cursor.Take(codeBytes, code))
                return failure();
            ShaderArtifactStageView record{static_cast<ShaderStage>(stage),
                                           {reinterpret_cast<const char *>(name.data()), name.size()},
                                           code};
            if (!RecordValid(record, std::span{result.stages}.first(result.stageCount)))
                return failure();
            result.stages[result.stageCount++] = record;
        }
        if (!cursor.Empty())
            return failure();
        return Result<ShaderArtifactPackageView>::Success(result);
    }

    /** @copydoc PackageShaderArtifactStages */
    Result<std::vector<std::uint8_t>> PackageShaderArtifactStages(const ShaderTargetBackend backend, const ShaderPayloadFormat format,
                                                                  const std::span<const ShaderArtifactStage> stages,
                                                                  const std::size_t maximumBytes) {
        const auto failure = [] {
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
        };
        if (!TargetValid(backend, format) || maximumBytes < HeaderBytes || maximumBytes > MaximumPackageBytes || stages.empty() ||
            stages.size() > ShaderArtifactPackageView::MaximumStages)
            return failure();
        std::array<ShaderArtifactStageView, ShaderArtifactPackageView::MaximumStages> views{};
        std::size_t required = HeaderBytes;
        for (std::size_t index = 0; index < stages.size(); ++index) {
            const auto &stage = stages[index];
            views[index] = {stage.stage, stage.entryPoint, stage.bytes};
            if (!RecordValid(views[index], std::span{views}.first(index)) || RecordBytes > maximumBytes - required)
                return failure();
            required += RecordBytes;
            if (stage.entryPoint.size() > maximumBytes - required)
                return failure();
            required += stage.entryPoint.size();
            if (stage.bytes.size() > maximumBytes - required)
                return failure();
            required += stage.bytes.size();
        }
        try {
            std::vector<std::uint8_t> output;
            output.reserve(required);
            output.insert(output.end(), Magic.begin(), Magic.end());
            Append(output, Version);
            Append(output, static_cast<std::uint8_t>(backend));
            Append(output, static_cast<std::uint8_t>(format));
            Append(output, static_cast<std::uint32_t>(stages.size()));
            for (const auto &stage : stages) {
                Append(output, static_cast<std::uint8_t>(stage.stage));
                Append(output, static_cast<std::uint32_t>(stage.entryPoint.size()));
                output.insert(output.end(), stage.entryPoint.begin(), stage.entryPoint.end());
                Append(output, static_cast<std::uint64_t>(stage.bytes.size()));
                output.insert(output.end(), stage.bytes.begin(), stage.bytes.end());
            }
            return Result<std::vector<std::uint8_t>>::Success(std::move(output));
        } catch (const std::bad_alloc &) {
            return failure();
        }
    }
}  // namespace Horo::Render
