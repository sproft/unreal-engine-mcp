#include "Commands/SproftBpCreateCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
    /** Convert an FJsonValue into a textual form FProperty::ImportText accepts. */
    FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return FString();
        }
        switch (Value->Type)
        {
            case EJson::String:
                return Value->AsString();
            case EJson::Number:
                return LexToString(Value->AsNumber());
            case EJson::Boolean:
                return Value->AsBool() ? TEXT("true") : TEXT("false");
            case EJson::Null:
                return TEXT("None");
            default:
            {
                FString Buffer;
                TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
                    TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Buffer);
                FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
                return Buffer;
            }
        }
    }

    /** Walk a small set of well-known short names to their UClass. */
    UClass* TryWellKnownShortName(const FString& Name)
    {
        struct FEntry
        {
            const TCHAR* Key;
            const TCHAR* Path;
        };
        static const FEntry Table[] = {
            { TEXT("Actor"),               TEXT("/Script/Engine.Actor") },
            { TEXT("AActor"),              TEXT("/Script/Engine.Actor") },
            { TEXT("Pawn"),                TEXT("/Script/Engine.Pawn") },
            { TEXT("APawn"),               TEXT("/Script/Engine.Pawn") },
            { TEXT("Character"),           TEXT("/Script/Engine.Character") },
            { TEXT("ACharacter"),          TEXT("/Script/Engine.Character") },
            { TEXT("GameMode"),            TEXT("/Script/Engine.GameMode") },
            { TEXT("AGameMode"),           TEXT("/Script/Engine.GameMode") },
            { TEXT("GameModeBase"),        TEXT("/Script/Engine.GameModeBase") },
            { TEXT("AGameModeBase"),       TEXT("/Script/Engine.GameModeBase") },
            { TEXT("GameState"),           TEXT("/Script/Engine.GameStateBase") },
            { TEXT("AGameStateBase"),      TEXT("/Script/Engine.GameStateBase") },
            { TEXT("PlayerController"),    TEXT("/Script/Engine.PlayerController") },
            { TEXT("APlayerController"),   TEXT("/Script/Engine.PlayerController") },
            { TEXT("PlayerState"),         TEXT("/Script/Engine.PlayerState") },
            { TEXT("APlayerState"),        TEXT("/Script/Engine.PlayerState") },
            { TEXT("AIController"),        TEXT("/Script/AIModule.AIController") },
            { TEXT("AAIController"),       TEXT("/Script/AIModule.AIController") },
            { TEXT("HUD"),                 TEXT("/Script/Engine.HUD") },
            { TEXT("AHUD"),                TEXT("/Script/Engine.HUD") },
            { TEXT("ActorComponent"),      TEXT("/Script/Engine.ActorComponent") },
            { TEXT("UActorComponent"),     TEXT("/Script/Engine.ActorComponent") },
            { TEXT("SceneComponent"),      TEXT("/Script/Engine.SceneComponent") },
            { TEXT("USceneComponent"),     TEXT("/Script/Engine.SceneComponent") },
            { TEXT("UserWidget"),          TEXT("/Script/UMG.UserWidget") },
            { TEXT("UUserWidget"),         TEXT("/Script/UMG.UserWidget") },
            { TEXT("Object"),              TEXT("/Script/CoreUObject.Object") },
            { TEXT("UObject"),             TEXT("/Script/CoreUObject.Object") },
            { TEXT("DataAsset"),           TEXT("/Script/Engine.DataAsset") },
            { TEXT("UDataAsset"),          TEXT("/Script/Engine.DataAsset") },
            { TEXT("PrimaryDataAsset"),    TEXT("/Script/Engine.PrimaryDataAsset") },
            { TEXT("UPrimaryDataAsset"),   TEXT("/Script/Engine.PrimaryDataAsset") },
            { TEXT("BlueprintFunctionLibrary"),  TEXT("/Script/Engine.BlueprintFunctionLibrary") },
            { TEXT("UBlueprintFunctionLibrary"), TEXT("/Script/Engine.BlueprintFunctionLibrary") },
        };
        for (const FEntry& E : Table)
        {
            if (Name == E.Key)
            {
                return LoadClass<UObject>(nullptr, E.Path);
            }
        }
        return nullptr;
    }

    /** Resolve a short name, full /Script path, or /Game BP path to a UClass. */
    UClass* ResolveParentClass(const FString& Input)
    {
        const FString Trimmed = Input.TrimStartAndEnd();
        if (Trimmed.IsEmpty())
        {
            return AActor::StaticClass();
        }

        // Full /Script/Module.ClassName path.
        if (Trimmed.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *Trimmed))
            {
                return Loaded;
            }
        }
        // /Game-rooted Blueprint class path; resolves the generated class.
        if (Trimmed.StartsWith(TEXT("/Game/")))
        {
            FString WithSuffix = Trimmed;
            if (!WithSuffix.EndsWith(TEXT("_C")))
            {
                WithSuffix += TEXT("_C");
            }
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *WithSuffix))
            {
                return Loaded;
            }
        }
        // Well-known short names.
        if (UClass* Hit = TryWellKnownShortName(Trimmed))
        {
            return Hit;
        }
        // In-memory class set probe with optional A/U prefix variants.
        TArray<FString> Candidates;
        Candidates.Add(Trimmed);
        if (!Trimmed.StartsWith(TEXT("A")) && !Trimmed.StartsWith(TEXT("U")))
        {
            Candidates.Add(TEXT("A") + Trimmed);
            Candidates.Add(TEXT("U") + Trimmed);
        }
        for (const FString& Candidate : Candidates)
        {
            if (UClass* Found = FindObject<UClass>(nullptr, *Candidate))
            {
                return Found;
            }
        }
        // Fallback to /Script/Engine.Foo.
        for (const FString& Candidate : Candidates)
        {
            const FString EnginePath = FString::Printf(TEXT("/Script/Engine.%s"), *Candidate);
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *EnginePath))
            {
                return Loaded;
            }
        }
        return nullptr;
    }

    /** Apply a flat property dict to the CDO with FProperty::ImportText. */
    void ApplyPropertyDictToCDO(
        UObject* DefaultObject,
        const TSharedPtr<FJsonObject>& PropsObj,
        TArray<TSharedPtr<FJsonValue>>& OutApplied,
        TArray<TSharedPtr<FJsonValue>>& OutSkipped)
    {
        if (!DefaultObject || !PropsObj.IsValid())
        {
            return;
        }
        FOutputDeviceNull NullDevice;
        for (const auto& Pair : PropsObj->Values)
        {
            const FString& PropName = Pair.Key;
            const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

            FProperty* Prop = FindFProperty<FProperty>(DefaultObject->GetClass(), *PropName);
            if (!Prop)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), PropName);
                Skip->SetStringField(TEXT("reason"), TEXT("not_a_uproperty"));
                OutSkipped.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }
            const FString TextValue = JsonValueToImportText(JsonVal);
            const TCHAR* TextPtr = *TextValue;
            const TCHAR* Result = Prop->ImportText_InContainer(
                TextPtr, DefaultObject, DefaultObject, PPF_None, &NullDevice);
            if (!Result)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), PropName);
                Skip->SetStringField(TEXT("reason"), TEXT("import_text_failed"));
                Skip->SetStringField(TEXT("attempted_value"), TextValue);
                OutSkipped.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }
            TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
            Applied->SetStringField(TEXT("name"), PropName);
            Applied->SetStringField(TEXT("type"), Prop->GetCPPType());
            OutApplied.Add(MakeShared<FJsonValueObject>(Applied));
        }
    }
}

FSproftBpCreateCommands::FSproftBpCreateCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpCreateCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_create"))
    {
        return HandleBpCreate(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_create command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpCreateCommands::HandleBpCreate(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Operation = TEXT("create");
    Params->TryGetStringField(TEXT("operation"), Operation);
    Operation = Operation.ToLower();

    if (Operation == TEXT("create") || Operation.IsEmpty())
    {
        return CreateBlueprint(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported bp_create operation '%s'. Supported: create"), *Operation));
}

TSharedPtr<FJsonObject> FSproftBpCreateCommands::CreateBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName;
    if (!Params->TryGetStringField(TEXT("name"), AssetName)
        && !Params->TryGetStringField(TEXT("blueprint_name"), AssetName)
        && !Params->TryGetStringField(TEXT("asset_name"), AssetName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }
    AssetName.TrimStartAndEndInline();
    if (AssetName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'name' parameter is empty"));
    }
    if (!FName::IsValidXName(AssetName, INVALID_OBJECTNAME_CHARACTERS))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset name '%s' contains invalid object characters"), *AssetName));
    }

    FString PackageRoot = TEXT("/Game/Blueprints");
    Params->TryGetStringField(TEXT("path"), PackageRoot);
    if (Params->HasField(TEXT("package_path")))
    {
        Params->TryGetStringField(TEXT("package_path"), PackageRoot);
    }
    PackageRoot.TrimStartAndEndInline();
    if (PackageRoot.IsEmpty())
    {
        PackageRoot = TEXT("/Game/Blueprints");
    }
    while (PackageRoot.EndsWith(TEXT("/")))
    {
        PackageRoot.LeftChopInline(1, EAllowShrinking::No);
    }
    if (!PackageRoot.StartsWith(TEXT("/Game")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Path '%s' must start with /Game"), *PackageRoot));
    }

    FString ParentClassInput;
    Params->TryGetStringField(TEXT("parent_class"), ParentClassInput);
    if (ParentClassInput.IsEmpty())
    {
        Params->TryGetStringField(TEXT("parent"), ParentClassInput);
    }
    UClass* ParentClass = ResolveParentClass(ParentClassInput);
    if (!ParentClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve parent class '%s'"), *ParentClassInput));
    }
    if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Cannot create a Blueprint subclass of '%s'"), *ParentClass->GetPathName()));
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

    const FString PackagePath = FString::Printf(TEXT("%s/%s"), *PackageRoot, *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

    UBlueprint* ExistingBP = nullptr;
    if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
    {
        if (!bOverwrite)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Asset '%s' already exists; pass overwrite=true to reuse it"), *PackagePath));
        }
        ExistingBP = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(PackagePath));
        if (!ExistingBP)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Existing asset at '%s' is not a Blueprint"), *PackagePath));
        }
    }

    UBlueprint* NewBP = ExistingBP;
    bool bWasCreated = false;
    if (!NewBP)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Failed to create package at '%s'"), *PackagePath));
        }
        NewBP = FKismetEditorUtilities::CreateBlueprint(
            ParentClass, Package, FName(*AssetName), BPTYPE_Normal, FName(TEXT("SproftBpCreate")));
        if (!NewBP)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("FKismetEditorUtilities::CreateBlueprint returned null for '%s'"), *AssetName));
        }
        NewBP->SetFlags(RF_Standalone | RF_Public);
        FAssetRegistryModule::AssetCreated(NewBP);
        Package->MarkPackageDirty();
        bWasCreated = true;
    }

    // Optional flat property dict on the CDO.
    TArray<TSharedPtr<FJsonValue>> AppliedProps;
    TArray<TSharedPtr<FJsonValue>> SkippedProps;
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && PropsObj->IsValid())
    {
        UClass* GenClass = NewBP->GeneratedClass ? NewBP->GeneratedClass : NewBP->ParentClass;
        UObject* CDO = GenClass ? GenClass->GetDefaultObject(/*bCreateIfNeeded=*/true) : nullptr;
        if (CDO)
        {
            ApplyPropertyDictToCDO(CDO, *PropsObj, AppliedProps, SkippedProps);
            FBlueprintEditorUtils::MarkBlueprintAsModified(NewBP);
        }
    }

    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(NewBP);
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(PackagePath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("create"));
    Result->SetStringField(TEXT("name"), AssetName);
    Result->SetStringField(TEXT("path"), PackagePath);
    Result->SetStringField(TEXT("object_path"), ObjectPath);
    Result->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    Result->SetStringField(TEXT("parent_class_short"), ParentClass->GetName());
    Result->SetBoolField(TEXT("created"), bWasCreated);
    Result->SetBoolField(TEXT("reused_existing"), !bWasCreated);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    Result->SetArrayField(TEXT("applied"), AppliedProps);
    Result->SetArrayField(TEXT("skipped"), SkippedProps);
    return Result;
}
