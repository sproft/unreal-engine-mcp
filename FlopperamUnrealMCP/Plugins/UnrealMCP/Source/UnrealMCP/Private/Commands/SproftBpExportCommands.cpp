#include "Commands/SproftBpExportCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Components/SceneComponent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
    UBlueprint* BpExport_ResolveBlueprintParam(const TSharedPtr<FJsonObject>& Params)
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

    /** Compact pin-type description matching the bp_graph helper so a
     *  caller can diff the payloads from the two tools. */
    FString BpExport_DescribePinType(const FEdGraphPinType& PinType)
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

    TSharedPtr<FJsonObject> SerialiseGraph(UEdGraph* Graph, const TCHAR* Kind, int32 MaxPinsPerNode)
    {
        TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
        GraphObj->SetStringField(TEXT("name"), Graph->GetName());
        GraphObj->SetStringField(TEXT("kind"), Kind);
        GraphObj->SetStringField(TEXT("graph_class"), Graph->GetClass()->GetName());
        GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

        // Per-node serialisation: name, class, title, position, optional
        // FName / signature for events, and a pin list capped at
        // MaxPinsPerNode so a 100-pin function does not blow the payload.
        TArray<TSharedPtr<FJsonValue>> NodeArr;
        TArray<TSharedPtr<FJsonValue>> EdgeArr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
            NodeObj->SetStringField(TEXT("node_name"), Node->GetName());
            NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
            NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
            NodeObj->SetNumberField(TEXT("position_x"), Node->NodePosX);
            NodeObj->SetNumberField(TEXT("position_y"), Node->NodePosY);
            NodeObj->SetStringField(TEXT("guid"), Node->NodeGuid.ToString());

            if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                NodeObj->SetStringField(TEXT("event_signature"), EventNode->EventReference.GetMemberName().ToString());
            }
            if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
            {
                NodeObj->SetStringField(TEXT("custom_event_name"), CustomEvent->CustomFunctionName.ToString());
            }

            TArray<TSharedPtr<FJsonValue>> PinArr;
            int32 PinIndex = 0;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin)
                {
                    continue;
                }
                if (PinIndex >= MaxPinsPerNode)
                {
                    NodeObj->SetBoolField(TEXT("pins_truncated"), true);
                    break;
                }
                ++PinIndex;
                TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
                PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
                PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
                PinObj->SetStringField(TEXT("type"), BpExport_DescribePinType(Pin->PinType));
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
                PinObj->SetNumberField(TEXT("link_count"), Pin->LinkedTo.Num());
                PinArr.Add(MakeShared<FJsonValueObject>(PinObj));

                // Edge collection: emit one row per output->input link so
                // the graph payload contains a flat edge list as well as
                // the per-node pin list.
                if (Pin->Direction == EGPD_Output)
                {
                    for (UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        if (!Linked || !Linked->GetOwningNode())
                        {
                            continue;
                        }
                        TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
                        Edge->SetStringField(TEXT("source_node"), Node->GetName());
                        Edge->SetStringField(TEXT("source_pin"), Pin->PinName.ToString());
                        Edge->SetStringField(TEXT("target_node"), Linked->GetOwningNode()->GetName());
                        Edge->SetStringField(TEXT("target_pin"), Linked->PinName.ToString());
                        Edge->SetBoolField(TEXT("is_exec"), Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
                        EdgeArr.Add(MakeShared<FJsonValueObject>(Edge));
                    }
                }
            }
            NodeObj->SetArrayField(TEXT("pins"), PinArr);
            NodeArr.Add(MakeShared<FJsonValueObject>(NodeObj));
        }

        GraphObj->SetArrayField(TEXT("nodes"), NodeArr);
        GraphObj->SetArrayField(TEXT("edges"), EdgeArr);
        GraphObj->SetNumberField(TEXT("edge_count"), EdgeArr.Num());
        return GraphObj;
    }

    /** Walk the SimpleConstructionScript and emit a tree-aware list. */
    TArray<TSharedPtr<FJsonValue>> SerialiseComponents(UBlueprint* Blueprint, bool bIncludeDefaults)
    {
        TArray<TSharedPtr<FJsonValue>> Components;
        USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
        if (!SCS)
        {
            return Components;
        }
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

            if (USceneComponent* SceneTemplate = Cast<USceneComponent>(SCSNode->ComponentTemplate))
            {
                TSharedPtr<FJsonObject> RelTransform = MakeShared<FJsonObject>();
                const FVector Loc = SceneTemplate->GetRelativeLocation();
                const FRotator Rot = SceneTemplate->GetRelativeRotation();
                const FVector Scale = SceneTemplate->GetRelativeScale3D();
                RelTransform->SetNumberField(TEXT("loc_x"), Loc.X);
                RelTransform->SetNumberField(TEXT("loc_y"), Loc.Y);
                RelTransform->SetNumberField(TEXT("loc_z"), Loc.Z);
                RelTransform->SetNumberField(TEXT("rot_pitch"), Rot.Pitch);
                RelTransform->SetNumberField(TEXT("rot_yaw"), Rot.Yaw);
                RelTransform->SetNumberField(TEXT("rot_roll"), Rot.Roll);
                RelTransform->SetNumberField(TEXT("scale_x"), Scale.X);
                RelTransform->SetNumberField(TEXT("scale_y"), Scale.Y);
                RelTransform->SetNumberField(TEXT("scale_z"), Scale.Z);
                Entry->SetObjectField(TEXT("relative_transform"), RelTransform);
            }

            // Resolve attach parent through the SCS tree.
            FString AttachParent;
            for (USCS_Node* Candidate : SCS->GetAllNodes())
            {
                if (!Candidate || Candidate == SCSNode)
                {
                    continue;
                }
                if (Candidate->GetChildNodes().Contains(SCSNode))
                {
                    AttachParent = Candidate->GetVariableName().ToString();
                    break;
                }
            }
            if (!AttachParent.IsEmpty())
            {
                Entry->SetStringField(TEXT("attach_parent"), AttachParent);
            }
            if (!SCSNode->AttachToName.IsNone())
            {
                Entry->SetStringField(TEXT("attach_socket"), SCSNode->AttachToName.ToString());
            }
            Entry->SetNumberField(TEXT("child_count"), SCSNode->GetChildNodes().Num());

            // Optional flat property dump. Skipping the default-equal pass
            // here keeps the snapshot diff-able: callers that just want
            // overrides can compare against the parent CDO themselves.
            if (bIncludeDefaults)
            {
                TSharedPtr<FJsonObject> DefaultsObj = MakeShared<FJsonObject>();
                UClass* Owner = SCSNode->ComponentTemplate->GetClass();
                for (TFieldIterator<FProperty> PropIt(Owner); PropIt; ++PropIt)
                {
                    FProperty* Prop = *PropIt;
                    if (!Prop || (Prop->PropertyFlags & CPF_Transient))
                    {
                        continue;
                    }
                    // ExportText against the template; consumers that need
                    // overrides-only can compare against the parent's CDO.
                    FString Exported;
                    Prop->ExportText_InContainer(0, Exported, SCSNode->ComponentTemplate, SCSNode->ComponentTemplate, nullptr, PPF_None);
                    if (!Exported.IsEmpty())
                    {
                        DefaultsObj->SetStringField(Prop->GetName(), Exported);
                    }
                }
                Entry->SetObjectField(TEXT("defaults"), DefaultsObj);
            }

            Components.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Components;
    }
}

FSproftBpExportCommands::FSproftBpExportCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpExportCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_export"))
    {
        return HandleBpExport(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_export command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpExportCommands::HandleBpExport(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    UBlueprint* Blueprint = BpExport_ResolveBlueprintParam(Params);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Blueprint not found or 'blueprint' parameter missing"));
    }

    bool bIncludeEvents = true;
    bool bIncludeFunctions = true;
    bool bIncludeMacros = true;
    bool bIncludeComponents = true;
    bool bIncludeVariables = true;
    bool bIncludeInterfaces = true;
    bool bIncludeDefaults = true;
    Params->TryGetBoolField(TEXT("include_events"), bIncludeEvents);
    Params->TryGetBoolField(TEXT("include_functions"), bIncludeFunctions);
    Params->TryGetBoolField(TEXT("include_macros"), bIncludeMacros);
    Params->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
    Params->TryGetBoolField(TEXT("include_variables"), bIncludeVariables);
    Params->TryGetBoolField(TEXT("include_interfaces"), bIncludeInterfaces);
    Params->TryGetBoolField(TEXT("include_defaults"), bIncludeDefaults);

    int32 MaxPinsPerNode = 64;
    int32 ParsedMax = 0;
    if (Params->TryGetNumberField(TEXT("max_pins_per_node"), ParsedMax) && ParsedMax > 0)
    {
        MaxPinsPerNode = ParsedMax;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), Blueprint->GetName());
    Result->SetStringField(TEXT("path"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("blueprint_class"), Blueprint->GetClass()->GetName());
    if (Blueprint->ParentClass)
    {
        Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetName());
        Result->SetStringField(TEXT("parent_class_path"), Blueprint->ParentClass->GetPathName());
    }
    Result->SetStringField(TEXT("blueprint_type"), [&]()
    {
        switch (Blueprint->BlueprintType)
        {
        case BPTYPE_Normal: return TEXT("normal");
        case BPTYPE_Const: return TEXT("const");
        case BPTYPE_MacroLibrary: return TEXT("macro_library");
        case BPTYPE_Interface: return TEXT("interface");
        case BPTYPE_LevelScript: return TEXT("level_script");
        case BPTYPE_FunctionLibrary: return TEXT("function_library");
        default: return TEXT("unknown");
        }
    }());

    // Variables
    if (bIncludeVariables)
    {
        TArray<TSharedPtr<FJsonValue>> VarArr;
        for (const FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
            VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
            VarObj->SetStringField(TEXT("type"), BpExport_DescribePinType(Var.VarType));
            VarObj->SetStringField(TEXT("category"), Var.Category.ToString());
            VarObj->SetStringField(TEXT("friendly_name"), Var.FriendlyName);
            VarObj->SetStringField(TEXT("default_value"), Var.DefaultValue);
            VarObj->SetBoolField(TEXT("editable"), (Var.PropertyFlags & CPF_Edit) != 0);
            VarObj->SetBoolField(TEXT("blueprint_read_only"), (Var.PropertyFlags & CPF_BlueprintReadOnly) != 0);
            VarObj->SetBoolField(TEXT("instance_editable"), (Var.PropertyFlags & CPF_DisableEditOnInstance) == 0);
            VarObj->SetBoolField(TEXT("expose_on_spawn"), (Var.PropertyFlags & CPF_ExposeOnSpawn) != 0);
            VarObj->SetBoolField(TEXT("replicated"), (Var.PropertyFlags & CPF_Net) != 0);
            VarArr.Add(MakeShared<FJsonValueObject>(VarObj));
        }
        Result->SetArrayField(TEXT("variables"), VarArr);
        Result->SetNumberField(TEXT("variable_count"), VarArr.Num());
    }

    // Components
    if (bIncludeComponents)
    {
        TArray<TSharedPtr<FJsonValue>> Components = SerialiseComponents(Blueprint, bIncludeDefaults);
        Result->SetArrayField(TEXT("components"), Components);
        Result->SetNumberField(TEXT("component_count"), Components.Num());
    }

    // Interfaces
    if (bIncludeInterfaces)
    {
        TArray<TSharedPtr<FJsonValue>> InterfaceArr;
        for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            if (Iface.Interface)
            {
                Entry->SetStringField(TEXT("name"), Iface.Interface->GetName());
                Entry->SetStringField(TEXT("path"), Iface.Interface->GetPathName());
            }
            else
            {
                Entry->SetStringField(TEXT("name"), TEXT("Unknown"));
            }
            Entry->SetNumberField(TEXT("graph_count"), Iface.Graphs.Num());
            InterfaceArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Result->SetArrayField(TEXT("interfaces"), InterfaceArr);
        Result->SetNumberField(TEXT("interface_count"), InterfaceArr.Num());
    }

    // Graphs: events / functions / macros / interface override graphs.
    TArray<TSharedPtr<FJsonValue>> GraphArr;
    if (bIncludeEvents)
    {
        for (UEdGraph* G : Blueprint->UbergraphPages)
        {
            if (G)
            {
                GraphArr.Add(MakeShared<FJsonValueObject>(SerialiseGraph(G, TEXT("ubergraph"), MaxPinsPerNode)));
            }
        }
    }
    if (bIncludeFunctions)
    {
        for (UEdGraph* G : Blueprint->FunctionGraphs)
        {
            if (G)
            {
                GraphArr.Add(MakeShared<FJsonValueObject>(SerialiseGraph(G, TEXT("function"), MaxPinsPerNode)));
            }
        }
    }
    if (bIncludeMacros)
    {
        for (UEdGraph* G : Blueprint->MacroGraphs)
        {
            if (G)
            {
                GraphArr.Add(MakeShared<FJsonValueObject>(SerialiseGraph(G, TEXT("macro"), MaxPinsPerNode)));
            }
        }
    }
    if (bIncludeInterfaces)
    {
        for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
        {
            for (UEdGraph* G : Iface.Graphs)
            {
                if (G)
                {
                    GraphArr.Add(MakeShared<FJsonValueObject>(SerialiseGraph(G, TEXT("interface"), MaxPinsPerNode)));
                }
            }
        }
    }
    Result->SetArrayField(TEXT("graphs"), GraphArr);
    Result->SetNumberField(TEXT("graph_count"), GraphArr.Num());

    Result->SetNumberField(TEXT("max_pins_per_node"), MaxPinsPerNode);
    return Result;
}
