#include "Commands/SproftLandscapeEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    /** Owning level name lookup, mirroring landscape_inspect. */
    FString LandscapeEdit_LevelLabel(ULevel* Level)
    {
        if (!Level) return FString();
        if (UWorld* OwningWorld = Cast<UWorld>(Level->GetOuter()))
        {
            return OwningWorld->GetName();
        }
        return Level->GetName();
    }

    /** Resolve an ALandscape by `GetName()` first, then by Outliner label.
     *  Walks every loaded level so streaming sublevels with their own
     *  ALandscape proxies still resolve. */
    ALandscape* ResolveLandscape(UWorld* World, const FString& Lookup, FString& OutLevelName)
    {
        if (!World || Lookup.IsEmpty()) return nullptr;
        ALandscape* ByName = nullptr;
        ALandscape* ByLabel = nullptr;
        FString NameLevel;
        FString LabelLevel;
        for (ULevel* Level : World->GetLevels())
        {
            if (!Level) continue;
            const FString LName = LandscapeEdit_LevelLabel(Level);
            for (AActor* Actor : Level->Actors)
            {
                ALandscape* L = Cast<ALandscape>(Actor);
                if (!L) continue;
                if (!ByName && L->GetName().Equals(Lookup, ESearchCase::IgnoreCase))
                {
                    ByName = L;
                    NameLevel = LName;
                }
                else if (!ByLabel && L->GetActorLabel().Equals(Lookup, ESearchCase::IgnoreCase))
                {
                    ByLabel = L;
                    LabelLevel = LName;
                }
            }
        }
        if (ByName)
        {
            OutLevelName = NameLevel;
            return ByName;
        }
        if (ByLabel)
        {
            OutLevelName = LabelLevel;
            return ByLabel;
        }
        return nullptr;
    }

    UMaterialInterface* ResolveMaterial(const FString& Input)
    {
        if (Input.IsEmpty()) return nullptr;
        if (Input.StartsWith(TEXT("/")))
        {
            return Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(Input));
        }
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(UMaterialInterface::StaticClass()->GetClassPathName(), Found, /*bSearchSubClasses=*/true);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Input, ESearchCase::IgnoreCase))
            {
                return Cast<UMaterialInterface>(Data.GetAsset());
            }
        }
        return nullptr;
    }

    /** Persist the level package so the heightmap texture writes land on
     *  disk. Returns the saved-asset path or empty when skipped. */
    FString SavePersistentLevel(UWorld* World)
    {
        if (!World) return FString();
        UPackage* Outer = World->GetOutermost();
        if (!Outer) return FString();
        const FString LevelPath = Outer->GetName();
        UEditorAssetLibrary::SaveAsset(LevelPath, /*bOnlyIfIsDirty=*/false);
        return LevelPath;
    }
}

FSproftLandscapeEditCommands::FSproftLandscapeEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftLandscapeEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("landscape_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown landscape_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
    }

    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.Equals(TEXT("set_landscape_material"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("set_material"), ESearchCase::IgnoreCase))
    {
        return HandleSetLandscapeMaterial(Params);
    }
    if (Op.Equals(TEXT("import_heightmap_png"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("import_heightmap"), ESearchCase::IgnoreCase))
    {
        return HandleImportHeightmapPng(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("landscape_edit: unsupported op '%s'. Supported: set_landscape_material, import_heightmap_png"), *Op));
}

TSharedPtr<FJsonObject> FSproftLandscapeEditCommands::HandleSetLandscapeMaterial(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor"), ActorName)
        && !Params->TryGetStringField(TEXT("landscape"), ActorName)
        && !Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor' parameter"));
    }
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material' parameter"));
    }

    FString LevelName;
    ALandscape* Landscape = ResolveLandscape(World, ActorName, LevelName);
    if (!Landscape)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve ALandscape '%s'"), *ActorName));
    }

    UMaterialInterface* NewMaterial = ResolveMaterial(MaterialPath);
    if (!NewMaterial)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UMaterialInterface '%s'"), *MaterialPath));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UMaterialInterface* Previous = Landscape->LandscapeMaterial;

    Landscape->Modify();
    Landscape->LandscapeMaterial = NewMaterial;

    // Mirror EditorSetLandscapeMaterial's PostEditChangeProperty
    // rebroadcast: the master setter on ALandscapeProxy fires this so
    // every component MIC rebuilds. The function itself is not
    // LANDSCAPE_API exported, so we replicate the body inline. The
    // PropertyChangedEvent target is the proxy class's
    // `LandscapeMaterial` FProperty.
#if WITH_EDITOR
    if (FProperty* MatProp = FindFProperty<FProperty>(Landscape->GetClass(), TEXT("LandscapeMaterial")))
    {
        FPropertyChangedEvent Event(MatProp);
        Landscape->PostEditChangeProperty(Event);
    }
#endif

    if (bSave)
    {
        SavePersistentLevel(World);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("set_landscape_material"));
    Result->SetStringField(TEXT("actor_name"), Landscape->GetName());
    Result->SetStringField(TEXT("actor_label"), Landscape->GetActorLabel());
    Result->SetStringField(TEXT("level"), LevelName);
    Result->SetStringField(TEXT("material_path"), NewMaterial->GetPathName());
    Result->SetStringField(TEXT("previous_material_path"), Previous ? Previous->GetPathName() : FString());
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftLandscapeEditCommands::HandleImportHeightmapPng(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor"), ActorName)
        && !Params->TryGetStringField(TEXT("landscape"), ActorName)
        && !Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor' parameter"));
    }
    FString PngPath;
    if (!Params->TryGetStringField(TEXT("path"), PngPath)
        && !Params->TryGetStringField(TEXT("png_path"), PngPath)
        && !Params->TryGetStringField(TEXT("file"), PngPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'path' parameter"));
    }
    FPaths::NormalizeFilename(PngPath);
    if (!IFileManager::Get().FileExists(*PngPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Heightmap PNG not found on disk: %s"), *PngPath));
    }

    FString LevelName;
    ALandscape* Landscape = ResolveLandscape(World, ActorName, LevelName);
    if (!Landscape)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve ALandscape '%s'"), *ActorName));
    }

    ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
    if (!LandscapeInfo)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Landscape has no registered ULandscapeInfo (open the level in the editor first)"));
    }

    int32 MinX = MAX_int32;
    int32 MinY = MAX_int32;
    int32 MaxX = MIN_int32;
    int32 MaxY = MIN_int32;
    if (!LandscapeInfo->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Landscape extent is empty (no components registered)"));
    }
    const int32 ExpectedWidth  = (MaxX - MinX) + 1;
    const int32 ExpectedHeight = (MaxY - MinY) + 1;
    if (ExpectedWidth <= 0 || ExpectedHeight <= 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Computed landscape extent is non-positive"));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // Pull the PNG bytes off disk and decode through the image-wrapper
    // module. The decoded FImage carries the source bit depth (8 or 16
    // for PNG) and a single-channel grayscale layout when authored as
    // such; tools like World Machine emit grayscale 16-bit PNGs.
    TArray<uint8> CompressedData;
    if (!FFileHelper::LoadFileToArray(CompressedData, *PngPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to read heightmap PNG: %s"), *PngPath));
    }

    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
    if (!ImageWrapper.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create PNG image wrapper"));
    }
    if (!ImageWrapper->SetCompressed(CompressedData.GetData(), CompressedData.Num()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("PNG header parse failed (not a valid PNG?)"));
    }

    const int32 PngWidth  = ImageWrapper->GetWidth();
    const int32 PngHeight = ImageWrapper->GetHeight();
    const int32 BitDepth  = ImageWrapper->GetBitDepth();

    if (PngWidth != ExpectedWidth || PngHeight != ExpectedHeight)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("PNG size (%d x %d) does not match landscape extent (%d x %d)"),
                PngWidth, PngHeight, ExpectedWidth, ExpectedHeight));
    }
    if (BitDepth != 8 && BitDepth != 16)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("PNG bit depth %d not supported (need 8 or 16)"), BitDepth));
    }

    // Decode to a 16-bit grayscale buffer. The landscape height range
    // is uint16; 8-bit PNGs are widened by * 257 so a flat 0xFF source
    // lands at 0xFFFF.
    TArray64<uint8> RawBytes;
    if (!ImageWrapper->GetRaw(ERGBFormat::Gray, BitDepth, RawBytes))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("PNG GetRaw failed"));
    }

    const int64 ExpectedRawCount = static_cast<int64>(PngWidth) * static_cast<int64>(PngHeight);
    if (ExpectedRawCount > static_cast<int64>(MAX_int32))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Heightmap exceeds 2^31 samples; tile-based import is a follow-on"));
    }
    TArray<uint16> Heights;
    Heights.SetNumUninitialized(static_cast<int32>(ExpectedRawCount));
    if (BitDepth == 16)
    {
        if (RawBytes.Num() != ExpectedRawCount * 2)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Decoded 16-bit PNG byte count %d is not %d"),
                    static_cast<int32>(RawBytes.Num()), static_cast<int32>(ExpectedRawCount * 2)));
        }
        FMemory::Memcpy(Heights.GetData(), RawBytes.GetData(), ExpectedRawCount * sizeof(uint16));
    }
    else
    {
        if (RawBytes.Num() != ExpectedRawCount)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Decoded 8-bit PNG byte count %d is not %d"),
                    static_cast<int32>(RawBytes.Num()), static_cast<int32>(ExpectedRawCount)));
        }
        for (int64 i = 0; i < ExpectedRawCount; ++i)
        {
            // Standard 8-bit-to-16-bit widening: byte * 257 = byte<<8 | byte
            Heights[i] = static_cast<uint16>(RawBytes[i]) * 257;
        }
    }

    Landscape->Modify();

    {
        FLandscapeEditDataInterface EditInterface(LandscapeInfo);
        // SetHeightData with stride=0 means tightly packed rows. We do
        // not request normal recomputation here; the proxy's standard
        // post-write pass picks up the new heights and the next save
        // step persists the heightmap textures.
        EditInterface.SetHeightData(MinX, MinY, MaxX, MaxY,
            Heights.GetData(),
            /*InStride=*/0,
            /*InCalcNormals=*/false);
        EditInterface.Flush();
    }

    Landscape->MarkPackageDirty();

    FString SavedLevelPath;
    if (bSave)
    {
        SavedLevelPath = SavePersistentLevel(World);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("import_heightmap_png"));
    Result->SetStringField(TEXT("actor_name"), Landscape->GetName());
    Result->SetStringField(TEXT("actor_label"), Landscape->GetActorLabel());
    Result->SetStringField(TEXT("level"), LevelName);
    Result->SetStringField(TEXT("path"), PngPath);
    Result->SetNumberField(TEXT("width"), PngWidth);
    Result->SetNumberField(TEXT("height"), PngHeight);
    Result->SetNumberField(TEXT("bit_depth"), BitDepth);
    Result->SetNumberField(TEXT("min_x"), MinX);
    Result->SetNumberField(TEXT("min_y"), MinY);
    Result->SetNumberField(TEXT("max_x"), MaxX);
    Result->SetNumberField(TEXT("max_y"), MaxY);
    Result->SetNumberField(TEXT("samples_written"), ExpectedRawCount);
    Result->SetBoolField(TEXT("saved"), bSave);
    if (!SavedLevelPath.IsEmpty())
    {
        Result->SetStringField(TEXT("saved_level_path"), SavedLevelPath);
    }
    return Result;
}
