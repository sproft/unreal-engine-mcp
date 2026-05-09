#include "Commands/SproftMetaSoundEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Interfaces/MetasoundOutputFormatInterfaces.h"
#include "Metasound.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundSource.h"
#include "PerPlatformProperties.h"
#include "UObject/Package.h"

namespace
{
    void SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
    {
        FString Trim = InPath;
        Trim.TrimEndInline();
        Trim.RemoveFromEnd(TEXT("/"));

        int32 LastSlash = INDEX_NONE;
        if (Trim.FindLastChar('/', LastSlash))
        {
            OutPackageDir = Trim.Left(LastSlash + 1);
            OutAssetName = Trim.Mid(LastSlash + 1);
        }
        else
        {
            OutPackageDir = TEXT("/Game/");
            OutAssetName = Trim;
        }

        int32 DotIdx = INDEX_NONE;
        if (OutAssetName.FindChar('.', DotIdx))
        {
            OutAssetName = OutAssetName.Left(DotIdx);
        }
    }

    bool ParseOutputFormat(const FString& Token, EMetaSoundOutputAudioFormat& Out)
    {
        const FString Lower = Token.ToLower();
        if (Lower == TEXT("mono") || Lower == TEXT("1.0"))
        {
            Out = EMetaSoundOutputAudioFormat::Mono;
            return true;
        }
        if (Lower == TEXT("stereo") || Lower == TEXT("2.0"))
        {
            Out = EMetaSoundOutputAudioFormat::Stereo;
            return true;
        }
        if (Lower == TEXT("quad") || Lower == TEXT("4.0"))
        {
            Out = EMetaSoundOutputAudioFormat::Quad;
            return true;
        }
        if (Lower == TEXT("5_1") || Lower == TEXT("5.1") || Lower == TEXT("fivedotone"))
        {
            Out = EMetaSoundOutputAudioFormat::FiveDotOne;
            return true;
        }
        if (Lower == TEXT("7_1") || Lower == TEXT("7.1") || Lower == TEXT("sevendotone"))
        {
            Out = EMetaSoundOutputAudioFormat::SevenDotOne;
            return true;
        }
        return false;
    }
}

FSproftMetaSoundEditCommands::FSproftMetaSoundEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftMetaSoundEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("metasound_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown metasound_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op == TEXT("create_metasound_source"))
    {
        return HandleCreateSource(Params);
    }
    if (Op == TEXT("create_metasound_patch"))
    {
        return HandleCreatePatch(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("metasound_edit: unsupported op '%s'. Supported: create_metasound_source, create_metasound_patch"), *Op));
}

TSharedPtr<FJsonObject> FSproftMetaSoundEditCommands::HandleCreateSource(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("path"), PackagePath)
        && !Params->TryGetStringField(TEXT("asset"), PackagePath)
        && !Params->TryGetStringField(TEXT("asset_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'path' parameter"));
    }
    if (!PackagePath.StartsWith(TEXT("/Game/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset path '%s' must start with /Game/"), *PackagePath));
    }

    FString PackageDir;
    FString AssetName;
    SplitPackagePath(PackagePath, PackageDir, AssetName);
    if (AssetName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not derive asset name from '%s'"), *PackagePath));
    }
    const FString AssetObjectPath = PackageDir + AssetName;

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    if (UEditorAssetLibrary::DoesAssetExist(AssetObjectPath) && !bOverwrite)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s (set 'overwrite': true to replace)"), *AssetObjectPath));
    }

    EMetaSoundOutputAudioFormat OutputFormat = EMetaSoundOutputAudioFormat::Stereo;
    FString FormatToken;
    if (Params->TryGetStringField(TEXT("output_format"), FormatToken)
        || Params->TryGetStringField(TEXT("format"), FormatToken)
        || Params->TryGetStringField(TEXT("channels"), FormatToken))
    {
        if (!ParseOutputFormat(FormatToken, OutputFormat))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unrecognised output_format '%s'. Use mono / stereo / quad / 5_1 / 7_1."),
                    *FormatToken));
        }
    }

    int32 SampleRate = 0;
    Params->TryGetNumberField(TEXT("sample_rate"), SampleRate);
    double BlockRate = 0.0;
    Params->TryGetNumberField(TEXT("block_rate"), BlockRate);

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UMetaSoundSource* NewSource = NewObject<UMetaSoundSource>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!NewSource)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UMetaSoundSource"));
    }

    NewSource->OutputFormat = OutputFormat;
#if WITH_EDITORONLY_DATA
    if (SampleRate > 0)
    {
        NewSource->SampleRateOverride = SampleRate;
    }
    if (BlockRate > 0.0)
    {
        NewSource->BlockRateOverride = static_cast<float>(BlockRate);
    }
#endif

    // Initialise the document + register the editor graph so the asset
    // opens cleanly in the MetaSound editor. GetChecked() asserts the
    // subsystem exists; that's true whenever the editor module is up.
    UMetaSoundEditorSubsystem& EditorSubsystem = UMetaSoundEditorSubsystem::GetChecked();
    EditorSubsystem.InitAsset(*NewSource, /*ReferencedMetaSound*/ nullptr, /*bClearDocument*/ false);
    EditorSubsystem.RegisterGraphWithFrontend(*NewSource);

    FAssetRegistryModule::AssetCreated(NewSource);
    Package->MarkPackageDirty();

    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("create_metasound_source"));
    Result->SetStringField(TEXT("name"), AssetName);
    Result->SetStringField(TEXT("path"), AssetObjectPath);
    Result->SetStringField(TEXT("class"), NewSource->GetClass()->GetName());
    Result->SetStringField(TEXT("output_format"), FormatToken.IsEmpty() ? TEXT("stereo") : FormatToken.ToLower());
    Result->SetNumberField(TEXT("sample_rate"), SampleRate);
    Result->SetNumberField(TEXT("block_rate"), BlockRate);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftMetaSoundEditCommands::HandleCreatePatch(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("path"), PackagePath)
        && !Params->TryGetStringField(TEXT("asset"), PackagePath)
        && !Params->TryGetStringField(TEXT("asset_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'path' parameter"));
    }
    if (!PackagePath.StartsWith(TEXT("/Game/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset path '%s' must start with /Game/"), *PackagePath));
    }

    FString PackageDir;
    FString AssetName;
    SplitPackagePath(PackagePath, PackageDir, AssetName);
    if (AssetName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not derive asset name from '%s'"), *PackagePath));
    }
    const FString AssetObjectPath = PackageDir + AssetName;

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    if (UEditorAssetLibrary::DoesAssetExist(AssetObjectPath) && !bOverwrite)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s (set 'overwrite': true to replace)"), *AssetObjectPath));
    }

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UMetaSoundPatch* NewPatch = NewObject<UMetaSoundPatch>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!NewPatch)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UMetaSoundPatch"));
    }

    UMetaSoundEditorSubsystem& EditorSubsystem = UMetaSoundEditorSubsystem::GetChecked();
    EditorSubsystem.InitAsset(*NewPatch, /*ReferencedMetaSound*/ nullptr, /*bClearDocument*/ false);
    EditorSubsystem.RegisterGraphWithFrontend(*NewPatch);

    FAssetRegistryModule::AssetCreated(NewPatch);
    Package->MarkPackageDirty();

    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("create_metasound_patch"));
    Result->SetStringField(TEXT("name"), AssetName);
    Result->SetStringField(TEXT("path"), AssetObjectPath);
    Result->SetStringField(TEXT("class"), NewPatch->GetClass()->GetName());
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}
