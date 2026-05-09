#include "Commands/SproftSoundAssetEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"

namespace
{
    void SoundAssetEdit_SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
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

    /** Resolve a USoundCue by name or path. Loads if needed. */
    USoundCue* ResolveSoundCue(const FString& Input)
    {
        if (Input.IsEmpty()) return nullptr;
        if (Input.StartsWith(TEXT("/")))
        {
            return Cast<USoundCue>(UEditorAssetLibrary::LoadAsset(Input));
        }
        // Short-name fallback: search asset registry under /Game.
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(USoundCue::StaticClass()->GetClassPathName(), Found);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Input, ESearchCase::IgnoreCase))
            {
                return Cast<USoundCue>(Data.GetAsset());
            }
        }
        return nullptr;
    }

    USoundWave* ResolveSoundWave(const FString& Input)
    {
        if (Input.IsEmpty()) return nullptr;
        if (Input.StartsWith(TEXT("/")))
        {
            return Cast<USoundWave>(UEditorAssetLibrary::LoadAsset(Input));
        }
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(USoundWave::StaticClass()->GetClassPathName(), Found, /*bSearchSubClasses=*/true);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Input, ESearchCase::IgnoreCase))
            {
                return Cast<USoundWave>(Data.GetAsset());
            }
        }
        return nullptr;
    }

    USoundAttenuation* ResolveSoundAttenuation(const FString& Input)
    {
        if (Input.IsEmpty()) return nullptr;
        if (Input.StartsWith(TEXT("/")))
        {
            return Cast<USoundAttenuation>(UEditorAssetLibrary::LoadAsset(Input));
        }
        return nullptr;
    }
}

FSproftSoundAssetEditCommands::FSproftSoundAssetEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftSoundAssetEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("sound_asset_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown sound_asset_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
    }

    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op == TEXT("create_sound_cue"))
    {
        return HandleCreateSoundCue(Params);
    }
    if (Op == TEXT("add_sound_node_wave_player") || Op == TEXT("add_wave_player"))
    {
        return HandleAddSoundNodeWavePlayer(Params);
    }
    if (Op == TEXT("set_attenuation"))
    {
        return HandleSetAttenuation(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("sound_asset_edit: unsupported op '%s'. Supported: create_sound_cue, add_sound_node_wave_player, set_attenuation"), *Op));
}

TSharedPtr<FJsonObject> FSproftSoundAssetEditCommands::HandleCreateSoundCue(const TSharedPtr<FJsonObject>& Params)
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
    SoundAssetEdit_SplitPackagePath(PackagePath, PackageDir, AssetName);
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

    USoundCue* NewCue = NewObject<USoundCue>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!NewCue)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create USoundCue"));
    }

    // Optional initial wave player when sound_wave is present.
    bool bWavePlayerAdded = false;
    USoundWave* InitialWave = nullptr;
    FString SoundWaveParam;
    if (Params->TryGetStringField(TEXT("sound_wave"), SoundWaveParam) && !SoundWaveParam.IsEmpty())
    {
        InitialWave = ResolveSoundWave(SoundWaveParam);
        if (!InitialWave)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Could not resolve USoundWave '%s'"), *SoundWaveParam));
        }
        if (USoundNodeWavePlayer* WavePlayer = NewCue->ConstructSoundNode<USoundNodeWavePlayer>())
        {
            WavePlayer->SetSoundWave(InitialWave);
            NewCue->FirstNode = WavePlayer;
#if WITH_EDITOR
            NewCue->LinkGraphNodesFromSoundNodes();
#endif
            bWavePlayerAdded = true;
        }
    }

    FAssetRegistryModule::AssetCreated(NewCue);
    Package->MarkPackageDirty();

    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("create_sound_cue"));
    Result->SetStringField(TEXT("name"), AssetName);
    Result->SetStringField(TEXT("path"), AssetObjectPath);
    Result->SetStringField(TEXT("class"), NewCue->GetClass()->GetName());
    Result->SetBoolField(TEXT("wave_player_added"), bWavePlayerAdded);
    if (InitialWave)
    {
        Result->SetStringField(TEXT("sound_wave"), InitialWave->GetPathName());
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftSoundAssetEditCommands::HandleAddSoundNodeWavePlayer(const TSharedPtr<FJsonObject>& Params)
{
    FString CueParam;
    if (!Params->TryGetStringField(TEXT("path"), CueParam)
        && !Params->TryGetStringField(TEXT("sound_cue"), CueParam)
        && !Params->TryGetStringField(TEXT("cue"), CueParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'path' / 'sound_cue' parameter"));
    }

    FString WaveParam;
    if (!Params->TryGetStringField(TEXT("sound_wave"), WaveParam)
        && !Params->TryGetStringField(TEXT("wave"), WaveParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'sound_wave' parameter"));
    }

    USoundCue* Cue = ResolveSoundCue(CueParam);
    if (!Cue)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve USoundCue '%s'"), *CueParam));
    }
    USoundWave* Wave = ResolveSoundWave(WaveParam);
    if (!Wave)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve USoundWave '%s'"), *WaveParam));
    }

    bool bConnectToRoot = true;
    Params->TryGetBoolField(TEXT("connect_to_root"), bConnectToRoot);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    Cue->Modify();

    USoundNodeWavePlayer* WavePlayer = Cue->ConstructSoundNode<USoundNodeWavePlayer>();
    if (!WavePlayer)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to construct USoundNodeWavePlayer"));
    }
    WavePlayer->SetSoundWave(Wave);

    if (bConnectToRoot)
    {
        Cue->FirstNode = WavePlayer;
    }

#if WITH_EDITOR
    Cue->LinkGraphNodesFromSoundNodes();
#endif

    Cue->MarkPackageDirty();

    if (bSave)
    {
        UEditorAssetLibrary::SaveLoadedAsset(Cue, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_sound_node_wave_player"));
    Result->SetStringField(TEXT("sound_cue"), Cue->GetPathName());
    Result->SetStringField(TEXT("sound_wave"), Wave->GetPathName());
    Result->SetStringField(TEXT("node_class"), WavePlayer->GetClass()->GetName());
    Result->SetStringField(TEXT("node_name"), WavePlayer->GetName());
    Result->SetBoolField(TEXT("connected_to_root"), bConnectToRoot);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftSoundAssetEditCommands::HandleSetAttenuation(const TSharedPtr<FJsonObject>& Params)
{
    FString CueParam;
    if (!Params->TryGetStringField(TEXT("path"), CueParam)
        && !Params->TryGetStringField(TEXT("sound_cue"), CueParam)
        && !Params->TryGetStringField(TEXT("cue"), CueParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'path' / 'sound_cue' parameter"));
    }
    USoundCue* Cue = ResolveSoundCue(CueParam);
    if (!Cue)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve USoundCue '%s'"), *CueParam));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // attenuation can be either:
    //   - a string `/Game/...` path (resolves the asset)
    //   - empty / null (clears the override)
    USoundAttenuation* AttenuationAsset = nullptr;
    bool bClear = false;
    FString AttenuationParam;
    if (Params->TryGetStringField(TEXT("attenuation"), AttenuationParam))
    {
        if (AttenuationParam.IsEmpty())
        {
            bClear = true;
        }
        else
        {
            AttenuationAsset = ResolveSoundAttenuation(AttenuationParam);
            if (!AttenuationAsset)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Could not resolve USoundAttenuation '%s'"), *AttenuationParam));
            }
        }
    }
    else if (const TSharedPtr<FJsonValue>* AttValue = Params->Values.Find(TEXT("attenuation")))
    {
        // Caller passed a non-string value; null is the documented
        // way to clear the override.
        if (AttValue->IsValid() && (*AttValue)->Type == EJson::Null)
        {
            bClear = true;
        }
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'attenuation' parameter (path string or null)"));
    }

    Cue->Modify();
    Cue->AttenuationSettings = AttenuationAsset;
    Cue->MarkPackageDirty();

    if (bSave)
    {
        UEditorAssetLibrary::SaveLoadedAsset(Cue, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("set_attenuation"));
    Result->SetStringField(TEXT("sound_cue"), Cue->GetPathName());
    if (AttenuationAsset)
    {
        Result->SetStringField(TEXT("attenuation"), AttenuationAsset->GetPathName());
        Result->SetBoolField(TEXT("cleared"), false);
    }
    else
    {
        Result->SetBoolField(TEXT("cleared"), bClear);
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}
