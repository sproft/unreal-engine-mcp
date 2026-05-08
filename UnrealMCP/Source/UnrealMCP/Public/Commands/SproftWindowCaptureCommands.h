#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: window_capture
 *
 * Captures a PNG screenshot of the active editor viewport to a file on disk.
 * Mirrors the documented behaviour of the hosted Flop "window_capture" tool.
 *
 * The implementation uses the public UE5 API: GEditor->GetActiveViewport()
 * combined with FViewport::ReadPixels and FImageUtils::PNGCompressImageArray.
 * Synchronous so the caller gets a final path back in one round trip.
 */
class UNREALMCP_API FSproftWindowCaptureCommands
{
public:
    FSproftWindowCaptureCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleWindowCapture(const TSharedPtr<FJsonObject>& Params);
};
