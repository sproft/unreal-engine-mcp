#include "Commands/SproftBpNodesCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FormatText.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    UBlueprint* ResolveBlueprintParam(const TSharedPtr<FJsonObject>& Params)
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

    /** Walk every graph the Blueprint owns and pass it to a callback. */
    template <typename FnType>
    void ForEachGraph(UBlueprint* Blueprint, FnType Callback)
    {
        for (UEdGraph* G : Blueprint->UbergraphPages)     { if (G) { Callback(G); } }
        for (UEdGraph* G : Blueprint->FunctionGraphs)     { if (G) { Callback(G); } }
        for (UEdGraph* G : Blueprint->MacroGraphs)        { if (G) { Callback(G); } }
        for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
        {
            for (UEdGraph* G : Iface.Graphs) { if (G) { Callback(G); } }
        }
    }

    /** Resolve a graph by name (case-insensitive, exact then substring). Defaults
     *  to the first event graph when the caller passes no name. */
    UEdGraph* ResolveGraph(UBlueprint* Blueprint, const FString& InName)
    {
        if (InName.IsEmpty())
        {
            return Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
        }
        UEdGraph* Match = nullptr;
        ForEachGraph(Blueprint, [&](UEdGraph* G)
        {
            if (Match) { return; }
            if (G->GetName().Equals(InName, ESearchCase::IgnoreCase)) { Match = G; }
        });
        if (Match) { return Match; }
        const FString Lower = InName.ToLower();
        ForEachGraph(Blueprint, [&](UEdGraph* G)
        {
            if (Match) { return; }
            if (G->GetName().ToLower().Contains(Lower)) { Match = G; }
        });
        return Match;
    }

    /** Resolve a UClass from a short name or a full /Script/Module.ClassName path. */
    UClass* ResolveTargetClass(const FString& InText)
    {
        if (InText.IsEmpty()) { return nullptr; }
        if (InText.StartsWith(TEXT("/")))
        {
            UClass* AsClass = Cast<UClass>(UEditorAssetLibrary::LoadAsset(InText));
            if (AsClass) { return AsClass; }
            // /Game/.../BP_Foo.BP_Foo paths point to UBlueprint; pull the
            // generated class out so the cast node target is sensible.
            if (UBlueprint* AsBP = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(InText)))
            {
                return AsBP->GeneratedClass;
            }
            return nullptr;
        }
        if (UClass* Direct = FindFirstObject<UClass>(*InText, EFindFirstObjectOptions::EnsureIfAmbiguous))
        {
            return Direct;
        }
        // Tolerate a "U" / "A" prefix on the short name.
        const FString WithoutPrefix = InText.RightChop(1);
        if (!WithoutPrefix.IsEmpty())
        {
            if (UClass* Stripped = FindFirstObject<UClass>(*WithoutPrefix, EFindFirstObjectOptions::EnsureIfAmbiguous))
            {
                return Stripped;
            }
        }
        return nullptr;
    }

    void ApplyPosition(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Spec)
    {
        if (!Spec.IsValid()) { return; }
        if (Spec->HasField(TEXT("position")))
        {
            const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
            if (Spec->TryGetArrayField(TEXT("position"), PositionArr) && PositionArr && PositionArr->Num() >= 2)
            {
                Node->NodePosX = static_cast<int32>((*PositionArr)[0]->AsNumber());
                Node->NodePosY = static_cast<int32>((*PositionArr)[1]->AsNumber());
                return;
            }
        }
        double X = 0.0;
        double Y = 0.0;
        Spec->TryGetNumberField(TEXT("pos_x"), X);
        Spec->TryGetNumberField(TEXT("pos_y"), Y);
        Node->NodePosX = static_cast<int32>(X);
        Node->NodePosY = static_cast<int32>(Y);
    }

    /** Optionally re-FName the node so callers can address it through `bp_wire`
     *  with a known label. Falls back to the auto-generated UE FName silently. */
    void ApplyDesiredName(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Spec)
    {
        if (!Spec.IsValid() || !Node) { return; }
        FString DesiredName;
        if (!Spec->TryGetStringField(TEXT("name"), DesiredName)
            && !Spec->TryGetStringField(TEXT("node_name"), DesiredName))
        {
            return;
        }
        if (DesiredName.IsEmpty()) { return; }
        const FName Candidate = MakeUniqueObjectName(Node->GetOuter(), Node->GetClass(), FName(*DesiredName));
        Node->Rename(*Candidate.ToString(), Node->GetOuter(), REN_DontCreateRedirectors | REN_NonTransactional);
    }

    /** Resolve a UFunction by /Script/Module.ClassName:FunctionName, by
     *  ClassName:FunctionName, or by bare FunctionName looking inside the
     *  Blueprint's own GeneratedClass. */
    UFunction* ResolveFunctionRef(UBlueprint* Blueprint, const FString& InFuncRef)
    {
        if (InFuncRef.IsEmpty()) { return nullptr; }
        FString Left, Right;
        if (InFuncRef.Split(TEXT(":"), &Left, &Right))
        {
            UClass* OwnerClass = ResolveTargetClass(Left);
            if (!OwnerClass) { return nullptr; }
            return OwnerClass->FindFunctionByName(FName(*Right));
        }
        // Bare name -> inspect the Blueprint's own class.
        UClass* OwnClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->SkeletonGeneratedClass;
        return OwnClass ? OwnClass->FindFunctionByName(FName(*InFuncRef)) : nullptr;
    }

    /** Apply optional pin defaults from a `pin_defaults` dict. Each entry
     *  maps pin name -> string (we run it through DefaultValue directly). */
    void ApplyPinDefaults(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Spec)
    {
        if (!Spec.IsValid() || !Node) { return; }
        const TSharedPtr<FJsonObject>* Defaults = nullptr;
        if (!Spec->TryGetObjectField(TEXT("pin_defaults"), Defaults) || !Defaults || !Defaults->IsValid())
        {
            return;
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Defaults)->Values)
        {
            UEdGraphPin* Pin = Node->FindPin(FName(*Pair.Key));
            if (!Pin) { continue; }
            if (Pair.Value->Type == EJson::String)
            {
                Pin->DefaultValue = Pair.Value->AsString();
            }
            else
            {
                FString Stringified;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Stringified);
                FJsonSerializer::Serialize(Pair.Value.ToSharedRef(), TEXT(""), Writer);
                Pin->DefaultValue = Stringified;
            }
        }
    }

    /** Build a small per-pin record for the response. */
    TArray<TSharedPtr<FJsonValue>> SnapshotPins(UEdGraphNode* Node)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        if (!Node) { return Out; }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) { continue; }
            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
            PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
            PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
            PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
            PinObj->SetBoolField(TEXT("is_exec"), Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
            Out.Add(MakeShared<FJsonValueObject>(PinObj));
        }
        return Out;
    }

    UK2Node* SpawnNodeForClass(UEdGraph* Graph, const FString& ClassToken, UBlueprint* Blueprint,
                               const TSharedPtr<FJsonObject>& Spec, FString& OutError)
    {
        const FString Token = ClassToken.ToLower();

        if (Token == TEXT("variable_get") || Token == TEXT("k2node_variableget"))
        {
            FString VarName;
            if (!Spec->TryGetStringField(TEXT("variable_name"), VarName)
                && !Spec->TryGetStringField(TEXT("variable"), VarName))
            {
                OutError = TEXT("variable_get requires 'variable_name'");
                return nullptr;
            }
            UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Graph);
            Node->VariableReference.SetSelfMember(FName(*VarName));
            return Node;
        }
        if (Token == TEXT("variable_set") || Token == TEXT("k2node_variableset"))
        {
            FString VarName;
            if (!Spec->TryGetStringField(TEXT("variable_name"), VarName)
                && !Spec->TryGetStringField(TEXT("variable"), VarName))
            {
                OutError = TEXT("variable_set requires 'variable_name'");
                return nullptr;
            }
            UK2Node_VariableSet* Node = NewObject<UK2Node_VariableSet>(Graph);
            Node->VariableReference.SetSelfMember(FName(*VarName));
            return Node;
        }
        if (Token == TEXT("call_function") || Token == TEXT("function") || Token == TEXT("k2node_callfunction"))
        {
            FString FuncRef;
            if (!Spec->TryGetStringField(TEXT("function"), FuncRef)
                && !Spec->TryGetStringField(TEXT("function_path"), FuncRef)
                && !Spec->TryGetStringField(TEXT("function_name"), FuncRef))
            {
                OutError = TEXT("call_function requires 'function' (e.g. 'KismetSystemLibrary:PrintString' or '/Script/Engine.KismetSystemLibrary:PrintString' or a bare name on the same Blueprint)");
                return nullptr;
            }
            UFunction* Func = ResolveFunctionRef(Blueprint, FuncRef);
            if (!Func)
            {
                OutError = FString::Printf(TEXT("Could not resolve function '%s'"), *FuncRef);
                return nullptr;
            }
            UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
            Node->SetFromFunction(Func);
            return Node;
        }
        if (Token == TEXT("branch") || Token == TEXT("if_then_else") || Token == TEXT("k2node_ifthenelse"))
        {
            return NewObject<UK2Node_IfThenElse>(Graph);
        }
        if (Token == TEXT("dynamic_cast") || Token == TEXT("cast") || Token == TEXT("k2node_dynamiccast"))
        {
            FString TargetText;
            if (!Spec->TryGetStringField(TEXT("target_class"), TargetText)
                && !Spec->TryGetStringField(TEXT("target"), TargetText))
            {
                OutError = TEXT("dynamic_cast requires 'target_class'");
                return nullptr;
            }
            UClass* Target = ResolveTargetClass(TargetText);
            if (!Target)
            {
                OutError = FString::Printf(TEXT("Could not resolve target_class '%s'"), *TargetText);
                return nullptr;
            }
            UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(Graph);
            Node->TargetType = Target;
            return Node;
        }
        if (Token == TEXT("self") || Token == TEXT("k2node_self"))
        {
            return NewObject<UK2Node_Self>(Graph);
        }
        if (Token == TEXT("format_text") || Token == TEXT("k2node_formattext"))
        {
            UK2Node_FormatText* Node = NewObject<UK2Node_FormatText>(Graph);
            return Node;
        }
        if (Token == TEXT("execution_sequence") || Token == TEXT("sequence") || Token == TEXT("k2node_executionsequence"))
        {
            return NewObject<UK2Node_ExecutionSequence>(Graph);
        }
        if (Token == TEXT("knot") || Token == TEXT("reroute") || Token == TEXT("k2node_knot"))
        {
            return NewObject<UK2Node_Knot>(Graph);
        }
        if (Token == TEXT("make_array") || Token == TEXT("k2node_makearray"))
        {
            return NewObject<UK2Node_MakeArray>(Graph);
        }
        if (Token == TEXT("custom_event") || Token == TEXT("k2node_customevent"))
        {
            FString EventName;
            Spec->TryGetStringField(TEXT("event_name"), EventName);
            if (EventName.IsEmpty())
            {
                EventName = TEXT("CustomEvent");
            }
            UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Graph);
            Node->CustomFunctionName = FName(*EventName);
            Node->bIsEditable = true;
            return Node;
        }
        if (Token == TEXT("event") || Token == TEXT("k2node_event"))
        {
            FString EventName;
            if (!Spec->TryGetStringField(TEXT("event_name"), EventName))
            {
                OutError = TEXT("event requires 'event_name' (e.g. ReceiveBeginPlay, ReceiveTick)");
                return nullptr;
            }
            // Default the owning class to the Blueprint's parent so events
            // resolve correctly for non-Actor parents (UserWidget etc.). The
            // caller can override with `event_class` for cross-class overrides.
            UClass* OwnerClass = Blueprint->ParentClass ? Blueprint->ParentClass.Get() : AActor::StaticClass();
            FString EventClassText;
            if (Spec->TryGetStringField(TEXT("event_class"), EventClassText))
            {
                if (UClass* Resolved = ResolveTargetClass(EventClassText))
                {
                    OwnerClass = Resolved;
                }
            }
            UK2Node_Event* Node = NewObject<UK2Node_Event>(Graph);
            Node->EventReference.SetExternalMember(FName(*EventName), OwnerClass);
            Node->bOverrideFunction = true;
            return Node;
        }

        OutError = FString::Printf(TEXT("Unsupported node class token '%s'. Supported: variable_get, variable_set, call_function, branch / if_then_else, dynamic_cast, self, format_text, execution_sequence, knot, make_array, custom_event, event"), *ClassToken);
        return nullptr;
    }
}

FSproftBpNodesCommands::FSproftBpNodesCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpNodesCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_nodes"))
    {
        return HandleBpNodes(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_nodes command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpNodesCommands::HandleBpNodes(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Operation = TEXT("add");
    Params->TryGetStringField(TEXT("op"), Operation);
    Params->TryGetStringField(TEXT("operation"), Operation);
    Operation = Operation.ToLower();

    UBlueprint* Blueprint = ResolveBlueprintParam(Params);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Blueprint not found or 'blueprint' parameter missing"));
    }

    if (Operation == TEXT("add") || Operation == TEXT("add_nodes") || Operation == TEXT("create"))
    {
        return AddNodes(Blueprint, Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported bp_nodes op '%s'. Supported: add"), *Operation));
}

TSharedPtr<FJsonObject> FSproftBpNodesCommands::AddNodes(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    FString GraphName;
    Params->TryGetStringField(TEXT("graph"), GraphName);
    Params->TryGetStringField(TEXT("graph_name"), GraphName);
    UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
    if (!Graph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Graph '%s' not found on Blueprint '%s'"), *GraphName, *Blueprint->GetName()));
    }

    const TArray<TSharedPtr<FJsonValue>>* NodeSpecs = nullptr;
    // Support either a single `node` object, a `nodes` array, or an inline
    // `class` (so callers can spawn a single node without wrapping in an array).
    TArray<TSharedPtr<FJsonValue>> Specs;
    if (Params->TryGetArrayField(TEXT("nodes"), NodeSpecs) && NodeSpecs)
    {
        Specs = *NodeSpecs;
    }
    else
    {
        const TSharedPtr<FJsonObject>* SingleSpec = nullptr;
        if (Params->TryGetObjectField(TEXT("node"), SingleSpec) && SingleSpec && SingleSpec->IsValid())
        {
            Specs.Add(MakeShared<FJsonValueObject>(*SingleSpec));
        }
        else if (Params->HasField(TEXT("class")))
        {
            // Treat the params object itself as a single node spec.
            Specs.Add(MakeShared<FJsonValueObject>(Params));
        }
    }

    if (Specs.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Pass either 'nodes' (array of {class, ...}) or 'node' (single object) or an inline 'class'"));
    }

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = false;
    Params->TryGetBoolField(TEXT("save"), bSave);

    TArray<TSharedPtr<FJsonValue>> Created;
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

        FString ClassToken;
        if (!Spec->TryGetStringField(TEXT("class"), ClassToken)
            && !Spec->TryGetStringField(TEXT("type"), ClassToken)
            && !Spec->TryGetStringField(TEXT("node_class"), ClassToken))
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("error"), TEXT("entry missing 'class' field"));
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }

        FString Err;
        UK2Node* Node = SpawnNodeForClass(Graph, ClassToken, Blueprint, Spec, Err);
        if (!Node)
        {
            TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
            Fail->SetNumberField(TEXT("index"), Index);
            Fail->SetStringField(TEXT("class"), ClassToken);
            Fail->SetStringField(TEXT("error"), Err.IsEmpty() ? TEXT("Failed to spawn node") : Err);
            Failures.Add(MakeShared<FJsonValueObject>(Fail));
            continue;
        }

        ApplyPosition(Node, Spec);
        Graph->AddNode(Node, /*bUserAction*/ true, /*bSelectNewNode*/ false);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Node->ReconstructNode();
        ApplyDesiredName(Node, Spec);
        ApplyPinDefaults(Node, Spec);

        TSharedPtr<FJsonObject> NodeOut = MakeShared<FJsonObject>();
        NodeOut->SetNumberField(TEXT("index"), Index);
        NodeOut->SetStringField(TEXT("class"), ClassToken);
        NodeOut->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
        NodeOut->SetStringField(TEXT("node_name"), Node->GetName());
        NodeOut->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
        NodeOut->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
        NodeOut->SetNumberField(TEXT("position_x"), Node->NodePosX);
        NodeOut->SetNumberField(TEXT("position_y"), Node->NodePosY);
        NodeOut->SetArrayField(TEXT("pins"), SnapshotPins(Node));
        Created.Add(MakeShared<FJsonValueObject>(NodeOut));
    }

    Graph->NotifyGraphChanged();
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
    Result->SetStringField(TEXT("operation"), TEXT("add_nodes"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("graph"), Graph->GetName());
    Result->SetNumberField(TEXT("requested"), Specs.Num());
    Result->SetNumberField(TEXT("created"), Created.Num());
    Result->SetNumberField(TEXT("failed"), Failures.Num());
    Result->SetArrayField(TEXT("nodes"), Created);
    Result->SetArrayField(TEXT("failures"), Failures);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}
