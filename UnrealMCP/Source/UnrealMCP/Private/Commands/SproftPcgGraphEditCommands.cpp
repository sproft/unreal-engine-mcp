#include "Commands/SproftPcgGraphEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "PCGCommon.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Data/Registry/PCGDataTypeIdentifier.h"
#include "Modules/ModuleManager.h"
#include "UObject/Class.h"

namespace
{
    /** Resolve a UPCGGraph by `/Game/...` path or short asset name. */
    UPCGGraph* ResolvePcgGraph(const FString& Input)
    {
        if (Input.IsEmpty()) return nullptr;
        if (Input.StartsWith(TEXT("/")))
        {
            return Cast<UPCGGraph>(UEditorAssetLibrary::LoadAsset(Input));
        }
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(UPCGGraph::StaticClass()->GetClassPathName(), Found, /*bSearchSubClasses=*/true);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Input, ESearchCase::IgnoreCase))
            {
                return Cast<UPCGGraph>(Data.GetAsset());
            }
        }
        return nullptr;
    }

    const TCHAR* PinUsageToken(EPCGPinUsage Usage)
    {
        switch (Usage)
        {
        case EPCGPinUsage::Normal:         return TEXT("normal");
        case EPCGPinUsage::Loop:           return TEXT("loop");
        case EPCGPinUsage::Feedback:       return TEXT("feedback");
        case EPCGPinUsage::DependencyOnly: return TEXT("dependency_only");
        default:                           return TEXT("unknown");
        }
    }

    const TCHAR* PinStatusToken(EPCGPinStatus Status)
    {
        switch (Status)
        {
        case EPCGPinStatus::Normal:               return TEXT("normal");
        case EPCGPinStatus::Required:             return TEXT("required");
        case EPCGPinStatus::Advanced:             return TEXT("advanced");
        case EPCGPinStatus::OverrideOrUserParam:  return TEXT("override_or_user_param");
        default:                                  return TEXT("unknown");
        }
    }

    /** Render one UPCGPin as a JSON dict. The pin's owner is implied by
     *  the surrounding node block; we surface label, allowed types,
     *  pin status / usage flags, and the local edge count. */
    TSharedPtr<FJsonObject> PinRecord(const UPCGPin* Pin, int32 LocalIndex)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("index"), LocalIndex);
        if (!Pin)
        {
            Out->SetStringField(TEXT("error"), TEXT("null pin"));
            return Out;
        }
        const FPCGPinProperties& Props = Pin->Properties;
        Out->SetStringField(TEXT("label"), Props.Label.ToString());
        Out->SetStringField(TEXT("type"), Props.AllowedTypes.ToString());
        Out->SetStringField(TEXT("usage"), PinUsageToken(Props.Usage));
        Out->SetStringField(TEXT("status"), PinStatusToken(Props.PinStatus));
        Out->SetBoolField(TEXT("multiple_data"), Props.bAllowMultipleData);
        Out->SetBoolField(TEXT("multiple_connections"), Props.AllowsMultipleConnections());
        Out->SetBoolField(TEXT("invisible"), Props.bInvisiblePin);
        Out->SetNumberField(TEXT("edge_count"), Pin->Edges.Num());
        return Out;
    }

    /** Walk a node's input / output pin list and stuff arrays into the
     *  emit dict in place. Returns the totals so the caller can roll
     *  them up. */
    void WriteNodePins(TSharedPtr<FJsonObject> Out, const UPCGNode* Node, bool bIncludePins)
    {
        if (!Node) return;
        const TArray<TObjectPtr<UPCGPin>>& InPins = Node->GetInputPins();
        const TArray<TObjectPtr<UPCGPin>>& OutPins = Node->GetOutputPins();
        Out->SetNumberField(TEXT("input_pin_count"),  InPins.Num());
        Out->SetNumberField(TEXT("output_pin_count"), OutPins.Num());
        if (!bIncludePins) return;

        TArray<TSharedPtr<FJsonValue>> InArr;
        InArr.Reserve(InPins.Num());
        for (int32 I = 0; I < InPins.Num(); ++I)
        {
            InArr.Add(MakeShared<FJsonValueObject>(PinRecord(InPins[I], I)));
        }
        Out->SetArrayField(TEXT("inputs"), InArr);

        TArray<TSharedPtr<FJsonValue>> OutArr;
        OutArr.Reserve(OutPins.Num());
        for (int32 I = 0; I < OutPins.Num(); ++I)
        {
            OutArr.Add(MakeShared<FJsonValueObject>(PinRecord(OutPins[I], I)));
        }
        Out->SetArrayField(TEXT("outputs"), OutArr);
    }
}

FSproftPcgGraphEditCommands::FSproftPcgGraphEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftPcgGraphEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("pcg_graph_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown pcg_graph_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
    }
    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op.Equals(TEXT("inspect"), ESearchCase::IgnoreCase))
    {
        return HandlePcgGraphInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("pcg_graph_edit: unsupported op '%s'. Only 'inspect' is shipped on this slice"), *Op));
}

TSharedPtr<FJsonObject> FSproftPcgGraphEditCommands::HandlePcgGraphInspect(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphParam;
    if (!Params->TryGetStringField(TEXT("graph"), GraphParam)
        && !Params->TryGetStringField(TEXT("path"), GraphParam)
        && !Params->TryGetStringField(TEXT("asset"), GraphParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'graph' parameter"));
    }
    UPCGGraph* Graph = ResolvePcgGraph(GraphParam);
    if (!Graph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UPCGGraph '%s'"), *GraphParam));
    }

    bool bIncludePins = true;
    bool bIncludeEdges = true;
    int32 MaxNodes = 1024;
    int32 MaxEdges = 4096;
    Params->TryGetBoolField(TEXT("include_pins"), bIncludePins);
    Params->TryGetBoolField(TEXT("include_edges"), bIncludeEdges);
    Params->TryGetNumberField(TEXT("max_nodes"), MaxNodes);
    Params->TryGetNumberField(TEXT("max_edges"), MaxEdges);
    if (MaxNodes < 1) MaxNodes = 1;
    if (MaxEdges < 1) MaxEdges = 1;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("inspect"));
    Result->SetStringField(TEXT("name"), Graph->GetName());
    Result->SetStringField(TEXT("path"), Graph->GetPathName());
    Result->SetStringField(TEXT("class"), Graph->GetClass()->GetName());

    // Map every UPCGNode pointer to its emitted index so the edge walk
    // can address nodes by integer rather than asset path. The graph
    // input / output nodes get -1 / -2 sentinels (real arrays use
    // 0..N-1) so a downstream consumer can branch cleanly.
    TMap<const UPCGNode*, int32> NodeIndex;
    UPCGNode* InputNode = Graph->GetInputNode();
    UPCGNode* OutputNode = Graph->GetOutputNode();
    if (InputNode)  { NodeIndex.Add(InputNode, -1); }
    if (OutputNode) { NodeIndex.Add(OutputNode, -2); }

    // Per-node walk. Truncates at MaxNodes and tags `nodes_truncated`.
    const TArray<UPCGNode*>& Nodes = Graph->GetNodes();
    const int32 TotalNodeCount = Nodes.Num();
    const int32 EmittedNodeCount = FMath::Min(TotalNodeCount, MaxNodes);

    TArray<TSharedPtr<FJsonValue>> NodeArr;
    NodeArr.Reserve(EmittedNodeCount);
    for (int32 I = 0; I < EmittedNodeCount; ++I)
    {
        UPCGNode* Node = Nodes[I];
        if (!Node)
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetNumberField(TEXT("index"), I);
            Skip->SetStringField(TEXT("error"), TEXT("null node"));
            NodeArr.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }
        NodeIndex.Add(Node, I);

        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("index"), I);
        Row->SetStringField(TEXT("name"), Node->GetFName().ToString());
        Row->SetStringField(TEXT("title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());

        if (UPCGSettings* Settings = Node->GetSettings())
        {
            Row->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
            Row->SetStringField(TEXT("settings_class_path"), Settings->GetClass()->GetPathName());
        }

        int32 PosX = 0;
        int32 PosY = 0;
        Node->GetNodePosition(PosX, PosY);
        TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
        PosObj->SetNumberField(TEXT("x"), PosX);
        PosObj->SetNumberField(TEXT("y"), PosY);
        Row->SetObjectField(TEXT("position"), PosObj);

        WriteNodePins(Row, Node, bIncludePins);
        NodeArr.Add(MakeShared<FJsonValueObject>(Row));
    }
    Result->SetArrayField(TEXT("nodes"), NodeArr);
    Result->SetNumberField(TEXT("node_count"), EmittedNodeCount);
    Result->SetNumberField(TEXT("node_count_total"), TotalNodeCount);
    Result->SetBoolField(TEXT("nodes_truncated"), TotalNodeCount > EmittedNodeCount);

    // Graph IO surface. The exposed input pins on the graph live on
    // the `InputNode`'s output pin array (downstream-facing from the
    // graph's perspective), and the exposed output pins on the graph
    // live on the `OutputNode`'s input pin array. PCG mirrors the
    // editor naming, so we surface both as straight pin lists keyed
    // by their PCG-side accessor.
    if (InputNode)
    {
        TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        Block->SetStringField(TEXT("name"), InputNode->GetFName().ToString());
        WriteNodePins(Block, InputNode, bIncludePins);
        Result->SetObjectField(TEXT("graph_inputs"), Block);
    }
    if (OutputNode)
    {
        TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        Block->SetStringField(TEXT("name"), OutputNode->GetFName().ToString());
        WriteNodePins(Block, OutputNode, bIncludePins);
        Result->SetObjectField(TEXT("graph_outputs"), Block);
    }

    // Edge walk. PCG names the edge endpoints `InputPin` (upstream
    // side, the source) and `OutputPin` (downstream side, the sink),
    // which is the reverse of typical editor convention. We surface
    // them as `from` (upstream) / `to` (downstream) so callers do not
    // have to remember the reversed naming. Enumerate edges off each
    // upstream pin (output pins on a node) so we never count the same
    // edge twice.
    if (bIncludeEdges)
    {
        int32 TotalEdgeCount = 0;
        TArray<TSharedPtr<FJsonValue>> EdgeArr;

        auto EmitEdgesForNode = [&](UPCGNode* Node, int32 NodeIdx)
        {
            if (!Node) return;
            const TArray<TObjectPtr<UPCGPin>>& OutPins = Node->GetOutputPins();
            for (int32 PinI = 0; PinI < OutPins.Num(); ++PinI)
            {
                const UPCGPin* SrcPin = OutPins[PinI];
                if (!SrcPin) continue;
                for (const UPCGEdge* Edge : SrcPin->Edges)
                {
                    if (!Edge) continue;
                    ++TotalEdgeCount;
                    if (EdgeArr.Num() >= MaxEdges) continue;

                    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                    Row->SetNumberField(TEXT("from_node"), NodeIdx);
                    Row->SetStringField(TEXT("from_pin"), SrcPin->Properties.Label.ToString());

                    const UPCGPin* DstPin = Edge->OutputPin;
                    if (DstPin)
                    {
                        if (const UPCGNode* DstNode = DstPin->Node)
                        {
                            const int32* Found = NodeIndex.Find(DstNode);
                            Row->SetNumberField(TEXT("to_node"), Found ? *Found : INDEX_NONE);
                        }
                        Row->SetStringField(TEXT("to_pin"), DstPin->Properties.Label.ToString());
                    }
                    EdgeArr.Add(MakeShared<FJsonValueObject>(Row));
                }
            }
        };

        // Walk the regular nodes plus the graph input node so we cover
        // every upstream-side pin in the graph.
        for (int32 I = 0; I < EmittedNodeCount; ++I)
        {
            EmitEdgesForNode(Nodes[I], I);
        }
        EmitEdgesForNode(InputNode, -1);

        Result->SetArrayField(TEXT("edges"), EdgeArr);
        Result->SetNumberField(TEXT("edge_count"), EdgeArr.Num());
        Result->SetNumberField(TEXT("edge_count_total"), TotalEdgeCount);
        Result->SetBoolField(TEXT("edges_truncated"), TotalEdgeCount > EdgeArr.Num());
    }

    return Result;
}
