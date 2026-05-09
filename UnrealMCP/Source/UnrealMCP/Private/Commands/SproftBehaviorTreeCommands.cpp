#include "Commands/SproftBehaviorTreeCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "EditorAssetLibrary.h"
#include "UObject/Class.h"

namespace
{
    /** Pretty-printed BT key-type token. Strips the BlackboardKeyType_
     *  prefix on the underlying class name so "BlackboardKeyType_Object"
     *  becomes "Object". */
    FString DescribeKeyType(UBlackboardKeyType* KeyType)
    {
        if (!KeyType)
        {
            return TEXT("none");
        }
        FString ClassName = KeyType->GetClass()->GetName();
        const TCHAR* Prefix = TEXT("BlackboardKeyType_");
        if (ClassName.StartsWith(Prefix))
        {
            ClassName = ClassName.RightChop(FCString::Strlen(Prefix));
        }
        return ClassName;
    }

    /** Resolve the inner class / enum / struct on typed Blackboard keys
     *  so a downstream consumer does not need to peek at the runtime
     *  type. We surface the BaseClass for Object / Class keys, the enum
     *  asset for Enum keys, and the struct asset for Struct keys. */
    void AppendKeyTypeExtras(UBlackboardKeyType* KeyType, TSharedPtr<FJsonObject>& Out)
    {
        if (UBlackboardKeyType_Object* AsObject = Cast<UBlackboardKeyType_Object>(KeyType))
        {
            if (AsObject->BaseClass)
            {
                Out->SetStringField(TEXT("base_class"), AsObject->BaseClass->GetPathName());
            }
        }
        else if (UBlackboardKeyType_Class* AsClass = Cast<UBlackboardKeyType_Class>(KeyType))
        {
            if (AsClass->BaseClass)
            {
                Out->SetStringField(TEXT("base_class"), AsClass->BaseClass->GetPathName());
            }
        }
        else if (UBlackboardKeyType_Enum* AsEnum = Cast<UBlackboardKeyType_Enum>(KeyType))
        {
            if (AsEnum->EnumType)
            {
                Out->SetStringField(TEXT("enum_path"), AsEnum->EnumType->GetPathName());
            }
        }
        else if (UBlackboardKeyType_Struct* AsStruct = Cast<UBlackboardKeyType_Struct>(KeyType))
        {
            if (AsStruct->DefaultValue.GetScriptStruct())
            {
                Out->SetStringField(TEXT("struct_path"), AsStruct->DefaultValue.GetScriptStruct()->GetPathName());
            }
        }
    }

    void SerialiseDecorator(UBTDecorator* Decorator, TArray<TSharedPtr<FJsonValue>>& OutArr)
    {
        if (!Decorator)
        {
            return;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Decorator->GetNodeName());
        Row->SetStringField(TEXT("class"), Decorator->GetClass()->GetName());
        Row->SetStringField(TEXT("class_path"), Decorator->GetClass()->GetPathName());
        Row->SetStringField(TEXT("kind"), TEXT("decorator"));
        OutArr.Add(MakeShared<FJsonValueObject>(Row));
    }

    void SerialiseService(UBTService* Service, TArray<TSharedPtr<FJsonValue>>& OutArr)
    {
        if (!Service)
        {
            return;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Service->GetNodeName());
        Row->SetStringField(TEXT("class"), Service->GetClass()->GetName());
        Row->SetStringField(TEXT("class_path"), Service->GetClass()->GetPathName());
        Row->SetStringField(TEXT("kind"), TEXT("service"));
        OutArr.Add(MakeShared<FJsonValueObject>(Row));
    }

    /** Forward declare so the composite case can recurse. */
    TSharedPtr<FJsonObject> SerialiseNode(UBTNode* Node, int32 Depth, int32 MaxDepth, bool bIncludeDecorators, bool bIncludeServices);

    void AppendCompositeExtras(UBTCompositeNode* Composite, TSharedPtr<FJsonObject>& Out)
    {
        if (!Composite)
        {
            return;
        }
        if (UBTComposite_SimpleParallel* AsParallel = Cast<UBTComposite_SimpleParallel>(Composite))
        {
            Out->SetStringField(TEXT("finish_mode"), AsParallel->FinishMode == EBTParallelMode::AbortBackground ? TEXT("immediate") : TEXT("delayed"));
        }
    }

    TSharedPtr<FJsonObject> SerialiseComposite(UBTCompositeNode* Composite, int32 Depth, int32 MaxDepth, bool bIncludeDecorators, bool bIncludeServices)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Composite->GetNodeName());
        Row->SetStringField(TEXT("class"), Composite->GetClass()->GetName());
        Row->SetStringField(TEXT("class_path"), Composite->GetClass()->GetPathName());
        Row->SetStringField(TEXT("kind"), TEXT("composite"));
        Row->SetNumberField(TEXT("depth"), Depth);

        AppendCompositeExtras(Composite, Row);

        if (bIncludeServices)
        {
            TArray<TSharedPtr<FJsonValue>> ServiceArr;
            for (UBTService* Service : Composite->Services)
            {
                SerialiseService(Service, ServiceArr);
            }
            Row->SetArrayField(TEXT("services"), ServiceArr);
        }

        TArray<TSharedPtr<FJsonValue>> ChildArr;
        if (Depth >= MaxDepth)
        {
            Row->SetBoolField(TEXT("children_truncated"), true);
            Row->SetArrayField(TEXT("children"), ChildArr);
            return Row;
        }
        for (const FBTCompositeChild& Child : Composite->Children)
        {
            TSharedPtr<FJsonObject> ChildRow = MakeShared<FJsonObject>();

            if (bIncludeDecorators)
            {
                TArray<TSharedPtr<FJsonValue>> DecoratorArr;
                for (UBTDecorator* Decorator : Child.Decorators)
                {
                    SerialiseDecorator(Decorator, DecoratorArr);
                }
                ChildRow->SetArrayField(TEXT("decorators"), DecoratorArr);
            }

            if (Child.ChildComposite)
            {
                ChildRow->SetObjectField(TEXT("node"), SerialiseComposite(Child.ChildComposite, Depth + 1, MaxDepth, bIncludeDecorators, bIncludeServices));
            }
            else if (Child.ChildTask)
            {
                ChildRow->SetObjectField(TEXT("node"), SerialiseNode(Child.ChildTask, Depth + 1, MaxDepth, bIncludeDecorators, bIncludeServices));
            }
            else
            {
                TSharedPtr<FJsonObject> Stub = MakeShared<FJsonObject>();
                Stub->SetStringField(TEXT("kind"), TEXT("empty"));
                ChildRow->SetObjectField(TEXT("node"), Stub);
            }
            ChildArr.Add(MakeShared<FJsonValueObject>(ChildRow));
        }
        Row->SetArrayField(TEXT("children"), ChildArr);
        Row->SetNumberField(TEXT("child_count"), Composite->Children.Num());
        return Row;
    }

    TSharedPtr<FJsonObject> SerialiseNode(UBTNode* Node, int32 Depth, int32 MaxDepth, bool bIncludeDecorators, bool bIncludeServices)
    {
        if (!Node)
        {
            TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
            Empty->SetStringField(TEXT("kind"), TEXT("empty"));
            return Empty;
        }
        if (UBTCompositeNode* AsComposite = Cast<UBTCompositeNode>(Node))
        {
            return SerialiseComposite(AsComposite, Depth, MaxDepth, bIncludeDecorators, bIncludeServices);
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Node->GetNodeName());
        Row->SetStringField(TEXT("class"), Node->GetClass()->GetName());
        Row->SetStringField(TEXT("class_path"), Node->GetClass()->GetPathName());
        Row->SetStringField(TEXT("kind"), Cast<UBTTaskNode>(Node) ? TEXT("task") : TEXT("node"));
        Row->SetNumberField(TEXT("depth"), Depth);
        return Row;
    }
}

FSproftBehaviorTreeCommands::FSproftBehaviorTreeCommands()
{
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("behavior_tree"))
    {
        return HandleBehaviorTree(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown behavior_tree command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString TreePath;
    if (!Params->TryGetStringField(TEXT("tree"), TreePath)
        && !Params->TryGetStringField(TEXT("tree_path"), TreePath)
        && !Params->TryGetStringField(TEXT("path"), TreePath)
        && !Params->TryGetStringField(TEXT("asset"), TreePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'tree' parameter"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(TreePath);
    UBehaviorTree* Tree = Cast<UBehaviorTree>(Asset);
    if (!Tree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UBehaviorTree"), *TreePath));
    }

    bool bIncludeBlackboard = true;
    bool bIncludeDecorators = true;
    bool bIncludeServices = true;
    Params->TryGetBoolField(TEXT("include_blackboard"), bIncludeBlackboard);
    Params->TryGetBoolField(TEXT("include_decorators"), bIncludeDecorators);
    Params->TryGetBoolField(TEXT("include_services"), bIncludeServices);

    int32 MaxDepth = 32;
    int32 ParsedMax = 0;
    if (Params->TryGetNumberField(TEXT("max_depth"), ParsedMax) && ParsedMax > 0)
    {
        MaxDepth = ParsedMax;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), Tree->GetName());
    Result->SetStringField(TEXT("path"), Tree->GetPathName());
    Result->SetStringField(TEXT("class"), Tree->GetClass()->GetName());

    // Tree-level (root) decorators are stored on the BehaviorTree asset
    // itself, not on the root composite.
    if (bIncludeDecorators)
    {
        TArray<TSharedPtr<FJsonValue>> RootDecoratorArr;
        for (UBTDecorator* Decorator : Tree->RootDecorators)
        {
            SerialiseDecorator(Decorator, RootDecoratorArr);
        }
        Result->SetArrayField(TEXT("root_decorators"), RootDecoratorArr);
    }

    if (Tree->RootNode)
    {
        Result->SetObjectField(TEXT("root_node"), SerialiseComposite(Tree->RootNode, 0, MaxDepth, bIncludeDecorators, bIncludeServices));
    }
    else
    {
        Result->SetBoolField(TEXT("has_root_node"), false);
    }

    // Blackboard pairing.
    UBlackboardData* Blackboard = Tree->GetBlackboardAsset();
    if (Blackboard)
    {
        Result->SetStringField(TEXT("blackboard_path"), Blackboard->GetPathName());
        Result->SetStringField(TEXT("blackboard_name"), Blackboard->GetName());
        if (Blackboard->Parent)
        {
            Result->SetStringField(TEXT("blackboard_parent_path"), Blackboard->Parent->GetPathName());
        }

        if (bIncludeBlackboard)
        {
            TArray<TSharedPtr<FJsonValue>> KeyArr;
            // Local entries (own + parent chain through ParentKeys when
            // available; ParentKeys lives under WITH_EDITORONLY_DATA so
            // we tag each entry with its source).
            for (const FBlackboardEntry& Entry : Blackboard->Keys)
            {
                TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
                KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
                KeyObj->SetStringField(TEXT("type"), DescribeKeyType(Entry.KeyType));
                if (Entry.KeyType)
                {
                    KeyObj->SetStringField(TEXT("type_class_path"), Entry.KeyType->GetClass()->GetPathName());
                    AppendKeyTypeExtras(Entry.KeyType, KeyObj);
                }
                KeyObj->SetBoolField(TEXT("instance_synced"), Entry.bInstanceSynced != 0);
                KeyObj->SetBoolField(TEXT("inherited"), false);
#if WITH_EDITORONLY_DATA
                if (!Entry.EntryDescription.IsEmpty())
                {
                    KeyObj->SetStringField(TEXT("description"), Entry.EntryDescription);
                }
                if (!Entry.EntryCategory.IsNone())
                {
                    KeyObj->SetStringField(TEXT("category"), Entry.EntryCategory.ToString());
                }
#endif
                KeyArr.Add(MakeShared<FJsonValueObject>(KeyObj));
            }
#if WITH_EDITORONLY_DATA
            for (const FBlackboardEntry& Entry : Blackboard->ParentKeys)
            {
                TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
                KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
                KeyObj->SetStringField(TEXT("type"), DescribeKeyType(Entry.KeyType));
                if (Entry.KeyType)
                {
                    KeyObj->SetStringField(TEXT("type_class_path"), Entry.KeyType->GetClass()->GetPathName());
                    AppendKeyTypeExtras(Entry.KeyType, KeyObj);
                }
                KeyObj->SetBoolField(TEXT("instance_synced"), Entry.bInstanceSynced != 0);
                KeyObj->SetBoolField(TEXT("inherited"), true);
                if (!Entry.EntryDescription.IsEmpty())
                {
                    KeyObj->SetStringField(TEXT("description"), Entry.EntryDescription);
                }
                if (!Entry.EntryCategory.IsNone())
                {
                    KeyObj->SetStringField(TEXT("category"), Entry.EntryCategory.ToString());
                }
                KeyArr.Add(MakeShared<FJsonValueObject>(KeyObj));
            }
#endif
            Result->SetArrayField(TEXT("blackboard_keys"), KeyArr);
            Result->SetNumberField(TEXT("blackboard_key_count"), KeyArr.Num());
            Result->SetBoolField(TEXT("blackboard_has_synced_keys"), Blackboard->HasSynchronizedKeys());
        }
    }
    else
    {
        Result->SetBoolField(TEXT("has_blackboard"), false);
    }

    return Result;
}
