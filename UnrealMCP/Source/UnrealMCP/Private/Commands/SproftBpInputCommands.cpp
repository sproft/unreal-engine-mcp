#include "Commands/SproftBpInputCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "K2Node_CallFunction.h"
#include "K2Node_EnhancedInputAction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
    /** Split "/Game/Foo/Bar" into ("/Game/Foo/", "Bar"). */
    void BpInput_SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
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

        // Strip a trailing ".AssetName" suffix if the caller passed an object path.
        int32 DotIdx = INDEX_NONE;
        if (OutAssetName.FindChar('.', DotIdx))
        {
            OutAssetName = OutAssetName.Left(DotIdx);
        }
    }

    /** Map a short value-type string to EInputActionValueType. Returns false if unrecognised. */
    bool TryParseInputValueType(const FString& InText, EInputActionValueType& OutType)
    {
        const FString Text = InText.ToLower().TrimStartAndEnd();
        if (Text == TEXT("bool") || Text == TEXT("boolean") || Text == TEXT("digital"))
        {
            OutType = EInputActionValueType::Boolean;
            return true;
        }
        if (Text == TEXT("axis1d") || Text == TEXT("axis_1d") || Text == TEXT("1d") || Text == TEXT("float"))
        {
            OutType = EInputActionValueType::Axis1D;
            return true;
        }
        if (Text == TEXT("axis2d") || Text == TEXT("axis_2d") || Text == TEXT("2d") || Text == TEXT("vector2d"))
        {
            OutType = EInputActionValueType::Axis2D;
            return true;
        }
        if (Text == TEXT("axis3d") || Text == TEXT("axis_3d") || Text == TEXT("3d") || Text == TEXT("vector3d") || Text == TEXT("vector"))
        {
            OutType = EInputActionValueType::Axis3D;
            return true;
        }
        return false;
    }

    /** Convert EInputActionValueType into a stable identifier for JSON results. */
    FString InputValueTypeToString(EInputActionValueType InType)
    {
        switch (InType)
        {
            case EInputActionValueType::Boolean: return TEXT("Boolean");
            case EInputActionValueType::Axis1D:  return TEXT("Axis1D");
            case EInputActionValueType::Axis2D:  return TEXT("Axis2D");
            case EInputActionValueType::Axis3D:  return TEXT("Axis3D");
        }
        return TEXT("Unknown");
    }
}

FSproftBpInputCommands::FSproftBpInputCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_input"))
    {
        return HandleBpInput(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_input command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::HandleBpInput(const TSharedPtr<FJsonObject>& Params)
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

    if (Operation == TEXT("create_input_action") || Operation == TEXT("create_action"))
    {
        return CreateInputAction(Params);
    }
    if (Operation == TEXT("create_input_mapping_context") || Operation == TEXT("create_imc")
        || Operation == TEXT("create_mapping_context"))
    {
        return CreateInputMappingContext(Params);
    }
    if (Operation == TEXT("add_mapping") || Operation == TEXT("map_key"))
    {
        return AddMapping(Params);
    }
    if (Operation == TEXT("add_action_event_node") || Operation == TEXT("add_event_node")
        || Operation == TEXT("wire_action") || Operation == TEXT("add_input_action_event"))
    {
        return AddActionEventNode(Params);
    }
    if (Operation == TEXT("add_action_modifier") || Operation == TEXT("add_modifier")
        || Operation == TEXT("add_mapping_modifier"))
    {
        return AddActionModifier(Params);
    }
    if (Operation == TEXT("add_action_trigger") || Operation == TEXT("add_trigger")
        || Operation == TEXT("add_mapping_trigger"))
    {
        return AddActionTrigger(Params);
    }
    if (Operation == TEXT("add_action_chord") || Operation == TEXT("add_chord")
        || Operation == TEXT("add_mapping_chord") || Operation == TEXT("add_chorded_action"))
    {
        return AddActionChord(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported bp_input operation '%s'. Supported: create_input_action, create_input_mapping_context, add_mapping, add_action_event_node, add_action_modifier, add_action_trigger, add_action_chord"), *Operation));
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::CreateInputAction(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'package_path' parameter"));
    }
    if (!PackagePath.StartsWith(TEXT("/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'package_path' must be an absolute content-browser path, got '%s'"), *PackagePath));
    }

    FString ValueTypeText = TEXT("Boolean");
    Params->TryGetStringField(TEXT("value_type"), ValueTypeText);

    EInputActionValueType ValueType = EInputActionValueType::Boolean;
    if (!TryParseInputValueType(ValueTypeText, ValueType))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported value_type '%s'. Supported: bool, axis1d, axis2d, axis3d"), *ValueTypeText));
    }

    bool bSaveAfterCreate = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterCreate);

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

    bool bTriggerWhenPaused = false;
    Params->TryGetBoolField(TEXT("trigger_when_paused"), bTriggerWhenPaused);

    FString Description;
    Params->TryGetStringField(TEXT("description"), Description);

    FString PackageDir;
    FString AssetName;
    BpInput_SplitPackagePath(PackagePath, PackageDir, AssetName);
    if (AssetName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not derive asset name from '%s'"), *PackagePath));
    }

    const FString AssetObjectPath = PackageDir + AssetName;
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

    UInputAction* NewAction = NewObject<UInputAction>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!NewAction)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UInputAction"));
    }

    NewAction->ValueType = ValueType;
    NewAction->bTriggerWhenPaused = bTriggerWhenPaused;
    if (!Description.IsEmpty())
    {
        NewAction->ActionDescription = FText::FromString(Description);
    }

    FAssetRegistryModule::AssetCreated(NewAction);
    Package->MarkPackageDirty();

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("create_input_action"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetStringField(TEXT("value_type"), InputValueTypeToString(ValueType));
    ResultObj->SetBoolField(TEXT("trigger_when_paused"), bTriggerWhenPaused);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::CreateInputMappingContext(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'package_path' parameter"));
    }
    if (!PackagePath.StartsWith(TEXT("/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'package_path' must be an absolute content-browser path, got '%s'"), *PackagePath));
    }

    bool bSaveAfterCreate = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterCreate);

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

    FString Description;
    Params->TryGetStringField(TEXT("description"), Description);

    FString PackageDir;
    FString AssetName;
    BpInput_SplitPackagePath(PackagePath, PackageDir, AssetName);
    if (AssetName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not derive asset name from '%s'"), *PackagePath));
    }

    const FString AssetObjectPath = PackageDir + AssetName;
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

    UInputMappingContext* NewIMC = NewObject<UInputMappingContext>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!NewIMC)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UInputMappingContext"));
    }

    if (!Description.IsEmpty())
    {
        NewIMC->ContextDescription = FText::FromString(Description);
    }

    FAssetRegistryModule::AssetCreated(NewIMC);
    Package->MarkPackageDirty();

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("create_input_mapping_context"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::AddMapping(const TSharedPtr<FJsonObject>& Params)
{
    FString IMCPath;
    if (!Params->TryGetStringField(TEXT("input_mapping_context"), IMCPath)
        && !Params->TryGetStringField(TEXT("imc"), IMCPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_mapping_context' parameter"));
    }

    FString ActionPath;
    if (!Params->TryGetStringField(TEXT("input_action"), ActionPath)
        && !Params->TryGetStringField(TEXT("action"), ActionPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_action' parameter"));
    }

    FString KeyText;
    if (!Params->TryGetStringField(TEXT("key"), KeyText))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'key' parameter (e.g. \"SpaceBar\", \"W\", \"Gamepad_FaceButton_Bottom\")"));
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    UObject* IMCAsset = UEditorAssetLibrary::LoadAsset(IMCPath);
    UInputMappingContext* IMC = Cast<UInputMappingContext>(IMCAsset);
    if (!IMC)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UInputMappingContext: %s"), *IMCPath));
    }

    UObject* ActionAsset = UEditorAssetLibrary::LoadAsset(ActionPath);
    UInputAction* Action = Cast<UInputAction>(ActionAsset);
    if (!Action)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UInputAction: %s"), *ActionPath));
    }

    const FKey Key(*KeyText);
    if (!Key.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("FKey '%s' is not a known engine key. Pass an FKey FName like 'SpaceBar', 'W', or 'Gamepad_FaceButton_Bottom'."), *KeyText));
    }

    // UInputMappingContext::MapKey is the documented public binding entry
    // point and returns a reference to the new FEnhancedActionKeyMapping.
    FEnhancedActionKeyMapping& NewMapping = IMC->MapKey(Action, Key);
    (void)NewMapping; // Currently unused; kept for clarity.

    if (UPackage* Package = IMC->GetOutermost())
    {
        Package->MarkPackageDirty();
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(IMCPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_mapping"));
    ResultObj->SetStringField(TEXT("input_mapping_context"), IMC->GetPathName());
    ResultObj->SetStringField(TEXT("input_action"), Action->GetPathName());
    ResultObj->SetStringField(TEXT("key"), Key.ToString());
    ResultObj->SetNumberField(TEXT("mapping_count"), IMC->GetMappings().Num());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::AddActionEventNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("blueprint"), BlueprintPath)
        && !Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath)
        && !Params->TryGetStringField(TEXT("target"), BlueprintPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint' parameter (path to the target Blueprint asset)"));
    }

    FString ActionPath;
    if (!Params->TryGetStringField(TEXT("input_action"), ActionPath)
        && !Params->TryGetStringField(TEXT("action"), ActionPath)
        && !Params->TryGetStringField(TEXT("action_path"), ActionPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'input_action' parameter (path to the UInputAction asset)"));
    }

    // Trigger pin to MakeLinkTo. Defaults to "Triggered" because that is the
    // event the consumer game wires for "press to do thing".
    FString TriggerPinName = TEXT("Triggered");
    Params->TryGetStringField(TEXT("trigger"), TriggerPinName);

    FString TargetFunctionName;
    Params->TryGetStringField(TEXT("connect_to_function"), TargetFunctionName);
    if (TargetFunctionName.IsEmpty())
    {
        Params->TryGetStringField(TEXT("function"), TargetFunctionName);
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    double NodeX = 0.0;
    double NodeY = 0.0;
    if (Params->HasField(TEXT("position")))
    {
        const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
        if (Params->TryGetArrayField(TEXT("position"), PositionArr) && PositionArr && PositionArr->Num() >= 2)
        {
            NodeX = (*PositionArr)[0]->AsNumber();
            NodeY = (*PositionArr)[1]->AsNumber();
        }
    }

    UObject* BlueprintAsset = UEditorAssetLibrary::LoadAsset(BlueprintPath);
    UBlueprint* Blueprint = Cast<UBlueprint>(BlueprintAsset);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a Blueprint: %s"), *BlueprintPath));
    }

    UObject* ActionAsset = UEditorAssetLibrary::LoadAsset(ActionPath);
    UInputAction* InputAction = Cast<UInputAction>(ActionAsset);
    if (!InputAction)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UInputAction: %s"), *ActionPath));
    }

    UEdGraph* EventGraph = FEpicUnrealMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
    if (!EventGraph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Failed to find or create the Blueprint's event graph"));
    }

    // Reuse an existing UK2Node_EnhancedInputAction for the same action if
    // present so we mirror UInputActionEventNodeSpawner's "do not duplicate"
    // contract from the engine's node spawner.
    UK2Node_EnhancedInputAction* ActionNode = nullptr;
    bool bReusedExisting = false;
    for (UEdGraphNode* ExistingNode : EventGraph->Nodes)
    {
        if (UK2Node_EnhancedInputAction* AsAction = Cast<UK2Node_EnhancedInputAction>(ExistingNode))
        {
            if (AsAction->InputAction == InputAction)
            {
                ActionNode = AsAction;
                bReusedExisting = true;
                break;
            }
        }
    }

    if (!ActionNode)
    {
        ActionNode = NewObject<UK2Node_EnhancedInputAction>(EventGraph);
        if (!ActionNode)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to construct UK2Node_EnhancedInputAction"));
        }
        ActionNode->InputAction = InputAction;
        ActionNode->NodePosX = static_cast<int32>(NodeX);
        ActionNode->NodePosY = static_cast<int32>(NodeY);
        EventGraph->AddNode(ActionNode, /*bUserAction*/ true, /*bSelectNewNode*/ false);
        ActionNode->CreateNewGuid();
        ActionNode->PostPlacedNewNode();
        ActionNode->AllocateDefaultPins();
    }

    // Validate the requested trigger pin actually exists. The pin names mirror
    // ETriggerEvent enum names (Triggered, Started, Ongoing, Canceled, Completed).
    UEdGraphPin* TriggerPin = ActionNode->FindPin(FName(*TriggerPinName), EGPD_Output);
    if (!TriggerPin)
    {
        // Build a friendly list of available trigger exec pins for the error.
        TArray<FString> AvailableTriggers;
        for (UEdGraphPin* Pin : ActionNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                AvailableTriggers.Add(Pin->PinName.ToString());
            }
        }
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Trigger pin '%s' not found on UK2Node_EnhancedInputAction. Available exec pins: %s"),
                *TriggerPinName, *FString::Join(AvailableTriggers, TEXT(", "))));
    }

    // Optional follow-on: spawn a CallFunction node and link the chosen
    // trigger exec pin into its exec input.
    UK2Node_CallFunction* CallNode = nullptr;
    if (!TargetFunctionName.IsEmpty())
    {
        UClass* TargetClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->SkeletonGeneratedClass;
        if (!TargetClass)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Blueprint has no generated class yet; compile the Blueprint once first"));
        }
        UFunction* TargetFunction = TargetClass->FindFunctionByName(FName(*TargetFunctionName));
        if (!TargetFunction)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Function '%s' not found on Blueprint class '%s'. Create it first or pass an existing name."),
                    *TargetFunctionName, *TargetClass->GetName()));
        }

        CallNode = NewObject<UK2Node_CallFunction>(EventGraph);
        if (!CallNode)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to construct UK2Node_CallFunction"));
        }
        CallNode->SetFromFunction(TargetFunction);
        CallNode->NodePosX = ActionNode->NodePosX + 320;
        CallNode->NodePosY = ActionNode->NodePosY;
        EventGraph->AddNode(CallNode, /*bUserAction*/ true, /*bSelectNewNode*/ false);
        CallNode->CreateNewGuid();
        CallNode->PostPlacedNewNode();
        CallNode->AllocateDefaultPins();

        // Wire the trigger exec pin into the function's input exec pin.
        UEdGraphPin* CallExecPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (!CallExecPin)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Function call node has no exec input pin (expected 'execute' pin)"));
        }
        TriggerPin->MakeLinkTo(CallExecPin);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_action_event_node"));
    ResultObj->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    ResultObj->SetStringField(TEXT("input_action"), InputAction->GetPathName());
    ResultObj->SetStringField(TEXT("event_node_name"), ActionNode->GetName());
    ResultObj->SetStringField(TEXT("event_node_guid"), ActionNode->NodeGuid.ToString());
    ResultObj->SetStringField(TEXT("trigger"), TriggerPinName);
    ResultObj->SetBoolField(TEXT("reused_existing_event_node"), bReusedExisting);
    if (CallNode)
    {
        ResultObj->SetStringField(TEXT("call_function_node_name"), CallNode->GetName());
        ResultObj->SetStringField(TEXT("connected_function"), TargetFunctionName);
    }
    ResultObj->SetBoolField(TEXT("compiled"), bCompile);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

namespace
{
    /** Render a JSON value as ImportText input. Mirrors the helper used
     *  in widget_edit / bp_component for the modifier-property dict. */
    FString BpInput_JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
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

    /** Resolve a UInputModifier subclass by short token, full UObject path,
     *  or a bare class name (with the engine's `UInputModifier` prefix
     *  added when the bare token has no leading `U`). */
    UClass* BpInput_ResolveModifierClass(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return nullptr;
        }
        const FString Lower = Token.ToLower();
        struct FShortTokenMap
        {
            const TCHAR* Token;
            UClass* (*Resolver)();
        };
        static const FShortTokenMap Map[] = {
            { TEXT("negate"),                  []() { return UInputModifierNegate::StaticClass(); } },
            { TEXT("scalar"),                  []() { return UInputModifierScalar::StaticClass(); } },
            { TEXT("dead_zone"),               []() { return UInputModifierDeadZone::StaticClass(); } },
            { TEXT("deadzone"),                []() { return UInputModifierDeadZone::StaticClass(); } },
            { TEXT("swizzle_axis"),            []() { return UInputModifierSwizzleAxis::StaticClass(); } },
            { TEXT("swizzle"),                 []() { return UInputModifierSwizzleAxis::StaticClass(); } },
        };
        for (const FShortTokenMap& Entry : Map)
        {
            if (Lower == Entry.Token)
            {
                return Entry.Resolver();
            }
        }

        if (Token.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UInputModifier>(nullptr, *Token))
            {
                return Loaded;
            }
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *Token))
        {
            if (Found->IsChildOf(UInputModifier::StaticClass()))
            {
                return Found;
            }
        }
        const FString EnhancedPath = FString::Printf(TEXT("/Script/EnhancedInput.%s"), *Token);
        if (UClass* Loaded = LoadClass<UInputModifier>(nullptr, *EnhancedPath))
        {
            return Loaded;
        }
        return nullptr;
    }
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::AddActionModifier(const TSharedPtr<FJsonObject>& Params)
{
    FString IMCPath;
    if (!Params->TryGetStringField(TEXT("input_mapping_context"), IMCPath)
        && !Params->TryGetStringField(TEXT("imc"), IMCPath)
        && !Params->TryGetStringField(TEXT("mapping_context"), IMCPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_mapping_context' parameter"));
    }

    FString ActionName;
    if (!Params->TryGetStringField(TEXT("input_action"), ActionName)
        && !Params->TryGetStringField(TEXT("action"), ActionName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_action' parameter (path or short name of the UInputAction the row binds)"));
    }

    FString KeyText;
    if (!Params->TryGetStringField(TEXT("key"), KeyText))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'key' parameter (matches the FKey on the mapping row)"));
    }

    FString ModifierToken;
    if (!Params->TryGetStringField(TEXT("modifier_class"), ModifierToken)
        && !Params->TryGetStringField(TEXT("modifier"), ModifierToken)
        && !Params->TryGetStringField(TEXT("class"), ModifierToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'modifier_class' parameter (negate / scalar / dead_zone / swizzle_axis or a UInputModifier subclass path)"));
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    UObject* IMCAsset = UEditorAssetLibrary::LoadAsset(IMCPath);
    UInputMappingContext* IMC = Cast<UInputMappingContext>(IMCAsset);
    if (!IMC)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UInputMappingContext: %s"), *IMCPath));
    }

    // The action argument can be a short name (resolved against the
    // mapping rows directly) or an asset path (resolved through the
    // asset registry first).
    const UInputAction* TargetAction = nullptr;
    if (ActionName.StartsWith(TEXT("/")))
    {
        if (UObject* AsAsset = UEditorAssetLibrary::LoadAsset(ActionName))
        {
            TargetAction = Cast<UInputAction>(AsAsset);
        }
    }

    const FKey TargetKey(*KeyText);
    if (!TargetKey.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("FKey '%s' is not a known engine key. Pass an FKey FName like 'SpaceBar', 'W', or 'Gamepad_FaceButton_Bottom'."), *KeyText));
    }

    const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
    int32 MatchedIndex = INDEX_NONE;
    for (int32 i = 0; i < Mappings.Num(); ++i)
    {
        const FEnhancedActionKeyMapping& Row = Mappings[i];
        if (Row.Key != TargetKey)
        {
            continue;
        }
        if (TargetAction)
        {
            if (Row.Action == TargetAction)
            {
                MatchedIndex = i;
                break;
            }
        }
        else if (Row.Action && Row.Action->GetName().Equals(ActionName, ESearchCase::IgnoreCase))
        {
            MatchedIndex = i;
            break;
        }
    }

    if (MatchedIndex == INDEX_NONE)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find a mapping row for action '%s' + key '%s' on %s. Run bp_input add_mapping first."), *ActionName, *KeyText, *IMCPath));
    }

    UClass* ModifierClass = BpInput_ResolveModifierClass(ModifierToken);
    if (!ModifierClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve modifier_class '%s'. Pass one of: negate, scalar, dead_zone, swizzle_axis, or a UInputModifier subclass path."), *ModifierToken));
    }
    if (!ModifierClass->IsChildOf(UInputModifier::StaticClass()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Resolved class '%s' is not a UInputModifier subclass."), *ModifierClass->GetName()));
    }

    // Construct the new modifier as an instanced subobject of the IMC.
    // The Modifiers array on FEnhancedActionKeyMapping is `Instanced`,
    // so we want one subobject per mapping; outered to the IMC keeps it
    // serialised inside the asset.
    UInputModifier* NewModifier = NewObject<UInputModifier>(IMC, ModifierClass, NAME_None, RF_Transactional);
    if (!NewModifier)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to construct UInputModifier '%s'"), *ModifierClass->GetName()));
    }

    // Apply optional flat property dict to the new modifier through
    // ImportText. Failed entries surface under skipped, mirroring the
    // chaos_edit / pcg_graph_edit / widget_edit set_slot_property
    // convention.
    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
    {
        FOutputDeviceNull NullDevice;
        for (const auto& Pair : (*PropsObj)->Values)
        {
            const FString& PropName = Pair.Key;
            const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

            FProperty* Prop = FindFProperty<FProperty>(ModifierClass, *PropName);
            if (!Prop)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), PropName);
                Skip->SetStringField(TEXT("reason"), TEXT("not_a_uproperty"));
                SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }
            const FString TextValue = BpInput_JsonValueToImportText(JsonVal);
            const TCHAR* TextPtr = *TextValue;
            const TCHAR* Result = Prop->ImportText_InContainer(TextPtr, NewModifier, NewModifier, PPF_None, &NullDevice);
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

    // Append to the row's Modifiers array. UInputMappingContext
    // exposes a non-const GetMapping accessor for the editor's binding
    // panel; we route through it for the same reason.
    FEnhancedActionKeyMapping& Row = IMC->GetMapping(MatchedIndex);
    Row.Modifiers.Add(NewModifier);

    if (UPackage* Package = IMC->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(IMCPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_action_modifier"));
    ResultObj->SetStringField(TEXT("input_mapping_context"), IMC->GetPathName());
    if (Row.Action)
    {
        ResultObj->SetStringField(TEXT("input_action"), Row.Action->GetPathName());
    }
    ResultObj->SetStringField(TEXT("key"), Row.Key.ToString());
    ResultObj->SetNumberField(TEXT("mapping_index"), MatchedIndex);
    ResultObj->SetStringField(TEXT("modifier_class"), ModifierClass->GetName());
    ResultObj->SetStringField(TEXT("modifier_class_path"), ModifierClass->GetPathName());
    ResultObj->SetNumberField(TEXT("modifier_count"), Row.Modifiers.Num());
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetArrayField(TEXT("skipped"), SkippedJson);
    ResultObj->SetNumberField(TEXT("applied_count"), AppliedJson.Num());
    ResultObj->SetNumberField(TEXT("skipped_count"), SkippedJson.Num());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

namespace
{
    /** Resolve a UInputTrigger subclass by short token, full UObject path,
     *  or a bare class name (with a `/Script/EnhancedInput.<Name>` fallback).
     *  The supported short tokens mirror the canonical UInputTrigger subclass
     *  set under `Plugins/EnhancedInput/Source/EnhancedInput/Public/InputTriggers.h`. */
    UClass* BpInput_ResolveTriggerClass(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return nullptr;
        }
        const FString Lower = Token.ToLower();
        struct FShortTokenMap
        {
            const TCHAR* Token;
            UClass* (*Resolver)();
        };
        static const FShortTokenMap Map[] = {
            { TEXT("pressed"),               []() { return UInputTriggerPressed::StaticClass(); } },
            { TEXT("released"),              []() { return UInputTriggerReleased::StaticClass(); } },
            { TEXT("hold"),                  []() { return UInputTriggerHold::StaticClass(); } },
            { TEXT("hold_and_release"),      []() { return UInputTriggerHoldAndRelease::StaticClass(); } },
            { TEXT("holdandrelease"),        []() { return UInputTriggerHoldAndRelease::StaticClass(); } },
            { TEXT("tap"),                   []() { return UInputTriggerTap::StaticClass(); } },
            { TEXT("pulse"),                 []() { return UInputTriggerPulse::StaticClass(); } },
            { TEXT("chord_action"),          []() { return UInputTriggerChordAction::StaticClass(); } },
            { TEXT("chordaction"),           []() { return UInputTriggerChordAction::StaticClass(); } },
            { TEXT("chord"),                 []() { return UInputTriggerChordAction::StaticClass(); } },
            { TEXT("down"),                  []() { return UInputTriggerDown::StaticClass(); } },
            { TEXT("repeated_tap"),          []() { return UInputTriggerRepeatedTap::StaticClass(); } },
            { TEXT("repeatedtap"),           []() { return UInputTriggerRepeatedTap::StaticClass(); } },
            { TEXT("combo"),                 []() { return UInputTriggerCombo::StaticClass(); } },
        };
        for (const FShortTokenMap& Entry : Map)
        {
            if (Lower == Entry.Token)
            {
                return Entry.Resolver();
            }
        }

        if (Token.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UInputTrigger>(nullptr, *Token))
            {
                return Loaded;
            }
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *Token))
        {
            if (Found->IsChildOf(UInputTrigger::StaticClass()))
            {
                return Found;
            }
        }
        const FString EnhancedPath = FString::Printf(TEXT("/Script/EnhancedInput.%s"), *Token);
        if (UClass* Loaded = LoadClass<UInputTrigger>(nullptr, *EnhancedPath))
        {
            return Loaded;
        }
        return nullptr;
    }
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::AddActionTrigger(const TSharedPtr<FJsonObject>& Params)
{
    // The mapping-row resolution mirrors AddActionModifier byte-for-byte:
    // the row is keyed by (action, key) and the optional `properties`
    // dict applies through `FProperty::ImportText_InContainer` against
    // the new UInputTrigger instance.
    FString IMCPath;
    if (!Params->TryGetStringField(TEXT("input_mapping_context"), IMCPath)
        && !Params->TryGetStringField(TEXT("imc"), IMCPath)
        && !Params->TryGetStringField(TEXT("mapping_context"), IMCPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_mapping_context' parameter"));
    }

    FString ActionName;
    if (!Params->TryGetStringField(TEXT("input_action"), ActionName)
        && !Params->TryGetStringField(TEXT("action"), ActionName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_action' parameter (path or short name of the UInputAction the row binds)"));
    }

    FString KeyText;
    if (!Params->TryGetStringField(TEXT("key"), KeyText))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'key' parameter (matches the FKey on the mapping row)"));
    }

    FString TriggerToken;
    if (!Params->TryGetStringField(TEXT("trigger_class"), TriggerToken)
        && !Params->TryGetStringField(TEXT("trigger"), TriggerToken)
        && !Params->TryGetStringField(TEXT("class"), TriggerToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'trigger_class' parameter (pressed / released / hold / hold_and_release / tap / pulse / chord_action / down / repeated_tap / combo, or a UInputTrigger subclass path)"));
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    UObject* IMCAsset = UEditorAssetLibrary::LoadAsset(IMCPath);
    UInputMappingContext* IMC = Cast<UInputMappingContext>(IMCAsset);
    if (!IMC)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UInputMappingContext: %s"), *IMCPath));
    }

    const UInputAction* TargetAction = nullptr;
    if (ActionName.StartsWith(TEXT("/")))
    {
        if (UObject* AsAsset = UEditorAssetLibrary::LoadAsset(ActionName))
        {
            TargetAction = Cast<UInputAction>(AsAsset);
        }
    }

    const FKey TargetKey(*KeyText);
    if (!TargetKey.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("FKey '%s' is not a known engine key. Pass an FKey FName like 'SpaceBar', 'W', or 'Gamepad_FaceButton_Bottom'."), *KeyText));
    }

    const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
    int32 MatchedIndex = INDEX_NONE;
    for (int32 i = 0; i < Mappings.Num(); ++i)
    {
        const FEnhancedActionKeyMapping& Row = Mappings[i];
        if (Row.Key != TargetKey)
        {
            continue;
        }
        if (TargetAction)
        {
            if (Row.Action == TargetAction)
            {
                MatchedIndex = i;
                break;
            }
        }
        else if (Row.Action && Row.Action->GetName().Equals(ActionName, ESearchCase::IgnoreCase))
        {
            MatchedIndex = i;
            break;
        }
    }

    if (MatchedIndex == INDEX_NONE)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find a mapping row for action '%s' + key '%s' on %s. Run bp_input add_mapping first."), *ActionName, *KeyText, *IMCPath));
    }

    UClass* TriggerClass = BpInput_ResolveTriggerClass(TriggerToken);
    if (!TriggerClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve trigger_class '%s'. Pass one of: pressed, released, hold, hold_and_release, tap, pulse, chord_action, down, repeated_tap, combo, or a UInputTrigger subclass path."), *TriggerToken));
    }
    if (!TriggerClass->IsChildOf(UInputTrigger::StaticClass()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Resolved class '%s' is not a UInputTrigger subclass."), *TriggerClass->GetName()));
    }

    // Construct the new trigger as an instanced subobject of the IMC.
    // The Triggers array on FEnhancedActionKeyMapping is `Instanced`,
    // so we want one subobject per mapping; outered to the IMC keeps
    // it serialised inside the asset (matching the editor's binding
    // panel convention).
    UInputTrigger* NewTrigger = NewObject<UInputTrigger>(IMC, TriggerClass, NAME_None, RF_Transactional);
    if (!NewTrigger)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to construct UInputTrigger '%s'"), *TriggerClass->GetName()));
    }

    // Apply optional flat property dict to the new trigger through
    // ImportText. Failed entries surface under skipped, mirroring the
    // chaos_edit / pcg_graph_edit / widget_edit set_slot_property
    // convention and the parallel `add_action_modifier` op.
    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
    {
        FOutputDeviceNull NullDevice;
        for (const auto& Pair : (*PropsObj)->Values)
        {
            const FString& PropName = Pair.Key;
            const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

            FProperty* Prop = FindFProperty<FProperty>(TriggerClass, *PropName);
            if (!Prop)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), PropName);
                Skip->SetStringField(TEXT("reason"), TEXT("not_a_uproperty"));
                SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }
            const FString TextValue = BpInput_JsonValueToImportText(JsonVal);
            const TCHAR* TextPtr = *TextValue;
            const TCHAR* Result = Prop->ImportText_InContainer(TextPtr, NewTrigger, NewTrigger, PPF_None, &NullDevice);
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

    // Append to the row's Triggers array through the non-const accessor.
    FEnhancedActionKeyMapping& Row = IMC->GetMapping(MatchedIndex);
    Row.Triggers.Add(NewTrigger);

    if (UPackage* Package = IMC->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(IMCPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_action_trigger"));
    ResultObj->SetStringField(TEXT("input_mapping_context"), IMC->GetPathName());
    if (Row.Action)
    {
        ResultObj->SetStringField(TEXT("input_action"), Row.Action->GetPathName());
    }
    ResultObj->SetStringField(TEXT("key"), Row.Key.ToString());
    ResultObj->SetNumberField(TEXT("mapping_index"), MatchedIndex);
    ResultObj->SetStringField(TEXT("trigger_class"), TriggerClass->GetName());
    ResultObj->SetStringField(TEXT("trigger_class_path"), TriggerClass->GetPathName());
    ResultObj->SetNumberField(TEXT("trigger_count"), Row.Triggers.Num());
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetArrayField(TEXT("skipped"), SkippedJson);
    ResultObj->SetNumberField(TEXT("applied_count"), AppliedJson.Num());
    ResultObj->SetNumberField(TEXT("skipped_count"), SkippedJson.Num());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::AddActionChord(const TSharedPtr<FJsonObject>& Params)
{
    // Declarative one-call wrapper that lays a UInputTriggerChordAction
    // down on a mapping row and binds its `ChordAction` slot to a
    // sibling UInputAction in one step. The existing `add_action_trigger`
    // op already covers the trigger-class side; this op fronts the
    // canonical chord-action shape so the caller does not have to
    // assemble the `chord_action` short token + `properties =
    // {ChordAction = /Game/...}` dict on the way in.
    FString IMCPath;
    if (!Params->TryGetStringField(TEXT("input_mapping_context"), IMCPath)
        && !Params->TryGetStringField(TEXT("imc"), IMCPath)
        && !Params->TryGetStringField(TEXT("mapping_context"), IMCPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_mapping_context' parameter"));
    }

    FString ActionName;
    if (!Params->TryGetStringField(TEXT("input_action"), ActionName)
        && !Params->TryGetStringField(TEXT("action"), ActionName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_action' parameter (path or short name of the UInputAction the row binds)"));
    }

    FString KeyText;
    if (!Params->TryGetStringField(TEXT("key"), KeyText))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'key' parameter (matches the FKey on the mapping row)"));
    }

    FString ChordActionParam;
    if (!Params->TryGetStringField(TEXT("chord_action"), ChordActionParam)
        && !Params->TryGetStringField(TEXT("chorded_action"), ChordActionParam)
        && !Params->TryGetStringField(TEXT("chord"), ChordActionParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'chord_action' parameter (path or short name of the sibling UInputAction this trigger needs held)"));
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    // Resolve the IMC.
    UObject* IMCAsset = UEditorAssetLibrary::LoadAsset(IMCPath);
    UInputMappingContext* IMC = Cast<UInputMappingContext>(IMCAsset);
    if (!IMC)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UInputMappingContext: %s"), *IMCPath));
    }

    // Resolve the row-target UInputAction (the action the row binds).
    const UInputAction* TargetAction = nullptr;
    if (ActionName.StartsWith(TEXT("/")))
    {
        if (UObject* AsAsset = UEditorAssetLibrary::LoadAsset(ActionName))
        {
            TargetAction = Cast<UInputAction>(AsAsset);
        }
    }

    const FKey TargetKey(*KeyText);
    if (!TargetKey.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("FKey '%s' is not a known engine key. Pass an FKey FName like 'SpaceBar', 'W', or 'Gamepad_FaceButton_Bottom'."), *KeyText));
    }

    const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
    int32 MatchedIndex = INDEX_NONE;
    for (int32 i = 0; i < Mappings.Num(); ++i)
    {
        const FEnhancedActionKeyMapping& Row = Mappings[i];
        if (Row.Key != TargetKey)
        {
            continue;
        }
        if (TargetAction)
        {
            if (Row.Action == TargetAction)
            {
                MatchedIndex = i;
                break;
            }
        }
        else if (Row.Action && Row.Action->GetName().Equals(ActionName, ESearchCase::IgnoreCase))
        {
            MatchedIndex = i;
            break;
        }
    }
    if (MatchedIndex == INDEX_NONE)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find a mapping row for action '%s' + key '%s' on %s. Run bp_input add_mapping first."),
                *ActionName, *KeyText, *IMCPath));
    }

    // Resolve the sibling UInputAction (the chord action; the IA the
    // user must be holding for this row to fire).
    UInputAction* ChordAction = nullptr;
    if (ChordActionParam.StartsWith(TEXT("/")))
    {
        if (UObject* AsAsset = UEditorAssetLibrary::LoadAsset(ChordActionParam))
        {
            ChordAction = Cast<UInputAction>(AsAsset);
        }
    }
    else
    {
        // Asset registry fallback for a short name (matches the
        // documented behaviour of the bp_input action resolvers).
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), Found, /*bSearchSubClasses=*/false);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(ChordActionParam, ESearchCase::IgnoreCase))
            {
                ChordAction = Cast<UInputAction>(Data.GetAsset());
                if (ChordAction) break;
            }
        }
    }
    if (!ChordAction)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve chord_action '%s' to a UInputAction. Pass a /Game/... path or a unique short name. The chord-action IA must exist before add_action_chord."),
                *ChordActionParam));
    }

    // Refuse self-chord. The engine accepts it but the runtime never
    // resolves: the row's own action cannot fire as its own chord
    // prerequisite. Surface a clear error rather than silently land a
    // broken trigger.
    if (TargetAction && TargetAction == ChordAction)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("chord_action '%s' is the same as input_action; a row cannot be chorded against its own action."),
                *ChordActionParam));
    }

    // NewObject the UInputTriggerChordAction subobject outered to the
    // IMC (matching the editor's `Instanced` UPROPERTY convention on
    // FEnhancedActionKeyMapping::Triggers).
    UInputTriggerChordAction* NewChord = NewObject<UInputTriggerChordAction>(
        IMC, UInputTriggerChordAction::StaticClass(), NAME_None, RF_Transactional);
    if (!NewChord)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to construct UInputTriggerChordAction"));
    }
    NewChord->ChordAction = ChordAction;

    // Optional flat property dict for future-proofing (any extra
    // EditAnywhere UPROPERTY a future UE version drops on the
    // UInputTriggerChordAction surface). Mirrors the convention shipped
    // by add_action_trigger / add_action_modifier.
    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
    {
        FOutputDeviceNull NullDevice;
        for (const auto& Pair : (*PropsObj)->Values)
        {
            const FString& PropName = Pair.Key;
            const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

            FProperty* Prop = FindFProperty<FProperty>(UInputTriggerChordAction::StaticClass(), *PropName);
            if (!Prop)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), PropName);
                Skip->SetStringField(TEXT("reason"), TEXT("not_a_uproperty"));
                SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }
            const FString TextValue = BpInput_JsonValueToImportText(JsonVal);
            const TCHAR* TextPtr = *TextValue;
            const TCHAR* Result = Prop->ImportText_InContainer(TextPtr, NewChord, NewChord, PPF_None, &NullDevice);
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

    // Append the new chord trigger to the mapping row.
    FEnhancedActionKeyMapping& Row = IMC->GetMapping(MatchedIndex);
    Row.Triggers.Add(NewChord);

    if (UPackage* Package = IMC->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(IMCPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_action_chord"));
    ResultObj->SetStringField(TEXT("input_mapping_context"), IMC->GetPathName());
    if (Row.Action)
    {
        ResultObj->SetStringField(TEXT("input_action"), Row.Action->GetPathName());
    }
    ResultObj->SetStringField(TEXT("key"), Row.Key.ToString());
    ResultObj->SetNumberField(TEXT("mapping_index"), MatchedIndex);
    ResultObj->SetStringField(TEXT("chord_action"), ChordAction->GetPathName());
    ResultObj->SetStringField(TEXT("trigger_class"), UInputTriggerChordAction::StaticClass()->GetName());
    ResultObj->SetStringField(TEXT("trigger_class_path"), UInputTriggerChordAction::StaticClass()->GetPathName());
    ResultObj->SetNumberField(TEXT("trigger_count"), Row.Triggers.Num());
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetArrayField(TEXT("skipped"), SkippedJson);
    ResultObj->SetNumberField(TEXT("applied_count"), AppliedJson.Num());
    ResultObj->SetNumberField(TEXT("skipped_count"), SkippedJson.Num());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}
