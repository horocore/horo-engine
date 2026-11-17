#include "Horo/Audio/Internal/PipeWireAudioBackend.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/NativeAudioFacts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <pthread.h>
#include <spa/param/audio/format-utils.h>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Preserve native error numbers in ordinary control diagnostics. */
        Error NativeError(const char *operation, const int status) {
            return MakeError(AudioErrors::BackendFailed, std::string(operation) + " PipeWire status=" + std::to_string(status));
        }

        /** @brief Host monotonic time; independent streams never share a hardware-clock guarantee. */
        std::uint64_t Nanoseconds() noexcept {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        /** @brief Bounded native metadata copy, rejecting truncation rather than aliasing endpoint identity. */
        template <std::size_t Size> bool Copy(std::array<char, Size> &output, const char *input) noexcept {
            if (!input)
                return false;
            const auto length = strnlen(input, Size);
            if (length == Size)
                return false;
            std::memcpy(output.data(), input, length + 1);
            return true;
        }

        /** @brief Complete supported speaker mapping; unknown channel positions fail negotiation. */
        constexpr std::array Positions{SPA_AUDIO_CHANNEL_FL,  SPA_AUDIO_CHANNEL_FR,  SPA_AUDIO_CHANNEL_FC,  SPA_AUDIO_CHANNEL_LFE,
                                       SPA_AUDIO_CHANNEL_SL,  SPA_AUDIO_CHANNEL_SR,  SPA_AUDIO_CHANNEL_RL,  SPA_AUDIO_CHANNEL_RR,
                                       SPA_AUDIO_CHANNEL_TFL, SPA_AUDIO_CHANNEL_TFR, SPA_AUDIO_CHANNEL_TRL, SPA_AUDIO_CHANNEL_TRR};
        constexpr std::array Roles{AudioSpeakerRole::FrontLeft,     AudioSpeakerRole::FrontRight,  AudioSpeakerRole::FrontCenter,
                                   AudioSpeakerRole::LowFrequency,  AudioSpeakerRole::SideLeft,    AudioSpeakerRole::SideRight,
                                   AudioSpeakerRole::BackLeft,      AudioSpeakerRole::BackRight,   AudioSpeakerRole::TopFrontLeft,
                                   AudioSpeakerRole::TopFrontRight, AudioSpeakerRole::TopBackLeft, AudioSpeakerRole::TopBackRight};

        /** @brief Private loop serializes registry, metadata, stream state and bounded PCM callbacks. */
        class PipeWireDriver final : public NativeAudioDriver {
        public:
            PipeWireDriver(const NativeAudioBackendConfig config, std::shared_ptr<PipeWireAudioLibrary> library)
                : config_(config), library_(std::move(library)), facts_(config.owner) {}

            AudioBackendKind Kind() const noexcept override {
                return AudioBackendKind::PipeWire;
            }

            ResourceDisposition Resources() const noexcept override {
                if (stopWorkerOwned_ || (render_.process && running_))
                    return ResourceDisposition::Retained;
                return loop_ ? ResourceDisposition::CallbackDetached : ResourceDisposition::Closed;
            }

            bool HasUndrainedRequiredFacts() const noexcept override {
                return facts_.HasLoss() || HasNativeLoss();
            }

            Result<AudioBackendProbe> Probe() override {
                if (const auto connected = Connect(); connected.HasError())
                    return Result<AudioBackendProbe>::Failure(connected.ErrorValue());
                AudioBackendProbe probe{.backend = Kind(),
                                        .compiled = true,
                                        .hostSupported = true,
                                        .availability = AudioBackendAvailability::Available,
                                        .revision = 1,
                                        .backendVersion = pw_get_library_version()};
                probe.features.fill(AudioCapabilitySupport::Unsupported);
                for (const auto feature : {AudioBackendCapability::PhysicalEnumeration, AudioBackendCapability::NativeHotplug,
                                           AudioBackendCapability::NativeDiagnostics})
                    probe.features[static_cast<std::size_t>(feature)] = AudioCapabilitySupport::Available;
                return Result<AudioBackendProbe>::Success(std::move(probe));
            }

            Result<AudioDeviceSnapshot> Enumerate() override {
                if (const auto connected = Connect(); connected.HasError())
                    return Result<AudioDeviceSnapshot>::Failure(connected.ErrorValue());
                Lock lock(loop_);
                if (!Roundtrip() || overflow_)
                    return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::DeviceUnavailable));
                if (snapshot_.revision == std::numeric_limits<std::uint64_t>::max())
                    return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
                auto registry = registry_;
                for (auto &entry : registry)
                    entry.present = false;
                AudioDeviceSnapshot next{.owner = config_.owner, .backend = Kind(), .revision = snapshot_.revision + 1};
                for (const auto &node : nodes_) {
                    if (!node.present)
                        continue;
                    auto found = std::find_if(registry.begin(), registry.end(), [this, &node](const auto &entry) {
                        return entry.serverCookie == serverCookie_ && entry.serial == node.serial.data();
                    });
                    if (found == registry.end()) {
                        if (registry.size() == 4'096)
                            return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
                        registry.push_back({node.serial.data(),
                                            node.name.data(),
                                            serverCookie_,
                                            {config_.owner, static_cast<std::uint32_t>(registry.size() + 1), 1},
                                            true});
                        found = std::prev(registry.end());
                    } else {
                        const auto old = std::find_if(registry_.begin(), registry_.end(), [this, &node](const auto &entry) {
                            return entry.serverCookie == serverCookie_ && entry.serial == node.serial.data();
                        });
                        if (old != registry_.end() && !old->present) {
                            if (found->id.generation == std::numeric_limits<std::uint32_t>::max())
                                return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
                            ++found->id.generation;
                        }
                        found->name = node.name.data();
                        found->present = true;
                    }
                    next.devices.push_back({found->id, node.description.data(), AudioDeviceClass::Unknown});
                }
                if (const auto name = DefaultDeviceName()) {
                    const auto found = std::find_if(registry.begin(), registry.end(), [&name](const auto &entry) {
                        return entry.present && entry.name == *name;
                    });
                    const auto matches = std::count_if(registry.begin(), registry.end(), [&name](const auto &entry) {
                        return entry.present && entry.name == *name;
                    });
                    if (found != registry.end() && matches == 1)
                        next.defaults.console = next.defaults.multimedia = found->id;
                }
                registry_ = std::move(registry);
                snapshot_ = next;
                return Result<AudioDeviceSnapshot>::Success(std::move(next));
            }

            Result<Opened> Open(const Backend::Open &request) override {
                auto result = Acquire(request);
                if (result.HasError()) {
                    const auto closed = Close();
                    if (closed.HasError())
                        return Result<Opened>::Failure(WithCause(closed.ErrorValue(), result.ErrorValue()));
                }
                return result;
            }

            Result<void> Start(const NativeAudioRenderPort render) override {
                if (!stream_ || !running_ || render_.process || stopWorkerOwned_)
                    return Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
                Lock lock(loop_);
                if (failed_.load())
                    return Result<void>::Failure(MakeError(AudioErrors::DeviceUnavailable));
                render_ = render;
                const auto status = pw_stream_set_active(stream_, true);
                if (status < 0) {
                    render_ = {};
                    return Result<void>::Failure(NativeError("PipeWire start", status));
                }
                return Result<void>::Success();
            }

            NativeRetirementResult Stop(const AudioMonotonicTimestamp deadline) override {
                if (Nanoseconds() >= deadline.nanoseconds)
                    return NativeRetirementResult::Failure(MakeError(AudioErrors::BackendFailed, "PipeWire stop deadline expired"));
                return RetireLoop();
            }

            NativeRetirementResult Close() override {
                if (render_.process && running_)
                    return NativeRetirementResult::Failure(MakeError(AudioErrors::RuntimeInactive));
                if (running_ || stopWorkerOwned_) {
                    auto retired = RetireLoop();
                    if (retired.HasError() || retired.Value() != NativeRetirementProgress::Complete)
                        return retired;
                }
                if (!CaptureNativeFacts())
                    return NativeRetirementResult::Failure(MakeError(AudioErrors::EventQueueInvalid));
                if (stream_) {
                    spa_hook_remove(&streamListener_);
                    pw_stream_destroy(stream_);
                    stream_ = nullptr;
                }
                if (metadata_) {
                    spa_hook_remove(&metadataListener_);
                    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(metadata_));
                    metadata_ = nullptr;
                }
                if (registryObject_) {
                    spa_hook_remove(&registryListener_);
                    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registryObject_));
                    registryObject_ = nullptr;
                }
                if (core_) {
                    spa_hook_remove(&coreListener_);
                    pw_core_disconnect(core_);
                    core_ = nullptr;
                }
                if (context_) {
                    pw_context_destroy(context_);
                    context_ = nullptr;
                }
                if (loop_) {
                    pw_thread_loop_destroy(loop_);
                    loop_ = nullptr;
                }
                // The last published catalog survives an ordinary close. Open revalidates its
                // server/serial against fresh native discovery; closing is not device removal.
                nodes_ = {};
                defaultMetadata_ = {};
                cookieKnown_ = false;
                selectedSerial_.clear();
                render_ = {};
                epoch_ = {};
                return NativeRetirementResult::Success(NativeRetirementProgress::Complete);
            }

            std::size_t DrainEvents(const std::span<Event> output) noexcept override {
                (void)CaptureNativeFacts();
                return facts_.Drain(output);
            }

        private:
            /** @brief Control-only lock protects loop-owned objects and discovery arrays; never acquired inside process. */
            struct Lock final {
                pw_thread_loop *loop;

                explicit Lock(pw_thread_loop *value) : loop(value) {
                    pw_thread_loop_lock(loop);
                }

                ~Lock() {
                    pw_thread_loop_unlock(loop);
                }
            };

            struct Node final {
                std::uint32_t global{};
                bool present{};
                std::array<char, 128> serial{};
                std::array<char, 256> name{};
                std::array<char, 257> description{};
            };

            struct Entry final {
                std::string serial, name;
                std::uint32_t serverCookie{};
                AudioDeviceId id;
                bool present{};
            };

            NativeAudioBackendConfig config_;
            std::shared_ptr<PipeWireAudioLibrary> library_;
            AudioDeviceSnapshot snapshot_;
            std::vector<Entry> registry_;
            std::array<Node, MaximumAudioDiscoveredDevices> nodes_{};
            std::array<char, 1024> defaultMetadata_{};
            pw_thread_loop *loop_{};
            pw_context *context_{};
            pw_core *core_{};
            pw_registry *registryObject_{};
            pw_metadata *metadata_{};
            pw_stream *stream_{};
            spa_hook coreListener_{}, registryListener_{}, metadataListener_{}, streamListener_{};
            NativeAudioRenderPort render_;
            AudioDeviceEpoch epoch_;
            spa_audio_info_raw actual_{};
            pw_stream_state streamState_{PW_STREAM_STATE_UNCONNECTED};
            int syncSequence_{}, completedSequence_{};
            bool running_{}, overflow_{}, formatKnown_{}, stopping_{};
            // Loop-protected server identity scopes node serials; callbacks write, locked control reads.
            std::uint32_t serverCookie_{};
            bool cookieKnown_{};
            // Discovery/state callbacks share the private loop; control observes fault/revision without taking its lock.
            std::atomic<bool> failed_{};
            std::atomic<std::uint64_t> changes_{};
            std::uint64_t drainedChanges_{};
            bool lossDrained_{};
            std::string selectedSerial_;
            NativeAudioFacts facts_;
            NativeAudioLossLatch loss_;

            /** @brief Publish one precise native failure cause without formatting or recovery on the native thread. */
            void RecordFailure(const DeviceLossCause cause) noexcept {
                loss_.Capture(cause);
                failed_.store(true, std::memory_order_release);
            }

            /** @brief Ignore connection-only probe failures once no admitted output epoch remains. */
            bool HasNativeLoss() const noexcept {
                return !lossDrained_ && epoch_.device.IsValid() && failed_.load();
            }

            /** @brief Capture loop/worker facts before reconnect resets native flags or cleanup releases proxies. */
            bool CaptureNativeFacts() noexcept {
                if (HasNativeLoss()) {
                    if (!facts_.CaptureLoss({epoch_.device, loss_.Read().value_or(DeviceLossCause::BackendFailure)}))
                        return false;
                    lossDrained_ = true;
                }
                const auto changes = changes_.load();
                if (changes != drainedChanges_) {
                    facts_.CaptureChange(snapshot_.revision);
                    drainedChanges_ = changes;
                }
                return true;
            }

            // Control owns the pthread handle until a successful nonblocking join. The worker may only
            // retire the loop and publish status; callback/control storage stays alive until that proof.
            pthread_t stopWorker_{};
            bool stopWorkerOwned_{};
            std::atomic<int> stopStatus_{};

            /** @brief Retire the private loop away from control; a stuck native lock/join retains this complete ownership island. */
            static void *StopEntry(void *context) noexcept {
                auto &self = *static_cast<PipeWireDriver *>(context);
                {
                    Lock lock(self.loop_);
                    self.stopping_ = true;
                    if (self.stream_) {
                        const auto status = pw_stream_set_active(self.stream_, false);
                        if (status < 0 && !self.failed_.load()) {
                            self.stopping_ = false;
                            self.stopStatus_.store(status, std::memory_order_release);
                            return nullptr;
                        }
                        (void)pw_stream_disconnect(self.stream_);
                    }
                }
                // PipeWire requires stop outside the loop lock. Return is the native thread's stop proof.
                pw_thread_loop_stop(self.loop_);
                return nullptr;
            }

            /** @brief Start at most one owned retirement worker and poll its actual exit without blocking control. */
            NativeRetirementResult RetireLoop() {
                if (!running_ && !stopWorkerOwned_)
                    return NativeRetirementResult::Success(NativeRetirementProgress::Complete);
                if (!stopWorkerOwned_) {
                    stopStatus_.store(0, std::memory_order_relaxed);
                    const auto created = pthread_create(&stopWorker_, nullptr, StopEntry, this);
                    if (created != 0)
                        return NativeRetirementResult::Failure(NativeError("PipeWire retirement worker", -created));
                    stopWorkerOwned_ = true;
                }
                const auto joined = pthread_tryjoin_np(stopWorker_, nullptr);
                if (joined == EBUSY)
                    return NativeRetirementResult::Success(NativeRetirementProgress::Pending);
                if (joined != 0)
                    return NativeRetirementResult::Failure(NativeError("PipeWire retirement join", -joined));
                stopWorkerOwned_ = false;
                const auto status = stopStatus_.load(std::memory_order_acquire);
                if (status < 0)
                    return NativeRetirementResult::Failure(NativeError("PipeWire deactivate", status));
                running_ = false;
                render_ = {};
                return NativeRetirementResult::Success(NativeRetirementProgress::Complete);
            }

            /** @brief Resolve bounded default metadata on control while the private loop lock protects its snapshot. */
            std::optional<std::string> DefaultDeviceName() const {
                if (!defaultMetadata_[0])
                    return std::nullopt;
                const auto parsed = nlohmann::json::parse(defaultMetadata_.data(), nullptr, false);
                if (!parsed.is_object() || !parsed.contains("name") || !parsed["name"].is_string())
                    return std::nullopt;
                return parsed["name"].get<std::string>();
            }

            /** @brief A default name must identify exactly one live node; duplicate presentation names never select by order. */
            bool MatchesCurrentDefault(const Node &selected) const {
                const auto name = DefaultDeviceName();
                return name && *name == selected.name.data() && std::count_if(nodes_.begin(), nodes_.end(), [&name](const auto &node) {
                    return node.present && *name == node.name.data();
                }) == 1;
            }

            /** @brief Complete an asynchronous registry/metadata roundtrip while releasing the loop lock during waits. */
            bool Roundtrip() {
                syncSequence_ = pw_core_sync(core_, PW_ID_CORE, 0);
                timespec expires{};
                pw_thread_loop_get_time(loop_, &expires, 1'000'000'000);
                while (!failed_.load() && completedSequence_ != syncSequence_)
                    if (pw_thread_loop_timed_wait_full(loop_, &expires) < 0)
                        return false;
                return !failed_.load();
            }

            /** @brief Explicit per-owner service connection; no global backend registration or SDL fallback. */
            Result<void> Connect() {
                if (stopWorkerOwned_)
                    return Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
                if (core_ && running_ && !failed_.load())
                    return Result<void>::Success();
                if (loop_)
                    return Result<void>::Failure(MakeError(AudioErrors::DeviceUnavailable));
                loop_ = pw_thread_loop_new("horo-audio-output", nullptr);
                if (!loop_)
                    return Result<void>::Failure(NativeError("PipeWire loop", -errno));
                context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
                if (!context_) {
                    (void)Close();
                    return Result<void>::Failure(NativeError("PipeWire context", -errno));
                }
                const auto started = pw_thread_loop_start(loop_);
                if (started < 0) {
                    (void)Close();
                    return Result<void>::Failure(NativeError("PipeWire loop start", started));
                }
                running_ = true;
                failed_.store(false);
                loss_.Reset();
                overflow_ = false;
                cookieKnown_ = false;
                completedSequence_ = 0;
                {
                    Lock lock(loop_);
                    core_ = pw_context_connect(context_, nullptr, 0);
                    if (core_) {
                        static const pw_core_events coreEvents{.version = PW_VERSION_CORE_EVENTS,
                                                               .info = CoreInfo,
                                                               .done = CoreDone,
                                                               .error = CoreError};
                        pw_core_add_listener(core_, &coreListener_, &coreEvents, this);
                        registryObject_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
                        static const pw_registry_events registryEvents{.version = PW_VERSION_REGISTRY_EVENTS,
                                                                       .global = Global,
                                                                       .global_remove = Removed};
                        if (registryObject_)
                            pw_registry_add_listener(registryObject_, &registryListener_, &registryEvents, this);
                        if (registryObject_ && Roundtrip() && Roundtrip() && cookieKnown_)
                            return Result<void>::Success();
                    }
                }
                (void)Close();
                return Result<void>::Failure(MakeError(AudioErrors::DeviceUnavailable, "PipeWire service discovery did not complete"));
            }

            /** @brief Prepare one inactive explicitly targeted float stream; actual format must be observed before publication. */
            Result<Opened> Acquire(const Backend::Open &request) {
                if (const auto connected = Connect(); connected.HasError())
                    return Result<Opened>::Failure(connected.ErrorValue());
                const auto selected = ResolveAudioDevice(snapshot_, request.format.device);
                const auto entry = std::find_if(registry_.begin(), registry_.end(), [&selected](const auto &value) {
                    return value.present && value.id == selected.device;
                });
                if (entry == registry_.end())
                    return Result<Opened>::Failure(MakeError(AudioErrors::DeviceUnavailable));
                spa_audio_info_raw desired{};
                desired.format = SPA_AUDIO_FORMAT_F32;
                desired.rate = request.format.preferred.sampleRate;
                desired.channels = static_cast<std::uint32_t>(request.format.preferred.layout.orderedChannels.size());
                if (request.format.preferred.layout.kind != AudioLayoutKind::Speaker)
                    return Result<Opened>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
                for (std::size_t index = 0; index < desired.channels; ++index) {
                    const auto *role = std::get_if<AudioSpeakerRole>(&request.format.preferred.layout.orderedChannels[index]);
                    const auto found = role ? std::find(Roles.begin(), Roles.end(), *role) : Roles.end();
                    if (found == Roles.end())
                        return Result<Opened>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
                    desired.position[index] = Positions[static_cast<std::size_t>(found - Roles.begin())];
                }
                Lock lock(loop_);
                if (!Roundtrip() || overflow_ || !cookieKnown_ || entry->serverCookie != serverCookie_)
                    return Result<Opened>::Failure(MakeError(AudioErrors::DeviceUnavailable));
                const auto current = std::find_if(nodes_.begin(), nodes_.end(), [&entry](const auto &node) {
                    return node.present && entry->serial == node.serial.data();
                });
                if (current == nodes_.end())
                    return Result<Opened>::Failure(MakeError(AudioErrors::HandleStale));
                if (std::holds_alternative<AudioDefaultDeviceRole>(request.format.device) && !MatchesCurrentDefault(*current))
                    return Result<Opened>::Failure(MakeError(AudioErrors::HandleStale));
                selectedSerial_ = entry->serial;
                const auto latency = std::to_string(request.format.period.preferredFrames) + "/" + std::to_string(desired.rate);
                const auto properties =
                    pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Game",
                                      PW_KEY_TARGET_OBJECT, entry->serial.c_str(), PW_KEY_NODE_LATENCY, latency.c_str(), nullptr);
                stream_ = pw_stream_new(core_, "Horo Audio", properties);
                if (!stream_)
                    return Result<Opened>::Failure(NativeError("PipeWire stream", -errno));
                static const pw_stream_events events{.version = PW_VERSION_STREAM_EVENTS,
                                                     .state_changed = StateChanged,
                                                     .param_changed = ParamChanged,
                                                     .process = Process};
                pw_stream_add_listener(stream_, &streamListener_, &events, this);
                std::array<std::byte, 2048> storage{};
                spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
                const spa_pod *parameters[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &desired)};
                formatKnown_ = false;
                stopping_ = false;
                streamState_ = PW_STREAM_STATE_UNCONNECTED;
                const auto status = pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                                                   PW_STREAM_FLAG_INACTIVE | PW_STREAM_FLAG_DONT_RECONNECT),
                                                      parameters, 1);
                if (status < 0)
                    return Result<Opened>::Failure(NativeError("PipeWire connect stream", status));
                timespec expires{};
                pw_thread_loop_get_time(loop_, &expires, 1'000'000'000);
                while (!failed_.load() && (!formatKnown_ || streamState_ != PW_STREAM_STATE_PAUSED))
                    if (pw_thread_loop_timed_wait_full(loop_, &expires) < 0)
                        return Result<Opened>::Failure(MakeError(AudioErrors::DeviceUnavailable));
                if (failed_.load())
                    return Result<Opened>::Failure(MakeError(AudioErrors::DeviceUnavailable));
                AudioProcessingFormat native{actual_.rate, {}};
                for (std::size_t index = 0; index < actual_.channels; ++index) {
                    const auto found = std::find(Positions.begin(), Positions.end(), actual_.position[index]);
                    if (found == Positions.end())
                        return Result<Opened>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
                    native.layout.orderedChannels.emplace_back(Roles[static_cast<std::size_t>(found - Positions.begin())]);
                }
                auto format = NegotiateNativeFloatOutput(request, native, snapshot_.revision, request.format.period.preferredFrames);
                if (format.HasError())
                    return Result<Opened>::Failure(format.ErrorValue());
                auto negotiated = std::move(format).Value();
                negotiated.nativePeriodFrames.reset();  // Graph quantum is not a hardware/device-period guarantee.
                epoch_ = request.plannedEpoch;
                lossDrained_ = false;
                return Result<Opened>::Success(
                    {std::move(negotiated), {.epoch = epoch_, .capturedAt = {config_.clockDomain, Nanoseconds()}}, AccessMode::Shared});
            }

            /** @brief Copy only the native server instance cookie; no native presentation strings cross into Horo identity. */
            static void CoreInfo(void *data, const pw_core_info *info) noexcept {
                if (!info)
                    return;
                auto &self = *static_cast<PipeWireDriver *>(data);
                if (self.cookieKnown_ && self.serverCookie_ != info->cookie) {
                    self.RecordFailure(DeviceLossCause::ServiceRestart);
                } else {
                    self.serverCookie_ = info->cookie;
                    self.cookieKnown_ = true;
                }
                pw_thread_loop_signal(self.loop_, false);
            }

            static void CoreDone(void *data, std::uint32_t, const int sequence) noexcept {
                auto &self = *static_cast<PipeWireDriver *>(data);
                self.completedSequence_ = sequence;
                pw_thread_loop_signal(self.loop_, false);
            }

            static void CoreError(void *data, const std::uint32_t id, int, const int result, const char *) noexcept {
                auto &self = *static_cast<PipeWireDriver *>(data);
                // Object errors (including optional metadata binding) do not prove the core or selected stream was lost.
                if (id != PW_ID_CORE)
                    return;
                auto cause = DeviceLossCause::BackendFailure;
                if (result == -EPIPE || result == -ECONNRESET || result == -ENOTCONN)
                    cause = DeviceLossCause::ServiceRestart;
                else if (result == -EACCES || result == -EPERM)
                    cause = DeviceLossCause::PermissionRevoked;
                self.RecordFailure(cause);
                pw_thread_loop_signal(self.loop_, false);
            }

            static void Global(void *data, const std::uint32_t id, std::uint32_t, const char *type, std::uint32_t,
                               const spa_dict *properties) noexcept {
                auto &self = *static_cast<PipeWireDriver *>(data);
                if (!properties || !type)
                    return;
                if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && !self.metadata_) {
                    const char *name = spa_dict_lookup(properties, PW_KEY_METADATA_NAME);
                    if (name && std::strcmp(name, "default") == 0) {
                        self.metadata_ =
                            static_cast<pw_metadata *>(pw_registry_bind(self.registryObject_, id, type, PW_VERSION_METADATA, 0));
                        static const pw_metadata_events events{.version = PW_VERSION_METADATA_EVENTS, .property = Metadata};
                        if (self.metadata_)
                            pw_metadata_add_listener(self.metadata_, &self.metadataListener_, &events, &self);
                    }
                    return;
                }
                const char *media = spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);
                if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !media || std::strcmp(media, "Audio/Sink") != 0)
                    return;
                const auto slot = std::find_if(self.nodes_.begin(), self.nodes_.end(), [](const auto &node) {
                    return !node.present;
                });
                if (slot == self.nodes_.end()) {
                    self.overflow_ = true;
                    return;
                }
                Node node;
                node.global = id;
                node.present = Copy(node.serial, spa_dict_lookup(properties, PW_KEY_OBJECT_SERIAL)) &&
                               Copy(node.name, spa_dict_lookup(properties, PW_KEY_NODE_NAME));
                if (!Copy(node.description, spa_dict_lookup(properties, PW_KEY_NODE_DESCRIPTION)))
                    Copy(node.description, "PipeWire audio output");
                if (!node.present) {
                    self.overflow_ = true;
                    return;
                }
                *slot = node;
                ++self.changes_;
            }

            static void Removed(void *data, const std::uint32_t id) noexcept {
                auto &self = *static_cast<PipeWireDriver *>(data);
                for (auto &node : self.nodes_)
                    if (node.present && node.global == id) {
                        if (self.selectedSerial_ == node.serial.data())
                            self.RecordFailure(DeviceLossCause::Disconnected);
                        node.present = false;
                        ++self.changes_;
                    }
            }

            static int Metadata(void *data, std::uint32_t, const char *key, const char *, const char *value) noexcept {
                auto &self = *static_cast<PipeWireDriver *>(data);
                if (key && std::strcmp(key, "default.audio.sink") == 0) {
                    self.defaultMetadata_ = {};
                    if (value && !Copy(self.defaultMetadata_, value))
                        self.overflow_ = true;
                    ++self.changes_;
                }
                return 0;
            }

            static void StateChanged(void *data, pw_stream_state, const pw_stream_state state, const char *) noexcept {
                auto &self = *static_cast<PipeWireDriver *>(data);
                self.streamState_ = state;
                if (!self.stopping_ && (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && self.render_.process)))
                    self.RecordFailure(state == PW_STREAM_STATE_UNCONNECTED ? DeviceLossCause::Disconnected
                                                                            : DeviceLossCause::BackendFailure);
                pw_thread_loop_signal(self.loop_, false);
            }

            static void ParamChanged(void *data, const std::uint32_t id, const spa_pod *parameter) noexcept {
                auto &self = *static_cast<PipeWireDriver *>(data);
                if (id != SPA_PARAM_Format || !parameter)
                    return;
                spa_audio_info_raw candidate{};
                self.formatKnown_ = spa_format_audio_raw_parse(parameter, &candidate) >= 0 && candidate.format == SPA_AUDIO_FORMAT_F32 &&
                                    candidate.channels > 0 && candidate.channels <= MaximumAudioChannels;
                if (self.render_.process &&
                    (candidate.rate != self.actual_.rate || candidate.channels != self.actual_.channels ||
                     !std::equal(candidate.position, candidate.position + candidate.channels, self.actual_.position))) {
                    self.RecordFailure(DeviceLossCause::FormatChanged);
                    pw_thread_loop_signal(self.loop_, false);
                    return;
                }
                if (self.formatKnown_)
                    self.actual_ = candidate;
                if (!self.formatKnown_)
                    self.RecordFailure(DeviceLossCause::FormatChanged);
                pw_thread_loop_signal(self.loop_, false);
            }

            /** @brief All PCM work stays bounded and allocation-free on the private loop's serial process callback. */
            static void Process(void *data) noexcept {
                auto &self = *static_cast<PipeWireDriver *>(data);
                auto *buffer = pw_stream_dequeue_buffer(self.stream_);
                if (!buffer)
                    return;
                auto *native = buffer->buffer;
                if (!native || native->n_datas != 1 || !native->datas[0].data || !native->datas[0].chunk || !self.actual_.channels) {
                    self.RecordFailure(DeviceLossCause::BackendFailure);
                    pw_stream_queue_buffer(self.stream_, buffer);
                    return;
                }
                auto &plane = native->datas[0];
                const auto stride = self.actual_.channels * sizeof(float);
                auto frames = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(plane.maxsize / stride, buffer->requested ? buffer->requested : plane.maxsize / stride));
                frames = std::min(frames, MaximumAudioCallbackFrames);
                auto *samples = static_cast<float *>(plane.data);
                std::fill_n(samples, static_cast<std::size_t>(frames) * self.actual_.channels, 0.0F);
                if (self.render_.process && !self.failed_.load())
                    self.render_.process(self.render_.context, {samples, static_cast<std::size_t>(frames) * self.actual_.channels}, frames);
                plane.chunk->offset = 0;
                plane.chunk->stride = static_cast<std::int32_t>(stride);
                plane.chunk->size = frames * stride;
                buffer->size = frames;
                pw_stream_queue_buffer(self.stream_, buffer);
            }
        };
    }  // namespace

    /** @copydoc CreatePipeWireAudioBackend */
    Result<std::unique_ptr<NativeAudioBackend>> CreatePipeWireAudioBackend(const NativeAudioBackendConfig config,
                                                                           std::shared_ptr<PipeWireAudioLibrary> library) {
        if (!library)
            return Result<std::unique_ptr<NativeAudioBackend>>::Failure(MakeError(AudioErrors::IdentityInvalid));
        try {
            return CreateNativeAudioBackend(config, std::make_unique<PipeWireDriver>(config, std::move(library)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<NativeAudioBackend>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc PipeWireAudioLibrary::~PipeWireAudioLibrary */
    PipeWireAudioLibrary::~PipeWireAudioLibrary() {
        if (initialized_)
            pw_deinit();
    }

    /** @copydoc InitializePipeWireAudioLibrary */
    Result<std::shared_ptr<PipeWireAudioLibrary>> InitializePipeWireAudioLibrary() {
        try {
            auto lease = std::shared_ptr<PipeWireAudioLibrary>(new PipeWireAudioLibrary);
            pw_init(nullptr, nullptr);
            lease->initialized_ = true;
            return Result<std::shared_ptr<PipeWireAudioLibrary>>::Success(std::move(lease));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<PipeWireAudioLibrary>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }
}  // namespace Horo::Audio::Backend
