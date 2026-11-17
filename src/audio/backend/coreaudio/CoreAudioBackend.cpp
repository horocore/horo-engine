#include "Horo/Audio/Internal/CoreAudioBackend.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/NativeAudioFacts.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <Block.h>
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <dispatch/dispatch.h>
#include <limits>
#include <thread>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Keep native status detail at the control diagnostic boundary. */
        Error NativeError(const char *operation, const OSStatus status) {
            return MakeError(AudioErrors::BackendFailed, std::string(operation) + " OSStatus=" + std::to_string(status));
        }

        /** @brief Native host steady-clock time; no relation to an output device's hardware clock is implied. */
        std::uint64_t Nanoseconds() noexcept {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        /** @brief Read one fixed-size HAL property with explicit output scope. */
        template <class T>
        OSStatus Property(const AudioObjectID object, const AudioObjectPropertySelector selector, T &value,
                          const AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
            const AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
            UInt32 size = sizeof(value);
            return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value);
        }

        /** @brief Copy and release an owned CF string without publishing a native UID as presentation text. */
        std::string StringProperty(const AudioObjectID object, const AudioObjectPropertySelector selector) {
            CFStringRef value{};
            if (Property(object, selector, value) != noErr || !value)
                return {};
            std::array<char, 257> buffer{};
            const bool converted = CFStringGetCString(value, buffer.data(), buffer.size(), kCFStringEncodingUTF8);
            CFRelease(value);
            return converted ? std::string(buffer.data()) : std::string{};
        }

        /** @brief Native HAL/AudioUnit ownership; property notifications use a private serial queue, never the render callback. */
        class CoreAudioDriver final : public NativeAudioDriver {
        public:
            explicit CoreAudioDriver(const NativeAudioBackendConfig config) : config_(config), facts_(config.owner) {}

            AudioBackendKind Kind() const noexcept override {
                return AudioBackendKind::CoreAudio;
            }

            ResourceDisposition Resources() const noexcept override {
                if (callbackAttached_ || admission_.load() || readers_.load() != 0 || cleanupRetained_)
                    return ResourceDisposition::Retained;
                return unit_ || queue_ || block_ || listenerState_ ? ResourceDisposition::CallbackDetached : ResourceDisposition::Closed;
            }

            bool HasUndrainedRequiredFacts() const noexcept override {
                return facts_.HasLoss() || HasNativeLoss();
            }

            Result<AudioBackendProbe> Probe() override {
                AudioBackendProbe probe{.backend = Kind(),
                                        .compiled = true,
                                        .hostSupported = true,
                                        .availability = AudioBackendAvailability::Available,
                                        .revision = 1,
                                        .backendVersion = "Core Audio HAL/AudioUnit"};
                probe.features.fill(AudioCapabilitySupport::Unsupported);
                for (const auto feature : {AudioBackendCapability::PhysicalEnumeration, AudioBackendCapability::NativeHotplug,
                                           AudioBackendCapability::HardwareLatency, AudioBackendCapability::NativeDiagnostics})
                    probe.features[static_cast<std::size_t>(feature)] = AudioCapabilitySupport::Available;
                return Result<AudioBackendProbe>::Success(std::move(probe));
            }

            Result<AudioDeviceSnapshot> Enumerate() override {
                const AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                                         kAudioObjectPropertyElementMain};
                UInt32 size{};
                auto status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size);
                if (status != noErr)
                    return Result<AudioDeviceSnapshot>::Failure(NativeError("Core Audio enumeration size", status));
                if (size % sizeof(AudioDeviceID) != 0 || size / sizeof(AudioDeviceID) > MaximumAudioDiscoveredDevices)
                    return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
                std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
                status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data());
                if (status != noErr)
                    return Result<AudioDeviceSnapshot>::Failure(NativeError("Core Audio enumeration", status));
                if (snapshot_.revision == std::numeric_limits<std::uint64_t>::max())
                    return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
                auto registry = registry_;
                for (auto &entry : registry)
                    entry.present = false;
                AudioDeviceSnapshot next{.owner = config_.owner, .backend = Kind(), .revision = snapshot_.revision + 1};
                for (const auto device : devices) {
                    AudioStreamBasicDescription format{};
                    if (Property(device, kAudioDevicePropertyStreamFormat, format, kAudioObjectPropertyScopeOutput) != noErr ||
                        format.mChannelsPerFrame == 0)
                        continue;
                    const auto uid = StringProperty(device, kAudioDevicePropertyDeviceUID);
                    if (uid.empty())
                        return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::DeviceUnavailable));
                    auto found = std::find_if(registry.begin(), registry.end(), [&uid](const auto &entry) {
                        return entry.uid == uid;
                    });
                    if (found == registry.end()) {
                        if (registry.size() == 4'096)
                            return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
                        registry.push_back({uid, device, {config_.owner, static_cast<std::uint32_t>(registry.size() + 1), 1}, true});
                        found = std::prev(registry.end());
                    } else {
                        const auto old = std::find_if(registry_.begin(), registry_.end(), [&uid](const auto &entry) {
                            return entry.uid == uid;
                        });
                        if (old != registry_.end() && !old->present) {
                            if (found->id.generation == std::numeric_limits<std::uint32_t>::max())
                                return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
                            ++found->id.generation;
                        }
                        found->native = device;
                        found->present = true;
                    }
                    auto name = StringProperty(device, kAudioObjectPropertyName);
                    next.devices.push_back({found->id, name.empty() ? "macOS audio output" : std::move(name), AudioDeviceClass::Unknown});
                }
                const auto defaultFor = [&registry](const AudioObjectPropertySelector selector) -> std::optional<AudioDeviceId> {
                    AudioDeviceID native{};
                    if (Property(kAudioObjectSystemObject, selector, native) != noErr)
                        return std::nullopt;
                    const auto found = std::find_if(registry.begin(), registry.end(), [native](const auto &entry) {
                        return entry.present && entry.native == native;
                    });
                    return found == registry.end() ? std::nullopt : std::optional(found->id);
                };
                next.defaults.console = defaultFor(kAudioHardwarePropertyDefaultSystemOutputDevice);
                next.defaults.multimedia = defaultFor(kAudioHardwarePropertyDefaultOutputDevice);
                // HAL has no distinct communications role; absence is explicit.
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
                render_ = render;
                admission_.store(true);
                callbackAttached_ = true;
                const auto status = AudioOutputUnitStart(unit_);
                if (status != noErr) {
                    (void)Stop({config_.clockDomain, Nanoseconds() + 1'000'000'000});
                    return Result<void>::Failure(NativeError("Core Audio start", status));
                }
                return Result<void>::Success();
            }

            NativeRetirementResult Stop(const AudioMonotonicTimestamp deadline) override {
                deadline_ = deadline;
                admission_.store(false);
                if (!unit_)
                    return NativeRetirementResult::Success(NativeRetirementProgress::Complete);
                auto status = AudioOutputUnitStop(unit_);
                if (status != noErr)
                    return NativeRetirementResult::Failure(NativeError("Core Audio stop", status));
                AURenderCallbackStruct empty{};
                status = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &empty, sizeof(empty));
                if (status != noErr)
                    return NativeRetirementResult::Failure(NativeError("Core Audio callback detach", status));
                while (readers_.load() != 0) {
                    if (Nanoseconds() >= deadline.nanoseconds)
                        return NativeRetirementResult::Failure(
                            MakeError(AudioErrors::BackendFailed, "Core Audio callback detach exceeded deadline"));
                    std::this_thread::yield();
                }
                if (initialized_) {
                    status = AudioUnitUninitialize(unit_);
                    if (status != noErr)
                        return NativeRetirementResult::Failure(NativeError("Core Audio stopped-unit uninitialize", status));
                    initialized_ = false;
                }
                status = AudioComponentInstanceDispose(unit_);
                if (status != noErr)
                    return NativeRetirementResult::Failure(NativeError("Core Audio stopped-unit dispose", status));
                unit_ = nullptr;
                render_ = {};
                callbackAttached_ = false;
                return NativeRetirementResult::Success(NativeRetirementProgress::Complete);
            }

            NativeRetirementResult Close() override {
                cleanupRetained_ = unit_ || queue_;
                if (callbackAttached_ || admission_.load() || readers_.load() != 0)
                    return NativeRetirementResult::Failure(MakeError(AudioErrors::RuntimeInactive));
                for (auto &listener : listeners_) {
                    if (!listener.attached)
                        continue;
                    const auto status = AudioObjectRemovePropertyListenerBlock(listener.object, &listener.address, queue_, block_);
                    if (status != noErr)
                        return NativeRetirementResult::Failure(NativeError("Core Audio listener detach", status));
                    listener.attached = false;
                }
                if (queue_) {
                    const auto semaphore = dispatch_semaphore_create(0);
                    if (!semaphore)
                        return NativeRetirementResult::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
                    dispatch_retain(semaphore);
                    dispatch_async(queue_, ^{
                      dispatch_semaphore_signal(semaphore);
                      dispatch_release(semaphore);
                    });
                    const auto now = Nanoseconds();
                    const auto expires = deadline_.nanoseconds ? deadline_.nanoseconds : now + 1'000'000'000;
                    const auto remaining = expires > now ? expires - now : 0;
                    const auto wait = static_cast<int64_t>(std::min<std::uint64_t>(remaining, std::numeric_limits<int64_t>::max()));
                    if (dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, wait)) != 0) {
                        // The block owns the semaphore until signalled; releasing our reference is safe under dispatch capture.
                        dispatch_release(semaphore);
                        return NativeRetirementResult::Failure(
                            MakeError(AudioErrors::BackendFailed, "Core Audio listener queue did not drain"));
                    }
                    dispatch_release(semaphore);
                    dispatch_release(queue_);
                    queue_ = nullptr;
                    if (block_)
                        Block_release(block_);
                    block_ = nullptr;
                }
                if (!CaptureNativeFacts())
                    return NativeRetirementResult::Failure(MakeError(AudioErrors::EventQueueInvalid));
                listenerState_.reset();  // Also retires the allocation prefix when queue creation failed.
                if (unit_) {
                    if (initialized_) {
                        const auto uninitialized = AudioUnitUninitialize(unit_);
                        if (uninitialized != noErr)
                            return NativeRetirementResult::Failure(NativeError("Core Audio uninitialize", uninitialized));
                        initialized_ = false;
                    }
                    const auto disposed = AudioComponentInstanceDispose(unit_);
                    if (disposed != noErr)
                        return NativeRetirementResult::Failure(NativeError("Core Audio dispose", disposed));
                    unit_ = nullptr;
                }
                cleanupRetained_ = false;
                epoch_ = {};
                return NativeRetirementResult::Success(NativeRetirementProgress::Complete);
            }

            std::size_t DrainEvents(const std::span<Event> output) noexcept override {
                (void)CaptureNativeFacts();
                return facts_.Drain(output);
            }

        private:
            struct Entry final {
                std::string uid;
                AudioDeviceID native{};
                AudioDeviceId id;
                bool present{};
            };

            struct ListenerState final {
                std::atomic<std::uint64_t> changes{};
                NativeAudioLossLatch loss;
                AudioDeviceID device{};
            };

            struct Listener final {
                AudioObjectID object{};
                AudioObjectPropertyAddress address{};
                bool attached{};
            };

            NativeAudioBackendConfig config_;
            AudioDeviceSnapshot snapshot_;
            std::vector<Entry> registry_;
            AudioUnit unit_{};
            AudioDeviceEpoch epoch_;
            NativeAudioRenderPort render_;
            std::uint32_t channels_{};
            // AudioUnit serializes render calls; admission/readers protect retained port lifetime through native stop.
            std::atomic<bool> admission_{};
            std::atomic<std::uint32_t> readers_{};
            std::atomic<bool> renderFault_{};
            // Prepared before Start and retained until Stop detaches render plus the listener queue is drained.
            std::unique_ptr<ListenerState> listenerState_;
            dispatch_queue_t queue_{};
            AudioObjectPropertyListenerBlock block_{};
            std::array<Listener, 5> listeners_{};
            AudioMonotonicTimestamp deadline_;
            std::uint64_t drainedChanges_{};
            bool lossDrained_{};
            bool initialized_{};
            bool cleanupRetained_{};
            bool callbackAttached_{};
            NativeAudioFacts facts_;

            /** @brief Bind native loss to its admitted device epoch while callback/listener storage remains owned. */
            bool HasNativeLoss() const noexcept {
                return !lossDrained_ && epoch_.device.IsValid() &&
                       ((listenerState_ && listenerState_->loss.Read().has_value()) || renderFault_.load());
            }

            /** @brief Preserve required facts before the detached native callback/listener objects are released. */
            bool CaptureNativeFacts() noexcept {
                if (HasNativeLoss()) {
                    const auto cause = listenerState_ ? listenerState_->loss.Read().value_or(DeviceLossCause::BackendFailure)
                                                      : DeviceLossCause::BackendFailure;
                    if (!facts_.CaptureLoss({epoch_.device, cause}))
                        return false;
                    lossDrained_ = true;
                }
                if (listenerState_) {
                    const auto changes = listenerState_->changes.load();
                    if (changes != drainedChanges_) {
                        facts_.CaptureChange(snapshot_.revision);
                        drainedChanges_ = changes;
                    }
                }
                return true;
            }

            /** @brief Resolve the exact discovered endpoint and revalidate any current system-default role. */
            Result<AudioDeviceID> SelectDevice(const Backend::Open &request) {
                const auto selected = ResolveAudioDevice(snapshot_, request.format.device);
                const auto found = std::find_if(registry_.begin(), registry_.end(), [&selected](const auto &entry) {
                    return entry.present && entry.id == selected.device;
                });
                if (found == registry_.end())
                    return Result<AudioDeviceID>::Failure(MakeError(AudioErrors::DeviceUnavailable));
                if (const auto *role = std::get_if<AudioDefaultDeviceRole>(&request.format.device)) {
                    AudioDeviceID current{};
                    const auto selector = *role == AudioDefaultDeviceRole::Console ? kAudioHardwarePropertyDefaultSystemOutputDevice
                                                                                   : kAudioHardwarePropertyDefaultOutputDevice;
                    const auto queried = Property(kAudioObjectSystemObject, selector, current);
                    if (queried != noErr)
                        return Result<AudioDeviceID>::Failure(NativeError("Core Audio default revalidation", queried));
                    if (current != found->native)
                        return Result<AudioDeviceID>::Failure(MakeError(AudioErrors::HandleStale));
                }
                return Result<AudioDeviceID>::Success(found->native);
            }

            /** @brief Query and validate the native mono/stereo processing tuple without acquiring an AudioUnit. */
            Result<AudioProcessingFormat> NativeFormat(const AudioDeviceID device) {
                AudioStreamBasicDescription hardware{};
                auto status = Property(device, kAudioDevicePropertyStreamFormat, hardware, kAudioObjectPropertyScopeOutput);
                if (status != noErr)
                    return Result<AudioProcessingFormat>::Failure(NativeError("Core Audio device format", status));
                if ((hardware.mChannelsPerFrame != 1 && hardware.mChannelsPerFrame != 2) || hardware.mSampleRate < MinimumAudioSampleRate ||
                    hardware.mSampleRate > MaximumAudioSampleRate || std::floor(hardware.mSampleRate) != hardware.mSampleRate)
                    return Result<AudioProcessingFormat>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
                const AudioProcessingFormat native{static_cast<std::uint32_t>(hardware.mSampleRate),
                                                   MakeAudioSpeakerLayout(hardware.mChannelsPerFrame == 1 ? AudioSpeakerPreset::Mono
                                                                                                          : AudioSpeakerPreset::Stereo)};
                return Result<AudioProcessingFormat>::Success(native);
            }

            /** @brief Acquire the output AudioUnit and bind its explicit endpoint with input disabled. */
            Result<void> CreateUnit(const AudioDeviceID device) {
                const AudioComponentDescription description{kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                                            kAudioUnitManufacturer_Apple, 0, 0};
                const auto component = AudioComponentFindNext(nullptr, &description);
                if (!component)
                    return Result<void>::Failure(MakeError(AudioErrors::DeviceUnavailable));
                auto status = AudioComponentInstanceNew(component, &unit_);
                const UInt32 enable = 1, disable = 0;
                if (status == noErr)
                    status =
                        AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enable, sizeof(enable));
                if (status == noErr)
                    status =
                        AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &disable, sizeof(disable));
                if (status == noErr)
                    status = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device,
                                                  sizeof(device));
                if (status != noErr)
                    return Result<void>::Failure(NativeError("Core Audio output binding", status));
                return Result<void>::Success();
            }

            /** @brief Set explicit float client rate, frame shape and channel semantics before initialization. */
            Result<void> ConfigureClient(const AudioProcessingFormat &native) {
                channels_ = static_cast<UInt32>(native.layout.orderedChannels.size());
                const UInt32 bytesPerFrame = channels_ * static_cast<UInt32>(sizeof(float));
                const AudioStreamBasicDescription client{static_cast<Float64>(native.sampleRate),
                                                         kAudioFormatLinearPCM,
                                                         kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian,
                                                         bytesPerFrame,
                                                         1,
                                                         bytesPerFrame,
                                                         channels_,
                                                         32,
                                                         0};
                auto status =
                    AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &client, sizeof(client));
                ::AudioChannelLayout channelLayout{};
                channelLayout.mChannelLayoutTag = channels_ == 1 ? kAudioChannelLayoutTag_Mono : kAudioChannelLayoutTag_Stereo;
                if (status == noErr)
                    status = AudioUnitSetProperty(unit_, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, 0, &channelLayout,
                                                  sizeof(channelLayout));
                if (status != noErr)
                    return Result<void>::Failure(NativeError("Core Audio client format", status));
                return Result<void>::Success();
            }

            /** @brief Install the bounded retained render entry and initialize the stopped unit. */
            Result<void> InitializeUnit() {
                const UInt32 maximum = MaximumAudioCallbackFrames;
                auto status = AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maximum,
                                                   sizeof(maximum));
                const AURenderCallbackStruct callback{Render, this};
                if (status == noErr)
                    status = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback,
                                                  sizeof(callback));
                if (status == noErr)
                    status = AudioUnitInitialize(unit_);
                if (status != noErr)
                    return Result<void>::Failure(NativeError("Core Audio HAL prepare", status));
                initialized_ = true;
                return Result<void>::Success();
            }

            /** @brief Acquire separately drained property listeners without sharing the native render callback. */
            Result<void> InstallListeners(const AudioDeviceID device) {
                listenerState_ = std::make_unique<ListenerState>();
                listenerState_->device = device;
                queue_ = dispatch_queue_create("horo.audio.coreaudio.properties", DISPATCH_QUEUE_SERIAL);
                if (!queue_)
                    return Result<void>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
                auto *state = listenerState_.get();
                block_ = Block_copy(^(UInt32 count, const AudioObjectPropertyAddress *addresses) {
                  state->changes.fetch_add(1);
                  for (UInt32 index = 0; index < count; ++index)
                      if (addresses[index].mSelector == kAudioDevicePropertyDeviceIsAlive)
                          state->loss.Capture(DeviceLossCause::Disconnected);
                      else if (addresses[index].mSelector == kAudioDevicePropertyNominalSampleRate)
                          state->loss.Capture(DeviceLossCause::FormatChanged);
                });
                if (!block_)
                    return Result<void>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
                listeners_ = {
                    {{kAudioObjectSystemObject,
                      {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}},
                     {kAudioObjectSystemObject,
                      {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}},
                     {kAudioObjectSystemObject,
                      {kAudioHardwarePropertyDefaultSystemOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}},
                     {device, {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}},
                     {device, {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}}}};
                for (auto &listener : listeners_) {
                    const auto status = AudioObjectAddPropertyListenerBlock(listener.object, &listener.address, queue_, block_);
                    if (status != noErr)
                        return Result<void>::Failure(NativeError("Core Audio property listener", status));
                    listener.attached = true;
                }
                return Result<void>::Success();
            }

            /** @brief Capture native device latency with reported provenance and leave unavailable evidence unknown. */
            AudioDeviceTimingReport Timing(const AudioProcessingFormat &native, const AudioDeviceID device) {
                AudioDeviceTimingReport timing{.epoch = epoch_, .capturedAt = {config_.clockDomain, Nanoseconds()}};
                UInt32 latencyFrames{};
                if (Property(device, kAudioDevicePropertyLatency, latencyFrames, kAudioObjectPropertyScopeOutput) == noErr)
                    timing.hardwareLatency = {.quality = AudioObservationQuality::Reported,
                                              .source = AudioObservationSource::BackendApi,
                                              .nanoseconds = static_cast<std::uint64_t>(latencyFrames) * 1'000'000'000 / native.sampleRate,
                                              .observedAt = timing.capturedAt};
                return timing;
            }

            /** @brief Prepare each native ownership stage in order; Open owns reverse cleanup on every rejection. */
            Result<Opened> Acquire(const Backend::Open &request) {
                deadline_ = {};  // A previous epoch's Stop deadline cannot govern this open-only rollback.
                const auto selected = SelectDevice(request);
                if (selected.HasError())
                    return Result<Opened>::Failure(selected.ErrorValue());
                const auto device = selected.Value();
                const auto native = NativeFormat(device);
                if (native.HasError())
                    return Result<Opened>::Failure(native.ErrorValue());
                UInt32 period{};
                const auto queried = Property(device, kAudioDevicePropertyBufferFrameSize, period);
                if (queried != noErr)
                    return Result<Opened>::Failure(NativeError("Core Audio period", queried));
                auto negotiated = NegotiateNativeFloatOutput(request, native.Value(), snapshot_.revision, period);
                if (negotiated.HasError())
                    return Result<Opened>::Failure(negotiated.ErrorValue());
                if (const auto created = CreateUnit(device); created.HasError())
                    return Result<Opened>::Failure(created.ErrorValue());
                if (const auto configured = ConfigureClient(native.Value()); configured.HasError())
                    return Result<Opened>::Failure(configured.ErrorValue());
                if (const auto initialized = InitializeUnit(); initialized.HasError())
                    return Result<Opened>::Failure(initialized.ErrorValue());
                if (const auto listening = InstallListeners(device); listening.HasError())
                    return Result<Opened>::Failure(listening.ErrorValue());
                epoch_ = request.plannedEpoch;
                renderFault_.store(false);
                lossDrained_ = false;
                drainedChanges_ = 0;
                return Result<Opened>::Success({std::move(negotiated).Value(), Timing(native.Value(), device), AccessMode::Shared});
            }

            /** @brief AudioUnit entry writes silence while closed and borrows only preallocated Horo render state. */
            static OSStatus Render(void *context, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *, UInt32, const UInt32 frames,
                                   AudioBufferList *buffers) noexcept {
                auto &self = *static_cast<CoreAudioDriver *>(context);
                self.readers_.fetch_add(1);
                if (!buffers || buffers->mNumberBuffers != 1 || frames > MaximumAudioCallbackFrames ||
                    buffers->mBuffers[0].mNumberChannels != self.channels_ || !buffers->mBuffers[0].mData ||
                    buffers->mBuffers[0].mDataByteSize < static_cast<std::size_t>(frames) * self.channels_ * sizeof(float)) {
                    if (self.listenerState_)
                        self.listenerState_->loss.Capture(DeviceLossCause::BackendFailure);
                    self.renderFault_.store(true);
                    if (flags)
                        *flags |= kAudioUnitRenderAction_OutputIsSilence;
                    self.readers_.fetch_sub(1);
                    return kAudioUnitErr_TooManyFramesToProcess;
                }
                const auto samples = static_cast<std::size_t>(frames) * self.channels_;
                std::fill_n(static_cast<float *>(buffers->mBuffers[0].mData), samples, 0.0F);
                if (self.admission_.load()) {
                    self.render_.process(self.render_.context, {static_cast<float *>(buffers->mBuffers[0].mData), samples}, frames);
                }
                self.readers_.fetch_sub(1);
                return noErr;
            }
        };
    }  // namespace

    /** @copydoc CreateCoreAudioBackend */
    Result<std::unique_ptr<NativeAudioBackend>> CreateCoreAudioBackend(const NativeAudioBackendConfig config) {
        try {
            return CreateNativeAudioBackend(config, std::make_unique<CoreAudioDriver>(config));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<NativeAudioBackend>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }
}  // namespace Horo::Audio::Backend
