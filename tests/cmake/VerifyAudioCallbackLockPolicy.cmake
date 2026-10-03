# Qualified source gate for the callback workload exercised by HoroAudioRealtimeSafetyHarnessTests.
# It covers these Horo-owned command-consumer and DSP implementation units, not
# arbitrary third-party/native mutex operations or a whole-program call graph.
set(forbidden_locks "std::(mutex|recursive_mutex|timed_mutex|shared_mutex|lock_guard|unique_lock|scoped_lock|shared_lock|lock[ \\t]*\\()|pthread_mutex_|EnterCriticalSection|AcquireSRWLock|os_unfair_lock")
if(NOT "std::mutex" MATCHES "${forbidden_locks}" OR "std::atomic" MATCHES "${forbidden_locks}")
    message(FATAL_ERROR "Audio callback lock policy self-test failed")
endif()
foreach(source IN ITEMS
        include/Horo/Audio/AudioCommandBuffer.h
        include/Horo/Audio/AudioResampler.h
        src/audio/commands/AudioCommandBuffer.cpp
        src/audio/resampling/AudioResampler.cpp
        src/audio/resampling/ResamplerKernel.h
        src/audio/resampling/ResamplerKernel.cpp
        src/audio/resampling/ResamplerSimd.h
        src/audio/resampling/ResamplerSimd.cpp)
    file(READ "${HORO_ENGINE_SOURCE_DIR}/${source}" contents)
    if(contents MATCHES "${forbidden_locks}")
        message(FATAL_ERROR "Forbidden raw lock primitive in audio callback workload unit: ${source}")
    endif()
endforeach()
