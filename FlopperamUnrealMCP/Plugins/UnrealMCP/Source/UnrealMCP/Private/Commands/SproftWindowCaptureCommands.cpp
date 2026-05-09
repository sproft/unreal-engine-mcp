#include "Commands/SproftWindowCaptureCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Editor.h"
#include "ImageUtils.h"
#include "UnrealClient.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

FSproftWindowCaptureCommands::FSproftWindowCaptureCommands()
{
}

TSharedPtr<FJsonObject> FSproftWindowCaptureCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("window_capture"))
    {
        return HandleWindowCapture(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown window capture command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftWindowCaptureCommands::HandleWindowCapture(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("GEditor is null"));
    }

    FViewport* Viewport = GEditor->GetActiveViewport();
    if (!Viewport)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No active editor viewport"));
    }

    // Resolve the destination file path.
    FString FilePath;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("file_path"), FilePath);
    }

    if (FilePath.IsEmpty())
    {
        const FString DefaultDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCPScreenshots"));
        const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
        FilePath = FPaths::Combine(DefaultDir, FString::Printf(TEXT("Capture_%s.png"), *Stamp));
    }

    // Ensure .png extension so PNGCompressImageArray output matches the file.
    if (!FilePath.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase))
    {
        FilePath += TEXT(".png");
    }

    FilePath = FPaths::ConvertRelativePathToFull(FilePath);

    // Make sure the destination directory exists.
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    const FString DirectoryPath = FPaths::GetPath(FilePath);
    if (!DirectoryPath.IsEmpty() && !PlatformFile.DirectoryExists(*DirectoryPath))
    {
        PlatformFile.CreateDirectoryTree(*DirectoryPath);
    }

    // Read pixels from the viewport.
    const FIntPoint Size = Viewport->GetSizeXY();
    if (Size.X <= 0 || Size.Y <= 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Active viewport has zero size"));
    }

    TArray<FColor> Bitmap;
    FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
    ReadFlags.SetLinearToGamma(false);
    if (!Viewport->ReadPixels(Bitmap, ReadFlags))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to read viewport pixels"));
    }

    if (Bitmap.Num() != Size.X * Size.Y)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Bitmap size does not match viewport size"));
    }

    // Drop alpha so screenshots are opaque.
    for (FColor& Pixel : Bitmap)
    {
        Pixel.A = 255;
    }

    // PNG compress and write.
    TArray64<uint8> PngBytes;
    FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Bitmap, PngBytes);

    if (!FFileHelper::SaveArrayToFile(PngBytes, *FilePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to write screenshot to '%s'"), *FilePath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("file_path"), FilePath);
    ResultObj->SetNumberField(TEXT("width"), Size.X);
    ResultObj->SetNumberField(TEXT("height"), Size.Y);
    ResultObj->SetNumberField(TEXT("byte_size"), PngBytes.Num());
    return ResultObj;
}
