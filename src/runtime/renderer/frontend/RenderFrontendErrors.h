#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::FrontendErrors {
    extern const ErrorCodeDescriptor AmbiguousPassWorkload;
    extern const ErrorCodeDescriptor ExecutorChangeDuringFrame;
    extern const ErrorCodeDescriptor FrameAlreadyActive;
    extern const ErrorCodeDescriptor FrameAlreadyExecuted;
    extern const ErrorCodeDescriptor FrameException;
    extern const ErrorCodeDescriptor FrameNotActive;
    extern const ErrorCodeDescriptor FrameNotExecuted;
    extern const ErrorCodeDescriptor InitializeException;
    extern const ErrorCodeDescriptor InvalidFrameToken;
    extern const ErrorCodeDescriptor InvalidBufferDescriptor;
    extern const ErrorCodeDescriptor InvalidMeshDescriptor;
    extern const ErrorCodeDescriptor InvalidRenderTargetDescriptor;
    extern const ErrorCodeDescriptor InvalidMemoryConfig;
    extern const ErrorCodeDescriptor InvalidResourceUploadLimits;
    extern const ErrorCodeDescriptor InvalidResourceRetirementLimits;
    extern const ErrorCodeDescriptor InvalidStaticMeshPass;
    extern const ErrorCodeDescriptor InvalidTargetExtent;
    extern const ErrorCodeDescriptor InvalidTextureDescriptor;
    extern const ErrorCodeDescriptor InvalidTextureViewDescriptor;
    extern const ErrorCodeDescriptor ResizeDuringFrame;
    extern const ErrorCodeDescriptor ResizeException;
    extern const ErrorCodeDescriptor ResourceAlreadyRetiring;
    extern const ErrorCodeDescriptor ResourceBackendException;
    extern const ErrorCodeDescriptor ResourceBufferUploadSizeMismatch;
    extern const ErrorCodeDescriptor ResourceChangeDuringFrame;
    extern const ErrorCodeDescriptor ResourceBackendInstanceInvalid;
    extern const ErrorCodeDescriptor ResourceCapacityExhausted;
    extern const ErrorCodeDescriptor ResourceCompletionInvalid;
    extern const ErrorCodeDescriptor ResourceCompletionRegressed;
    extern const ErrorCodeDescriptor ResourceCompletionUnknownQueue;
    extern const ErrorCodeDescriptor ResourceDependencyNotReady;
    extern const ErrorCodeDescriptor ResourceHandleMalformed;
    extern const ErrorCodeDescriptor ResourceNotPending;
    extern const ErrorCodeDescriptor ResourceNotReady;
    extern const ErrorCodeDescriptor ResourceOperationPending;
    extern const ErrorCodeDescriptor ResourceOperationCancelled;
    extern const ErrorCodeDescriptor ResourceOperationUnknown;
    extern const ErrorCodeDescriptor ResourceOwnerExhausted;
    extern const ErrorCodeDescriptor ResourceQueueFull;
    extern const ErrorCodeDescriptor ResourceRegistryStopped;
    extern const ErrorCodeDescriptor ResourceUploadCapacityExceeded;
    extern const ErrorCodeDescriptor ResourceUnsupported;
    extern const ErrorCodeDescriptor ResourceSlotOutOfRange;
    extern const ErrorCodeDescriptor ResourceSubmissionCapacityExceeded;
    extern const ErrorCodeDescriptor ResourceStale;
    extern const ErrorCodeDescriptor ResourceWrongOwner;
    extern const ErrorCodeDescriptor ResourceWrongType;
    extern const ErrorCodeDescriptor StaleRenderTarget;
    extern const ErrorCodeDescriptor StaticMeshExecutorAlreadyAttached;
    extern const ErrorCodeDescriptor StaticMeshExecutorMissing;
    extern const ErrorCodeDescriptor TargetReleaseDuringFrame;
}  // namespace Horo::Render::FrontendErrors
