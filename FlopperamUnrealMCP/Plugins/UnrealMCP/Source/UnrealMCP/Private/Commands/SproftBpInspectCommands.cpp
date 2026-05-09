#include "Commands/SproftBpInspectCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "UObject/Class.h"

namespace
{
    /** Best-effort textual rendering of an FEdGraphPinType. */
    FString BpInspect_DescribePinType(const FEdGraphPinType& PinType)
    {
        FString Result = PinType.PinCategory.ToString();
        if (!PinType.PinSubCategory.IsNone())
        {
            Result += TEXT(":") + PinType.PinSubCategory.ToString();
        }
        if (PinType.PinSubCategoryObject.IsValid())
        {
            Result += TEXT("<") + PinType.PinSubCategoryObject->GetName() + TEXT(">");
        }
        if (PinType.IsArray())
        {
            Result += TEXT("[]");
        }
        else if (PinType.IsSet())
        {
            Result += TEXT("{set}");
        }
        else if (PinType.IsMap())
        {
            Result += TEXT("{map}");
        }
        return Result;
    }

    UBlueprint* BpInspect_ResolveBlueprintParam(const TSharedPtr<FJsonObject>& Params)
    {
        FString Input;
        if (!Params->TryGetStringField(TEXT("blueprint"), Input)
            && !Params->TryGetStringField(TEXT("blueprint_path"), Input)
            && !Params->TryGetStringField(TEXT("blueprint_name"), Input)
            && !Params->TryGetStringField(TEXT("name"), Input))
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
}

FSproftBpInspectCommands::FSproftBpInspectCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpInspectCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_inspect"))
    {
        return HandleBpInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_inspect command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpInspectCommands::HandleBpInspect(const TSharedPtr<FJsonObject>& Params)
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

    UBlueprint* Blueprint = BpInspect_ResolveBlueprintParam(Params);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Blueprint not found or 'blueprint' parameter missing"));
    }

    if (Operation == TEXT("list_variables") || Operation == TEXT("variables"))
    {
        return ListVariables(Blueprint);
    }
    if (Operation == TEXT("list_functions") || Operation == TEXT("functions"))
    {
        return ListFunctions(Blueprint);
    }
    if (Operation == TEXT("list_events") || Operation == TEXT("events"))
    {
        return ListEvents(Blueprint);
    }
    if (Operation == TEXT("list_components") || Operation == TEXT("components"))
    {
        return ListComponents(Blueprint);
    }
    if (Operation == TEXT("find_node") || Operation == TEXT("find"))
    {
        return FindNode(Blueprint, Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported bp_inspect op '%s'. Supported: list_variables, list_functions, list_events, list_components, find_node"), *Operation));
}

TSharedPtr<FJsonObject> FSproftBpInspectCommands::ListVariables(UBlueprint* Blueprint)
{
    TArray<TSharedPtr<FJsonValue>> Vars;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Var.VarName.ToString());
        Entry->SetStringField(TEXT("type"), BpInspect_DescribePinType(Var.VarType));
        Entry->SetStringField(TEXT("default_value"), Var.DefaultValue);
        Entry->SetBoolField(TEXT("editable_on_instance"), (Var.PropertyFlags & CPF_Edit) != 0);
        Entry->SetBoolField(TEXT("blueprint_read_only"), (Var.PropertyFlags & CPF_BlueprintReadOnly) != 0);
        Entry->SetBoolField(TEXT("expose_on_spawn"), (Var.PropertyFlags & CPF_ExposeOnSpawn) != 0);
        Entry->SetStringField(TEXT("category"), Var.Category.ToString());
        if (!Var.FriendlyName.IsEmpty())
        {
            Entry->SetStringField(TEXT("friendly_name"), Var.FriendlyName);
        }
        Vars.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("list_variables"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetNumberField(TEXT("count"), Vars.Num());
    Result->SetArrayField(TEXT("variables"), Vars);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpInspectCommands::ListFunctions(UBlueprint* Blueprint)
{
    TArray<TSharedPtr<FJsonValue>> Functions;
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (!Graph)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Graph->GetName());
        Entry->SetStringField(TEXT("graph_class"), Graph->GetClass()->GetName());
        Entry->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
        Functions.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TArray<TSharedPtr<FJsonValue>> Macros;
    for (UEdGraph* Graph : Blueprint->MacroGraphs)
    {
        if (!Graph)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Graph->GetName());
        Entry->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
        Macros.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("list_functions"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetNumberField(TEXT("function_count"), Functions.Num());
    Result->SetArrayField(TEXT("functions"), Functions);
    Result->SetNumberField(TEXT("macro_count"), Macros.Num());
    Result->SetArrayField(TEXT("macros"), Macros);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpInspectCommands::ListEvents(UBlueprint* Blueprint)
{
    TArray<TSharedPtr<FJsonValue>> Events;
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (!Graph)
        {
            continue;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("kind"), TEXT("event"));
                Entry->SetStringField(TEXT("name"), EventNode->EventReference.GetMemberName().ToString());
                Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
                Entry->SetStringField(TEXT("graph"), Graph->GetName());
                Entry->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
                Events.Add(MakeShared<FJsonValueObject>(Entry));
            }
            else if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("kind"), TEXT("custom_event"));
                Entry->SetStringField(TEXT("name"), CustomEvent->CustomFunctionName.ToString());
                Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
                Entry->SetStringField(TEXT("graph"), Graph->GetName());
                Entry->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
                Events.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("list_events"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetNumberField(TEXT("count"), Events.Num());
    Result->SetArrayField(TEXT("events"), Events);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpInspectCommands::ListComponents(UBlueprint* Blueprint)
{
    TArray<TSharedPtr<FJsonValue>> Components;
    if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* SCSNode : SCS->GetAllNodes())
        {
            if (!SCSNode || !SCSNode->ComponentTemplate)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), SCSNode->GetVariableName().ToString());
            Entry->SetStringField(TEXT("class"), SCSNode->ComponentTemplate->GetClass()->GetName());
            Entry->SetStringField(TEXT("class_path"), SCSNode->ComponentTemplate->GetClass()->GetPathName());
            Entry->SetBoolField(TEXT("is_root"), SCSNode == SCS->GetDefaultSceneRootNode());
            Entry->SetBoolField(TEXT("is_scene_component"), Cast<USceneComponent>(SCSNode->ComponentTemplate) != nullptr);

            // Resolve attach parent / socket info for scene components.
            if (USceneComponent* SceneTemplate = Cast<USceneComponent>(SCSNode->ComponentTemplate))
            {
                if (!SCSNode->AttachToName.IsNone())
                {
                    Entry->SetStringField(TEXT("attach_socket"), SCSNode->AttachToName.ToString());
                }
            }
            // Resolve attach parent by walking the SCS tree.
            for (USCS_Node* CandidateParent : SCS->GetAllNodes())
            {
                if (!CandidateParent || CandidateParent == SCSNode)
                {
                    continue;
                }
                if (CandidateParent->GetChildNodes().Contains(SCSNode))
                {
                    Entry->SetStringField(TEXT("attach_parent"), CandidateParent->GetVariableName().ToString());
                    break;
                }
            }
            Entry->SetNumberField(TEXT("child_count"), SCSNode->GetChildNodes().Num());
            Components.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("list_components"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetNumberField(TEXT("count"), Components.Num());
    Result->SetArrayField(TEXT("components"), Components);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpInspectCommands::FindNode(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    FString ClassPattern;
    Params->TryGetStringField(TEXT("class_pattern"), ClassPattern);
    FString TitlePattern;
    Params->TryGetStringField(TEXT("title_pattern"), TitlePattern);
    if (ClassPattern.IsEmpty() && TitlePattern.IsEmpty())
    {
        // Fall back to a single 'pattern' that matches either field.
        FString GeneralPattern;
        Params->TryGetStringField(TEXT("pattern"), GeneralPattern);
        if (GeneralPattern.IsEmpty())
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("find_node requires 'class_pattern' or 'title_pattern' (or 'pattern' for either)"));
        }
        ClassPattern = GeneralPattern;
        TitlePattern = GeneralPattern;
    }
    const FString ClassPatternLower = ClassPattern.ToLower();
    const FString TitlePatternLower = TitlePattern.ToLower();

    int32 Limit = 64;
    int32 ParsedLimit = 0;
    if (Params->TryGetNumberField(TEXT("limit"), ParsedLimit) && ParsedLimit > 0)
    {
        Limit = ParsedLimit;
    }

    auto NodeMatches = [&](UEdGraphNode* Node) -> bool
    {
        if (!Node)
        {
            return false;
        }
        if (!ClassPatternLower.IsEmpty()
            && Node->GetClass()->GetName().ToLower().Contains(ClassPatternLower))
        {
            return true;
        }
        if (!TitlePatternLower.IsEmpty()
            && Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().ToLower().Contains(TitlePatternLower))
        {
            return true;
        }
        return false;
    };

    auto WalkGraph = [&](UEdGraph* Graph, const TCHAR* GraphKind, TArray<TSharedPtr<FJsonValue>>& Out)
    {
        if (!Graph)
        {
            return;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Out.Num() >= Limit)
            {
                return;
            }
            if (!NodeMatches(Node))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("graph"), Graph->GetName());
            Entry->SetStringField(TEXT("graph_kind"), GraphKind);
            Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
            Entry->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
            Entry->SetStringField(TEXT("node_name"), Node->GetName());
            Out.Add(MakeShared<FJsonValueObject>(Entry));
        }
    };

    TArray<TSharedPtr<FJsonValue>> Matches;
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        WalkGraph(Graph, TEXT("ubergraph"), Matches);
    }
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        WalkGraph(Graph, TEXT("function"), Matches);
    }
    for (UEdGraph* Graph : Blueprint->MacroGraphs)
    {
        WalkGraph(Graph, TEXT("macro"), Matches);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("find_node"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    if (!ClassPattern.IsEmpty())
    {
        Result->SetStringField(TEXT("class_pattern"), ClassPattern);
    }
    if (!TitlePattern.IsEmpty())
    {
        Result->SetStringField(TEXT("title_pattern"), TitlePattern);
    }
    Result->SetNumberField(TEXT("count"), Matches.Num());
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetArrayField(TEXT("matches"), Matches);
    return Result;
}
