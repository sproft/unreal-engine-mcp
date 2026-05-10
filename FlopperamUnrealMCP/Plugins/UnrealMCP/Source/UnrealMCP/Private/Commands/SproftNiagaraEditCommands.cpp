#include "Commands/SproftNiagaraEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraTypes.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
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
    if (Op.Equals(TEXT("set_emitter_local_parameter"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("set_local_parameter"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("set_parameter"), ESearchCase::IgnoreCase))
    {
        return HandleSetEmitterLocalParameter(Params);
    }
    if (Op.Equals(TEXT("add_module_to_stage"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("add_module"), ESearchCase::IgnoreCase))
    {
        return HandleAddModuleToStage(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("niagara_edit: unsupported op '%s'. Supported: create_niagara_system, add_emitter_from_asset, set_emitter_local_parameter, add_module_to_stage"), *Op));
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

namespace
{
    /** Resolve a Niagara parameter type token into an FNiagaraTypeDefinition.
     *  Returns false on miss; the supported tokens map onto the
     *  documented `FNiagaraTypeDefinition::Get*Def()` accessors. */
    bool ResolveNiagaraTypeToken(const FString& InToken, FNiagaraTypeDefinition& OutType, FString& OutCanonical)
    {
        const FString T = InToken.ToLower();
        if (T == TEXT("float"))
        {
            OutType = FNiagaraTypeDefinition::GetFloatDef();
            OutCanonical = TEXT("float");
            return true;
        }
        if (T == TEXT("int") || T == TEXT("int32"))
        {
            OutType = FNiagaraTypeDefinition::GetIntDef();
            OutCanonical = TEXT("int");
            return true;
        }
        if (T == TEXT("bool"))
        {
            OutType = FNiagaraTypeDefinition::GetBoolDef();
            OutCanonical = TEXT("bool");
            return true;
        }
        if (T == TEXT("vec2") || T == TEXT("vector2") || T == TEXT("fvector2f"))
        {
            OutType = FNiagaraTypeDefinition::GetVec2Def();
            OutCanonical = TEXT("vec2");
            return true;
        }
        if (T == TEXT("vec3") || T == TEXT("vector") || T == TEXT("vector3") || T == TEXT("fvector3f"))
        {
            OutType = FNiagaraTypeDefinition::GetVec3Def();
            OutCanonical = TEXT("vec3");
            return true;
        }
        if (T == TEXT("vec4") || T == TEXT("vector4") || T == TEXT("fvector4f"))
        {
            OutType = FNiagaraTypeDefinition::GetVec4Def();
            OutCanonical = TEXT("vec4");
            return true;
        }
        if (T == TEXT("color") || T == TEXT("linear_color") || T == TEXT("flinearcolor"))
        {
            OutType = FNiagaraTypeDefinition::GetColorDef();
            OutCanonical = TEXT("color");
            return true;
        }
        if (T == TEXT("quat") || T == TEXT("fquat4f"))
        {
            OutType = FNiagaraTypeDefinition::GetQuatDef();
            OutCanonical = TEXT("quat");
            return true;
        }
        return false;
    }

    /** Walk a Json value (number / array of numbers) into a tightly-
     *  packed `float` array of the requested length. Returns true on a
     *  successful read. The caller passes a single number for scalars
     *  and an array for vectors / colors. */
    bool ReadFloatChannels(const TSharedPtr<FJsonValue>& Value, int32 ExpectedCount, TArray<float>& OutChannels, FString& OutErr)
    {
        OutChannels.Reset();
        if (!Value.IsValid())
        {
            OutErr = TEXT("Missing 'value'");
            return false;
        }
        if (ExpectedCount == 1)
        {
            if (Value->Type == EJson::Number)
            {
                OutChannels.Add(static_cast<float>(Value->AsNumber()));
                return true;
            }
            if (Value->Type == EJson::Boolean)
            {
                OutChannels.Add(Value->AsBool() ? 1.0f : 0.0f);
                return true;
            }
            OutErr = TEXT("Expected number for scalar 'value'");
            return false;
        }
        if (Value->Type != EJson::Array)
        {
            OutErr = FString::Printf(TEXT("Expected array of length %d for 'value'"), ExpectedCount);
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
        if (Arr.Num() < ExpectedCount)
        {
            OutErr = FString::Printf(TEXT("Expected array of length %d for 'value', got %d"), ExpectedCount, Arr.Num());
            return false;
        }
        for (int32 I = 0; I < ExpectedCount; ++I)
        {
            OutChannels.Add(static_cast<float>(Arr[I]->AsNumber()));
        }
        return true;
    }
}

TSharedPtr<FJsonObject> FSproftNiagaraEditCommands::HandleSetEmitterLocalParameter(const TSharedPtr<FJsonObject>& Params)
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

    FString HandleToken;
    if (!Params->TryGetStringField(TEXT("emitter"), HandleToken)
        && !Params->TryGetStringField(TEXT("emitter_handle"), HandleToken)
        && !Params->TryGetStringField(TEXT("handle_name"), HandleToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'emitter' parameter"));
    }

    FString ParameterName;
    if (!Params->TryGetStringField(TEXT("parameter_name"), ParameterName)
        && !Params->TryGetStringField(TEXT("parameter"), ParameterName)
        && !Params->TryGetStringField(TEXT("name"), ParameterName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'parameter_name' parameter"));
    }

    FString TypeToken;
    if (!Params->TryGetStringField(TEXT("parameter_type"), TypeToken)
        && !Params->TryGetStringField(TEXT("type"), TypeToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'parameter_type' parameter"));
    }

    FNiagaraTypeDefinition TypeDef;
    FString CanonicalType;
    if (!ResolveNiagaraTypeToken(TypeToken, TypeDef, CanonicalType))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported 'parameter_type' '%s' (try float / int / bool / vec2 / vec3 / vec4 / color / quat)"), *TypeToken));
    }

    // Walk the system's emitter handles and match by name. The handle's
    // display name is the editor-side mutable label; we fall back to
    // the source emitter `GetName()` for handles that have not been
    // renamed.
    FNiagaraEmitterHandle* MatchedHandle = nullptr;
    int32 HandleIndex = INDEX_NONE;
    TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
    for (int32 I = 0; I < Handles.Num(); ++I)
    {
        FNiagaraEmitterHandle& H = Handles[I];
        const FString HName = H.GetName().ToString();
        FString SourceName;
        if (UNiagaraEmitter* SrcEmitter = H.GetInstance().Emitter)
        {
            SourceName = SrcEmitter->GetName();
        }
        if (HName.Equals(HandleToken, ESearchCase::IgnoreCase)
            || (!SourceName.IsEmpty() && SourceName.Equals(HandleToken, ESearchCase::IgnoreCase)))
        {
            MatchedHandle = &H;
            HandleIndex = I;
            break;
        }
    }
    if (!MatchedHandle)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve emitter handle '%s' on system '%s'"),
                *HandleToken, *System->GetPathName()));
    }

    FVersionedNiagaraEmitterData* EmitterData = MatchedHandle->GetEmitterData();
    if (!EmitterData)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Emitter handle '%s' on system '%s' has no emitter data"),
                *HandleToken, *System->GetPathName()));
    }

    FString ScriptToken;
    Params->TryGetStringField(TEXT("script"), ScriptToken);
    UNiagaraScript* TargetScript = nullptr;
    FString CanonicalScript;
    if (ScriptToken.IsEmpty() || ScriptToken.Equals(TEXT("spawn"), ESearchCase::IgnoreCase))
    {
        TargetScript = EmitterData->SpawnScriptProps.Script;
        CanonicalScript = TEXT("spawn");
    }
    else if (ScriptToken.Equals(TEXT("update"), ESearchCase::IgnoreCase))
    {
        TargetScript = EmitterData->UpdateScriptProps.Script;
        CanonicalScript = TEXT("update");
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported 'script' '%s' (try spawn / update)"), *ScriptToken));
    }
    if (!TargetScript)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Emitter handle '%s' has no '%s' script"),
                *HandleToken, *CanonicalScript));
    }

    // Resolve the value into a tightly-packed float buffer matching
    // the parameter's type size. Bool and int are written as raw int
    // bytes; floats / vectors / colors / quats land as a `float`
    // array. Niagara's parameter store stores the bytes as a flat
    // packed buffer keyed by FNiagaraVariable.
    const int32 TypeSize = TypeDef.GetSize();
    TArray<uint8> Buffer;
    Buffer.SetNumZeroed(TypeSize);

    TSharedPtr<FJsonValue> ValueJson = Params->TryGetField(TEXT("value"));
    if (!ValueJson.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }

    if (CanonicalType == TEXT("bool"))
    {
        // FNiagaraBool stores int32 with True=-1 / False=0; the
        // documented surface is FNiagaraBool::SetValue. We avoid the
        // ImportText path here because the JSON literal is already a
        // boolean.
        if (ValueJson->Type != EJson::Boolean && ValueJson->Type != EJson::Number)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Expected boolean / number for 'value' on bool parameter"));
        }
        const bool bVal = (ValueJson->Type == EJson::Boolean) ? ValueJson->AsBool() : (ValueJson->AsNumber() != 0.0);
        // FNiagaraBool::True == -1, False == 0 (per NiagaraTypes.h);
        // its size is 4 bytes.
        const int32 EncodedBool = bVal ? -1 : 0;
        if (TypeSize != static_cast<int32>(sizeof(int32)))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unexpected bool TypeDef size %d"), TypeSize));
        }
        FMemory::Memcpy(Buffer.GetData(), &EncodedBool, sizeof(int32));
    }
    else if (CanonicalType == TEXT("int"))
    {
        if (ValueJson->Type != EJson::Number && ValueJson->Type != EJson::Boolean)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Expected number for 'value' on int parameter"));
        }
        const int32 IntVal = static_cast<int32>(FMath::RoundToDouble(ValueJson->AsNumber()));
        if (TypeSize != static_cast<int32>(sizeof(int32)))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unexpected int TypeDef size %d"), TypeSize));
        }
        FMemory::Memcpy(Buffer.GetData(), &IntVal, sizeof(int32));
    }
    else
    {
        // Float family: scalar (1) / vec2 (2) / vec3 (3) / vec4 / color / quat (4).
        int32 ExpectedFloats = 1;
        if (CanonicalType == TEXT("vec2")) ExpectedFloats = 2;
        else if (CanonicalType == TEXT("vec3")) ExpectedFloats = 3;
        else if (CanonicalType == TEXT("vec4") || CanonicalType == TEXT("color") || CanonicalType == TEXT("quat"))
        {
            ExpectedFloats = 4;
        }
        const int32 ExpectedSize = ExpectedFloats * static_cast<int32>(sizeof(float));
        if (ExpectedSize != TypeSize)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Channel count %d (size %d) does not match type size %d"),
                    ExpectedFloats, ExpectedSize, TypeSize));
        }

        TArray<float> Channels;
        FString ReadErr;
        if (!ReadFloatChannels(ValueJson, ExpectedFloats, Channels, ReadErr))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ReadErr);
        }
        FMemory::Memcpy(Buffer.GetData(), Channels.GetData(), TypeSize);
    }

    // Build the FNiagaraVariable. The parameter store keys on
    // (TypeDef, FName) pairs.
    FNiagaraVariable Variable(TypeDef, FName(*ParameterName));
    Variable.SetData(Buffer.GetData());

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    TargetScript->Modify();
    FNiagaraParameterStore& Store = TargetScript->RapidIterationParameters;
    const bool bAddIfMissing = true;
    const bool bWroteData = Store.SetParameterData(Buffer.GetData(), Variable, bAddIfMissing);

    System->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(System->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("set_emitter_local_parameter"));
    Out->SetStringField(TEXT("system"), System->GetPathName());
    Out->SetStringField(TEXT("emitter_handle"), MatchedHandle->GetName().ToString());
    Out->SetNumberField(TEXT("emitter_handle_index"), HandleIndex);
    Out->SetStringField(TEXT("script"), CanonicalScript);
    Out->SetStringField(TEXT("script_path"), TargetScript->GetPathName());
    Out->SetStringField(TEXT("parameter_name"), ParameterName);
    Out->SetStringField(TEXT("parameter_type"), CanonicalType);
    Out->SetNumberField(TEXT("parameter_size"), TypeSize);
    Out->SetBoolField(TEXT("wrote_data"), bWroteData);
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
#else
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        TEXT("niagara_edit set_emitter_local_parameter requires WITH_EDITORONLY_DATA"));
#endif
}

TSharedPtr<FJsonObject> FSproftNiagaraEditCommands::HandleAddModuleToStage(const TSharedPtr<FJsonObject>& Params)
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

    FString HandleToken;
    if (!Params->TryGetStringField(TEXT("emitter"), HandleToken)
        && !Params->TryGetStringField(TEXT("emitter_handle"), HandleToken)
        && !Params->TryGetStringField(TEXT("handle_name"), HandleToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'emitter' parameter"));
    }

    FString StageToken;
    if (!Params->TryGetStringField(TEXT("stage"), StageToken)
        && !Params->TryGetStringField(TEXT("script"), StageToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'stage' parameter (try SpawnScript / UpdateScript)"));
    }

    FString ModuleToken;
    if (!Params->TryGetStringField(TEXT("module"), ModuleToken)
        && !Params->TryGetStringField(TEXT("module_path"), ModuleToken)
        && !Params->TryGetStringField(TEXT("module_script"), ModuleToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'module' parameter (path to a Module-usage UNiagaraScript)"));
    }
    UNiagaraScript* ModuleScript = ResolveAssetOfClass<UNiagaraScript>(ModuleToken);
    if (!ModuleScript)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UNiagaraScript '%s'"), *ModuleToken));
    }
    if (ModuleScript->GetUsage() != ENiagaraScriptUsage::Module)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UNiagaraScript '%s' has Usage='%d'; expected ENiagaraScriptUsage::Module"),
                *ModuleScript->GetPathName(), static_cast<int32>(ModuleScript->GetUsage())));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    int32 TargetIndex = INDEX_NONE;
    {
        double TargetIndexValue = -1.0;
        if (Params->TryGetNumberField(TEXT("target_index"), TargetIndexValue)
            || Params->TryGetNumberField(TEXT("index"), TargetIndexValue))
        {
            TargetIndex = static_cast<int32>(TargetIndexValue);
        }
    }

    FString SuggestedName;
    Params->TryGetStringField(TEXT("suggested_name"), SuggestedName);

    // Walk the system's emitter handles and match by name.
    FNiagaraEmitterHandle* MatchedHandle = nullptr;
    int32 HandleIndex = INDEX_NONE;
    TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
    for (int32 I = 0; I < Handles.Num(); ++I)
    {
        FNiagaraEmitterHandle& H = Handles[I];
        const FString HName = H.GetName().ToString();
        FString SourceName;
        if (UNiagaraEmitter* SrcEmitter = H.GetInstance().Emitter)
        {
            SourceName = SrcEmitter->GetName();
        }
        if (HName.Equals(HandleToken, ESearchCase::IgnoreCase)
            || (!SourceName.IsEmpty() && SourceName.Equals(HandleToken, ESearchCase::IgnoreCase)))
        {
            MatchedHandle = &H;
            HandleIndex = I;
            break;
        }
    }
    if (!MatchedHandle)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve emitter handle '%s' on system '%s'"),
                *HandleToken, *System->GetPathName()));
    }

    FVersionedNiagaraEmitterData* EmitterData = MatchedHandle->GetEmitterData();
    if (!EmitterData)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Emitter handle '%s' has no emitter data"), *HandleToken));
    }

    // Resolve the script for the chosen stage and the matching usage.
    // The hosted Flop tool documents `SpawnScript` / `UpdateScript`
    // tokens; we accept the same tokens plus a few ergonomic variants
    // (`particle_spawn` / `particle_update` / `spawn` / `update`).
    UNiagaraScript* TargetScript = nullptr;
    ENiagaraScriptUsage TargetUsage = ENiagaraScriptUsage::ParticleSpawnScript;
    FString CanonicalStage;
    {
        const FString StageLower = StageToken.ToLower();
        if (StageLower == TEXT("spawnscript") || StageLower == TEXT("spawn")
            || StageLower == TEXT("particle_spawn") || StageLower == TEXT("particle_spawn_script"))
        {
            TargetScript = EmitterData->SpawnScriptProps.Script;
            TargetUsage = ENiagaraScriptUsage::ParticleSpawnScript;
            CanonicalStage = TEXT("SpawnScript");
        }
        else if (StageLower == TEXT("updatescript") || StageLower == TEXT("update")
            || StageLower == TEXT("particle_update") || StageLower == TEXT("particle_update_script"))
        {
            TargetScript = EmitterData->UpdateScriptProps.Script;
            TargetUsage = ENiagaraScriptUsage::ParticleUpdateScript;
            CanonicalStage = TEXT("UpdateScript");
        }
        else
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unsupported 'stage' '%s' (try SpawnScript / UpdateScript)"), *StageToken));
        }
    }
    if (!TargetScript)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Emitter handle '%s' has no '%s'"),
                *HandleToken, *CanonicalStage));
    }

    // Walk the script's source graph for the UNiagaraNodeOutput whose
    // GetUsage() matches the chosen stage. UNiagaraGraph::FindOutputNode
    // is not exported as NIAGARAEDITOR_API; the public source pointer
    // is reachable through UNiagaraScript::GetLatestSource and the
    // NodeGraph is a plain UPROPERTY on UNiagaraScriptSource.
    UNiagaraScriptSourceBase* SourceBase = TargetScript->GetLatestSource();
    UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
    if (!Source || !Source->NodeGraph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Script '%s' has no source graph"), *TargetScript->GetPathName()));
    }

    UNiagaraNodeOutput* OutputNode = nullptr;
    for (const TObjectPtr<UEdGraphNode>& NodePtr : Source->NodeGraph->Nodes)
    {
        UNiagaraNodeOutput* Candidate = Cast<UNiagaraNodeOutput>(NodePtr.Get());
        if (Candidate && Candidate->GetUsage() == TargetUsage)
        {
            OutputNode = Candidate;
            break;
        }
    }
    if (!OutputNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find a UNiagaraNodeOutput for stage '%s' in script '%s'"),
                *CanonicalStage, *TargetScript->GetPathName()));
    }

    // AddScriptModuleToStack is the documented public surface
    // (NIAGARAEDITOR_API). The function spawns a UNiagaraNodeFunctionCall
    // wrapping the module script, wires it into the stage's parameter
    // map chain, and patches up the UPROPERTYs that the stack viewmodel
    // refreshes automatically.
    UNiagaraNodeFunctionCall* NewModuleNode =
        FNiagaraStackGraphUtilities::AddScriptModuleToStack(
            ModuleScript, *OutputNode, TargetIndex, SuggestedName);
    if (!NewModuleNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("FNiagaraStackGraphUtilities::AddScriptModuleToStack returned null for module '%s' on stage '%s'"),
                *ModuleScript->GetPathName(), *CanonicalStage));
    }

    // Mark the source as desynchronised so the next compile re-runs.
    SourceBase->MarkNotSynchronized(TEXT("Sproft niagara_edit add_module_to_stage"));

    System->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(System->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("add_module_to_stage"));
    Out->SetStringField(TEXT("system"), System->GetPathName());
    Out->SetStringField(TEXT("emitter_handle"), MatchedHandle->GetName().ToString());
    Out->SetNumberField(TEXT("emitter_handle_index"), HandleIndex);
    Out->SetStringField(TEXT("stage"), CanonicalStage);
    Out->SetStringField(TEXT("module"), ModuleScript->GetPathName());
    Out->SetStringField(TEXT("module_node"), NewModuleNode->GetName());
    Out->SetStringField(TEXT("module_node_guid"), NewModuleNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    if (TargetIndex != INDEX_NONE)
    {
        Out->SetNumberField(TEXT("target_index"), TargetIndex);
    }
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
#else
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        TEXT("niagara_edit add_module_to_stage requires WITH_EDITORONLY_DATA"));
#endif
}
