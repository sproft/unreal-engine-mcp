#include "Commands/SproftBpGraphCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "UObject/Class.h"

namespace
{
    UBlueprint* BpGraph_ResolveBlueprintParam(const TSharedPtr<FJsonObject>& Params)
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

    FString BpGraph_DescribePinType(const FEdGraphPinType& PinType)
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

    /** Walk a Blueprint's graphs and pass each to a callback with its kind label. */
    template <typename FnType>
    void BpGraph_ForEachGraph(UBlueprint* Blueprint, FnType Callback)
    {
        for (UEdGraph* G : Blueprint->UbergraphPages)
        {
            if (G) { Callback(G, TEXT("ubergraph")); }
        }
        for (UEdGraph* G : Blueprint->FunctionGraphs)
        {
            if (G) { Callback(G, TEXT("function")); }
        }
        for (UEdGraph* G : Blueprint->MacroGraphs)
        {
            if (G) { Callback(G, TEXT("macro")); }
        }
        for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
        {
            for (UEdGraph* G : Iface.Graphs)
            {
                if (G) { Callback(G, TEXT("interface")); }
            }
        }
        // Note: in modern UE, the construction script lives on
        // FunctionGraphs as `UserConstructionScript`, so we don't enumerate
        // SimpleConstructionScript separately here.
    }

    /** Resolve a graph by name (case-insensitive), preferring an exact match. */
    UEdGraph* ResolveGraphByName(UBlueprint* Blueprint, const FString& InName, FString& OutKind)
    {
        const FString Lower = InName.ToLower();
        UEdGraph* Match = nullptr;
        FString MatchKind;
        BpGraph_ForEachGraph(Blueprint, [&](UEdGraph* G, const TCHAR* Kind)
        {
            if (Match)
            {
                return;
            }
            if (G->GetName().Equals(InName, ESearchCase::IgnoreCase))
            {
                Match = G;
                MatchKind = Kind;
            }
        });
        if (Match)
        {
            OutKind = MatchKind;
            return Match;
        }
        // Fallback: substring.
        BpGraph_ForEachGraph(Blueprint, [&](UEdGraph* G, const TCHAR* Kind)
        {
            if (Match)
            {
                return;
            }
            if (G->GetName().ToLower().Contains(Lower))
            {
                Match = G;
                MatchKind = Kind;
            }
        });
        if (Match)
        {
            OutKind = MatchKind;
        }
        return Match;
    }

    UEdGraphNode* ResolveNodeByName(UEdGraph* Graph, const FString& InName)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->GetName().Equals(InName, ESearchCase::IgnoreCase))
            {
                return Node;
            }
        }
        // Fallback: a substring match on the title.
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().ToLower().Contains(InName.ToLower()))
            {
                return Node;
            }
        }
        return nullptr;
    }
}

FSproftBpGraphCommands::FSproftBpGraphCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpGraphCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_graph"))
    {
        return HandleBpGraph(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_graph command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpGraphCommands::HandleBpGraph(const TSharedPtr<FJsonObject>& Params)
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

    UBlueprint* Blueprint = BpGraph_ResolveBlueprintParam(Params);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Blueprint not found or 'blueprint' parameter missing"));
    }

    if (Operation == TEXT("list_graphs") || Operation == TEXT("graphs"))
    {
        return ListGraphs(Blueprint);
    }
    if (Operation == TEXT("list_nodes") || Operation == TEXT("nodes"))
    {
        return ListNodes(Blueprint, Params);
    }
    if (Operation == TEXT("get_node") || Operation == TEXT("node"))
    {
        return GetNode(Blueprint, Params);
    }
    if (Operation == TEXT("list_connections") || Operation == TEXT("connections") || Operation == TEXT("edges"))
    {
        return ListConnections(Blueprint, Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported bp_graph op '%s'. Supported: list_graphs, list_nodes, get_node, list_connections"), *Operation));
}

TSharedPtr<FJsonObject> FSproftBpGraphCommands::ListGraphs(UBlueprint* Blueprint)
{
    TArray<TSharedPtr<FJsonValue>> Entries;
    BpGraph_ForEachGraph(Blueprint, [&](UEdGraph* G, const TCHAR* Kind)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), G->GetName());
        Entry->SetStringField(TEXT("kind"), Kind);
        Entry->SetStringField(TEXT("graph_class"), G->GetClass()->GetName());
        Entry->SetNumberField(TEXT("node_count"), G->Nodes.Num());
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    });

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("list_graphs"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetNumberField(TEXT("count"), Entries.Num());
    Result->SetArrayField(TEXT("graphs"), Entries);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpGraphCommands::ListNodes(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    FString GraphName;
    if (!Params->TryGetStringField(TEXT("graph"), GraphName)
        && !Params->TryGetStringField(TEXT("graph_name"), GraphName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'graph' parameter is required"));
    }
    FString Kind;
    UEdGraph* Graph = ResolveGraphByName(Blueprint, GraphName, Kind);
    if (!Graph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Graph '%s' not found on Blueprint '%s'"), *GraphName, *Blueprint->GetName()));
    }

    FString ClassPattern;
    Params->TryGetStringField(TEXT("include_class_pattern"), ClassPattern);
    Params->TryGetStringField(TEXT("class_pattern"), ClassPattern);
    FString TitlePattern;
    Params->TryGetStringField(TEXT("include_title_pattern"), TitlePattern);
    Params->TryGetStringField(TEXT("title_pattern"), TitlePattern);
    const FString ClassPatternLower = ClassPattern.ToLower();
    const FString TitlePatternLower = TitlePattern.ToLower();

    int32 Limit = 256;
    int32 ParsedLimit = 0;
    if (Params->TryGetNumberField(TEXT("limit"), ParsedLimit) && ParsedLimit > 0)
    {
        Limit = ParsedLimit;
    }

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || Nodes.Num() >= Limit)
        {
            continue;
        }
        if (!ClassPatternLower.IsEmpty() && !Node->GetClass()->GetName().ToLower().Contains(ClassPatternLower))
        {
            continue;
        }
        if (!TitlePatternLower.IsEmpty()
            && !Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().ToLower().Contains(TitlePatternLower))
        {
            continue;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("node_name"), Node->GetName());
        Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
        Entry->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
        Entry->SetNumberField(TEXT("position_x"), Node->NodePosX);
        Entry->SetNumberField(TEXT("position_y"), Node->NodePosY);
        Entry->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
        Nodes.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("list_nodes"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("graph"), Graph->GetName());
    Result->SetStringField(TEXT("graph_kind"), Kind);
    Result->SetNumberField(TEXT("count"), Nodes.Num());
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetArrayField(TEXT("nodes"), Nodes);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpGraphCommands::GetNode(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    FString GraphName;
    if (!Params->TryGetStringField(TEXT("graph"), GraphName)
        && !Params->TryGetStringField(TEXT("graph_name"), GraphName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'graph' parameter is required"));
    }
    FString NodeName;
    if (!Params->TryGetStringField(TEXT("node"), NodeName)
        && !Params->TryGetStringField(TEXT("node_name"), NodeName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'node' parameter is required"));
    }

    FString Kind;
    UEdGraph* Graph = ResolveGraphByName(Blueprint, GraphName, Kind);
    if (!Graph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Graph '%s' not found"), *GraphName));
    }
    UEdGraphNode* Node = ResolveNodeByName(Graph, NodeName);
    if (!Node)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Node '%s' not found in graph '%s'"), *NodeName, *Graph->GetName()));
    }

    TArray<TSharedPtr<FJsonValue>> Pins;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin)
        {
            continue;
        }
        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
        PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
        PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        PinObj->SetStringField(TEXT("type"), BpGraph_DescribePinType(Pin->PinType));
        PinObj->SetBoolField(TEXT("is_exec"), Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
        if (!Pin->DefaultValue.IsEmpty())
        {
            PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
        }
        if (!Pin->AutogeneratedDefaultValue.IsEmpty())
        {
            PinObj->SetStringField(TEXT("autogen_default"), Pin->AutogeneratedDefaultValue);
        }
        if (Pin->DefaultObject)
        {
            PinObj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
        }
        TArray<TSharedPtr<FJsonValue>> Connections;
        for (UEdGraphPin* Linked : Pin->LinkedTo)
        {
            if (!Linked || !Linked->GetOwningNode())
            {
                continue;
            }
            TSharedPtr<FJsonObject> Conn = MakeShared<FJsonObject>();
            Conn->SetStringField(TEXT("target_node"), Linked->GetOwningNode()->GetName());
            Conn->SetStringField(TEXT("target_node_title"), Linked->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
            Conn->SetStringField(TEXT("target_pin"), Linked->PinName.ToString());
            Conn->SetStringField(TEXT("target_pin_type"), BpGraph_DescribePinType(Linked->PinType));
            Connections.Add(MakeShared<FJsonValueObject>(Conn));
        }
        PinObj->SetArrayField(TEXT("connections"), Connections);
        Pins.Add(MakeShared<FJsonValueObject>(PinObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("get_node"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("graph"), Graph->GetName());
    Result->SetStringField(TEXT("graph_kind"), Kind);
    Result->SetStringField(TEXT("node_name"), Node->GetName());
    Result->SetStringField(TEXT("class"), Node->GetClass()->GetName());
    Result->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
    Result->SetNumberField(TEXT("position_x"), Node->NodePosX);
    Result->SetNumberField(TEXT("position_y"), Node->NodePosY);
    Result->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
    Result->SetArrayField(TEXT("pins"), Pins);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpGraphCommands::ListConnections(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    FString GraphName;
    if (!Params->TryGetStringField(TEXT("graph"), GraphName)
        && !Params->TryGetStringField(TEXT("graph_name"), GraphName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'graph' parameter is required"));
    }
    FString Kind;
    UEdGraph* Graph = ResolveGraphByName(Blueprint, GraphName, Kind);
    if (!Graph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Graph '%s' not found"), *GraphName));
    }

    bool bIncludeExec = true;
    Params->TryGetBoolField(TEXT("include_exec"), bIncludeExec);
    bool bIncludeData = true;
    Params->TryGetBoolField(TEXT("include_data"), bIncludeData);

    int32 Limit = 1024;
    int32 ParsedLimit = 0;
    if (Params->TryGetNumberField(TEXT("limit"), ParsedLimit) && ParsedLimit > 0)
    {
        Limit = ParsedLimit;
    }

    TArray<TSharedPtr<FJsonValue>> Edges;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || Edges.Num() >= Limit)
        {
            continue;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output || Edges.Num() >= Limit)
            {
                continue;
            }
            const bool bIsExec = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
            if (bIsExec && !bIncludeExec)
            {
                continue;
            }
            if (!bIsExec && !bIncludeData)
            {
                continue;
            }
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                if (!Linked || !Linked->GetOwningNode() || Edges.Num() >= Limit)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
                Edge->SetStringField(TEXT("source_node"), Node->GetName());
                Edge->SetStringField(TEXT("source_node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
                Edge->SetStringField(TEXT("source_pin"), Pin->PinName.ToString());
                Edge->SetStringField(TEXT("target_node"), Linked->GetOwningNode()->GetName());
                Edge->SetStringField(TEXT("target_node_title"), Linked->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
                Edge->SetStringField(TEXT("target_pin"), Linked->PinName.ToString());
                Edge->SetBoolField(TEXT("is_exec"), bIsExec);
                Edges.Add(MakeShared<FJsonValueObject>(Edge));
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("list_connections"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("graph"), Graph->GetName());
    Result->SetStringField(TEXT("graph_kind"), Kind);
    Result->SetNumberField(TEXT("count"), Edges.Num());
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetArrayField(TEXT("connections"), Edges);
    return Result;
}
