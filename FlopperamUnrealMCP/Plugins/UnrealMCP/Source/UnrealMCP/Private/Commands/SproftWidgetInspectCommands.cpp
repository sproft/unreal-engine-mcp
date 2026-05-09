#include "Commands/SproftWidgetInspectCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/NamedSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "EditorAssetLibrary.h"
#include "WidgetBlueprint.h"

namespace
{
    /** Build a nested JSON description of a widget and its children. */
    TSharedPtr<FJsonObject> BuildWidgetTreeNode(UWidget* Widget)
    {
        TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
        if (!Widget)
        {
            NodeObj->SetStringField(TEXT("name"), TEXT("(null)"));
            NodeObj->SetStringField(TEXT("class"), TEXT("(null)"));
            return NodeObj;
        }

        NodeObj->SetStringField(TEXT("name"), Widget->GetName());
        NodeObj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
        NodeObj->SetStringField(TEXT("class_path"), Widget->GetClass()->GetPathName());
        NodeObj->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);

        TArray<TSharedPtr<FJsonValue>> ChildrenArray;
        if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
        {
            const int32 ChildCount = Panel->GetChildrenCount();
            for (int32 Idx = 0; Idx < ChildCount; ++Idx)
            {
                UWidget* Child = Panel->GetChildAt(Idx);
                ChildrenArray.Add(MakeShared<FJsonValueObject>(BuildWidgetTreeNode(Child)));
            }
        }
        NodeObj->SetArrayField(TEXT("children"), ChildrenArray);
        return NodeObj;
    }
}

FSproftWidgetInspectCommands::FSproftWidgetInspectCommands()
{
}

TSharedPtr<FJsonObject> FSproftWidgetInspectCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("widget_inspect"))
    {
        return HandleWidgetInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown widget_inspect command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftWidgetInspectCommands::HandleWidgetInspect(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Operation = TEXT("inspect");
    Params->TryGetStringField(TEXT("operation"), Operation);
    Operation = Operation.ToLower();
    if (Operation != TEXT("inspect") && Operation != TEXT("tree"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported widget_inspect operation '%s'. Supported: inspect"), *Operation));
    }

    FString WidgetBlueprintPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }

    bool bIncludeVariables = true;
    bool bIncludeFunctions = true;
    bool bIncludeFlatList = true;
    bool bIncludeNamedSlots = true;
    Params->TryGetBoolField(TEXT("include_variables"), bIncludeVariables);
    Params->TryGetBoolField(TEXT("include_functions"), bIncludeFunctions);
    Params->TryGetBoolField(TEXT("include_flat"), bIncludeFlatList);
    Params->TryGetBoolField(TEXT("include_named_slots"), bIncludeNamedSlots);

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(WidgetBlueprintPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WidgetBlueprintPath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("inspect"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath);
    ResultObj->SetStringField(TEXT("name"), WBP->GetName());
    ResultObj->SetStringField(TEXT("parent_class"),
        WBP->ParentClass ? WBP->ParentClass->GetPathName() : TEXT("None"));

    UWidgetTree* Tree = WBP->WidgetTree;
    UWidget* Root = Tree ? Tree->RootWidget : nullptr;

    if (Root)
    {
        ResultObj->SetObjectField(TEXT("tree"), BuildWidgetTreeNode(Root));
    }
    else
    {
        ResultObj->SetStringField(TEXT("tree"), TEXT("(empty)"));
    }

    // Flat list of every widget in the tree.
    TSet<FName> WidgetNamesInTree;
    if (bIncludeFlatList && Tree)
    {
        TArray<UWidget*> AllWidgets;
        Tree->GetAllWidgets(AllWidgets);

        TArray<TSharedPtr<FJsonValue>> FlatArray;
        for (UWidget* Widget : AllWidgets)
        {
            if (!Widget)
            {
                continue;
            }
            WidgetNamesInTree.Add(Widget->GetFName());

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Widget->GetName());
            Entry->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
            Entry->SetStringField(TEXT("class_path"), Widget->GetClass()->GetPathName());
            Entry->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
            Entry->SetBoolField(TEXT("is_panel"), Widget->IsA<UPanelWidget>());
            FlatArray.Add(MakeShared<FJsonValueObject>(Entry));
        }
        ResultObj->SetArrayField(TEXT("widgets"), FlatArray);
        ResultObj->SetNumberField(TEXT("widget_count"), AllWidgets.Num());
    }

    // Named slots exposed by the asset for content injection.
    if (bIncludeNamedSlots && Tree)
    {
        TArray<TSharedPtr<FJsonValue>> NamedSlotArray;
        Tree->ForEachWidget([&NamedSlotArray](UWidget* Widget)
        {
            if (UNamedSlot* AsSlot = Cast<UNamedSlot>(Widget))
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), AsSlot->GetName());
                Entry->SetBoolField(TEXT("has_content"), AsSlot->GetChildrenCount() > 0);
                NamedSlotArray.Add(MakeShared<FJsonValueObject>(Entry));
            }
        });
        ResultObj->SetArrayField(TEXT("named_slots"), NamedSlotArray);
    }

    // Blueprint variables that are NOT already widget tree members. Widget
    // tree widgets exposed as variables show up in NewVariables too, so we
    // exclude those to keep the list focused on user-declared state.
    if (bIncludeVariables)
    {
        TArray<TSharedPtr<FJsonValue>> VariableArray;
        for (const FBPVariableDescription& Variable : WBP->NewVariables)
        {
            if (WidgetNamesInTree.Contains(Variable.VarName))
            {
                continue;
            }
            TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
            VarObj->SetStringField(TEXT("name"), Variable.VarName.ToString());
            VarObj->SetStringField(TEXT("type"), Variable.VarType.PinCategory.ToString());
            if (Variable.VarType.PinSubCategoryObject.IsValid())
            {
                VarObj->SetStringField(TEXT("subtype"), Variable.VarType.PinSubCategoryObject->GetName());
            }
            VarObj->SetStringField(TEXT("default_value"), Variable.DefaultValue);
            VarObj->SetBoolField(TEXT("is_editable"), (Variable.PropertyFlags & CPF_Edit) != 0);
            VariableArray.Add(MakeShared<FJsonValueObject>(VarObj));
        }
        ResultObj->SetArrayField(TEXT("variables"), VariableArray);
    }

    if (bIncludeFunctions)
    {
        TArray<TSharedPtr<FJsonValue>> FunctionArray;
        for (UEdGraph* Graph : WBP->FunctionGraphs)
        {
            if (!Graph)
            {
                continue;
            }
            TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
            FuncObj->SetStringField(TEXT("name"), Graph->GetName());
            FuncObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
            FunctionArray.Add(MakeShared<FJsonValueObject>(FuncObj));
        }
        ResultObj->SetArrayField(TEXT("functions"), FunctionArray);
    }

    return ResultObj;
}
