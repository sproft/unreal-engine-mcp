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
#include "K2Node_CallFunction.h"
#include "K2Node_EnhancedInputAction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

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

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported bp_input operation '%s'. Supported: create_input_action, create_input_mapping_context, add_mapping, add_action_event_node"), *Operation));
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
