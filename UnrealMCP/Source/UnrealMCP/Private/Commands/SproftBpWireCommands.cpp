#include "Commands/SproftBpWireCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Class.h"

namespace
{
    UBlueprint* BpWire_ResolveBlueprintParam(const TSharedPtr<FJsonObject>& Params)
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

    template <typename FnType>
    void BpWire_ForEachGraph(UBlueprint* Blueprint, FnType Callback)
    {
        for (UEdGraph* G : Blueprint->UbergraphPages)     { if (G) { Callback(G); } }
        for (UEdGraph* G : Blueprint->FunctionGraphs)     { if (G) { Callback(G); } }
        for (UEdGraph* G : Blueprint->MacroGraphs)        { if (G) { Callback(G); } }
        for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
        {
            for (UEdGraph* G : Iface.Graphs) { if (G) { Callback(G); } }
        }
    }

    UEdGraph* BpWire_ResolveGraph(UBlueprint* Blueprint, const FString& InName)
    {
        if (InName.IsEmpty())
        {
            return Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
        }
        UEdGraph* Match = nullptr;
        BpWire_ForEachGraph(Blueprint, [&](UEdGraph* G)
        {
            if (Match) { return; }
            if (G->GetName().Equals(InName, ESearchCase::IgnoreCase)) { Match = G; }
        });
        if (Match) { return Match; }
        const FString Lower = InName.ToLower();
        BpWire_ForEachGraph(Blueprint, [&](UEdGraph* G)
        {
            if (Match) { return; }
            if (G->GetName().ToLower().Contains(Lower)) { Match = G; }
        });
        return Match;
    }

    UEdGraphNode* BpWire_ResolveNode(UEdGraph* Graph, const FString& InName)
    {
        if (InName.IsEmpty()) { return nullptr; }
        // Pass 1: GUID match.
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid.IsValid() && Node->NodeGuid.ToString().Equals(InName, ESearchCase::IgnoreCase))
            {
                return Node;
            }
        }
        // Pass 2: FName match.
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->GetName().Equals(InName, ESearchCase::IgnoreCase))
            {
                return Node;
            }
        }
        // Pass 3: title substring fallback (handy for "Print String" etc.).
        const FString Lower = InName.ToLower();
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().ToLower().Contains(Lower))
            {
                return Node;
            }
        }
        return nullptr;
    }

    UEdGraphPin* FindPinFlexible(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
    {
        if (!Node) { return nullptr; }
        // Pass 1: direction-strict FName match.
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == Direction && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }
        // Pass 2: any-direction FName match (caller will see the validation
        // error and we surface a useful "wrong direction" message).
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }
        return nullptr;
    }
}

FSproftBpWireCommands::FSproftBpWireCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpWireCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_wire"))
    {
        return HandleBpWire(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_wire command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpWireCommands::HandleBpWire(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Operation = TEXT("connect");
    Params->TryGetStringField(TEXT("op"), Operation);
    Params->TryGetStringField(TEXT("operation"), Operation);
    Operation = Operation.ToLower();
    const bool bDisconnectOp = (Operation == TEXT("disconnect") || Operation == TEXT("break"));
    if (Operation != TEXT("connect") && Operation != TEXT("wire") && Operation != TEXT("disconnect") && Operation != TEXT("break"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported bp_wire op '%s'. Supported: connect, disconnect"), *Operation));
    }

    UBlueprint* Blueprint = BpWire_ResolveBlueprintParam(Params);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Blueprint not found or 'blueprint' parameter missing"));
    }

    FString GraphName;
    Params->TryGetStringField(TEXT("graph"), GraphName);
    Params->TryGetStringField(TEXT("graph_name"), GraphName);
    UEdGraph* Graph = BpWire_ResolveGraph(Blueprint, GraphName);
    if (!Graph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Graph '%s' not found on Blueprint '%s'"), *GraphName, *Blueprint->GetName()));
    }

    // Collect edges. Either a list on `connections` / `edges` / `wires`, or
    // an inline single edge through source_node / source_pin / dest_node /
    // dest_pin keys on the params object.
    TArray<TSharedPtr<FJsonValue>> Specs;
    const TArray<TSharedPtr<FJsonValue>>* SpecArray = nullptr;
    if (Params->TryGetArrayField(TEXT("connections"), SpecArray)
        || Params->TryGetArrayField(TEXT("edges"), SpecArray)
        || Params->TryGetArrayField(TEXT("wires"), SpecArray))
    {
        if (SpecArray) { Specs = *SpecArray; }
    }
    if (Specs.Num() == 0)
    {
        if (Params->HasField(TEXT("source_node")) || Params->HasField(TEXT("dest_node")) || Params->HasField(TEXT("target_node")))
        {
            Specs.Add(MakeShared<FJsonValueObject>(Params));
        }
    }

    if (Specs.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Pass either 'connections' (array of {source_node, source_pin, dest_node, dest_pin}) or an inline edge"));
    }

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = false;
    Params->TryGetBoolField(TEXT("save"), bSave);

    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

    TArray<TSharedPtr<FJsonValue>> Applied;
    TArray<TSharedPtr<FJsonValue>> Failures;
    for (int32 Index = 0; Index < Specs.Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& Entry = Specs[Index];
        if (!Entry.IsValid() || Entry->Type != EJson::Object)
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), TEXT("entry is not a JSON object"));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }
        TSharedPtr<FJsonObject> Spec = Entry->AsObject();
        if (!Spec.IsValid())
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), TEXT("entry is empty"));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }

        // Per-entry disconnect flag overrides the op default.
        bool bEntryDisconnect = bDisconnectOp;
        Spec->TryGetBoolField(TEXT("disconnect"), bEntryDisconnect);

        FString SourceNodeName;
        if (!Spec->TryGetStringField(TEXT("source_node"), SourceNodeName)
            && !Spec->TryGetStringField(TEXT("from_node"), SourceNodeName)
            && !Spec->TryGetStringField(TEXT("source"), SourceNodeName))
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), TEXT("missing 'source_node'"));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }
        FString SourcePinName;
        if (!Spec->TryGetStringField(TEXT("source_pin"), SourcePinName)
            && !Spec->TryGetStringField(TEXT("from_pin"), SourcePinName))
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), TEXT("missing 'source_pin'"));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }
        FString DestNodeName;
        if (!Spec->TryGetStringField(TEXT("dest_node"), DestNodeName)
            && !Spec->TryGetStringField(TEXT("target_node"), DestNodeName)
            && !Spec->TryGetStringField(TEXT("to_node"), DestNodeName))
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), TEXT("missing 'dest_node'"));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }
        FString DestPinName;
        if (!Spec->TryGetStringField(TEXT("dest_pin"), DestPinName)
            && !Spec->TryGetStringField(TEXT("target_pin"), DestPinName)
            && !Spec->TryGetStringField(TEXT("to_pin"), DestPinName))
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), TEXT("missing 'dest_pin'"));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }

        UEdGraphNode* SourceNode = BpWire_ResolveNode(Graph, SourceNodeName);
        if (!SourceNode)
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), FString::Printf(TEXT("source_node '%s' not found"), *SourceNodeName));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }
        UEdGraphNode* DestNode = BpWire_ResolveNode(Graph, DestNodeName);
        if (!DestNode)
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), FString::Printf(TEXT("dest_node '%s' not found"), *DestNodeName));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }

        UEdGraphPin* SourcePin = FindPinFlexible(SourceNode, SourcePinName, EGPD_Output);
        if (!SourcePin)
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), FString::Printf(TEXT("source_pin '%s' not found on '%s'"), *SourcePinName, *SourceNode->GetName()));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }
        UEdGraphPin* DestPin = FindPinFlexible(DestNode, DestPinName, EGPD_Input);
        if (!DestPin)
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), FString::Printf(TEXT("dest_pin '%s' not found on '%s'"), *DestPinName, *DestNode->GetName()));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }

        if (SourcePin->Direction != EGPD_Output)
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), FString::Printf(TEXT("source_pin '%s' is not an output pin"), *SourcePinName));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }
        if (DestPin->Direction != EGPD_Input)
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), FString::Printf(TEXT("dest_pin '%s' is not an input pin"), *DestPinName));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }

        if (bEntryDisconnect)
        {
            if (SourcePin->LinkedTo.Contains(DestPin))
            {
                SourcePin->BreakLinkTo(DestPin);
                TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
                Edge->SetNumberField(TEXT("index"), Index);
                Edge->SetStringField(TEXT("source_node"), SourceNode->GetName());
                Edge->SetStringField(TEXT("source_pin"), SourcePin->PinName.ToString());
                Edge->SetStringField(TEXT("dest_node"), DestNode->GetName());
                Edge->SetStringField(TEXT("dest_pin"), DestPin->PinName.ToString());
                Edge->SetStringField(TEXT("action"), TEXT("disconnected"));
                Applied.Add(MakeShared<FJsonValueObject>(Edge));
            }
            else
            {
                TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
                Fail->SetNumberField(TEXT("index"), Index);
                Fail->SetStringField(TEXT("error"), TEXT("pins were not connected; nothing to break"));
                Failures.Add(MakeShared<FJsonValueObject>(Fail));
            }
            continue;
        }

        // Validate compatibility through the K2 schema. Falling back to a
        // category-equality check is fine for non-K2 graphs.
        bool bAllowConnection = false;
        FString CompatErr;
        if (K2Schema)
        {
            const FPinConnectionResponse Resp = K2Schema->CanCreateConnection(SourcePin, DestPin);
            bAllowConnection = (Resp.Response != CONNECT_RESPONSE_DISALLOW);
            if (!bAllowConnection)
            {
                CompatErr = Resp.Message.ToString();
            }
        }
        else
        {
            bAllowConnection = (SourcePin->PinType.PinCategory == DestPin->PinType.PinCategory);
            if (!bAllowConnection)
            {
                CompatErr = TEXT("pin categories do not match");
            }
        }
        if (!bAllowConnection)
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), CompatErr.IsEmpty() ? TEXT("incompatible pins") : CompatErr);
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }

        SourcePin->MakeLinkTo(DestPin);
        TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
        Edge->SetNumberField(TEXT("index"), Index);
        Edge->SetStringField(TEXT("source_node"), SourceNode->GetName());
        Edge->SetStringField(TEXT("source_pin"), SourcePin->PinName.ToString());
        Edge->SetStringField(TEXT("dest_node"), DestNode->GetName());
        Edge->SetStringField(TEXT("dest_pin"), DestPin->PinName.ToString());
        Edge->SetStringField(TEXT("action"), TEXT("connected"));
        Applied.Add(MakeShared<FJsonValueObject>(Edge));
    }

    Graph->NotifyGraphChanged();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), bDisconnectOp ? TEXT("disconnect") : TEXT("connect"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("graph"), Graph->GetName());
    Result->SetNumberField(TEXT("requested"), Specs.Num());
    Result->SetNumberField(TEXT("applied"), Applied.Num());
    Result->SetNumberField(TEXT("failed"), Failures.Num());
    Result->SetArrayField(TEXT("connections"), Applied);
    Result->SetArrayField(TEXT("failures"), Failures);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}
