#include "EditorAudioPreviewPreparation.h"
#include "Horo/Audio/AudioSourceImporter.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace Horo::Editor::AudioPreviewHostDetail {
    namespace {
        constexpr std::size_t SourceLimit = 16U << 20U;
        constexpr std::size_t PcmLimit = 32U << 20U;

        struct DecodeState final {
            std::ifstream input;
            std::vector<float> samples;
        };

        /** @brief Performs bounded random-access reads on the host/control invocation, never on Audio callbacks. */
        Result<std::size_t> Read(DecodeState &state, const std::uint64_t offset, const std::span<std::byte> bytes) {
            auto &input = state.input;
            input.clear();
            input.seekg(static_cast<std::streamoff>(offset));
            std::array<char, 4096> streamCharacters;
            std::size_t copied{};
            while (copied < bytes.size()) {
                const auto count = std::min(bytes.size() - copied, streamCharacters.size());
                input.read(streamCharacters.data(), static_cast<std::streamsize>(count));
                if (input.bad())
                    return Result<std::size_t>::Failure(MakeError(AudioComponentPreviewErrors::MissingAsset));
                const auto received = static_cast<std::size_t>(input.gcount());
                for (std::size_t index = 0; index < received; ++index)
                    bytes[copied + index] = static_cast<std::byte>(static_cast<unsigned char>(streamCharacters[index]));
                copied += received;
                if (received != count)
                    break;
            }
            return Result<std::size_t>::Success(copied);
        }

        /** @brief Copies bounded importer blocks; partially decoded samples never become an active voice. */
        Result<void> Write(DecodeState &state, const std::uint64_t firstFrame, const Audio::AudioProcessingFormat &format,
                           const std::span<const Audio::AudioSample> samples) {
            if (firstFrame * format.layout.orderedChannels.size() != state.samples.size() ||
                samples.size() > PcmLimit / sizeof(float) - state.samples.size())
                return Result<void>::Failure(MakeError(Audio::AudioErrors::MemoryBudgetExceeded));
            state.samples.insert(state.samples.end(), samples.begin(), samples.end());
            return Result<void>::Success();
        }

        /** @brief Adapts the importer's invocation-only erased callbacks to the concrete owned decode state. */
        Result<Audio::AudioSourceImportCandidate> Import(DecodeState &state, const std::uint64_t bytes) {
            const Audio::AudioSourceReader reader{&state, bytes,
                                                  [](auto *context, const std::uint64_t offset, const std::span<std::byte> destination) {
                return Read(*static_cast<DecodeState *>(context), offset, destination);
            }};
            const Audio::AudioDecodedFrameSink sink{&state, [](auto *context, const std::uint64_t firstFrame,
                                                               const Audio::AudioProcessingFormat &format,
                                                               const std::span<const Audio::AudioSample> samples) {
                return Write(*static_cast<DecodeState *>(context), firstFrame, format, samples);
            }};
            return Audio::ImportCoreAudioSource(reader, sink,
                                                {.maximumSourceBytes = SourceLimit,
                                                 .maximumDecodedFrames = 4U << 20U,
                                                 .maximumDecodedBytes = PcmLimit});
        }

        /** @brief Converts the portable UTF-8 asset path without using the Windows narrow code page. */
        Result<std::filesystem::path> NativeSourcePath(const std::string_view portable) {
            std::u8string utf8;
            utf8.reserve(portable.size());
            for (const char byte : portable)
                utf8.push_back(static_cast<char8_t>(byte));
            try {
                return Result<std::filesystem::path>::Success(std::filesystem::path{utf8});
            } catch (const std::filesystem::filesystem_error &error) {
                return Result<std::filesystem::path>::Failure(MakeError(AudioComponentPreviewErrors::MissingAsset, error.what()));
            }
        }

        /** @brief Resolves symlinks before permitting a project-scoped source read. */
        Result<std::filesystem::path> Resolve(const AudioComponentPreviewRequest &request, const Assets::AssetRecord &record) {
            std::error_code error;
            const auto root = std::filesystem::canonical(request.projectRoot, error);
            if (error)
                return Result<std::filesystem::path>::Failure(MakeError(AudioComponentPreviewErrors::MissingAsset));
            const auto nativeSource = NativeSourcePath(record.sourcePath.String());
            if (nativeSource.HasError())
                return Result<std::filesystem::path>::Failure(nativeSource.ErrorValue());
            const auto source = std::filesystem::canonical(root / nativeSource.Value(), error);
            if (const auto relative = source.lexically_relative(root);
                error || relative.empty() || relative.is_absolute() || *relative.begin() == "..")
                return Result<std::filesystem::path>::Failure(MakeError(AudioComponentPreviewErrors::MissingAsset));
            return Result<std::filesystem::path>::Success(source);
        }
    }  // namespace

    /** @copydoc Decode */
    Result<DecodedSource> Decode(const AudioComponentPreviewRequest &request) {
        if (!request.source.sound.IsAssigned())
            return Result<DecodedSource>::Failure(MakeError(AudioComponentPreviewErrors::MissingAsset));
        const auto *clip = std::get_if<Audio::AudioClipId>(&request.source.sound.target);
        if (request.source.sound.kind != Audio::AudioSoundReferenceKind::Clip || !clip)
            return Result<DecodedSource>::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
        const auto *record = request.assets.Find(clip->Asset());
        if (!record)
            return Result<DecodedSource>::Failure(MakeError(AudioComponentPreviewErrors::MissingAsset));
        auto source = Resolve(request, *record);
        if (source.HasError())
            return Result<DecodedSource>::Failure(source.ErrorValue());
        std::error_code error;
        const auto bytes = std::filesystem::file_size(source.Value(), error);
        if (error || bytes == 0 || bytes > SourceLimit)
            return Result<DecodedSource>::Failure(MakeError(AudioComponentPreviewErrors::MissingAsset));
        DecodeState state{std::ifstream{source.Value(), std::ios::binary}, {}};
        if (!state.input)
            return Result<DecodedSource>::Failure(MakeError(AudioComponentPreviewErrors::MissingAsset));
        auto imported = Import(state, bytes);
        if (imported.HasError())
            return Result<DecodedSource>::Failure(imported.ErrorValue());
        DecodedSource decoded{imported.Value().decodedFormat, {}, static_cast<std::uint32_t>(imported.Value().frameCount)};
        decoded.planes.resize(decoded.format.layout.orderedChannels.size());
        for (std::size_t channel = 0; channel < decoded.planes.size(); ++channel) {
            auto &plane = decoded.planes[channel];
            plane.resize(decoded.frames);
            for (std::size_t frame = 0; frame < decoded.frames; ++frame)
                plane[frame] = state.samples[frame * decoded.planes.size() + channel];
        }
        return Result<DecodedSource>::Success(std::move(decoded));
    }
}  // namespace Horo::Editor::AudioPreviewHostDetail
