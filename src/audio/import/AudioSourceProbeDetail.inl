struct WaveProbeState final {
    SourceProbe probe{.container = AudioContainerIds::Wave, .codec = AudioCodecIds::Pcm};
    std::uint16_t blockAlign{};
    std::optional<std::uint64_t> dataBytes;
};

struct WaveEncoding final {
    std::uint16_t tag{};
    std::uint16_t significantBits{};
};

[[nodiscard]] Result<WaveEncoding> ReadWaveEncoding(const std::span<const std::byte> bytes, const std::uint64_t byteCount) {
    WaveEncoding encoding{ReadLittle<std::uint16_t>(bytes), ReadLittle<std::uint16_t>(bytes.subspan(14))};
    if (encoding.tag != 0xFFFEU)
        return Result<WaveEncoding>::Success(encoding);
    if (byteCount < 40 || ReadLittle<std::uint16_t>(bytes.subspan(16)) < 22)
        return Result<WaveEncoding>::Failure(MakeImportError(AudioErrors::SourceInvalid));
    encoding.significantBits = ReadLittle<std::uint16_t>(bytes.subspan(18));
    encoding.tag = ReadLittle<std::uint16_t>(bytes.subspan(24));
    return Result<WaveEncoding>::Success(encoding);
}

[[nodiscard]] bool SupportedWaveShape(const WaveEncoding &encoding, const std::uint16_t channels, const std::uint16_t blockAlign,
                                      const std::uint32_t maximumChannels) noexcept {
    const bool supportedEncoding = encoding.tag == 1 || encoding.tag == 3;
    const bool validChannels = channels > 0 && channels <= maximumChannels;
    return supportedEncoding && validChannels && blockAlign > 0 && blockAlign % channels == 0;
}

[[nodiscard]] bool SupportedPcmFormat(const AudioPcmFormat &pcm, const std::uint16_t bytesPerSample,
                                      const std::uint16_t significantBits) noexcept {
    return bytesPerSample <= std::numeric_limits<std::uint8_t>::max() && significantBits <= std::numeric_limits<std::uint8_t>::max() &&
           ValidateAudioPcmFormat(pcm);
}

[[nodiscard]] Result<AudioPcmFormat> ParseWaveFormat(ReaderState &reader, const std::uint64_t offset, const std::uint64_t byteCount,
                                                     std::uint16_t &blockAlign) {
    if (byteCount < WaveFormatMinimumBytes || byteCount > 64)
        return Result<AudioPcmFormat>::Failure(MakeImportError(AudioErrors::SourceInvalid));
    std::array<std::byte, 64> bytes{};
    if (const auto read = ReadExact(reader, offset, std::span{bytes}.first(static_cast<std::size_t>(byteCount))); read.HasError())
        return Result<AudioPcmFormat>::Failure(read.ErrorValue());
    const auto channels = ReadLittle<std::uint16_t>(std::span{bytes}.subspan(2));
    blockAlign = ReadLittle<std::uint16_t>(std::span{bytes}.subspan(12));
    auto encoding = ReadWaveEncoding(bytes, byteCount);
    if (encoding.HasError())
        return Result<AudioPcmFormat>::Failure(encoding.ErrorValue());
    const auto encodingValue = std::move(encoding).Value();
    if (!SupportedWaveShape(encodingValue, channels, blockAlign, reader.limits.maximumChannels))
        return Result<AudioPcmFormat>::Failure(MakeImportError(AudioErrors::SourceUnsupported));
    const auto bytesPerSample = static_cast<std::uint16_t>(blockAlign / channels);
    const AudioPcmFormat pcm{.encoding = encodingValue.tag == 3               ? AudioPcmEncoding::IeeeFloat
                                         : encodingValue.significantBits == 8 ? AudioPcmEncoding::UnsignedInteger
                                                                              : AudioPcmEncoding::SignedInteger,
                             .packing = AudioPcmPacking::Interleaved,
                             .byteOrder = AudioByteOrder::LittleEndian,
                             .bytesPerSample = static_cast<std::uint8_t>(bytesPerSample),
                             .significantBits = static_cast<std::uint8_t>(encodingValue.significantBits)};
    if (!SupportedPcmFormat(pcm, bytesPerSample, encodingValue.significantBits))
        return Result<AudioPcmFormat>::Failure(MakeImportError(AudioErrors::SourceUnsupported));
    return Result<AudioPcmFormat>::Success(pcm);
}

[[nodiscard]] Result<std::vector<AudioLoopRegion>> ParseWaveLoops(ReaderState &reader, const std::uint64_t offset,
                                                                  const std::uint64_t byteCount) {
    if (byteCount < 36)
        return Result<std::vector<AudioLoopRegion>>::Failure(MakeImportError(AudioErrors::SourceInvalid));
    std::array<std::byte, 36> header{};
    if (const auto read = ReadExact(reader, offset, header); read.HasError())
        return Result<std::vector<AudioLoopRegion>>::Failure(read.ErrorValue());
    const auto loopCount = ReadLittle<std::uint32_t>(std::span{header}.subspan(28));
    std::uint64_t loopBytes{};
    if (loopCount > reader.limits.maximumLoopRegions || !CheckedMultiply(loopCount, 24, loopBytes) || loopBytes > byteCount - 36)
        return Result<std::vector<AudioLoopRegion>>::Failure(MakeImportError(AudioErrors::SourceLimitExceeded));
    std::vector<AudioLoopRegion> loops;
    loops.reserve(loopCount);
    std::array<std::byte, 24> loop{};
    for (std::uint32_t index = 0; index < loopCount; ++index) {
        if (const auto read = ReadExact(reader, offset + 36 + index * loop.size(), loop); read.HasError())
            return Result<std::vector<AudioLoopRegion>>::Failure(read.ErrorValue());
        const auto start = ReadLittle<std::uint32_t>(std::span{loop}.subspan(8));
        const auto inclusiveEnd = ReadLittle<std::uint32_t>(std::span{loop}.subspan(12));
        if (inclusiveEnd < start || inclusiveEnd == std::numeric_limits<std::uint32_t>::max())
            return Result<std::vector<AudioLoopRegion>>::Failure(MakeImportError(AudioErrors::SourceInvalid));
        loops.push_back({AudioAssetElementId{index + 1}, start, static_cast<std::uint64_t>(inclusiveEnd) + 1});
    }
    return Result<std::vector<AudioLoopRegion>>::Success(std::move(loops));
}

[[nodiscard]] Result<void> ParseWaveChunk(ReaderState &reader, const std::span<const std::byte> header, const std::uint64_t offset,
                                          const std::uint64_t byteCount, WaveProbeState &state) {
    if (EqualFour(header, "fmt ")) {
        if (state.probe.pcm)
            return Result<void>::Failure(MakeImportError(AudioErrors::SourceInvalid));
        auto format = ParseWaveFormat(reader, offset, byteCount, state.blockAlign);
        if (format.HasError())
            return Result<void>::Failure(format.ErrorValue());
        state.probe.pcm = std::move(format).Value();
    } else if (EqualFour(header, "data")) {
        if (state.dataBytes || byteCount == 0)
            return Result<void>::Failure(MakeImportError(AudioErrors::SourceInvalid));
        state.dataBytes = byteCount;
    } else if (EqualFour(header, "smpl")) {
        if (!state.probe.loops.empty())
            return Result<void>::Failure(MakeImportError(AudioErrors::SourceInvalid));
        auto loops = ParseWaveLoops(reader, offset, byteCount);
        if (loops.HasError())
            return Result<void>::Failure(loops.ErrorValue());
        state.probe.loops = std::move(loops).Value();
    }
    return Result<void>::Success();
}

struct WaveChunk final {
    std::array<std::byte, ChunkHeaderBytes> header{};
    std::uint64_t payloadOffset{};
    std::uint64_t byteCount{};
    std::uint64_t nextOffset{};
};

[[nodiscard]] Result<WaveChunk> ReadWaveChunk(ReaderState &reader, const std::uint64_t cursor, const std::uint64_t riffEnd) {
    if (riffEnd - cursor < ChunkHeaderBytes)
        return Result<WaveChunk>::Failure(MakeImportError(AudioErrors::SourceInvalid));
    WaveChunk chunk;
    if (const auto read = ReadExact(reader, cursor, chunk.header); read.HasError())
        return Result<WaveChunk>::Failure(read.ErrorValue());
    chunk.byteCount = ReadLittle<std::uint32_t>(std::span{chunk.header}.subspan(4));
    std::uint64_t payloadEnd{};
    if (!CheckedAdd(cursor, ChunkHeaderBytes, chunk.payloadOffset) || !CheckedAdd(chunk.payloadOffset, chunk.byteCount, payloadEnd) ||
        payloadEnd > riffEnd || !CheckedAdd(payloadEnd, chunk.byteCount & 1U, chunk.nextOffset) || chunk.nextOffset > riffEnd)
        return Result<WaveChunk>::Failure(MakeImportError(AudioErrors::SourceInvalid));
    return Result<WaveChunk>::Success(chunk);
}

[[nodiscard]] Result<std::uint64_t> ReadWaveEnd(ReaderState &reader) {
    std::array<std::byte, RiffHeaderBytes> header{};
    if (const auto read = ReadExact(reader, 0, header); read.HasError())
        return Result<std::uint64_t>::Failure(read.ErrorValue());
    if (!EqualFour(header, "RIFF") || !EqualFour(std::span{header}.subspan(8), "WAVE"))
        return Result<std::uint64_t>::Failure(MakeImportError(AudioErrors::SourceUnsupported));
    std::uint64_t riffEnd{};
    if (!CheckedAdd(ReadLittle<std::uint32_t>(std::span{header}.subspan(4, 4)), 8, riffEnd) || riffEnd < RiffHeaderBytes ||
        riffEnd > reader.source.size)
        return Result<std::uint64_t>::Failure(MakeImportError(AudioErrors::SourceInvalid));
    return Result<std::uint64_t>::Success(riffEnd);
}

[[nodiscard]] Result<SourceProbe> ProbeWave(ReaderState &reader) {
    auto end = ReadWaveEnd(reader);
    if (end.HasError())
        return Result<SourceProbe>::Failure(end.ErrorValue());
    const auto riffEnd = end.Value();
    WaveProbeState state;
    std::uint64_t cursor = RiffHeaderBytes;
    while (cursor < riffEnd) {
        auto chunk = ReadWaveChunk(reader, cursor, riffEnd);
        if (chunk.HasError())
            return Result<SourceProbe>::Failure(chunk.ErrorValue());
        const auto chunkValue = std::move(chunk).Value();
        if (const auto parsed = ParseWaveChunk(reader, chunkValue.header, chunkValue.payloadOffset, chunkValue.byteCount, state);
            parsed.HasError())
            return Result<SourceProbe>::Failure(parsed.ErrorValue());
        cursor = chunkValue.nextOffset;
    }
    if (!state.probe.pcm || !state.dataBytes || *state.dataBytes % state.blockAlign != 0)
        return Result<SourceProbe>::Failure(MakeImportError(AudioErrors::SourceInvalid));
    return Result<SourceProbe>::Success(std::move(state.probe));
}

[[nodiscard]] Result<std::size_t> ReadVorbisPacketBytes(ReaderState &reader, const std::span<const std::byte> pageHeader) {
    const auto segmentCount = std::to_integer<std::uint8_t>(pageHeader[26]);
    if (segmentCount == 0)
        return Result<std::size_t>::Failure(MakeImportError(AudioErrors::SourceInvalid));
    std::array<std::byte, 255> segments{};
    if (const auto read = ReadExact(reader, pageHeader.size(), std::span{segments}.first(segmentCount)); read.HasError())
        return Result<std::size_t>::Failure(read.ErrorValue());
    std::size_t packetBytes{};
    for (std::size_t index = 0; index < segmentCount; ++index) {
        packetBytes += std::to_integer<std::uint8_t>(segments[index]);
        if (segments[index] != std::byte{255})
            return packetBytes >= 30 && packetBytes <= 65'025 ? Result<std::size_t>::Success(packetBytes)
                                                              : Result<std::size_t>::Failure(MakeImportError(AudioErrors::SourceInvalid));
    }
    return Result<std::size_t>::Failure(MakeImportError(AudioErrors::SourceInvalid));
}

[[nodiscard]] bool ValidVorbisIdentification(const std::span<const std::byte> packet, const std::uint32_t maximumChannels) noexcept {
    constexpr std::array Signature{std::byte{1},   std::byte{'v'}, std::byte{'o'}, std::byte{'r'},
                                   std::byte{'b'}, std::byte{'i'}, std::byte{'s'}};
    if (!std::ranges::equal(Signature, packet.first(Signature.size())))
        return false;
    const auto channels = std::to_integer<std::uint8_t>(packet[11]);
    const auto sampleRate = ReadLittle<std::uint32_t>(packet.subspan(12));
    return channels > 0 && channels <= maximumChannels && sampleRate >= MinimumAudioSampleRate && sampleRate <= MaximumAudioSampleRate;
}

[[nodiscard]] Result<SourceProbe> ProbeOggVorbis(ReaderState &reader) {
    std::array<std::byte, OggPageHeaderBytes> header{};
    if (const auto read = ReadExact(reader, 0, header); read.HasError())
        return Result<SourceProbe>::Failure(read.ErrorValue());
    if (!EqualFour(header, "OggS") || std::to_integer<std::uint8_t>(header[4]) != 0)
        return Result<SourceProbe>::Failure(MakeImportError(AudioErrors::SourceUnsupported));
    auto packetBytes = ReadVorbisPacketBytes(reader, header);
    if (packetBytes.HasError())
        return Result<SourceProbe>::Failure(packetBytes.ErrorValue());
    std::vector<std::byte> packet(packetBytes.Value());
    const auto segmentCount = std::to_integer<std::uint8_t>(header[26]);
    if (const auto read = ReadExact(reader, header.size() + segmentCount, packet); read.HasError())
        return Result<SourceProbe>::Failure(read.ErrorValue());
    if (!ValidVorbisIdentification(packet, reader.limits.maximumChannels))
        return Result<SourceProbe>::Failure(MakeImportError(AudioErrors::SourceUnsupported));
    return Result<SourceProbe>::Success(
        {.container = AudioContainerIds::Ogg, .codec = AudioCodecIds::Vorbis, .pcm = std::nullopt, .loops = {}});
}
