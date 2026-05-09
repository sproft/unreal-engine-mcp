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
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

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

    /** Resolve a UPCGSettings subclass by short name, full path, or
     *  trailing-substring against the loaded class registry. Returns
     *  nullptr on miss. */
    UClass* ResolveSettingsClass(const FString& Token)
    {
        if (Token.IsEmpty()) return nullptr;

        // Full /Script/Module.ClassName path.
        if (Token.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Direct = FindObject<UClass>(nullptr, *Token))
            {
                if (Direct->IsChildOf(UPCGSettings::StaticClass())) return Direct;
            }
            return nullptr;
        }

        // Short-name match across loaded UPCGSettings subclasses. We
        // accept exact match (case-insensitive) on either GetName() or
        // a leading-U trim, and otherwise fall back to substring.
        UClass* ExactMatch = nullptr;
        UClass* SubstringMatch = nullptr;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* C = *It;
            if (!C || !C->IsChildOf(UPCGSettings::StaticClass())) continue;
            if (C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
            const FString Name = C->GetName();
            if (Name.Equals(Token, ESearchCase::IgnoreCase))
            {
                ExactMatch = C;
                break;
            }
            // Engine convention is "UPCGFooBarSettings" -> token "FooBar"
            // or "PCGFooBarSettings"; do an unprefixed compare too.
            FString Stripped = Name;
            Stripped.RemoveFromStart(TEXT("PCG"), ESearchCase::IgnoreCase);
            Stripped.RemoveFromEnd(TEXT("Settings"), ESearchCase::IgnoreCase);
            if (Stripped.Equals(Token, ESearchCase::IgnoreCase))
            {
                ExactMatch = C;
                break;
            }
            if (!SubstringMatch && Name.Contains(Token, ESearchCase::IgnoreCase))
            {
                SubstringMatch = C;
            }
        }
        return ExactMatch ? ExactMatch : SubstringMatch;
    }

    /** Resolve a UPCGNode under a graph by exact FName first, then by
     *  case-insensitive substring on FName / GetNodeTitle. */
    UPCGNode* ResolveNode(UPCGGraph* Graph, const FString& Token)
    {
        if (!Graph || Token.IsEmpty()) return nullptr;
        UPCGNode* SubstringMatch = nullptr;
        for (UPCGNode* Node : Graph->GetNodes())
        {
            if (!Node) continue;
            const FString Name = Node->GetFName().ToString();
            if (Name.Equals(Token, ESearchCase::IgnoreCase)) return Node;
            const FString Title = Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString();
            if (Title.Equals(Token, ESearchCase::IgnoreCase)) return Node;
            if (!SubstringMatch
                && (Name.Contains(Token, ESearchCase::IgnoreCase)
                    || Title.Contains(Token, ESearchCase::IgnoreCase)))
            {
                SubstringMatch = Node;
            }
        }
        return SubstringMatch;
    }

    /** Pick a default pin label by direction. Returns NAME_None when
     *  the node has no matching pins (the caller surfaces a clear
     *  error in that case). */
    FName DefaultPinLabel(UPCGNode* Node, bool bOutput)
    {
        if (!Node) return NAME_None;
        const TArray<TObjectPtr<UPCGPin>>& Pins = bOutput ? Node->GetOutputPins() : Node->GetInputPins();
        for (const UPCGPin* Pin : Pins)
        {
            if (Pin) return Pin->Properties.Label;
        }
        return NAME_None;
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
    if (Op.Equals(TEXT("add_node"), ESearchCase::IgnoreCase))
    {
        return HandleAddNode(Params);
    }
    if (Op.Equals(TEXT("connect_pins"), ESearchCase::IgnoreCase))
    {
        return HandleConnectPins(Params);
    }
    if (Op.Equals(TEXT("remove_node"), ESearchCase::IgnoreCase))
    {
        return HandleRemoveNode(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("pcg_graph_edit: unsupported op '%s'. Supported: inspect, add_node, connect_pins, remove_node"), *Op));
}

TSharedPtr<FJsonObject> FSproftPcgGraphEditCommands::HandleAddNode(const TSharedPtr<FJsonObject>& Params)
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

    FString SettingsClassToken;
    if (!Params->TryGetStringField(TEXT("settings_class"), SettingsClassToken)
        && !Params->TryGetStringField(TEXT("class"), SettingsClassToken)
        && !Params->TryGetStringField(TEXT("node_class"), SettingsClassToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'settings_class' parameter"));
    }
    UClass* SettingsClass = ResolveSettingsClass(SettingsClassToken);
    if (!SettingsClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UPCGSettings subclass '%s'"), *SettingsClassToken));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UPCGSettings* DefaultSettings = nullptr;
    UPCGNode* NewNode = Graph->AddNodeOfType(SettingsClass, DefaultSettings);
    if (!NewNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UPCGGraph::AddNodeOfType returned null for settings class '%s'"), *SettingsClass->GetName()));
    }

    // Optional rename. PCG node FName is independent of settings class
    // name, so a designer-readable alias keeps the downstream
    // connect_pins call from depending on engine-mangled GUIDs.
    FString NodeName;
    if (Params->TryGetStringField(TEXT("node_name"), NodeName) && !NodeName.IsEmpty())
    {
        NewNode->Rename(*NodeName, /*NewOuter=*/nullptr, REN_DontCreateRedirectors);
    }

    // Optional 2D editor position.
    const TSharedPtr<FJsonObject>* PositionObj = nullptr;
    if (Params->TryGetObjectField(TEXT("position"), PositionObj) && PositionObj && PositionObj->IsValid())
    {
        double XVal = 0.0;
        double YVal = 0.0;
        (*PositionObj)->TryGetNumberField(TEXT("x"), XVal);
        (*PositionObj)->TryGetNumberField(TEXT("y"), YVal);
        NewNode->SetNodePosition(static_cast<int32>(XVal), static_cast<int32>(YVal));
    }

    Graph->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Graph->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("add_node"));
    Out->SetStringField(TEXT("graph"), Graph->GetPathName());
    Out->SetStringField(TEXT("node_name"), NewNode->GetFName().ToString());
    Out->SetStringField(TEXT("node_title"), NewNode->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
    Out->SetStringField(TEXT("settings_class"), SettingsClass->GetName());
    Out->SetStringField(TEXT("settings_class_path"), SettingsClass->GetPathName());
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}

TSharedPtr<FJsonObject> FSproftPcgGraphEditCommands::HandleConnectPins(const TSharedPtr<FJsonObject>& Params)
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

    FString FromNodeToken;
    FString ToNodeToken;
    Params->TryGetStringField(TEXT("from_node"), FromNodeToken);
    Params->TryGetStringField(TEXT("to_node"), ToNodeToken);
    if (FromNodeToken.IsEmpty() || ToNodeToken.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Both 'from_node' and 'to_node' are required"));
    }

    // The graph IO nodes are not in the `Nodes` array and need explicit
    // sentinel tokens. We accept "input" / "graph_input" for
    // GetInputNode and "output" / "graph_output" for GetOutputNode.
    auto ResolveOptionalIoNode = [&](const FString& Token, bool bWantInput) -> UPCGNode*
    {
        if (Token.Equals(TEXT("input"), ESearchCase::IgnoreCase)
            || Token.Equals(TEXT("graph_input"), ESearchCase::IgnoreCase)
            || Token.Equals(TEXT("graph input"), ESearchCase::IgnoreCase))
        {
            return Graph->GetInputNode();
        }
        if (Token.Equals(TEXT("output"), ESearchCase::IgnoreCase)
            || Token.Equals(TEXT("graph_output"), ESearchCase::IgnoreCase)
            || Token.Equals(TEXT("graph output"), ESearchCase::IgnoreCase))
        {
            return Graph->GetOutputNode();
        }
        return ResolveNode(Graph, Token);
    };

    UPCGNode* FromNode = ResolveOptionalIoNode(FromNodeToken, /*bWantInput=*/true);
    UPCGNode* ToNode = ResolveOptionalIoNode(ToNodeToken, /*bWantInput=*/false);
    if (!FromNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve from_node '%s'"), *FromNodeToken));
    }
    if (!ToNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve to_node '%s'"), *ToNodeToken));
    }

    FString FromPinToken;
    FString ToPinToken;
    Params->TryGetStringField(TEXT("from_pin"), FromPinToken);
    Params->TryGetStringField(TEXT("to_pin"), ToPinToken);

    // Default to the first matching pin on each side when the caller
    // does not specify. Output pins from the upstream node, input pins
    // on the downstream node.
    FName FromPinLabel = FromPinToken.IsEmpty() ? DefaultPinLabel(FromNode, /*bOutput=*/true)  : FName(*FromPinToken);
    FName ToPinLabel   = ToPinToken.IsEmpty()   ? DefaultPinLabel(ToNode,   /*bOutput=*/false) : FName(*ToPinToken);
    if (FromPinLabel.IsNone())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("from_node '%s' has no output pins"), *FromNode->GetFName().ToString()));
    }
    if (ToPinLabel.IsNone())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("to_node '%s' has no input pins"), *ToNode->GetFName().ToString()));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UPCGNode* Result = Graph->AddEdge(FromNode, FromPinLabel, ToNode, ToPinLabel);
    if (!Result)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UPCGGraph::AddEdge refused %s.%s -> %s.%s"),
                *FromNode->GetFName().ToString(), *FromPinLabel.ToString(),
                *ToNode->GetFName().ToString(),   *ToPinLabel.ToString()));
    }

    Graph->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Graph->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("connect_pins"));
    Out->SetStringField(TEXT("graph"), Graph->GetPathName());
    Out->SetStringField(TEXT("from_node"), FromNode->GetFName().ToString());
    Out->SetStringField(TEXT("from_pin"), FromPinLabel.ToString());
    Out->SetStringField(TEXT("to_node"), ToNode->GetFName().ToString());
    Out->SetStringField(TEXT("to_pin"), ToPinLabel.ToString());
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}

TSharedPtr<FJsonObject> FSproftPcgGraphEditCommands::HandleRemoveNode(const TSharedPtr<FJsonObject>& Params)
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

    FString NodeToken;
    Params->TryGetStringField(TEXT("node"), NodeToken);
    if (NodeToken.IsEmpty()) Params->TryGetStringField(TEXT("node_name"), NodeToken);
    if (NodeToken.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node' parameter"));
    }
    UPCGNode* Node = ResolveNode(Graph, NodeToken);
    if (!Node)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve node '%s' on graph '%s'"), *NodeToken, *Graph->GetPathName()));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    const FString RemovedName = Node->GetFName().ToString();
    Graph->RemoveNode(Node);

    Graph->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Graph->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("remove_node"));
    Out->SetStringField(TEXT("graph"), Graph->GetPathName());
    Out->SetStringField(TEXT("removed_node"), RemovedName);
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
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
