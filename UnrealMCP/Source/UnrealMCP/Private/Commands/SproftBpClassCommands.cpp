#include "Commands/SproftBpClassCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Class.h"
#include "UObject/Interface.h"
#include "UObject/TopLevelAssetPath.h"

namespace
{
    UBlueprint* BpClass_ResolveBlueprintParam(const TSharedPtr<FJsonObject>& Params)
    {
        FString Input;
        if (!Params->TryGetStringField(TEXT("blueprint"), Input)
            && !Params->TryGetStringField(TEXT("blueprint_path"), Input)
            && !Params->TryGetStringField(TEXT("blueprint_name"), Input))
        {
            return nullptr;
        }
        UBlueprint* BP = FEpicUnrealMCPCommonUtils::FindBlueprint(Input);
        if (!BP && Input.StartsWith(TEXT("/")))
        {
            BP = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(Input));
        }
        return BP;
    }

    /** Walk a small set of well-known short names to their UClass. Mirrors bp_create. */
    UClass* BpClass_TryWellKnownShortName(const FString& Name)
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

    /** Resolve a parent class spec mirroring bp_create's BpClass_ResolveParentClass. */
    UClass* BpClass_ResolveParentClass(const FString& Input)
    {
        const FString Trimmed = Input.TrimStartAndEnd();
        if (Trimmed.IsEmpty())
        {
            return nullptr;
        }
        if (Trimmed.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *Trimmed))
            {
                return Loaded;
            }
        }
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
        if (UClass* Hit = BpClass_TryWellKnownShortName(Trimmed))
        {
            return Hit;
        }
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

    /** Resolve an interface specifier into a UInterface UClass and matching FTopLevelAssetPath. */
    UClass* ResolveInterfaceClass(const FString& Input, FTopLevelAssetPath& OutPath, FString& OutError)
    {
        const FString Trimmed = Input.TrimStartAndEnd();
        if (Trimmed.IsEmpty())
        {
            OutError = TEXT("interface path is empty");
            return nullptr;
        }
        UClass* IfaceClass = nullptr;

        // Full /Script/Module.IName path.
        if (Trimmed.StartsWith(TEXT("/Script/")))
        {
            IfaceClass = LoadClass<UObject>(nullptr, *Trimmed);
            if (IfaceClass)
            {
                OutPath = FTopLevelAssetPath(*Trimmed);
                return IfaceClass;
            }
        }
        // /Game/ Blueprint Interface.
        if (Trimmed.StartsWith(TEXT("/Game/")))
        {
            FString WithSuffix = Trimmed;
            if (!WithSuffix.EndsWith(TEXT("_C")))
            {
                WithSuffix += TEXT("_C");
            }
            IfaceClass = LoadClass<UObject>(nullptr, *WithSuffix);
            if (IfaceClass)
            {
                // FTopLevelAssetPath wants the package + asset (no _C suffix).
                FString Package, ObjectName;
                Trimmed.Split(TEXT("."), &Package, &ObjectName);
                if (Package.IsEmpty())
                {
                    Package = Trimmed;
                    int32 LastSlash = INDEX_NONE;
                    Trimmed.FindLastChar('/', LastSlash);
                    if (LastSlash != INDEX_NONE)
                    {
                        ObjectName = Trimmed.Mid(LastSlash + 1);
                    }
                }
                if (ObjectName.IsEmpty())
                {
                    int32 LastSlash = INDEX_NONE;
                    Package.FindLastChar('/', LastSlash);
                    if (LastSlash != INDEX_NONE)
                    {
                        ObjectName = Package.Mid(LastSlash + 1);
                    }
                }
                OutPath = FTopLevelAssetPath(*Package, *ObjectName);
                return IfaceClass;
            }
        }
        // Short name fallback.
        TArray<FString> Candidates;
        Candidates.Add(Trimmed);
        if (!Trimmed.StartsWith(TEXT("U")) && !Trimmed.StartsWith(TEXT("I")))
        {
            Candidates.Add(TEXT("U") + Trimmed);
            Candidates.Add(TEXT("I") + Trimmed);
        }
        for (const FString& Candidate : Candidates)
        {
            UClass* Found = FindObject<UClass>(nullptr, *Candidate);
            if (Found && Found->HasAnyClassFlags(CLASS_Interface))
            {
                OutPath = FTopLevelAssetPath(Found->GetPathName());
                return Found;
            }
        }
        OutError = FString::Printf(TEXT("Could not resolve interface '%s'"), *Trimmed);
        return nullptr;
    }
}

FSproftBpClassCommands::FSproftBpClassCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpClassCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_class"))
    {
        return HandleBpClass(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_class command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpClassCommands::HandleBpClass(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Operation;
    if (!Params->TryGetStringField(TEXT("op"), Operation)
        && !Params->TryGetStringField(TEXT("operation"), Operation))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'op' parameter"));
    }
    Operation = Operation.ToLower();

    UBlueprint* Blueprint = BpClass_ResolveBlueprintParam(Params);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Blueprint not found or 'blueprint' parameter missing"));
    }

    if (Operation == TEXT("read") || Operation == TEXT("get"))
    {
        return ReadClass(Blueprint);
    }
    if (Operation == TEXT("set_parent") || Operation == TEXT("reparent"))
    {
        return SetParent(Blueprint, Params);
    }
    if (Operation == TEXT("set_class_settings") || Operation == TEXT("set_settings"))
    {
        return SetClassSettings(Blueprint, Params);
    }
    if (Operation == TEXT("add_interface"))
    {
        return AddInterface(Blueprint, Params);
    }
    if (Operation == TEXT("remove_interface"))
    {
        return RemoveInterface(Blueprint, Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported bp_class op '%s'. Supported: read, set_parent, set_class_settings, add_interface, remove_interface"), *Operation));
}

TSharedPtr<FJsonObject> FSproftBpClassCommands::ReadClass(UBlueprint* Blueprint)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("read"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    if (Blueprint->ParentClass)
    {
        Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetPathName());
        Result->SetStringField(TEXT("parent_class_short"), Blueprint->ParentClass->GetName());
    }
    Result->SetNumberField(TEXT("blueprint_type"), (double)Blueprint->BlueprintType);
#if WITH_EDITORONLY_DATA
    Result->SetStringField(TEXT("display_name"), Blueprint->BlueprintDisplayName);
    Result->SetStringField(TEXT("description"), Blueprint->BlueprintDescription);
    Result->SetStringField(TEXT("namespace"), Blueprint->BlueprintNamespace);
    Result->SetStringField(TEXT("category"), Blueprint->BlueprintCategory);
    TArray<TSharedPtr<FJsonValue>> Hide;
    for (const FString& C : Blueprint->HideCategories)
    {
        Hide.Add(MakeShared<FJsonValueString>(C));
    }
    Result->SetArrayField(TEXT("hide_categories"), Hide);
#endif

    TArray<TSharedPtr<FJsonValue>> Interfaces;
    for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
    {
        TSharedPtr<FJsonObject> IfaceObj = MakeShared<FJsonObject>();
        if (Iface.Interface)
        {
            IfaceObj->SetStringField(TEXT("name"), Iface.Interface->GetName());
            IfaceObj->SetStringField(TEXT("path"), Iface.Interface->GetPathName());
        }
        else
        {
            IfaceObj->SetStringField(TEXT("name"), TEXT("(unresolved)"));
        }
        Interfaces.Add(MakeShared<FJsonValueObject>(IfaceObj));
    }
    Result->SetArrayField(TEXT("interfaces"), Interfaces);
    Result->SetBoolField(TEXT("is_data_only"), FBlueprintEditorUtils::IsDataOnlyBlueprint(Blueprint));
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpClassCommands::SetParent(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    FString ParentInput;
    if (!Params->TryGetStringField(TEXT("parent_class"), ParentInput)
        && !Params->TryGetStringField(TEXT("parent"), ParentInput))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'parent_class' parameter is required"));
    }
    UClass* NewParent = BpClass_ResolveParentClass(ParentInput);
    if (!NewParent)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve parent class '%s'"), *ParentInput));
    }
    if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(NewParent))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Cannot reparent to '%s' (not a valid Blueprint parent)"), *NewParent->GetPathName()));
    }
    if (NewParent == Blueprint->ParentClass)
    {
        TSharedPtr<FJsonObject> Same = MakeShared<FJsonObject>();
        Same->SetStringField(TEXT("operation"), TEXT("set_parent"));
        Same->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
        Same->SetStringField(TEXT("parent_class"), NewParent->GetPathName());
        Same->SetStringField(TEXT("status"), TEXT("unchanged"));
        return Same;
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UClass* OldParent = Blueprint->ParentClass;
    Blueprint->Modify();
    if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
    {
        SCS->Modify();
    }
    Blueprint->ParentClass = NewParent;
    if (UBlueprintGeneratedClass* GenClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass))
    {
        GenClass->PrepareToConformSparseClassData(NewParent->GetSparseClassDataStruct());
    }
    FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("set_parent"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("old_parent_class"), OldParent ? OldParent->GetPathName() : TEXT(""));
    Result->SetStringField(TEXT("new_parent_class"), NewParent->GetPathName());
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpClassCommands::SetClassSettings(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    bool bAnyChange = false;
    TArray<TSharedPtr<FJsonValue>> Updated;

#if WITH_EDITORONLY_DATA
    auto SetStringIf = [&](const TCHAR* Key, FString& Field, const TCHAR* OutKey)
    {
        FString In;
        if (Params->TryGetStringField(Key, In))
        {
            Field = In;
            TSharedPtr<FJsonObject> Hit = MakeShared<FJsonObject>();
            Hit->SetStringField(TEXT("name"), OutKey);
            Hit->SetStringField(TEXT("value"), In);
            Updated.Add(MakeShared<FJsonValueObject>(Hit));
            bAnyChange = true;
        }
    };
    SetStringIf(TEXT("description"), Blueprint->BlueprintDescription, TEXT("description"));
    SetStringIf(TEXT("display_name"), Blueprint->BlueprintDisplayName, TEXT("display_name"));
    SetStringIf(TEXT("namespace"), Blueprint->BlueprintNamespace, TEXT("namespace"));
    SetStringIf(TEXT("category"), Blueprint->BlueprintCategory, TEXT("category"));

    const TArray<TSharedPtr<FJsonValue>>* HideArray = nullptr;
    if (Params->TryGetArrayField(TEXT("hide_categories"), HideArray) && HideArray)
    {
        TArray<FString> NewHide;
        for (const TSharedPtr<FJsonValue>& V : *HideArray)
        {
            if (V.IsValid() && V->Type == EJson::String)
            {
                NewHide.Add(V->AsString());
            }
        }
        Blueprint->HideCategories = NewHide;
        TSharedPtr<FJsonObject> Hit = MakeShared<FJsonObject>();
        Hit->SetStringField(TEXT("name"), TEXT("hide_categories"));
        Hit->SetNumberField(TEXT("count"), NewHide.Num());
        Updated.Add(MakeShared<FJsonValueObject>(Hit));
        bAnyChange = true;
    }
#endif

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    if (bAnyChange)
    {
        Blueprint->MarkPackageDirty();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
        }
        if (bSave)
        {
            UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("set_class_settings"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetArrayField(TEXT("updated"), Updated);
    Result->SetBoolField(TEXT("compiled"), bAnyChange && bCompile);
    Result->SetBoolField(TEXT("saved"), bAnyChange && bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpClassCommands::AddInterface(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    FString InterfaceInput;
    if (!Params->TryGetStringField(TEXT("interface"), InterfaceInput)
        && !Params->TryGetStringField(TEXT("interface_path"), InterfaceInput)
        && !Params->TryGetStringField(TEXT("path"), InterfaceInput))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'interface' parameter is required"));
    }
    FTopLevelAssetPath InterfacePath;
    FString ResolveError;
    UClass* IfaceClass = ResolveInterfaceClass(InterfaceInput, InterfacePath, ResolveError);
    if (!IfaceClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ResolveError);
    }

    // Check for duplicate.
    for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
    {
        if (Iface.Interface == IfaceClass)
        {
            TSharedPtr<FJsonObject> Same = MakeShared<FJsonObject>();
            Same->SetStringField(TEXT("operation"), TEXT("add_interface"));
            Same->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
            Same->SetStringField(TEXT("interface"), IfaceClass->GetPathName());
            Same->SetStringField(TEXT("status"), TEXT("already_implemented"));
            return Same;
        }
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    if (!FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfacePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("ImplementNewInterface failed for '%s'"), *InterfacePath.ToString()));
    }

    Blueprint->MarkPackageDirty();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_interface"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("interface"), IfaceClass->GetPathName());
    Result->SetStringField(TEXT("interface_short"), IfaceClass->GetName());
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpClassCommands::RemoveInterface(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    FString InterfaceInput;
    if (!Params->TryGetStringField(TEXT("interface"), InterfaceInput)
        && !Params->TryGetStringField(TEXT("interface_path"), InterfaceInput)
        && !Params->TryGetStringField(TEXT("path"), InterfaceInput))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'interface' parameter is required"));
    }
    FTopLevelAssetPath InterfacePath;
    FString ResolveError;
    UClass* IfaceClass = ResolveInterfaceClass(InterfaceInput, InterfacePath, ResolveError);
    if (!IfaceClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ResolveError);
    }
    bool bFound = false;
    for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
    {
        if (Iface.Interface == IfaceClass)
        {
            bFound = true;
            break;
        }
    }
    if (!bFound)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint does not implement interface '%s'"), *IfaceClass->GetPathName()));
    }

    bool bPreserve = false;
    Params->TryGetBoolField(TEXT("preserve_functions"), bPreserve);
    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfacePath, bPreserve);

    Blueprint->MarkPackageDirty();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("remove_interface"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("interface"), IfaceClass->GetPathName());
    Result->SetBoolField(TEXT("preserve_functions"), bPreserve);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}
