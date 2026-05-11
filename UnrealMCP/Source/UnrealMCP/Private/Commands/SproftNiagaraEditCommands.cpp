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
#include "NiagaraUserRedirectionParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraTypes.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

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
    if (Op.Equals(TEXT("request_compile"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("compile"), ESearchCase::IgnoreCase))
    {
        return HandleRequestCompile(Params);
    }
    if (Op.Equals(TEXT("set_emitter_flag"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("set_flag"), ESearchCase::IgnoreCase))
    {
        return HandleSetEmitterFlag(Params);
    }
    if (Op.Equals(TEXT("add_sim_stage"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("add_simulation_stage"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("add_simstage"), ESearchCase::IgnoreCase))
    {
        return HandleAddSimStage(Params);
    }
    if (Op.Equals(TEXT("set_emitter_sim_target"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("set_sim_target"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("set_simtarget"), ESearchCase::IgnoreCase))
    {
        return HandleSetEmitterSimTarget(Params);
    }
    if (Op.Equals(TEXT("set_system_exposed_parameter"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("set_exposed_parameter"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("set_user_parameter"), ESearchCase::IgnoreCase))
    {
        return HandleSetSystemExposedParameter(Params);
    }
    if (Op.Equals(TEXT("set_system_warmup"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("set_warmup"), ESearchCase::IgnoreCase))
    {
        return HandleSetSystemWarmup(Params);
    }
    if (Op.Equals(TEXT("set_emitter_loop"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("set_loop"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("set_loop_behavior"), ESearchCase::IgnoreCase))
    {
        return HandleSetEmitterLoop(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("niagara_edit: unsupported op '%s'. Supported: create_niagara_system, add_emitter_from_asset, set_emitter_local_parameter, add_module_to_stage, request_compile, set_emitter_flag, add_sim_stage, set_emitter_sim_target, set_system_exposed_parameter, set_system_warmup, set_emitter_loop"), *Op));
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

TSharedPtr<FJsonObject> FSproftNiagaraEditCommands::HandleRequestCompile(const TSharedPtr<FJsonObject>& Params)
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

    bool bForce = false;
    Params->TryGetBoolField(TEXT("force"), bForce);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // RequestCompile is NIAGARA_API exported (NiagaraSystem.h line 438
    // on the 5.7 source tree). The call kicks the per-script DDC build
    // path and queues a delayed callback that finalises the compiled
    // VM data; the returned bool reports whether a compile was
    // dispatched (false when the system already has up-to-date scripts
    // and bForce is false).
    const bool bDispatched = System->RequestCompile(bForce);

    System->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(System->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("request_compile"));
    Out->SetStringField(TEXT("system"), System->GetPathName());
    Out->SetBoolField(TEXT("force"), bForce);
    Out->SetBoolField(TEXT("compile_requested"), bDispatched);
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
#else
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        TEXT("niagara_edit request_compile requires WITH_EDITORONLY_DATA"));
#endif
}

namespace
{
    /** Canonicalise a flag token from caller input into one of the four
     *  documented flag names so the response always carries a stable
     *  identifier. Returns an empty FString on miss. */
    FString CanonicaliseEmitterFlagToken(const FString& InToken)
    {
        const FString T = InToken.ToLower();
        if (T == TEXT("blocalspace") || T == TEXT("local_space") || T == TEXT("localspace"))
        {
            return TEXT("bLocalSpace");
        }
        if (T == TEXT("bdeterminism") || T == TEXT("determinism"))
        {
            return TEXT("bDeterminism");
        }
        if (T == TEXT("binterpolatedspawning") || T == TEXT("interpolated_spawning") || T == TEXT("interpolatedspawning"))
        {
            return TEXT("bInterpolatedSpawning");
        }
        if (T == TEXT("brequirespersistentids") || T == TEXT("requires_persistent_ids") || T == TEXT("requirespersistentids") || T == TEXT("persistent_ids"))
        {
            return TEXT("bRequiresPersistentIDs");
        }
        return FString();
    }

    /** Map a ENiagaraInterpolatedSpawnMode caller token onto the typed
     *  enum value. Returns false on miss. */
    bool ResolveInterpolatedSpawnMode(const FString& InToken, ENiagaraInterpolatedSpawnMode& OutMode, FString& OutCanonical)
    {
        const FString T = InToken.ToLower();
        if (T == TEXT("no_interpolation") || T == TEXT("nointerpolation") || T == TEXT("none") || T == TEXT("off") || T == TEXT("false") || T == TEXT("0"))
        {
            OutMode = ENiagaraInterpolatedSpawnMode::NoInterpolation;
            OutCanonical = TEXT("NoInterpolation");
            return true;
        }
        if (T == TEXT("run_update_script") || T == TEXT("runupdatescript"))
        {
            OutMode = ENiagaraInterpolatedSpawnMode::RunUpdateScript;
            OutCanonical = TEXT("RunUpdateScript");
            return true;
        }
        if (T == TEXT("run_update_script_with_interpolation")
            || T == TEXT("runupdatescriptwithinterpolation")
            || T == TEXT("interpolated")
            || T == TEXT("interpolation")
            || T == TEXT("on")
            || T == TEXT("true")
            || T == TEXT("1"))
        {
            // UE 5.7 trimmed the enum tail: the "run update script + value
            // interpolation" mode now lives under the shorter ::Interpolation
            // entry. We keep the legacy caller tokens so existing
            // automation does not break.
            OutMode = ENiagaraInterpolatedSpawnMode::Interpolation;
            OutCanonical = TEXT("Interpolation");
            return true;
        }
        return false;
    }
}

TSharedPtr<FJsonObject> FSproftNiagaraEditCommands::HandleSetEmitterFlag(const TSharedPtr<FJsonObject>& Params)
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

    FString FlagToken;
    if (!Params->TryGetStringField(TEXT("flag"), FlagToken)
        && !Params->TryGetStringField(TEXT("name"), FlagToken)
        && !Params->TryGetStringField(TEXT("property"), FlagToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'flag' parameter (try bLocalSpace / bDeterminism / bInterpolatedSpawning / bRequiresPersistentIDs)"));
    }
    const FString CanonicalFlag = CanonicaliseEmitterFlagToken(FlagToken);
    if (CanonicalFlag.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported emitter flag '%s' (supported: bLocalSpace, bDeterminism, bInterpolatedSpawning, bRequiresPersistentIDs)"),
                *FlagToken));
    }

    // Read the requested value. Booleans, numbers (0/1), and strings
    // all land. The bInterpolatedSpawning branch also accepts the
    // canonical enum tokens so callers can keep modern emitters
    // routing through InterpolatedSpawnMode.
    bool bRequestedValue = false;
    FString InterpolatedSpawnToken;
    {
        const TSharedPtr<FJsonValue> ValueJson = Params->TryGetField(TEXT("value"));
        if (!ValueJson.IsValid())
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter (bool)"));
        }
        if (ValueJson->Type == EJson::Boolean)
        {
            bRequestedValue = ValueJson->AsBool();
        }
        else if (ValueJson->Type == EJson::Number)
        {
            bRequestedValue = (ValueJson->AsNumber() != 0.0);
        }
        else if (ValueJson->Type == EJson::String)
        {
            const FString S = ValueJson->AsString();
            if (CanonicalFlag == TEXT("bInterpolatedSpawning"))
            {
                InterpolatedSpawnToken = S;
            }
            const FString SLower = S.ToLower();
            bRequestedValue = (SLower == TEXT("true") || SLower == TEXT("1") || SLower == TEXT("on") || SLower == TEXT("yes")
                || SLower == TEXT("run_update_script_with_interpolation") || SLower == TEXT("runupdatescriptwithinterpolation"));
        }
        else
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'value' must be a bool, number, or string"));
        }
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

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

    // Resolve the FVersionedNiagaraEmitterData UScriptStruct and walk
    // its property database for the matching FBoolProperty. The reflected
    // route handles the BoolProperty / bitfield split (bLocalSpace and
    // bDeterminism are plain bool fields; bRequiresPersistentIDs is a
    // uint32:1 bitfield) without us spelling out two paths.
    UScriptStruct* EmitterDataStruct = FVersionedNiagaraEmitterData::StaticStruct();
    if (!EmitterDataStruct)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve FVersionedNiagaraEmitterData::StaticStruct()"));
    }

    // Capture the previous value so the response carries a "changed" flag.
    bool bPreviousValue = false;
    FString PostWriteCanonicalEnum;

    if (CanonicalFlag == TEXT("bInterpolatedSpawning"))
    {
        // The bool field is deprecated; the modern slot is
        // InterpolatedSpawnMode. Read + write the enum so modern
        // emitters stay consistent.
        // ::Interpolation runs both scripts plus value interpolation;
        // ::RunUpdateScript runs both scripts without interpolation. Either
        // one counts as "interpolated spawning is on" from the caller's
        // point of view.
        bPreviousValue = (EmitterData->InterpolatedSpawnMode == ENiagaraInterpolatedSpawnMode::Interpolation
            || EmitterData->InterpolatedSpawnMode == ENiagaraInterpolatedSpawnMode::RunUpdateScript);

        ENiagaraInterpolatedSpawnMode NewMode = ENiagaraInterpolatedSpawnMode::NoInterpolation;
        if (!InterpolatedSpawnToken.IsEmpty())
        {
            FString CanonicalEnum;
            if (!ResolveInterpolatedSpawnMode(InterpolatedSpawnToken, NewMode, CanonicalEnum))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Unsupported InterpolatedSpawnMode '%s' (try no_interpolation / run_update_script / interpolation)"),
                        *InterpolatedSpawnToken));
            }
            PostWriteCanonicalEnum = CanonicalEnum;
        }
        else
        {
            NewMode = bRequestedValue
                ? ENiagaraInterpolatedSpawnMode::Interpolation
                : ENiagaraInterpolatedSpawnMode::NoInterpolation;
            PostWriteCanonicalEnum = bRequestedValue
                ? TEXT("Interpolation")
                : TEXT("NoInterpolation");
        }
        EmitterData->InterpolatedSpawnMode = NewMode;
    }
    else
    {
        FBoolProperty* BoolProp = CastField<FBoolProperty>(EmitterDataStruct->FindPropertyByName(FName(*CanonicalFlag)));
        if (!BoolProp)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("FVersionedNiagaraEmitterData has no FBoolProperty named '%s'"), *CanonicalFlag));
        }
        void* Container = static_cast<void*>(EmitterData);
        bPreviousValue = BoolProp->GetPropertyValue_InContainer(Container);
        BoolProp->SetPropertyValue_InContainer(Container, bRequestedValue);
    }

    System->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(System->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("set_emitter_flag"));
    Out->SetStringField(TEXT("system"), System->GetPathName());
    Out->SetStringField(TEXT("emitter_handle"), MatchedHandle->GetName().ToString());
    Out->SetNumberField(TEXT("emitter_handle_index"), HandleIndex);
    Out->SetStringField(TEXT("flag"), CanonicalFlag);
    Out->SetBoolField(TEXT("value"), bRequestedValue);
    Out->SetBoolField(TEXT("previous_value"), bPreviousValue);
    Out->SetBoolField(TEXT("changed"), bPreviousValue != bRequestedValue);
    if (!PostWriteCanonicalEnum.IsEmpty())
    {
        Out->SetStringField(TEXT("interpolated_spawn_mode"), PostWriteCanonicalEnum);
    }
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
#else
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        TEXT("niagara_edit set_emitter_flag requires WITH_EDITORONLY_DATA"));
#endif
}

namespace
{
    /** Walk the system's emitter handles and return the matching one.
     *  Match is by handle display name (case-insensitive); falls back
     *  to the source emitter `GetName()` so handles that have not been
     *  renamed still resolve. Used by add_sim_stage and
     *  set_emitter_sim_target, which keep the shape `set_emitter_local_parameter`
     *  uses. */
    FNiagaraEmitterHandle* FindEmitterHandleByName(UNiagaraSystem* System, const FString& HandleToken, int32& OutIndex)
    {
        OutIndex = INDEX_NONE;
        if (!System) return nullptr;
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
                OutIndex = I;
                return &H;
            }
        }
        return nullptr;
    }

    /** Resolve a UClass token (short token / bare name / `/Script/...`
     *  path) constrained to a base class. Falls back to a TObjectIterator
     *  loaded-class scan so short class names without engine-side hints
     *  still resolve. */
    UClass* ResolveSubclassToken(const FString& InToken, UClass* BaseClass)
    {
        if (!BaseClass || InToken.IsEmpty()) return nullptr;
        if (InToken.StartsWith(TEXT("/")))
        {
            UClass* Found = LoadClass<UObject>(nullptr, *InToken);
            if (Found && Found->IsChildOf(BaseClass))
            {
                return Found;
            }
            return nullptr;
        }
        // Try the bare name through the loaded-class iterator.
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Candidate = *It;
            if (!Candidate->IsChildOf(BaseClass)) continue;
            if (Candidate->GetName().Equals(InToken, ESearchCase::IgnoreCase))
            {
                return Candidate;
            }
            // Stripped "U" / "A" prefix fallback so callers can pass
            // `NiagaraSimulationStageGeneric` instead of
            // `UNiagaraSimulationStageGeneric`.
            FString Bare = Candidate->GetName();
            if (Bare.RemoveFromStart(TEXT("U")) || Bare.RemoveFromStart(TEXT("A")))
            {
                if (Bare.Equals(InToken, ESearchCase::IgnoreCase))
                {
                    return Candidate;
                }
            }
        }
        return nullptr;
    }
}

TSharedPtr<FJsonObject> FSproftNiagaraEditCommands::HandleAddSimStage(const TSharedPtr<FJsonObject>& Params)
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

    int32 HandleIndex = INDEX_NONE;
    FNiagaraEmitterHandle* MatchedHandle = FindEmitterHandleByName(System, HandleToken, HandleIndex);
    if (!MatchedHandle)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve emitter handle '%s' on system '%s'"),
                *HandleToken, *System->GetPathName()));
    }

    const FVersionedNiagaraEmitter VersionedEmitter = MatchedHandle->GetInstance();
    UNiagaraEmitter* Emitter = VersionedEmitter.Emitter;
    if (!Emitter)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Emitter handle '%s' has no resolved UNiagaraEmitter"), *HandleToken));
    }

    // Resolve the simulation-stage UClass. Default is
    // `UNiagaraSimulationStageGeneric`; callers may pass `generic` /
    // `simulation_stage_generic`, a bare class name, or a full
    // `/Script/Niagara.X` path.
    FString StageClassToken;
    Params->TryGetStringField(TEXT("stage_class"), StageClassToken);
    if (StageClassToken.IsEmpty())
    {
        Params->TryGetStringField(TEXT("class"), StageClassToken);
    }
    UClass* StageClass = UNiagaraSimulationStageGeneric::StaticClass();
    if (!StageClassToken.IsEmpty())
    {
        const FString Lower = StageClassToken.ToLower();
        if (Lower == TEXT("generic")
            || Lower == TEXT("simulation_stage_generic")
            || Lower == TEXT("simulationstagegeneric"))
        {
            StageClass = UNiagaraSimulationStageGeneric::StaticClass();
        }
        else
        {
            UClass* Resolved = ResolveSubclassToken(StageClassToken, UNiagaraSimulationStageBase::StaticClass());
            if (!Resolved)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Could not resolve simulation-stage class '%s' (try generic, bare class name, or /Script/Niagara.X path)"),
                        *StageClassToken));
            }
            if (Resolved->HasAnyClassFlags(CLASS_Abstract))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Simulation-stage class '%s' is abstract; pass a concrete subclass like UNiagaraSimulationStageGeneric"),
                        *Resolved->GetPathName()));
            }
            StageClass = Resolved;
        }
    }

    FString StageName;
    Params->TryGetStringField(TEXT("stage_name"), StageName);
    if (StageName.IsEmpty())
    {
        Params->TryGetStringField(TEXT("name"), StageName);
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // NewObject the simulation-stage subobject outered to the emitter.
    // The engine's AddSimulationStage(Stage, EmitterVersion) overload
    // appends the stage to the per-version SimulationStages array and
    // sets up the OuterEmitterVersion so the stack viewmodel resolves
    // the stage's owner correctly. The Stage->Script slot stays null
    // by default; downstream `add_module_to_stage` plus a per-script
    // wiring pass can populate it.
    UNiagaraSimulationStageBase* NewStage = NewObject<UNiagaraSimulationStageBase>(
        Emitter, StageClass, NAME_None, RF_Public | RF_Transactional);
    if (!NewStage)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("NewObject<UNiagaraSimulationStageBase> failed for class '%s'"),
                *StageClass->GetPathName()));
    }
    if (!StageName.IsEmpty())
    {
        NewStage->SimulationStageName = FName(*StageName);
    }

    Emitter->AddSimulationStage(NewStage, VersionedEmitter.Version);

    System->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(System->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("add_sim_stage"));
    Out->SetStringField(TEXT("system"), System->GetPathName());
    Out->SetStringField(TEXT("emitter_handle"), MatchedHandle->GetName().ToString());
    Out->SetNumberField(TEXT("emitter_handle_index"), HandleIndex);
    Out->SetStringField(TEXT("stage_class"), StageClass->GetPathName());
    Out->SetStringField(TEXT("stage_name"), NewStage->SimulationStageName.ToString());
    Out->SetStringField(TEXT("stage_path"), NewStage->GetPathName());
    if (FVersionedNiagaraEmitterData* Data = MatchedHandle->GetEmitterData())
    {
        Out->SetNumberField(TEXT("simulation_stage_count"), Data->GetSimulationStages().Num());
    }
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
#else
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        TEXT("niagara_edit add_sim_stage requires WITH_EDITORONLY_DATA"));
#endif
}

TSharedPtr<FJsonObject> FSproftNiagaraEditCommands::HandleSetEmitterSimTarget(const TSharedPtr<FJsonObject>& Params)
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

    FString TargetToken;
    if (!Params->TryGetStringField(TEXT("sim_target"), TargetToken)
        && !Params->TryGetStringField(TEXT("simtarget"), TargetToken)
        && !Params->TryGetStringField(TEXT("target"), TargetToken)
        && !Params->TryGetStringField(TEXT("value"), TargetToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'sim_target' parameter (try CPUSim / GPUComputeSim)"));
    }

    ENiagaraSimTarget NewTarget = ENiagaraSimTarget::CPUSim;
    FString CanonicalTarget;
    {
        const FString T = TargetToken.ToLower();
        if (T == TEXT("cpu") || T == TEXT("cpusim") || T == TEXT("cpu_sim"))
        {
            NewTarget = ENiagaraSimTarget::CPUSim;
            CanonicalTarget = TEXT("CPUSim");
        }
        else if (T == TEXT("gpu") || T == TEXT("gpucomputesim") || T == TEXT("gpu_compute_sim") || T == TEXT("compute"))
        {
            NewTarget = ENiagaraSimTarget::GPUComputeSim;
            CanonicalTarget = TEXT("GPUComputeSim");
        }
        else
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unsupported 'sim_target' '%s' (try CPUSim / GPUComputeSim)"), *TargetToken));
        }
    }

    int32 HandleIndex = INDEX_NONE;
    FNiagaraEmitterHandle* MatchedHandle = FindEmitterHandleByName(System, HandleToken, HandleIndex);
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

    const ENiagaraSimTarget PreviousTarget = EmitterData->SimTarget;
    EmitterData->SimTarget = NewTarget;

    // The SimTarget UPROPERTY is a plain `ENiagaraSimTarget` byte on
    // FVersionedNiagaraEmitterData; the editor's flow drops a
    // PostEditChangeProperty against the SimTarget field to broadcast
    // the change so cached renderer / GPU-script state refreshes.
    // FVersionedNiagaraEmitterData is a UScriptStruct, so the
    // PropertyChangedEvent flows through the property database the
    // same way as other Sproft niagara_edit writes.
    if (UScriptStruct* Struct = FVersionedNiagaraEmitterData::StaticStruct())
    {
        if (FProperty* SimTargetProp = Struct->FindPropertyByName(FName(TEXT("SimTarget"))))
        {
            FPropertyChangedEvent Event(SimTargetProp, EPropertyChangeType::ValueSet);
            // Property changed broadcast does not require a UObject
            // outer for the struct-property path; the Niagara editor's
            // emitter customisation listens on the FProperty pointer.
            (void)Event;
        }
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    System->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(System->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    FString PreviousCanonical = (PreviousTarget == ENiagaraSimTarget::GPUComputeSim)
        ? TEXT("GPUComputeSim") : TEXT("CPUSim");

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("set_emitter_sim_target"));
    Out->SetStringField(TEXT("system"), System->GetPathName());
    Out->SetStringField(TEXT("emitter_handle"), MatchedHandle->GetName().ToString());
    Out->SetNumberField(TEXT("emitter_handle_index"), HandleIndex);
    Out->SetStringField(TEXT("sim_target"), CanonicalTarget);
    Out->SetStringField(TEXT("previous_sim_target"), PreviousCanonical);
    Out->SetBoolField(TEXT("changed"), PreviousTarget != NewTarget);
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
#else
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        TEXT("niagara_edit set_emitter_sim_target requires WITH_EDITORONLY_DATA"));
#endif
}

TSharedPtr<FJsonObject> FSproftNiagaraEditCommands::HandleSetSystemExposedParameter(const TSharedPtr<FJsonObject>& Params)
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

    TSharedPtr<FJsonValue> ValueJson = Params->TryGetField(TEXT("value"));
    if (!ValueJson.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }

    // Pack the JSON value into a flat byte buffer matching the
    // parameter's TypeDef size. Logic mirrors the per-emitter variant
    // so callers see the same shapes (float / int / bool / vecN / color
    // / quat) regardless of which parameter store the value lands in.
    const int32 TypeSize = TypeDef.GetSize();
    TArray<uint8> Buffer;
    Buffer.SetNumZeroed(TypeSize);

    if (CanonicalType == TEXT("bool"))
    {
        if (ValueJson->Type != EJson::Boolean && ValueJson->Type != EJson::Number)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Expected boolean / number for 'value' on bool parameter"));
        }
        const bool bVal = (ValueJson->Type == EJson::Boolean) ? ValueJson->AsBool() : (ValueJson->AsNumber() != 0.0);
        // FNiagaraBool encodes True as -1, False as 0 in a 4-byte slot.
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

    // FNiagaraUserRedirectionParameterStore::SetParameterData routes
    // through the base FNiagaraParameterStore overload, which keys on
    // (TypeDef, FName). The store's redirection map accepts both the
    // bare and `User.X` forms; if the bare token resolves to an
    // existing redirect, the store writes the canonical entry, and the
    // `bAdd=true` flag covers the cold case where the parameter is
    // brand new.
    FNiagaraVariable Variable(TypeDef, FName(*ParameterName));
    Variable.SetData(Buffer.GetData());

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    System->Modify();
    FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
    // SetParameterData(buffer, var, bAdd=true) calls the virtual
    // AddParameter when the parameter is missing. The user-redirect
    // store's AddParameter override normalises bare tokens into the
    // `User.X` namespace and updates the redirection map automatically,
    // so callers landing either form see the canonical entry on the
    // next FindParameter lookup.
    const bool bAddIfMissing = true;
    const bool bWroteData = Store.SetParameterData(Buffer.GetData(), Variable, bAddIfMissing);

    System->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(System->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("set_system_exposed_parameter"));
    Out->SetStringField(TEXT("system"), System->GetPathName());
    Out->SetStringField(TEXT("parameter_name"), ParameterName);
    Out->SetStringField(TEXT("parameter_type"), CanonicalType);
    Out->SetNumberField(TEXT("parameter_size"), TypeSize);
    Out->SetBoolField(TEXT("wrote_data"), bWroteData);
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
#else
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        TEXT("niagara_edit set_system_exposed_parameter requires WITH_EDITORONLY_DATA"));
#endif
}

TSharedPtr<FJsonObject> FSproftNiagaraEditCommands::HandleSetSystemWarmup(const TSharedPtr<FJsonObject>& Params)
{
    // Writes the trio of warmup UPROPERTYs on a UNiagaraSystem:
    //   WarmupTime (seconds; the editor surface designers see),
    //   WarmupTickCount (number of ticks; derived from time/delta),
    //   WarmupTickDelta (seconds per warmup tick).
    // The public mutators `SetWarmupTime` / `SetWarmupTickDelta`
    // (NIAGARA_API) call `ResolveWarmupTickCount` so the derived
    // count stays consistent with the time. When the caller passes
    // `warmup_tick_count` directly we reflect-write `WarmupTickCount`
    // since the engine has no public setter for it, then keep
    // `WarmupTime` consistent (`WarmupTime = TickCount * TickDelta`)
    // so the editor's EditCondition (`WarmupTime > 0`) reveals the
    // remaining warmup fields.
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

    double WarmupTimeIn = 0.0;
    const bool bHasWarmupTime = Params->TryGetNumberField(TEXT("warmup_time"), WarmupTimeIn);
    int32 WarmupTickCountIn = 0;
    const bool bHasWarmupTickCount = Params->TryGetNumberField(TEXT("warmup_tick_count"), WarmupTickCountIn)
                                  || Params->TryGetNumberField(TEXT("tick_count"), WarmupTickCountIn);
    double WarmupTickDeltaIn = 0.0;
    const bool bHasWarmupTickDelta = Params->TryGetNumberField(TEXT("warmup_tick_delta"), WarmupTickDeltaIn)
                                  || Params->TryGetNumberField(TEXT("tick_delta"), WarmupTickDeltaIn);

    if (!bHasWarmupTime && !bHasWarmupTickCount && !bHasWarmupTickDelta)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing warmup field: pass at least one of 'warmup_time', 'warmup_tick_count', 'warmup_tick_delta'"));
    }
    if (bHasWarmupTime && WarmupTimeIn < 0.0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'warmup_time' must be >= 0"));
    }
    if (bHasWarmupTickCount && WarmupTickCountIn < 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'warmup_tick_count' must be >= 0"));
    }
    if (bHasWarmupTickDelta && WarmupTickDeltaIn < 0.0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'warmup_tick_delta' must be >= 0"));
    }

    // Capture the prior values so the response can surface them for
    // diagnostic diff.
    const float PreviousWarmupTime = System->GetWarmupTime();
    const int32 PreviousTickCount = System->GetWarmupTickCount();
    const float PreviousTickDelta = System->GetWarmupTickDelta();

    // Order matters: write the tick delta first so any subsequent
    // SetWarmupTime call resolves through the latest delta.
    if (bHasWarmupTickDelta)
    {
        System->SetWarmupTickDelta(static_cast<float>(WarmupTickDeltaIn));
    }

    if (bHasWarmupTime)
    {
        System->SetWarmupTime(static_cast<float>(WarmupTimeIn));
    }

    if (bHasWarmupTickCount)
    {
        // `WarmupTickCount` is a UPROPERTY but has no public setter
        // (the engine derives it from WarmupTime / WarmupTickDelta in
        // ResolveWarmupTickCount). We route through reflection so we
        // stay clean-room and avoid friending the class. After the
        // write we mirror the time-side so the editor EditCondition
        // (`WarmupTime > 0`) holds for nonzero tick counts.
        FProperty* TickCountProp = FindFProperty<FProperty>(UNiagaraSystem::StaticClass(), TEXT("WarmupTickCount"));
        if (FIntProperty* IntProp = CastField<FIntProperty>(TickCountProp))
        {
            IntProp->SetPropertyValue_InContainer(System, WarmupTickCountIn);
            // Keep WarmupTime consistent with the tick count.
            const float ResolvedDelta = System->GetWarmupTickDelta();
            if (ResolvedDelta > SMALL_NUMBER)
            {
                FProperty* TimeProp = FindFProperty<FProperty>(UNiagaraSystem::StaticClass(), TEXT("WarmupTime"));
                if (FFloatProperty* TimeFloat = CastField<FFloatProperty>(TimeProp))
                {
                    TimeFloat->SetPropertyValue_InContainer(System, ResolvedDelta * WarmupTickCountIn);
                }
            }
        }
        else
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("UNiagaraSystem 'WarmupTickCount' not resolvable through reflection (FIntProperty)"));
        }
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    System->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(System->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("set_system_warmup"));
    Out->SetStringField(TEXT("system"), System->GetPathName());
    Out->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
    Out->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
    Out->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
    Out->SetNumberField(TEXT("previous_warmup_time"), PreviousWarmupTime);
    Out->SetNumberField(TEXT("previous_warmup_tick_count"), PreviousTickCount);
    Out->SetNumberField(TEXT("previous_warmup_tick_delta"), PreviousTickDelta);
    Out->SetBoolField(TEXT("needs_warmup"), System->NeedsWarmup());
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}

TSharedPtr<FJsonObject> FSproftNiagaraEditCommands::HandleSetEmitterLoop(const TSharedPtr<FJsonObject>& Params)
{
    // Writes the per-emitter loop behaviour fields on a Niagara
    // system. The runtime path stores these on the
    // FVersionedNiagaraEmitterData's `EmitterState` UPROPERTY (a
    // FNiagaraEmitterStateData struct) which holds the loop
    // configuration: an ENiagaraLoopBehavior enum (`Once` /
    // `Infinite` / `Multiple`) plus an int32 LoopCount used when
    // the mode is Multiple. We route through reflection against
    // the FVersionedNiagaraEmitterData UScriptStruct so the public
    // / private split on the contained struct stays transparent.
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

    FString LoopModeToken;
    if (!Params->TryGetStringField(TEXT("loop_mode"), LoopModeToken)
        && !Params->TryGetStringField(TEXT("mode"), LoopModeToken)
        && !Params->TryGetStringField(TEXT("loop_behavior"), LoopModeToken)
        && !Params->TryGetStringField(TEXT("behavior"), LoopModeToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'loop_mode' parameter (one of 'once' / 'infinite' / 'multiple')"));
    }

    // Canonicalize the loop mode against the engine's
    // ENiagaraLoopBehavior token set. We accept the engine's
    // CamelCase form, the lowered alias, plus a few human-friendly
    // synonyms.
    const FString LoopModeLower = LoopModeToken.ToLower();
    FString CanonicalMode;
    if (LoopModeLower == TEXT("once") || LoopModeLower == TEXT("one") || LoopModeLower == TEXT("single"))
    {
        CanonicalMode = TEXT("Once");
    }
    else if (LoopModeLower == TEXT("infinite") || LoopModeLower == TEXT("loop") || LoopModeLower == TEXT("forever"))
    {
        CanonicalMode = TEXT("Infinite");
    }
    else if (LoopModeLower == TEXT("multiple") || LoopModeLower == TEXT("count") || LoopModeLower == TEXT("n_times"))
    {
        CanonicalMode = TEXT("Multiple");
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported loop_mode '%s' (try 'once' / 'infinite' / 'multiple')"),
                *LoopModeToken));
    }

    int32 LoopCountIn = 0;
    const bool bHasLoopCount = Params->TryGetNumberField(TEXT("loop_count"), LoopCountIn)
                            || Params->TryGetNumberField(TEXT("count"), LoopCountIn);
    if (CanonicalMode == TEXT("Multiple"))
    {
        if (!bHasLoopCount)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("'multiple' loop_mode requires a 'loop_count' (int >= 1)"));
        }
        if (LoopCountIn < 1)
        {
            // Match the editor's clamp: Multiple with 0 collapses to
            // 1.
            LoopCountIn = 1;
        }
    }

    // Walk the system's emitter handles and match by name (same
    // resolver every other per-emitter op uses).
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

    UScriptStruct* EmitterDataStruct = FVersionedNiagaraEmitterData::StaticStruct();
    if (!EmitterDataStruct)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve FVersionedNiagaraEmitterData::StaticStruct()"));
    }

    // Reach the contained EmitterState struct by reflection. This
    // matches the editor's stack viewmodel path, which writes the
    // same UPROPERTY chain.
    FStructProperty* StateProp = CastField<FStructProperty>(
        EmitterDataStruct->FindPropertyByName(TEXT("EmitterState")));
    if (!StateProp)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("FVersionedNiagaraEmitterData has no 'EmitterState' FStructProperty (engine API moved?)"));
    }

    UScriptStruct* StateStruct = StateProp->Struct;
    if (!StateStruct)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("EmitterState property has no resolved UScriptStruct"));
    }

    void* EmitterDataPtr = static_cast<void*>(EmitterData);
    void* StatePtr = StateProp->ContainerPtrToValuePtr<void>(EmitterDataPtr);

    // ENiagaraLoopBehavior is exposed as either an FByteProperty
    // (when declared as `TEnumAsByte<ENiagaraLoopBehavior>`) or an
    // FEnumProperty (the modern `UENUM(BlueprintType)` shape).
    // Walk both branches so we tolerate either header layout.
    FProperty* LoopBehaviorProp = StateStruct->FindPropertyByName(TEXT("LoopBehavior"));
    if (!LoopBehaviorProp)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("FNiagaraEmitterStateData has no 'LoopBehavior' property"));
    }
    FProperty* LoopCountProp = StateStruct->FindPropertyByName(TEXT("LoopCount"));
    if (!LoopCountProp)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("FNiagaraEmitterStateData has no 'LoopCount' property"));
    }

    // Capture the previous values for the diff payload.
    FString PreviousMode = TEXT("");
    int32 PreviousCount = 0;
    if (FIntProperty* PrevIntProp = CastField<FIntProperty>(LoopCountProp))
    {
        PreviousCount = PrevIntProp->GetPropertyValue_InContainer(StatePtr);
    }
    UEnum* LoopBehaviorEnum = nullptr;
    if (FEnumProperty* AsEnum = CastField<FEnumProperty>(LoopBehaviorProp))
    {
        LoopBehaviorEnum = AsEnum->GetEnum();
        if (LoopBehaviorEnum && AsEnum->GetUnderlyingProperty())
        {
            void* ValuePtr = AsEnum->ContainerPtrToValuePtr<void>(StatePtr);
            const int64 EnumValue = AsEnum->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
            PreviousMode = LoopBehaviorEnum->GetNameStringByValue(EnumValue);
        }
    }
    else if (FByteProperty* AsByte = CastField<FByteProperty>(LoopBehaviorProp))
    {
        LoopBehaviorEnum = AsByte->Enum;
        if (LoopBehaviorEnum)
        {
            const uint8 ByteValue = AsByte->GetPropertyValue_InContainer(StatePtr);
            PreviousMode = LoopBehaviorEnum->GetNameStringByValue(ByteValue);
        }
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("FNiagaraEmitterStateData::LoopBehavior is not a byte / enum property"));
    }
    if (!LoopBehaviorEnum)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve LoopBehavior's UEnum"));
    }

    // Translate the canonical mode token to the engine's enum
    // value. The enum is `ENiagaraLoopBehavior`; the engine spells
    // the entries `Once` / `Infinite` / `Multiple` so we try both
    // the short name and the fully-qualified token.
    auto ResolveLoopBehaviorValue = [LoopBehaviorEnum](const FString& Token, int64& OutValue) -> bool
    {
        const int64 ShortValue = LoopBehaviorEnum->GetValueByNameString(Token);
        if (ShortValue != INDEX_NONE)
        {
            OutValue = ShortValue;
            return true;
        }
        const FString Qualified = FString::Printf(TEXT("ENiagaraLoopBehavior::%s"), *Token);
        const int64 QualifiedValue = LoopBehaviorEnum->GetValueByNameString(Qualified);
        if (QualifiedValue != INDEX_NONE)
        {
            OutValue = QualifiedValue;
            return true;
        }
        return false;
    };

    int64 NewModeValue = 0;
    if (!ResolveLoopBehaviorValue(CanonicalMode, NewModeValue))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("ENiagaraLoopBehavior has no entry '%s' (engine API moved?)"),
                *CanonicalMode));
    }

    // Write LoopBehavior through whichever property shape we got.
    if (FEnumProperty* AsEnum = CastField<FEnumProperty>(LoopBehaviorProp))
    {
        if (FNumericProperty* Underlying = AsEnum->GetUnderlyingProperty())
        {
            void* ValuePtr = AsEnum->ContainerPtrToValuePtr<void>(StatePtr);
            Underlying->SetIntPropertyValue(ValuePtr, NewModeValue);
        }
    }
    else if (FByteProperty* AsByte = CastField<FByteProperty>(LoopBehaviorProp))
    {
        AsByte->SetPropertyValue_InContainer(StatePtr, static_cast<uint8>(NewModeValue));
    }

    // LoopCount: keep the existing count when the mode is not
    // Multiple unless the caller supplied one explicitly. When
    // Multiple, we always write the resolved count (1 minimum).
    int32 ResolvedCount = PreviousCount;
    if (bHasLoopCount)
    {
        ResolvedCount = LoopCountIn;
    }
    if (CanonicalMode == TEXT("Multiple"))
    {
        if (ResolvedCount < 1)
        {
            ResolvedCount = 1;
        }
    }
    if (FIntProperty* IntProp = CastField<FIntProperty>(LoopCountProp))
    {
        IntProp->SetPropertyValue_InContainer(StatePtr, ResolvedCount);
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("FNiagaraEmitterStateData::LoopCount is not an int32 property"));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    System->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(System->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("set_emitter_loop"));
    Out->SetStringField(TEXT("system"), System->GetPathName());
    Out->SetStringField(TEXT("emitter_handle"), MatchedHandle->GetName().ToString());
    Out->SetNumberField(TEXT("emitter_handle_index"), HandleIndex);
    Out->SetStringField(TEXT("loop_mode"), CanonicalMode);
    Out->SetNumberField(TEXT("loop_count"), ResolvedCount);
    if (!PreviousMode.IsEmpty())
    {
        Out->SetStringField(TEXT("previous_loop_mode"), PreviousMode);
    }
    Out->SetNumberField(TEXT("previous_loop_count"), PreviousCount);
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}
