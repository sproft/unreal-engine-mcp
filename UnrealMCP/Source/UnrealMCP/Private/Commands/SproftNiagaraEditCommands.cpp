#include "Commands/SproftNiagaraEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"

namespace
{
    void NiagaraEdit_SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
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

    /** Resolve a UObject by `/Game/...` path or short asset name. The
     *  short-name fallback walks the asset registry filtered to the
     *  given UClass. */
    template <typename T>
    T* ResolveAssetOfClass(const FString& Token)
    {
        if (Token.IsEmpty()) return nullptr;
        if (Token.StartsWith(TEXT("/")))
        {
            return Cast<T>(UEditorAssetLibrary::LoadAsset(Token));
        }
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(T::StaticClass()->GetClassPathName(), Found, /*bSearchSubClasses=*/true);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Token, ESearchCase::IgnoreCase))
            {
                return Cast<T>(Data.GetAsset());
            }
        }
        return nullptr;
    }
}

FSproftNiagaraEditCommands::FSproftNiagaraEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftNiagaraEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("niagara_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown niagara_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
    }

    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op == TEXT("create_niagara_system"))
    {
        return HandleCreateSystem(Params);
    }
    if (Op.Equals(TEXT("add_emitter_from_asset"), ESearchCase::IgnoreCase))
    {
        return HandleAddEmitterFromAsset(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("niagara_edit: unsupported op '%s'. Supported: create_niagara_system, add_emitter_from_asset"), *Op));
}

TSharedPtr<FJsonObject> FSproftNiagaraEditCommands::HandleCreateSystem(const TSharedPtr<FJsonObject>& Params)
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
    NiagaraEdit_SplitPackagePath(PackagePath, PackageDir, AssetName);
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

    UNiagaraSystem* NewSystem = NewObject<UNiagaraSystem>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!NewSystem)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UNiagaraSystem"));
    }

    // Initialise the asset's spawn / update script source so the file
    // opens cleanly in the Niagara editor. We pass
    // bCreateDefaultNodes=false so we do not pull
    // FNiagaraStackGraphUtilities (NiagaraEditor private) in;
    // an emitter-less system opens with a "no emitter" warning, which
    // is the documented behaviour for this minimum-cut slice.
    UNiagaraSystemFactoryNew::InitializeSystem(NewSystem, /*bCreateDefaultNodes=*/false);

    FAssetRegistryModule::AssetCreated(NewSystem);
    Package->MarkPackageDirty();

    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("create_niagara_system"));
    Out->SetStringField(TEXT("name"), AssetName);
    Out->SetStringField(TEXT("path"), AssetObjectPath);
    Out->SetStringField(TEXT("class"), NewSystem->GetClass()->GetName());
    Out->SetBoolField(TEXT("saved"), bSave);
    Out->SetBoolField(TEXT("has_emitters"), false);
    Out->SetStringField(TEXT("editor_warning"),
        TEXT("System has no emitters; the Niagara editor will surface a 'no emitter' warning. "
             "Emitter authoring stays on the BACKLOG for this minimum-cut slice."));
    return Out;
}

TSharedPtr<FJsonObject> FSproftNiagaraEditCommands::HandleAddEmitterFromAsset(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITORONLY_DATA
    FString SystemToken;
    if (!Params->TryGetStringField(TEXT("system"), SystemToken)
        && !Params->TryGetStringField(TEXT("system_path"), SystemToken)
        && !Params->TryGetStringField(TEXT("path"), SystemToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system' parameter"));
    }
    UNiagaraSystem* System = ResolveAssetOfClass<UNiagaraSystem>(SystemToken);
    if (!System)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UNiagaraSystem '%s'"), *SystemToken));
    }

    FString EmitterToken;
    if (!Params->TryGetStringField(TEXT("emitter"), EmitterToken)
        && !Params->TryGetStringField(TEXT("emitter_path"), EmitterToken)
        && !Params->TryGetStringField(TEXT("source_emitter"), EmitterToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'emitter' parameter"));
    }
    UNiagaraEmitter* SourceEmitter = ResolveAssetOfClass<UNiagaraEmitter>(EmitterToken);
    if (!SourceEmitter)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UNiagaraEmitter '%s'"), *EmitterToken));
    }

    // Pick the system-side display name. Default mirrors the source
    // emitter's GetName() so the system surfaces a designer-readable
    // row in the emitter list.
    FString HandleNameStr;
    Params->TryGetStringField(TEXT("handle_name"), HandleNameStr);
    if (HandleNameStr.IsEmpty())
    {
        Params->TryGetStringField(TEXT("name"), HandleNameStr);
    }
    if (HandleNameStr.IsEmpty())
    {
        HandleNameStr = SourceEmitter->GetName();
    }

    // Resolve the version GUID. Default to the source emitter's
    // currently exposed version so the system pulls the active branch.
    const FNiagaraAssetVersion ExposedVersion = SourceEmitter->GetExposedVersion();
    FGuid VersionGuid = ExposedVersion.VersionGuid;
    FString VersionToken;
    if (Params->TryGetStringField(TEXT("version_guid"), VersionToken) && !VersionToken.IsEmpty())
    {
        FGuid Parsed;
        if (FGuid::Parse(VersionToken, Parsed))
        {
            VersionGuid = Parsed;
        }
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    FNiagaraEmitterHandle NewHandle = System->AddEmitterHandle(*SourceEmitter, FName(*HandleNameStr), VersionGuid);
    if (!NewHandle.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UNiagaraSystem::AddEmitterHandle returned an invalid handle for source emitter '%s'"),
                *SourceEmitter->GetPathName()));
    }

    System->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(System->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("add_emitter_from_asset"));
    Out->SetStringField(TEXT("system"), System->GetPathName());
    Out->SetStringField(TEXT("source_emitter"), SourceEmitter->GetPathName());
    Out->SetStringField(TEXT("handle_name"), NewHandle.GetName().ToString());
    Out->SetStringField(TEXT("handle_id"), NewHandle.GetId().ToString());
    Out->SetStringField(TEXT("version_guid"), VersionGuid.ToString());
    Out->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
#else
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        TEXT("niagara_edit add_emitter_from_asset requires WITH_EDITORONLY_DATA"));
#endif
}
