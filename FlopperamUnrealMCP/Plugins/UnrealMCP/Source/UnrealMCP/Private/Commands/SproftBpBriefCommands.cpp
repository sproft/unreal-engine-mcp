#include "Commands/SproftBpBriefCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Components/ActorComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"

namespace
{
    FString BlueprintTypeLabel(EBlueprintType Type)
    {
        switch (Type)
        {
            case BPTYPE_Normal:           return TEXT("Normal");
            case BPTYPE_Const:            return TEXT("Const");
            case BPTYPE_MacroLibrary:     return TEXT("MacroLibrary");
            case BPTYPE_Interface:        return TEXT("Interface");
            case BPTYPE_LevelScript:      return TEXT("LevelScript");
            case BPTYPE_FunctionLibrary:  return TEXT("FunctionLibrary");
            default:                      return TEXT("Unknown");
        }
    }
}

FSproftBpBriefCommands::FSproftBpBriefCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpBriefCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_brief"))
    {
        return HandleBpBrief(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_brief command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpBriefCommands::HandleBpBrief(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString BlueprintInput;
    if (!Params->TryGetStringField(TEXT("blueprint"), BlueprintInput)
        && !Params->TryGetStringField(TEXT("blueprint_path"), BlueprintInput)
        && !Params->TryGetStringField(TEXT("blueprint_name"), BlueprintInput)
        && !Params->TryGetStringField(TEXT("name"), BlueprintInput))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint' parameter"));
    }

    UBlueprint* Blueprint = FEpicUnrealMCPCommonUtils::FindBlueprint(BlueprintInput);
    if (!Blueprint && BlueprintInput.StartsWith(TEXT("/")))
    {
        // Last-ditch: a raw asset path with no `.AssetName` suffix.
        Blueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(BlueprintInput));
    }
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintInput));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("brief"));
    Result->SetStringField(TEXT("name"), Blueprint->GetName());
    Result->SetStringField(TEXT("path"), Blueprint->GetPathName());
    if (UPackage* Package = Blueprint->GetOutermost())
    {
        Result->SetStringField(TEXT("package_name"), Package->GetName());
    }

    if (Blueprint->ParentClass)
    {
        Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetPathName());
        Result->SetStringField(TEXT("parent_class_short"), Blueprint->ParentClass->GetName());
    }
    else
    {
        Result->SetStringField(TEXT("parent_class"), TEXT(""));
        Result->SetStringField(TEXT("parent_class_short"), TEXT(""));
    }

    Result->SetStringField(TEXT("blueprint_type"), BlueprintTypeLabel(Blueprint->BlueprintType));

    // Variable count.
    Result->SetNumberField(TEXT("variable_count"), Blueprint->NewVariables.Num());

    // Function and macro counts.
    int32 FunctionCount = 0;
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph)
        {
            ++FunctionCount;
        }
    }
    Result->SetNumberField(TEXT("function_count"), FunctionCount);

    int32 MacroCount = 0;
    for (UEdGraph* Graph : Blueprint->MacroGraphs)
    {
        if (Graph)
        {
            ++MacroCount;
        }
    }
    Result->SetNumberField(TEXT("macro_count"), MacroCount);

    // Event-graph node count + named-event list.
    int32 EventGraphNodeCount = 0;
    TArray<TSharedPtr<FJsonValue>> EventList;
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (!Graph)
        {
            continue;
        }
        EventGraphNodeCount += Graph->Nodes.Num();
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), EventNode->EventReference.GetMemberName().ToString());
                Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
                EventList.Add(MakeShared<FJsonValueObject>(Entry));
            }
            else if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), CustomEvent->CustomFunctionName.ToString());
                Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
                EventList.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    }
    Result->SetNumberField(TEXT("event_graph_node_count"), EventGraphNodeCount);
    Result->SetArrayField(TEXT("events"), EventList);

    // Components summary.
    TArray<TSharedPtr<FJsonValue>> Components;
    if (Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (!SCSNode || !SCSNode->ComponentTemplate)
            {
                continue;
            }
            TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
            CompObj->SetStringField(TEXT("name"), SCSNode->GetVariableName().ToString());
            CompObj->SetStringField(TEXT("class"), SCSNode->ComponentTemplate->GetClass()->GetName());
            CompObj->SetBoolField(TEXT("is_root"), SCSNode == Blueprint->SimpleConstructionScript->GetDefaultSceneRootNode());
            Components.Add(MakeShared<FJsonValueObject>(CompObj));
        }
    }
    Result->SetArrayField(TEXT("components"), Components);
    Result->SetNumberField(TEXT("component_count"), Components.Num());

    // Implemented interfaces.
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
