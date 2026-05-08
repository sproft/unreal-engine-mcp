#include "Commands/SproftBpComponentCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/OutputDeviceNull.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/UnrealType.h"

namespace
{
    /** Resolve a class name or path into a UActorComponent subclass. Tries
     *  full paths first, then a short-name probe in the loaded class set, then
     *  several Engine-namespace fallbacks with optional `U` prefix and
     *  `Component` suffix. Returns nullptr on failure. */
    UClass* ResolveComponentClass(const FString& InClassPath)
    {
        if (InClassPath.IsEmpty())
        {
            return nullptr;
        }

        // Full object path of the form `/Script/Module.ClassName`.
        if (InClassPath.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadClass<UActorComponent>(nullptr, *InClassPath))
            {
                return Loaded;
            }
            // BP-class path: `/Game/Foo/MyComp.MyComp_C`
            const FString WithSuffix = InClassPath + TEXT("_C");
            if (UClass* LoadedSuffix = LoadClass<UActorComponent>(nullptr, *WithSuffix))
            {
                return LoadedSuffix;
            }
            return nullptr;
        }

        TArray<FString> Candidates;
        Candidates.Add(InClassPath);
        if (!InClassPath.StartsWith(TEXT("U")))
        {
            Candidates.Add(TEXT("U") + InClassPath);
        }
        if (!InClassPath.EndsWith(TEXT("Component")))
        {
            Candidates.Add(InClassPath + TEXT("Component"));
            if (!InClassPath.StartsWith(TEXT("U")))
            {
                Candidates.Add(TEXT("U") + InClassPath + TEXT("Component"));
            }
        }

        // Try the in-memory class set first.
        for (const FString& Candidate : Candidates)
        {
            if (UClass* Found = FindObject<UClass>(nullptr, *Candidate))
            {
                if (Found->IsChildOf(UActorComponent::StaticClass()))
                {
                    return Found;
                }
            }
        }

        // Then probe the Engine and EnhancedInput namespaces (the latter ships
        // UInputComponent subclasses we care about).
        const TArray<FString> Namespaces = {
            TEXT("/Script/Engine.%s"),
            TEXT("/Script/EnhancedInput.%s"),
            TEXT("/Script/UMG.%s"),
        };
        for (const FString& Candidate : Candidates)
        {
            for (const FString& Format : Namespaces)
            {
                const FString Path = FString::Printf(*Format, *Candidate);
                if (UClass* Loaded = LoadClass<UActorComponent>(nullptr, *Path))
                {
                    return Loaded;
                }
            }
        }

        return nullptr;
    }

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
}

FSproftBpComponentCommands::FSproftBpComponentCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpComponentCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_component"))
    {
        return HandleBpComponent(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_component command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpComponentCommands::HandleBpComponent(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Operation;
    if (!Params->TryGetStringField(TEXT("operation"), Operation))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'operation' parameter"));
    }
    Operation = Operation.ToLower();

    if (Operation == TEXT("add_component") || Operation == TEXT("add"))
    {
        return AddComponent(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported bp_component operation '%s'. Supported: add_component"), *Operation));
}

TSharedPtr<FJsonObject> FSproftBpComponentCommands::AddComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint"), BlueprintName)
        && !Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint' parameter"));
    }

    FString ComponentClassName;
    if (!Params->TryGetStringField(TEXT("component_class"), ComponentClassName)
        && !Params->TryGetStringField(TEXT("class"), ComponentClassName)
        && !Params->TryGetStringField(TEXT("component_type"), ComponentClassName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_class' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName)
        && !Params->TryGetStringField(TEXT("name"), ComponentName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }

    FString ParentComponentName;
    Params->TryGetStringField(TEXT("parent_component"), ParentComponentName);

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UBlueprint* Blueprint = FEpicUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    if (!SCS)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript (not an Actor-class Blueprint)"), *Blueprint->GetName()));
    }

    UClass* ComponentClass = ResolveComponentClass(ComponentClassName);
    if (!ComponentClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve component class '%s'. Pass a short class name or a full /Script/Module.ClassName path."), *ComponentClassName));
    }
    if (ComponentClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Component class '%s' is abstract and cannot be instantiated"), *ComponentClass->GetPathName()));
    }
    if (!ComponentClass->IsChildOf(UActorComponent::StaticClass()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is not a UActorComponent subclass"), *ComponentClass->GetPathName()));
    }

    // Reject duplicate component variable names ahead of time so we do not
    // silently create a node that the editor will then have to disambiguate.
    if (SCS->FindSCSNode(*ComponentName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Component '%s' already exists on Blueprint '%s'"), *ComponentName, *Blueprint->GetName()));
    }

    USCS_Node* NewNode = SCS->CreateNode(ComponentClass, *ComponentName);
    if (!NewNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create SCS node for class '%s'"), *ComponentClass->GetName()));
    }

    // Apply optional relative transform on the template if it is a scene component.
    USceneComponent* SceneTemplate = Cast<USceneComponent>(NewNode->ComponentTemplate);
    if (SceneTemplate)
    {
        if (Params->HasField(TEXT("location")))
        {
            SceneTemplate->SetRelativeLocation(
                FEpicUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
        }
        if (Params->HasField(TEXT("rotation")))
        {
            SceneTemplate->SetRelativeRotation(
                FEpicUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation")));
        }
        if (Params->HasField(TEXT("scale")))
        {
            SceneTemplate->SetRelativeScale3D(
                FEpicUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")));
        }
    }

    // Apply optional flat property dict via reflection on the template object.
    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid()
        && NewNode->ComponentTemplate)
    {
        UActorComponent* Template = NewNode->ComponentTemplate;
        FOutputDeviceNull NullDevice;
        for (const auto& Pair : (*PropsObj)->Values)
        {
            const FString& PropName = Pair.Key;
            const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

            FProperty* Prop = FindFProperty<FProperty>(Template->GetClass(), *PropName);
            if (!Prop)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), PropName);
                Skip->SetStringField(TEXT("reason"), TEXT("not_a_uproperty"));
                SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }

            const FString TextValue = JsonValueToImportText(JsonVal);
            const TCHAR* TextPtr = *TextValue;
            const TCHAR* Result = Prop->ImportText_InContainer(
                TextPtr, Template, Template, PPF_None, &NullDevice);
            if (Result == nullptr)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), PropName);
                Skip->SetStringField(TEXT("reason"), TEXT("import_text_failed"));
                Skip->SetStringField(TEXT("attempted_value"), TextValue);
                SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }

            TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
            Applied->SetStringField(TEXT("name"), PropName);
            Applied->SetStringField(TEXT("type"), Prop->GetCPPType());
            AppliedJson.Add(MakeShared<FJsonValueObject>(Applied));
        }
    }

    // Decide attachment. If a parent_component name was supplied and resolves
    // to a scene-component SCS node, attach there. Otherwise add at the root.
    USCS_Node* ResolvedParent = nullptr;
    bool bAttachedAsRoot = false;
    if (!ParentComponentName.IsEmpty())
    {
        ResolvedParent = SCS->FindSCSNode(*ParentComponentName);
        if (!ResolvedParent)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Parent component '%s' not found on Blueprint '%s'"), *ParentComponentName, *Blueprint->GetName()));
        }
        if (!Cast<USceneComponent>(ResolvedParent->ComponentTemplate))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Parent component '%s' is not a scene component"), *ParentComponentName));
        }
        if (!SceneTemplate)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Cannot attach non-scene component '%s' to parent '%s'"), *ComponentClass->GetName(), *ParentComponentName));
        }
        ResolvedParent->AddChildNode(NewNode);
    }
    else
    {
        SCS->AddNode(NewNode);
        bAttachedAsRoot = true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_component"));
    ResultObj->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    ResultObj->SetStringField(TEXT("component_name"), ComponentName);
    ResultObj->SetStringField(TEXT("component_class"), ComponentClass->GetPathName());
    ResultObj->SetBoolField(TEXT("is_scene_component"), SceneTemplate != nullptr);
    if (ResolvedParent)
    {
        ResultObj->SetStringField(TEXT("parent_component"), ResolvedParent->GetVariableName().ToString());
    }
    ResultObj->SetBoolField(TEXT("attached_as_root"), bAttachedAsRoot);
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetArrayField(TEXT("skipped"), SkippedJson);
    ResultObj->SetBoolField(TEXT("compiled"), bCompile);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}
