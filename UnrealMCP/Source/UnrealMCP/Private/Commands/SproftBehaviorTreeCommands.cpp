#include "Commands/SproftBehaviorTreeCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardAssetProvider.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyEnums.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Decorators/BTDecorator_Cooldown.h"
#include "BehaviorTree/Decorators/BTDecorator_ForceSuccess.h"
#include "BehaviorTree/Decorators/BTDecorator_Loop.h"
#include "BehaviorTree/Decorators/BTDecorator_TimeLimit.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "BehaviorTree/Tasks/BTTask_PlayAnimation.h"
#include "BehaviorTree/Tasks/BTTask_PlaySound.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "EditorAssetLibrary.h"
#include "Misc/OutputDeviceNull.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

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

    /** Split a `/Game/Subdir/AssetName` path into directory + asset
     *  name. Mirrors the helper in `SproftAssetFactoryCommands` so the
     *  edit slice does not fight the asset-creation flow. */
    void BehaviorTree_SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
    {
        FString Trim = InPath;
        Trim.TrimEndInline();
        Trim.RemoveFromEnd(TEXT("/"));

        int32 LastSlash = INDEX_NONE;
        if (Trim.FindLastChar('/', LastSlash))
        {
            OutPackageDir = Trim.Left(LastSlash + 1);
            OutAssetName = Trim.Mid(LastSlash + 1);
        }
        else
        {
            OutPackageDir = TEXT("/Game/");
            OutAssetName = Trim;
        }

        int32 DotIdx = INDEX_NONE;
        if (OutAssetName.FindChar('.', DotIdx))
        {
            OutAssetName = OutAssetName.Left(DotIdx);
        }
    }

    /** Resolve the small set of root-composite classes the edit slice
     *  ships, plus an escape hatch for any UBTCompositeNode subclass
     *  loaded by full UClass path. */
    UClass* ResolveCompositeClass(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return nullptr;
        }
        FString Lower = Token.ToLower();
        if (Lower == TEXT("selector") || Lower == TEXT("btcomposite_selector"))
        {
            return UBTComposite_Selector::StaticClass();
        }
        if (Lower == TEXT("sequence") || Lower == TEXT("btcomposite_sequence"))
        {
            return UBTComposite_Sequence::StaticClass();
        }
        if (Lower == TEXT("simple_parallel") || Lower == TEXT("simpleparallel")
            || Lower == TEXT("parallel") || Lower == TEXT("btcomposite_simpleparallel"))
        {
            return UBTComposite_SimpleParallel::StaticClass();
        }
        if (Token.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadClass<UBTCompositeNode>(nullptr, *Token))
            {
                return Loaded;
            }
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *Token))
        {
            if (Found->IsChildOf(UBTCompositeNode::StaticClass()))
            {
                return Found;
            }
        }
        return nullptr;
    }

    /** Generic UBTNode subclass resolver. Accepts a short token with a
     *  case-insensitive lookup against the UE5 stock task / decorator /
     *  service set, a `/Script/Module.ClassName` path, and a `/Game/...`
     *  Blueprint class path (auto-suffixed with `_C`). */
    UClass* ResolveBTNodeClass(const FString& Token, UClass* RequiredBase)
    {
        if (Token.IsEmpty())
        {
            return nullptr;
        }
        const FString Lower = Token.ToLower();

        // Composite shortcuts (also valid as add_child_task targets).
        if (Lower == TEXT("selector"))      return UBTComposite_Selector::StaticClass();
        if (Lower == TEXT("sequence"))      return UBTComposite_Sequence::StaticClass();
        if (Lower == TEXT("simple_parallel") || Lower == TEXT("parallel")
            || Lower == TEXT("simpleparallel"))
        {
            return UBTComposite_SimpleParallel::StaticClass();
        }

        // Stock task shortcuts.
        if (Lower == TEXT("wait"))           return UBTTask_Wait::StaticClass();
        if (Lower == TEXT("move_to") || Lower == TEXT("moveto"))
        {
            return UBTTask_MoveTo::StaticClass();
        }
        if (Lower == TEXT("play_animation") || Lower == TEXT("playanimation"))
        {
            return UBTTask_PlayAnimation::StaticClass();
        }
        if (Lower == TEXT("play_sound") || Lower == TEXT("playsound"))
        {
            return UBTTask_PlaySound::StaticClass();
        }
        if (Lower == TEXT("run_behavior") || Lower == TEXT("runbehavior"))
        {
            return UBTTask_RunBehavior::StaticClass();
        }

        // Stock decorator shortcuts.
        if (Lower == TEXT("blackboard"))    return UBTDecorator_Blackboard::StaticClass();
        if (Lower == TEXT("cooldown"))      return UBTDecorator_Cooldown::StaticClass();
        if (Lower == TEXT("force_success") || Lower == TEXT("forcesuccess"))
        {
            return UBTDecorator_ForceSuccess::StaticClass();
        }
        if (Lower == TEXT("loop"))          return UBTDecorator_Loop::StaticClass();
        if (Lower == TEXT("time_limit") || Lower == TEXT("timelimit"))
        {
            return UBTDecorator_TimeLimit::StaticClass();
        }

        // Stock service shortcuts.
        if (Lower == TEXT("default_focus") || Lower == TEXT("defaultfocus"))
        {
            return UBTService_DefaultFocus::StaticClass();
        }

        // Full path / Blueprint class path.
        if (Token.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *Token))
            {
                if (!RequiredBase || Loaded->IsChildOf(RequiredBase))
                {
                    return Loaded;
                }
            }
        }
        if (Token.StartsWith(TEXT("/Game/")))
        {
            FString WithSuffix = Token;
            if (!WithSuffix.EndsWith(TEXT("_C")))
            {
                WithSuffix += TEXT("_C");
            }
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *WithSuffix))
            {
                if (!RequiredBase || Loaded->IsChildOf(RequiredBase))
                {
                    return Loaded;
                }
            }
        }
        // Fallback: look up the bare token plus an `/Script/AIModule.<Name>`
        // shape for users who pass `BTTask_Wait` style tokens.
        if (UClass* Found = FindObject<UClass>(nullptr, *Token))
        {
            if (!RequiredBase || Found->IsChildOf(RequiredBase))
            {
                return Found;
            }
        }
        const FString AIPath = FString::Printf(TEXT("/Script/AIModule.%s"), *Token);
        if (UClass* Loaded = LoadClass<UObject>(nullptr, *AIPath))
        {
            if (!RequiredBase || Loaded->IsChildOf(RequiredBase))
            {
                return Loaded;
            }
        }
        return nullptr;
    }

    /** Convert an FJsonValue into a textual form FProperty::ImportText
     *  accepts. Mirrors the helper in SproftBpComponentCommands. */
    FString BehaviorTree_JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return FString();
        }
        switch (Value->Type)
        {
            case EJson::String:
                return Value->AsString();
            case EJson::Number:
                return LexToString(Value->AsNumber());
            case EJson::Boolean:
                return Value->AsBool() ? TEXT("true") : TEXT("false");
            case EJson::Null:
                return TEXT("None");
            default:
            {
                FString Buffer;
                TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
                    TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Buffer);
                FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
                return Buffer;
            }
        }
    }

    /** Apply a flat property dict against a UObject through
     *  `FProperty::ImportText_InContainer`. Failed entries land on
     *  OutSkipped with a reason. Mirrors the slot-property pass on
     *  widget_edit / bp_component. */
    void ApplyFlatProperties(UObject* Target, const TSharedPtr<FJsonObject>* PropsObj,
                             TArray<TSharedPtr<FJsonValue>>& OutApplied,
                             TArray<TSharedPtr<FJsonValue>>& OutSkipped)
    {
        if (!Target || !PropsObj || !(*PropsObj).IsValid())
        {
            return;
        }
        FOutputDeviceNull NullDevice;
        for (const auto& Pair : (*PropsObj)->Values)
        {
            const FString& PropName = Pair.Key;
            const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

            FProperty* Prop = FindFProperty<FProperty>(Target->GetClass(), *PropName);
            if (!Prop)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), PropName);
                Skip->SetStringField(TEXT("reason"), TEXT("not_a_uproperty"));
                OutSkipped.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }
            const FString TextValue = BehaviorTree_JsonValueToImportText(JsonVal);
            const TCHAR* TextPtr = *TextValue;
            const TCHAR* Result = Prop->ImportText_InContainer(
                TextPtr, Target, Target, PPF_None, &NullDevice);
            if (Result == nullptr)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), PropName);
                Skip->SetStringField(TEXT("reason"), TEXT("import_text_failed"));
                Skip->SetStringField(TEXT("attempted_value"), TextValue);
                OutSkipped.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }
            TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
            Applied->SetStringField(TEXT("name"), PropName);
            Applied->SetStringField(TEXT("type"), Prop->GetCPPType());
            OutApplied.Add(MakeShared<FJsonValueObject>(Applied));
        }
    }

    /** Recursive walk: find a composite by `GetNodeName()` substring.
     *  Returns nullptr if no match. The `root` sentinel returns the
     *  tree's RootNode unconditionally. */
    UBTCompositeNode* FindCompositeByName(UBTCompositeNode* Search, const FString& Target)
    {
        if (!Search)
        {
            return nullptr;
        }
        if (Search->GetNodeName().Contains(Target))
        {
            return Search;
        }
        for (const FBTCompositeChild& Child : Search->Children)
        {
            if (Child.ChildComposite)
            {
                if (UBTCompositeNode* Found = FindCompositeByName(Child.ChildComposite, Target))
                {
                    return Found;
                }
            }
        }
        return nullptr;
    }

    /** Recursive walk: find a child slot that owns a node whose
     *  `GetNodeName()` contains Target. Returns the parent composite
     *  plus the child index so callers can reach `Children[Idx]`. */
    bool FindChildSlot(UBTCompositeNode* Search, const FString& Target,
                       UBTCompositeNode*& OutParent, int32& OutIndex)
    {
        if (!Search)
        {
            return false;
        }
        for (int32 Idx = 0; Idx < Search->Children.Num(); ++Idx)
        {
            const FBTCompositeChild& Child = Search->Children[Idx];
            UBTNode* ChildNode = Child.ChildComposite ? static_cast<UBTNode*>(Child.ChildComposite)
                                                      : static_cast<UBTNode*>(Child.ChildTask);
            if (ChildNode && ChildNode->GetNodeName().Contains(Target))
            {
                OutParent = Search;
                OutIndex = Idx;
                return true;
            }
            if (Child.ChildComposite)
            {
                if (FindChildSlot(Child.ChildComposite, Target, OutParent, OutIndex))
                {
                    return true;
                }
            }
        }
        return false;
    }

    /** Resolve a target BT asset and dispatch the common preamble:
     *  load + cast + null-check. */
    UBehaviorTree* LoadTargetTree(const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FString TreePath;
        if (!Params->TryGetStringField(TEXT("tree"), TreePath)
            && !Params->TryGetStringField(TEXT("tree_path"), TreePath)
            && !Params->TryGetStringField(TEXT("path"), TreePath)
            && !Params->TryGetStringField(TEXT("asset"), TreePath))
        {
            OutError = TEXT("Missing 'tree' parameter");
            return nullptr;
        }
        UObject* Asset = UEditorAssetLibrary::LoadAsset(TreePath);
        UBehaviorTree* Tree = Cast<UBehaviorTree>(Asset);
        if (!Tree)
        {
            OutError = FString::Printf(TEXT("Asset at '%s' is not a UBehaviorTree"), *TreePath);
            return nullptr;
        }
        return Tree;
    }
}

FSproftBehaviorTreeCommands::FSproftBehaviorTreeCommands()
{
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("behavior_tree"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown behavior_tree command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }
    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op == TEXT("inspect"))
    {
        return HandleBehaviorTree(Params);
    }
    if (Op == TEXT("create_behavior_tree"))
    {
        return HandleCreateBehaviorTree(Params);
    }
    if (Op == TEXT("add_root_composite"))
    {
        return HandleAddRootComposite(Params);
    }
    if (Op == TEXT("add_child_task") || Op == TEXT("add_child"))
    {
        return HandleAddChildTask(Params);
    }
    if (Op == TEXT("add_decorator"))
    {
        return HandleAddDecorator(Params);
    }
    if (Op == TEXT("set_root_decorator") || Op == TEXT("add_root_decorator"))
    {
        return HandleSetRootDecorator(Params);
    }
    if (Op == TEXT("add_blackboard_decorator") || Op == TEXT("add_bb_decorator"))
    {
        return HandleAddBlackboardDecorator(Params);
    }
    if (Op == TEXT("add_service"))
    {
        return HandleAddService(Params);
    }
    if (Op == TEXT("set_blackboard"))
    {
        return HandleSetBlackboard(Params);
    }
    if (Op == TEXT("add_blackboard_key"))
    {
        return HandleAddBlackboardKey(Params);
    }
    if (Op == TEXT("remove_blackboard_key"))
    {
        return HandleRemoveBlackboardKey(Params);
    }
    if (Op == TEXT("rename_blackboard_key") || Op == TEXT("rename_key"))
    {
        return HandleRenameBlackboardKey(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("behavior_tree: unsupported op '%s'"), *Op));
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

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleCreateBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("tree"), PackagePath)
        && !Params->TryGetStringField(TEXT("path"), PackagePath)
        && !Params->TryGetStringField(TEXT("tree_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'tree' parameter"));
    }
    if (!PackagePath.StartsWith(TEXT("/Game/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Tree path '%s' must start with /Game/"), *PackagePath));
    }

    FString PackageDir;
    FString AssetName;
    BehaviorTree_SplitPackagePath(PackagePath, PackageDir, AssetName);
    if (AssetName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not derive asset name from '%s'"), *PackagePath));
    }
    const FString AssetObjectPath = PackageDir + AssetName;

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    if (UEditorAssetLibrary::DoesAssetExist(AssetObjectPath) && !bOverwrite)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s (set 'overwrite': true to replace)"),
                *AssetObjectPath));
    }

    // Resolve the optional Blackboard before we touch the package so an
    // unresolvable path errors out early.
    UBlackboardData* BlackboardAsset = nullptr;
    FString BlackboardPath;
    if (Params->TryGetStringField(TEXT("blackboard"), BlackboardPath)
        || Params->TryGetStringField(TEXT("blackboard_path"), BlackboardPath))
    {
        if (!BlackboardPath.IsEmpty())
        {
            UObject* Loaded = UEditorAssetLibrary::LoadAsset(BlackboardPath);
            BlackboardAsset = Cast<UBlackboardData>(Loaded);
            if (!BlackboardAsset)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Asset at '%s' is not a UBlackboardData"), *BlackboardPath));
            }
        }
    }

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UBehaviorTree* Tree = NewObject<UBehaviorTree>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!Tree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UBehaviorTree"));
    }
    if (BlackboardAsset)
    {
        Tree->BlackboardAsset = BlackboardAsset;
    }

    FAssetRegistryModule::AssetCreated(Tree);
    Package->MarkPackageDirty();

    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("create_behavior_tree"));
    Result->SetStringField(TEXT("name"), AssetName);
    Result->SetStringField(TEXT("path"), AssetObjectPath);
    Result->SetStringField(TEXT("class"), Tree->GetClass()->GetName());
    if (BlackboardAsset)
    {
        Result->SetStringField(TEXT("blackboard_path"), BlackboardAsset->GetPathName());
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleAddRootComposite(const TSharedPtr<FJsonObject>& Params)
{
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

    FString CompositeToken;
    if (!Params->TryGetStringField(TEXT("composite_class"), CompositeToken)
        && !Params->TryGetStringField(TEXT("composite"), CompositeToken)
        && !Params->TryGetStringField(TEXT("class"), CompositeToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'composite_class' parameter (selector / sequence / simple_parallel)"));
    }
    UClass* CompositeClass = ResolveCompositeClass(CompositeToken);
    if (!CompositeClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unrecognised composite class '%s'. Use 'selector', 'sequence', 'simple_parallel', or a /Script/... UBTCompositeNode subclass path."),
                *CompositeToken));
    }

    bool bReplace = false;
    Params->TryGetBoolField(TEXT("replace"), bReplace);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    if (Tree->RootNode && !bReplace)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Tree '%s' already has a RootNode (%s); pass 'replace': true to overwrite"),
                *Tree->GetName(), *Tree->RootNode->GetClass()->GetName()));
    }

    // Outer the new composite under the tree asset so it travels with
    // the package on save.
    UBTCompositeNode* NewRoot = NewObject<UBTCompositeNode>(
        Tree, CompositeClass, NAME_None, RF_Transactional);
    if (!NewRoot)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to NewObject composite of class '%s'"), *CompositeClass->GetName()));
    }

    Tree->RootNode = NewRoot;
    Tree->MarkPackageDirty();

    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Tree->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_root_composite"));
    Result->SetStringField(TEXT("tree"), Tree->GetPathName());
    Result->SetStringField(TEXT("composite_class"), CompositeClass->GetName());
    Result->SetStringField(TEXT("composite_class_path"), CompositeClass->GetPathName());
    Result->SetStringField(TEXT("root_node_name"), NewRoot->GetName());
    Result->SetBoolField(TEXT("saved"), bSave);
    Result->SetBoolField(TEXT("replaced"), bReplace);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleAddChildTask(const TSharedPtr<FJsonObject>& Params)
{
    FString TreeError;
    UBehaviorTree* Tree = LoadTargetTree(Params, TreeError);
    if (!Tree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TreeError);
    }
    if (!Tree->RootNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Tree '%s' has no RootNode; call add_root_composite first"),
                *Tree->GetName()));
    }

    FString ParentTarget;
    Params->TryGetStringField(TEXT("parent"), ParentTarget);
    if (ParentTarget.IsEmpty())
    {
        Params->TryGetStringField(TEXT("parent_name"), ParentTarget);
    }
    UBTCompositeNode* ParentComposite = nullptr;
    if (ParentTarget.IsEmpty() || ParentTarget.Equals(TEXT("root"), ESearchCase::IgnoreCase))
    {
        ParentComposite = Tree->RootNode;
    }
    else
    {
        ParentComposite = FindCompositeByName(Tree->RootNode, ParentTarget);
    }
    if (!ParentComposite)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No composite matching '%s' under tree '%s'"),
                *ParentTarget, *Tree->GetName()));
    }

    FString TaskClassToken;
    if (!Params->TryGetStringField(TEXT("task_class"), TaskClassToken)
        && !Params->TryGetStringField(TEXT("class"), TaskClassToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'task_class' parameter"));
    }
    UClass* ChildClass = ResolveBTNodeClass(TaskClassToken, UBTNode::StaticClass());
    if (!ChildClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UBTNode class '%s'"), *TaskClassToken));
    }
    if (ChildClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is abstract"), *ChildClass->GetPathName()));
    }
    const bool bIsComposite = ChildClass->IsChildOf(UBTCompositeNode::StaticClass());
    const bool bIsTask = ChildClass->IsChildOf(UBTTaskNode::StaticClass());
    if (!bIsComposite && !bIsTask)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is not a UBTCompositeNode or UBTTaskNode subclass"),
                *ChildClass->GetPathName()));
    }

    FString FriendlyName;
    Params->TryGetStringField(TEXT("node_name"), FriendlyName);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    const bool bHasProperties = Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid();

    // Outer the new node under the BT asset so it travels with the
    // package on save. The runtime exec / memory indices are populated
    // by the BT graph's RebuildExecutionOrder when the asset is opened
    // or compiled in the editor.
    UBTNode* NewChild = NewObject<UBTNode>(Tree, ChildClass, NAME_None, RF_Transactional);
    if (!NewChild)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to NewObject UBTNode of class '%s'"), *ChildClass->GetName()));
    }
    NewChild->InitializeFromAsset(*Tree);
    if (!FriendlyName.IsEmpty())
    {
        // UBTNode::NodeName is editor-data only; GetNodeName() returns
        // the class display name when empty.
#if WITH_EDITORONLY_DATA
        if (FStrProperty* StrProp = CastField<FStrProperty>(
            FindFProperty<FProperty>(UBTNode::StaticClass(), TEXT("NodeName"))))
        {
            StrProp->SetPropertyValue_InContainer(NewChild, FriendlyName);
        }
#endif
    }

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    if (bHasProperties)
    {
        ApplyFlatProperties(NewChild, PropsObj, AppliedJson, SkippedJson);
    }

    FBTCompositeChild& Slot = ParentComposite->Children.AddDefaulted_GetRef();
    if (bIsComposite)
    {
        Slot.ChildComposite = Cast<UBTCompositeNode>(NewChild);
    }
    else
    {
        Slot.ChildTask = Cast<UBTTaskNode>(NewChild);
    }

    Tree->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Tree->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_child_task"));
    Result->SetStringField(TEXT("tree"), Tree->GetPathName());
    Result->SetStringField(TEXT("parent"), ParentComposite->GetNodeName());
    Result->SetStringField(TEXT("class"), ChildClass->GetName());
    Result->SetStringField(TEXT("class_path"), ChildClass->GetPathName());
    Result->SetStringField(TEXT("kind"), bIsComposite ? TEXT("composite") : TEXT("task"));
    Result->SetStringField(TEXT("node_name"), NewChild->GetNodeName());
    Result->SetStringField(TEXT("object_name"), NewChild->GetName());
    Result->SetNumberField(TEXT("child_index"), ParentComposite->Children.Num() - 1);
    if (bHasProperties)
    {
        Result->SetArrayField(TEXT("applied_properties"), AppliedJson);
        Result->SetArrayField(TEXT("skipped_properties"), SkippedJson);
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleAddDecorator(const TSharedPtr<FJsonObject>& Params)
{
    FString TreeError;
    UBehaviorTree* Tree = LoadTargetTree(Params, TreeError);
    if (!Tree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TreeError);
    }
    if (!Tree->RootNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Tree '%s' has no RootNode"), *Tree->GetName()));
    }

    FString TargetName;
    if (!Params->TryGetStringField(TEXT("target"), TargetName)
        && !Params->TryGetStringField(TEXT("target_name"), TargetName)
        && !Params->TryGetStringField(TEXT("node"), TargetName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'target' parameter (decorator attaches to a child slot under a composite)"));
    }

    UBTCompositeNode* ParentComposite = nullptr;
    int32 ChildIndex = INDEX_NONE;
    if (!FindChildSlot(Tree->RootNode, TargetName, ParentComposite, ChildIndex))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No child slot matching '%s' under tree '%s'"),
                *TargetName, *Tree->GetName()));
    }

    FString DecoratorClassToken;
    if (!Params->TryGetStringField(TEXT("decorator_class"), DecoratorClassToken)
        && !Params->TryGetStringField(TEXT("class"), DecoratorClassToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'decorator_class' parameter"));
    }
    UClass* DecoratorClass = ResolveBTNodeClass(DecoratorClassToken, UBTDecorator::StaticClass());
    if (!DecoratorClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UBTDecorator class '%s'"), *DecoratorClassToken));
    }
    if (DecoratorClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is abstract"), *DecoratorClass->GetPathName()));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    const bool bHasProperties = Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid();

    UBTDecorator* NewDecorator = NewObject<UBTDecorator>(Tree, DecoratorClass, NAME_None, RF_Transactional);
    if (!NewDecorator)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to NewObject UBTDecorator of class '%s'"), *DecoratorClass->GetName()));
    }
    NewDecorator->InitializeFromAsset(*Tree);

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    if (bHasProperties)
    {
        ApplyFlatProperties(NewDecorator, PropsObj, AppliedJson, SkippedJson);
    }

    ParentComposite->Children[ChildIndex].Decorators.Add(NewDecorator);

    Tree->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Tree->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    const FBTCompositeChild& Slot = ParentComposite->Children[ChildIndex];
    UBTNode* SlotNode = Slot.ChildComposite ? static_cast<UBTNode*>(Slot.ChildComposite)
                                             : static_cast<UBTNode*>(Slot.ChildTask);
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_decorator"));
    Result->SetStringField(TEXT("tree"), Tree->GetPathName());
    Result->SetStringField(TEXT("parent_composite"), ParentComposite->GetNodeName());
    Result->SetNumberField(TEXT("child_index"), ChildIndex);
    if (SlotNode)
    {
        Result->SetStringField(TEXT("target_node"), SlotNode->GetNodeName());
    }
    Result->SetStringField(TEXT("class"), DecoratorClass->GetName());
    Result->SetStringField(TEXT("class_path"), DecoratorClass->GetPathName());
    Result->SetStringField(TEXT("decorator_name"), NewDecorator->GetNodeName());
    Result->SetStringField(TEXT("object_name"), NewDecorator->GetName());
    Result->SetNumberField(TEXT("decorator_index"), Slot.Decorators.Num() - 1);
    if (bHasProperties)
    {
        Result->SetArrayField(TEXT("applied_properties"), AppliedJson);
        Result->SetArrayField(TEXT("skipped_properties"), SkippedJson);
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleSetRootDecorator(const TSharedPtr<FJsonObject>& Params)
{
    // Tree-level decorators live on `UBehaviorTree::RootDecorators`,
    // a TArray<TObjectPtr<UBTDecorator>> on the asset itself rather
    // than on the root composite. The BT editor exposes this chain
    // under "Add Decorator" on the root composite node; the runtime
    // gates the subtree on the same chain when the BT is referenced
    // through a `RunBehavior` task.
    FString TreeError;
    UBehaviorTree* Tree = LoadTargetTree(Params, TreeError);
    if (!Tree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TreeError);
    }

    FString DecoratorClassToken;
    if (!Params->TryGetStringField(TEXT("decorator_class"), DecoratorClassToken)
        && !Params->TryGetStringField(TEXT("class"), DecoratorClassToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'decorator_class' parameter"));
    }
    UClass* DecoratorClass = ResolveBTNodeClass(DecoratorClassToken, UBTDecorator::StaticClass());
    if (!DecoratorClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UBTDecorator class '%s'"), *DecoratorClassToken));
    }
    if (DecoratorClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is abstract"), *DecoratorClass->GetPathName()));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    const bool bHasProperties = Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid();

    UBTDecorator* NewDecorator = NewObject<UBTDecorator>(Tree, DecoratorClass, NAME_None, RF_Transactional);
    if (!NewDecorator)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to NewObject UBTDecorator of class '%s'"), *DecoratorClass->GetName()));
    }
    NewDecorator->InitializeFromAsset(*Tree);

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    if (bHasProperties)
    {
        ApplyFlatProperties(NewDecorator, PropsObj, AppliedJson, SkippedJson);
    }

    Tree->RootDecorators.Add(NewDecorator);

    Tree->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Tree->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("set_root_decorator"));
    Result->SetStringField(TEXT("tree"), Tree->GetPathName());
    Result->SetStringField(TEXT("class"), DecoratorClass->GetName());
    Result->SetStringField(TEXT("class_path"), DecoratorClass->GetPathName());
    Result->SetStringField(TEXT("decorator_name"), NewDecorator->GetNodeName());
    Result->SetStringField(TEXT("object_name"), NewDecorator->GetName());
    Result->SetNumberField(TEXT("decorator_index"), Tree->RootDecorators.Num() - 1);
    Result->SetNumberField(TEXT("root_decorator_count"), Tree->RootDecorators.Num());
    if (bHasProperties)
    {
        Result->SetArrayField(TEXT("applied_properties"), AppliedJson);
        Result->SetArrayField(TEXT("skipped_properties"), SkippedJson);
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleAddBlackboardDecorator(const TSharedPtr<FJsonObject>& Params)
{
    FString TreeError;
    UBehaviorTree* Tree = LoadTargetTree(Params, TreeError);
    if (!Tree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TreeError);
    }
    if (!Tree->RootNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Tree '%s' has no RootNode"), *Tree->GetName()));
    }

    // Walk to the child slot we attach the decorator under.
    FString TargetName;
    if (!Params->TryGetStringField(TEXT("target"), TargetName)
        && !Params->TryGetStringField(TEXT("target_name"), TargetName)
        && !Params->TryGetStringField(TEXT("node"), TargetName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'target' parameter (decorator attaches to a child slot under a composite)"));
    }
    UBTCompositeNode* ParentComposite = nullptr;
    int32 ChildIndex = INDEX_NONE;
    if (!FindChildSlot(Tree->RootNode, TargetName, ParentComposite, ChildIndex))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No child slot matching '%s' under tree '%s'"),
                *TargetName, *Tree->GetName()));
    }

    // The Blackboard asset has to exist for the key to resolve.
    UBlackboardData* Blackboard = Tree->GetBlackboardAsset();
    if (!Blackboard)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Tree '%s' has no Blackboard asset; pair one with set_blackboard before adding a Blackboard decorator"),
                *Tree->GetName()));
    }

    // Required: the key name. Resolve it against the blackboard's own
    // and inherited entries so we can pick the matching key type.
    FString KeyToken;
    if (!Params->TryGetStringField(TEXT("key"), KeyToken)
        && !Params->TryGetStringField(TEXT("key_name"), KeyToken)
        && !Params->TryGetStringField(TEXT("blackboard_key"), KeyToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'key' parameter (Blackboard key name)"));
    }

    auto FindKeyEntry = [Blackboard](const FString& KeyName) -> const FBlackboardEntry*
    {
        for (const FBlackboardEntry& Entry : Blackboard->Keys)
        {
            if (Entry.EntryName.ToString().Equals(KeyName, ESearchCase::IgnoreCase))
            {
                return &Entry;
            }
        }
#if WITH_EDITORONLY_DATA
        for (const FBlackboardEntry& Entry : Blackboard->ParentKeys)
        {
            if (Entry.EntryName.ToString().Equals(KeyName, ESearchCase::IgnoreCase))
            {
                return &Entry;
            }
        }
#endif
        return nullptr;
    };
    const FBlackboardEntry* KeyEntry = FindKeyEntry(KeyToken);
    if (!KeyEntry)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blackboard '%s' has no key named '%s'"),
                *Blackboard->GetName(), *KeyToken));
    }

    // Required: the condition. The four documented tokens map onto
    // the standard EBasicKeyOperation / EArithmeticKeyOperation /
    // ETextKeyOperation triple the editor's Blackboard-decorator
    // detail panel exposes. IsSet / IsNotSet use the Basic family;
    // IsEqualTo / IsNotEqualTo route through the arithmetic family
    // for numeric / bool / enum keys and the text family for
    // FName / FString.
    FString ConditionToken;
    if (!Params->TryGetStringField(TEXT("condition"), ConditionToken)
        && !Params->TryGetStringField(TEXT("operation"), ConditionToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'condition' parameter (try IsSet / IsNotSet / IsEqualTo / IsNotEqualTo)"));
    }
    FString ConditionLower = ConditionToken.ToLower();
    ConditionLower.ReplaceInline(TEXT("_"), TEXT(""));
    ConditionLower.ReplaceInline(TEXT(" "), TEXT(""));

    enum class EBBDecoratorCondition : uint8
    {
        IsSet,
        IsNotSet,
        IsEqualTo,
        IsNotEqualTo,
    };
    EBBDecoratorCondition Condition = EBBDecoratorCondition::IsSet;
    FString ConditionCanonical;
    if (ConditionLower == TEXT("isset") || ConditionLower == TEXT("set"))
    {
        Condition = EBBDecoratorCondition::IsSet;
        ConditionCanonical = TEXT("IsSet");
    }
    else if (ConditionLower == TEXT("isnotset") || ConditionLower == TEXT("notset") || ConditionLower == TEXT("unset"))
    {
        Condition = EBBDecoratorCondition::IsNotSet;
        ConditionCanonical = TEXT("IsNotSet");
    }
    else if (ConditionLower == TEXT("isequalto") || ConditionLower == TEXT("equalto") || ConditionLower == TEXT("equal") || ConditionLower == TEXT("eq"))
    {
        Condition = EBBDecoratorCondition::IsEqualTo;
        ConditionCanonical = TEXT("IsEqualTo");
    }
    else if (ConditionLower == TEXT("isnotequalto") || ConditionLower == TEXT("notequalto") || ConditionLower == TEXT("notequal") || ConditionLower == TEXT("ne"))
    {
        Condition = EBBDecoratorCondition::IsNotEqualTo;
        ConditionCanonical = TEXT("IsNotEqualTo");
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported condition '%s' (try IsSet / IsNotSet / IsEqualTo / IsNotEqualTo)"),
                *ConditionToken));
    }

    // Classify the key type so we know which operation family to write
    // (Basic / Arithmetic / Text) and which payload field (IntValue /
    // FloatValue / StringValue) carries the comparison value.
    enum class EBBDecoratorFamily : uint8
    {
        Basic,
        Arithmetic,
        Text,
    };
    EBBDecoratorFamily Family = EBBDecoratorFamily::Basic;
    FString KeyTypeName = TEXT("none");
    bool bRequiresValue = (Condition == EBBDecoratorCondition::IsEqualTo || Condition == EBBDecoratorCondition::IsNotEqualTo);
    if (KeyEntry->KeyType)
    {
        KeyTypeName = KeyEntry->KeyType->GetClass()->GetName();
        if (KeyEntry->KeyType->IsA<UBlackboardKeyType_Name>()
            || KeyEntry->KeyType->IsA<UBlackboardKeyType_String>())
        {
            Family = EBBDecoratorFamily::Text;
        }
        else if (KeyEntry->KeyType->IsA<UBlackboardKeyType_Int>()
                 || KeyEntry->KeyType->IsA<UBlackboardKeyType_Float>()
                 || KeyEntry->KeyType->IsA<UBlackboardKeyType_Bool>()
                 || KeyEntry->KeyType->IsA<UBlackboardKeyType_Enum>())
        {
            Family = EBBDecoratorFamily::Arithmetic;
        }
        else
        {
            // Object / Class / Vector / Rotator / Struct keys only
            // support the Basic family (IsSet / IsNotSet). Asking for
            // IsEqualTo on those is a caller error.
            Family = EBBDecoratorFamily::Basic;
        }
    }
    // IsSet / IsNotSet always use the Basic family regardless of key.
    if (Condition == EBBDecoratorCondition::IsSet || Condition == EBBDecoratorCondition::IsNotSet)
    {
        Family = EBBDecoratorFamily::Basic;
    }
    if (bRequiresValue && Family == EBBDecoratorFamily::Basic && KeyEntry->KeyType)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blackboard key '%s' (type '%s') does not support IsEqualTo / IsNotEqualTo (only IsSet / IsNotSet)"),
                *KeyToken, *KeyTypeName));
    }

    // Optional comparison value. Read once into a strongly typed
    // payload so we can route to IntValue / FloatValue / StringValue
    // based on the key family.
    int32 PayloadInt = 0;
    float PayloadFloat = 0.0f;
    FString PayloadString;
    bool bPayloadProvided = false;
    if (Params->HasField(TEXT("value")) || Params->HasField(TEXT("comparison_value"))
        || Params->HasField(TEXT("compare_to")))
    {
        TSharedPtr<FJsonValue> Val = Params->TryGetField(TEXT("value"));
        if (!Val.IsValid()) { Val = Params->TryGetField(TEXT("comparison_value")); }
        if (!Val.IsValid()) { Val = Params->TryGetField(TEXT("compare_to")); }
        if (Val.IsValid() && Val->Type != EJson::Null)
        {
            bPayloadProvided = true;
            if (Val->Type == EJson::Number)
            {
                const double D = Val->AsNumber();
                PayloadInt = static_cast<int32>(D);
                PayloadFloat = static_cast<float>(D);
                PayloadString = FString::Printf(TEXT("%g"), D);
            }
            else if (Val->Type == EJson::Boolean)
            {
                PayloadInt = Val->AsBool() ? 1 : 0;
                PayloadFloat = Val->AsBool() ? 1.0f : 0.0f;
                PayloadString = Val->AsBool() ? TEXT("true") : TEXT("false");
            }
            else if (Val->Type == EJson::String)
            {
                PayloadString = Val->AsString();
                LexFromString(PayloadInt, *PayloadString);
                LexFromString(PayloadFloat, *PayloadString);
            }
        }
    }
    if (bRequiresValue && !bPayloadProvided)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Condition '%s' requires a 'value' (the comparison target)"),
                *ConditionCanonical));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // Spawn the decorator. NewObject runs the same constructor path
    // HandleAddDecorator uses; InitializeFromAsset hooks up the
    // FBlackboardKeySelector against the tree's Blackboard.
    UBTDecorator_Blackboard* NewDecorator = NewObject<UBTDecorator_Blackboard>(
        Tree, UBTDecorator_Blackboard::StaticClass(), NAME_None, RF_Transactional);
    if (!NewDecorator)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Failed to NewObject UBTDecorator_Blackboard"));
    }

    // Reflection-write the protected UPROPERTY fields. Going through
    // the property database bypasses C++ access controls without us
    // touching engine private headers. The BlackboardKey field is an
    // FBlackboardKeySelector struct; we write only SelectedKeyName
    // and let InitializeFromAsset resolve the rest.
    UClass* DecoratorClass = NewDecorator->GetClass();
    FProperty* BlackboardKeyProp = DecoratorClass->FindPropertyByName(TEXT("BlackboardKey"));
    if (FStructProperty* SelectorProp = CastField<FStructProperty>(BlackboardKeyProp))
    {
        void* SelectorPtr = SelectorProp->ContainerPtrToValuePtr<void>(NewDecorator);
        // SelectedKeyName is a public field on FBlackboardKeySelector,
        // so the struct property's inner FNameProperty lands the
        // FName cleanly.
        FProperty* SelectedKeyNameProp = SelectorProp->Struct->FindPropertyByName(TEXT("SelectedKeyName"));
        if (FNameProperty* NameProp = CastField<FNameProperty>(SelectedKeyNameProp))
        {
            NameProp->SetPropertyValue(NameProp->ContainerPtrToValuePtr<void>(SelectorPtr),
                FName(*KeyEntry->EntryName.ToString()));
        }
    }

    // OperationType is a uint8 the engine indexes into the
    // EBlackboardKeyOperation::Type enum (Basic / Arithmetic / Text).
    if (FProperty* OperationTypeProp = DecoratorClass->FindPropertyByName(TEXT("OperationType")))
    {
        if (FByteProperty* ByteProp = CastField<FByteProperty>(OperationTypeProp))
        {
            uint8 OpType = static_cast<uint8>(EBlackboardKeyOperation::Basic);
            if (Family == EBBDecoratorFamily::Arithmetic)
            {
                OpType = static_cast<uint8>(EBlackboardKeyOperation::Arithmetic);
            }
            else if (Family == EBBDecoratorFamily::Text)
            {
                OpType = static_cast<uint8>(EBlackboardKeyOperation::Text);
            }
            ByteProp->SetPropertyValue(ByteProp->ContainerPtrToValuePtr<void>(NewDecorator), OpType);
        }
    }

    // BasicOperation / ArithmeticOperation / TextOperation are
    // TEnumAsByte under WITH_EDITORONLY_DATA. Writing the matching
    // enum value through the byte field surface lands the IsSet /
    // IsNotSet / Equal / NotEqual selection the editor row exposes.
#if WITH_EDITORONLY_DATA
    auto WriteByteEnum = [&DecoratorClass, &NewDecorator](const TCHAR* FieldName, uint8 Value)
    {
        if (FProperty* Prop = DecoratorClass->FindPropertyByName(FieldName))
        {
            if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
            {
                ByteProp->SetPropertyValue(ByteProp->ContainerPtrToValuePtr<void>(NewDecorator), Value);
            }
        }
    };

    if (Family == EBBDecoratorFamily::Basic)
    {
        const uint8 BasicVal = (Condition == EBBDecoratorCondition::IsSet)
            ? static_cast<uint8>(EBasicKeyOperation::Set)
            : static_cast<uint8>(EBasicKeyOperation::NotSet);
        WriteByteEnum(TEXT("BasicOperation"), BasicVal);
    }
    else if (Family == EBBDecoratorFamily::Arithmetic)
    {
        const uint8 ArithVal = (Condition == EBBDecoratorCondition::IsEqualTo)
            ? static_cast<uint8>(EArithmeticKeyOperation::Equal)
            : static_cast<uint8>(EArithmeticKeyOperation::NotEqual);
        WriteByteEnum(TEXT("ArithmeticOperation"), ArithVal);
    }
    else if (Family == EBBDecoratorFamily::Text)
    {
        const uint8 TextVal = (Condition == EBBDecoratorCondition::IsEqualTo)
            ? static_cast<uint8>(ETextKeyOperation::Equal)
            : static_cast<uint8>(ETextKeyOperation::NotEqual);
        WriteByteEnum(TEXT("TextOperation"), TextVal);
    }
#endif

    // Land the comparison payload on the right field. IntValue and
    // FloatValue cover the arithmetic family; StringValue covers
    // both the text family (for FName / FString keys) and is the
    // engine's go-to storage slot for enum-key arithmetic ops
    // (see UBTDecorator_Blackboard::RefreshEnumBasedDecorator).
    auto WriteFloat = [&DecoratorClass, NewDecorator](const TCHAR* FieldName, float Value)
    {
        if (FProperty* Prop = DecoratorClass->FindPropertyByName(FieldName))
        {
            if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
            {
                FloatProp->SetPropertyValue(FloatProp->ContainerPtrToValuePtr<void>(NewDecorator), Value);
            }
        }
    };
    auto WriteInt = [&DecoratorClass, NewDecorator](const TCHAR* FieldName, int32 Value)
    {
        if (FProperty* Prop = DecoratorClass->FindPropertyByName(FieldName))
        {
            if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
            {
                IntProp->SetPropertyValue(IntProp->ContainerPtrToValuePtr<void>(NewDecorator), Value);
            }
        }
    };
    auto WriteString = [&DecoratorClass, NewDecorator](const TCHAR* FieldName, const FString& Value)
    {
        if (FProperty* Prop = DecoratorClass->FindPropertyByName(FieldName))
        {
            if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
            {
                StrProp->SetPropertyValue(StrProp->ContainerPtrToValuePtr<void>(NewDecorator), Value);
            }
        }
    };

    if (bPayloadProvided)
    {
        WriteInt(TEXT("IntValue"), PayloadInt);
        WriteFloat(TEXT("FloatValue"), PayloadFloat);
        WriteString(TEXT("StringValue"), PayloadString);
    }

    // NotifyObserver picks how often the runtime restarts the branch
    // when the observed key changes. Default ResultChange matches the
    // editor's default; callers can override through the
    // `notify_observer` field if needed.
    FString NotifyToken;
    if (Params->TryGetStringField(TEXT("notify_observer"), NotifyToken)
        || Params->TryGetStringField(TEXT("notify"), NotifyToken))
    {
        if (FProperty* NotifyProp = DecoratorClass->FindPropertyByName(TEXT("NotifyObserver")))
        {
            if (FByteProperty* ByteProp = CastField<FByteProperty>(NotifyProp))
            {
                const FString NLower = NotifyToken.ToLower();
                uint8 NotifyVal = static_cast<uint8>(EBTBlackboardRestart::ResultChange);
                if (NLower == TEXT("valuechange") || NLower == TEXT("value_change") || NLower == TEXT("on_value_change"))
                {
                    NotifyVal = static_cast<uint8>(EBTBlackboardRestart::ValueChange);
                }
                ByteProp->SetPropertyValue(ByteProp->ContainerPtrToValuePtr<void>(NewDecorator), NotifyVal);
            }
        }
    }

    // Resolves the selector against the tree's Blackboard so
    // SelectedKeyID + SelectedKeyType match the named key. Without
    // this step the decorator runs against InvalidKey at game time.
    NewDecorator->InitializeFromAsset(*Tree);

    // Attach to the child slot. Same path HandleAddDecorator uses.
    ParentComposite->Children[ChildIndex].Decorators.Add(NewDecorator);

    Tree->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Tree->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    const FBTCompositeChild& Slot = ParentComposite->Children[ChildIndex];
    UBTNode* SlotNode = Slot.ChildComposite ? static_cast<UBTNode*>(Slot.ChildComposite)
                                             : static_cast<UBTNode*>(Slot.ChildTask);
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_blackboard_decorator"));
    Result->SetStringField(TEXT("tree"), Tree->GetPathName());
    Result->SetStringField(TEXT("parent_composite"), ParentComposite->GetNodeName());
    Result->SetNumberField(TEXT("child_index"), ChildIndex);
    if (SlotNode)
    {
        Result->SetStringField(TEXT("target_node"), SlotNode->GetNodeName());
    }
    Result->SetStringField(TEXT("class"), UBTDecorator_Blackboard::StaticClass()->GetName());
    Result->SetStringField(TEXT("class_path"), UBTDecorator_Blackboard::StaticClass()->GetPathName());
    Result->SetStringField(TEXT("decorator_name"), NewDecorator->GetNodeName());
    Result->SetStringField(TEXT("object_name"), NewDecorator->GetName());
    Result->SetNumberField(TEXT("decorator_index"), Slot.Decorators.Num() - 1);
    Result->SetStringField(TEXT("key"), KeyEntry->EntryName.ToString());
    Result->SetStringField(TEXT("key_type"), DescribeKeyType(KeyEntry->KeyType));
    Result->SetStringField(TEXT("condition"), ConditionCanonical);
    Result->SetStringField(TEXT("operation_family"),
        Family == EBBDecoratorFamily::Basic ? TEXT("Basic")
        : Family == EBBDecoratorFamily::Arithmetic ? TEXT("Arithmetic")
        : TEXT("Text"));
    Result->SetBoolField(TEXT("value_provided"), bPayloadProvided);
    if (bPayloadProvided)
    {
        Result->SetNumberField(TEXT("int_value"), PayloadInt);
        Result->SetNumberField(TEXT("float_value"), PayloadFloat);
        Result->SetStringField(TEXT("string_value"), PayloadString);
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleAddService(const TSharedPtr<FJsonObject>& Params)
{
    FString TreeError;
    UBehaviorTree* Tree = LoadTargetTree(Params, TreeError);
    if (!Tree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TreeError);
    }
    if (!Tree->RootNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Tree '%s' has no RootNode"), *Tree->GetName()));
    }

    FString TargetName;
    Params->TryGetStringField(TEXT("target"), TargetName);
    if (TargetName.IsEmpty())
    {
        Params->TryGetStringField(TEXT("target_name"), TargetName);
    }
    UBTCompositeNode* TargetComposite = nullptr;
    if (TargetName.IsEmpty() || TargetName.Equals(TEXT("root"), ESearchCase::IgnoreCase))
    {
        TargetComposite = Tree->RootNode;
    }
    else
    {
        TargetComposite = FindCompositeByName(Tree->RootNode, TargetName);
    }
    if (!TargetComposite)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No composite matching '%s' under tree '%s' (services attach to composites)"),
                *TargetName, *Tree->GetName()));
    }

    FString ServiceClassToken;
    if (!Params->TryGetStringField(TEXT("service_class"), ServiceClassToken)
        && !Params->TryGetStringField(TEXT("class"), ServiceClassToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'service_class' parameter"));
    }
    UClass* ServiceClass = ResolveBTNodeClass(ServiceClassToken, UBTService::StaticClass());
    if (!ServiceClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UBTService class '%s'"), *ServiceClassToken));
    }
    if (ServiceClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is abstract"), *ServiceClass->GetPathName()));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    const bool bHasProperties = Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid();

    UBTService* NewService = NewObject<UBTService>(Tree, ServiceClass, NAME_None, RF_Transactional);
    if (!NewService)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to NewObject UBTService of class '%s'"), *ServiceClass->GetName()));
    }
    NewService->InitializeFromAsset(*Tree);

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    if (bHasProperties)
    {
        ApplyFlatProperties(NewService, PropsObj, AppliedJson, SkippedJson);
    }

    TargetComposite->Services.Add(NewService);

    Tree->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Tree->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_service"));
    Result->SetStringField(TEXT("tree"), Tree->GetPathName());
    Result->SetStringField(TEXT("target_composite"), TargetComposite->GetNodeName());
    Result->SetStringField(TEXT("class"), ServiceClass->GetName());
    Result->SetStringField(TEXT("class_path"), ServiceClass->GetPathName());
    Result->SetStringField(TEXT("service_name"), NewService->GetNodeName());
    Result->SetStringField(TEXT("object_name"), NewService->GetName());
    Result->SetNumberField(TEXT("service_index"), TargetComposite->Services.Num() - 1);
    if (bHasProperties)
    {
        Result->SetArrayField(TEXT("applied_properties"), AppliedJson);
        Result->SetArrayField(TEXT("skipped_properties"), SkippedJson);
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

// =============================================================================
// Blackboard key edits.
//
// `set_blackboard` rebinds the BT's BlackboardAsset slot.
// `add_blackboard_key` appends an FBlackboardEntry with a typed
// UBlackboardKeyType subclass on the chosen Blackboard.
// `remove_blackboard_key` removes a key from a chosen Blackboard's
// `Keys` array by FName.
//
// The Blackboard is resolved either through an explicit `blackboard`
// path or, when only `tree` is set, through the BT's BlackboardAsset
// slot (when bound).
// =============================================================================

namespace
{
    UBlackboardData* ResolveBlackboardArg(const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FString BlackboardInput;
        if (Params->TryGetStringField(TEXT("blackboard"), BlackboardInput)
            || Params->TryGetStringField(TEXT("blackboard_path"), BlackboardInput))
        {
            if (BlackboardInput.IsEmpty())
            {
                OutError = TEXT("'blackboard' must not be empty");
                return nullptr;
            }
            UObject* Resolved = nullptr;
            if (BlackboardInput.StartsWith(TEXT("/")))
            {
                Resolved = UEditorAssetLibrary::LoadAsset(BlackboardInput);
            }
            else
            {
                FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
                TArray<FAssetData> Found;
                AssetRegistry.Get().GetAssetsByClass(UBlackboardData::StaticClass()->GetClassPathName(), Found);
                for (const FAssetData& Data : Found)
                {
                    if (Data.AssetName.ToString().Equals(BlackboardInput, ESearchCase::IgnoreCase))
                    {
                        Resolved = Data.GetAsset();
                        break;
                    }
                }
            }
            UBlackboardData* BBData = Cast<UBlackboardData>(Resolved);
            if (!BBData)
            {
                OutError = FString::Printf(TEXT("Asset '%s' is not a UBlackboardData"), *BlackboardInput);
                return nullptr;
            }
            return BBData;
        }

        // Fallback: resolve via the tree's BlackboardAsset slot.
        FString TreeError;
        UBehaviorTree* Tree = LoadTargetTree(Params, TreeError);
        if (!Tree)
        {
            OutError = TEXT("Provide either 'blackboard' or 'tree' (with a bound BlackboardAsset)");
            return nullptr;
        }
        UBlackboardData* Bound = Tree->GetBlackboardAsset();
        if (!Bound)
        {
            OutError = FString::Printf(TEXT("Tree '%s' has no BlackboardAsset; pass 'blackboard' explicitly"),
                *Tree->GetName());
            return nullptr;
        }
        return Bound;
    }

    UClass* ResolveBlackboardKeyTypeClass(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return UBlackboardKeyType_Bool::StaticClass();
        }
        const FString Lower = Token.ToLower();
        if (Lower == TEXT("bool") || Lower == TEXT("boolean"))            return UBlackboardKeyType_Bool::StaticClass();
        if (Lower == TEXT("int") || Lower == TEXT("integer") || Lower == TEXT("int32")) return UBlackboardKeyType_Int::StaticClass();
        if (Lower == TEXT("float") || Lower == TEXT("real"))               return UBlackboardKeyType_Float::StaticClass();
        if (Lower == TEXT("string"))                                       return UBlackboardKeyType_String::StaticClass();
        if (Lower == TEXT("name"))                                         return UBlackboardKeyType_Name::StaticClass();
        if (Lower == TEXT("vector"))                                       return UBlackboardKeyType_Vector::StaticClass();
        if (Lower == TEXT("rotator"))                                      return UBlackboardKeyType_Rotator::StaticClass();
        if (Lower == TEXT("object"))                                       return UBlackboardKeyType_Object::StaticClass();
        if (Lower == TEXT("class"))                                        return UBlackboardKeyType_Class::StaticClass();
        if (Lower == TEXT("enum"))                                         return UBlackboardKeyType_Enum::StaticClass();
        if (Lower == TEXT("struct"))                                       return UBlackboardKeyType_Struct::StaticClass();

        // Full path or short class name fallback.
        if (Token.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadClass<UBlackboardKeyType>(nullptr, *Token))
            {
                return Loaded;
            }
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *Token))
        {
            if (Found->IsChildOf(UBlackboardKeyType::StaticClass()))
            {
                return Found;
            }
        }
        // Try the AIModule namespace prefix.
        const FString WithBackboardPrefix = FString::Printf(TEXT("/Script/AIModule.UBlackboardKeyType_%s"), *Token);
        if (UClass* Loaded = LoadClass<UBlackboardKeyType>(nullptr, *WithBackboardPrefix))
        {
            return Loaded;
        }
        return nullptr;
    }

    /** Load a UClass at a `/Script/...` path, a `/Game/...` Blueprint
     *  class path (auto-suffixed with `_C`), or a short class name. */
    UClass* ResolveAnyClass(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return nullptr;
        }
        if (Token.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *Token))
            {
                return Loaded;
            }
        }
        if (Token.StartsWith(TEXT("/Game/")))
        {
            FString WithSuffix = Token;
            if (!WithSuffix.EndsWith(TEXT("_C")))
            {
                WithSuffix += TEXT("_C");
            }
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *WithSuffix))
            {
                return Loaded;
            }
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *Token))
        {
            return Found;
        }
        // Engine fallback for short tokens like "Actor" / "Pawn".
        const FString EngineTry = FString::Printf(TEXT("/Script/Engine.%s"), *Token);
        if (UClass* Loaded = LoadClass<UObject>(nullptr, *EngineTry))
        {
            return Loaded;
        }
        return nullptr;
    }

    UEnum* ResolveEnumPath(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return nullptr;
        }
        if (Token.StartsWith(TEXT("/")))
        {
            if (UObject* Loaded = StaticLoadObject(UEnum::StaticClass(), nullptr, *Token))
            {
                return Cast<UEnum>(Loaded);
            }
            return nullptr;
        }
        return FindObject<UEnum>(nullptr, *Token);
    }

    UScriptStruct* ResolveScriptStructPath(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return nullptr;
        }
        if (Token.StartsWith(TEXT("/")))
        {
            if (UObject* Loaded = StaticLoadObject(UScriptStruct::StaticClass(), nullptr, *Token))
            {
                return Cast<UScriptStruct>(Loaded);
            }
            return nullptr;
        }
        return FindObject<UScriptStruct>(nullptr, *Token);
    }

    void SaveBlackboardIfRequested(UBlackboardData* BBData, bool bSave)
    {
        if (!BBData) return;
        BBData->MarkPackageDirty();
        if (bSave)
        {
            UEditorAssetLibrary::SaveAsset(BBData->GetPathName(), /*bOnlyIfIsDirty=*/false);
        }
    }
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleSetBlackboard(const TSharedPtr<FJsonObject>& Params)
{
    FString TreeError;
    UBehaviorTree* Tree = LoadTargetTree(Params, TreeError);
    if (!Tree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TreeError);
    }

    bool bClear = false;
    Params->TryGetBoolField(TEXT("clear"), bClear);

    UBlackboardData* NewBlackboard = nullptr;
    FString BlackboardInput;
    if (!bClear)
    {
        if (!Params->TryGetStringField(TEXT("blackboard"), BlackboardInput)
            && !Params->TryGetStringField(TEXT("blackboard_path"), BlackboardInput))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'blackboard' parameter (or pass 'clear': true to unbind the slot)"));
        }
        if (BlackboardInput.IsEmpty())
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("'blackboard' must not be empty"));
        }
        UObject* Resolved = nullptr;
        if (BlackboardInput.StartsWith(TEXT("/")))
        {
            Resolved = UEditorAssetLibrary::LoadAsset(BlackboardInput);
        }
        else
        {
            FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
            TArray<FAssetData> Found;
            AssetRegistry.Get().GetAssetsByClass(UBlackboardData::StaticClass()->GetClassPathName(), Found);
            for (const FAssetData& Data : Found)
            {
                if (Data.AssetName.ToString().Equals(BlackboardInput, ESearchCase::IgnoreCase))
                {
                    Resolved = Data.GetAsset();
                    break;
                }
            }
        }
        NewBlackboard = Cast<UBlackboardData>(Resolved);
        if (!NewBlackboard)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Asset '%s' is not a UBlackboardData"), *BlackboardInput));
        }
    }

    Tree->BlackboardAsset = NewBlackboard;

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    Tree->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Tree->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("set_blackboard"));
    Result->SetStringField(TEXT("tree"), Tree->GetPathName());
    if (NewBlackboard)
    {
        Result->SetStringField(TEXT("blackboard_path"), NewBlackboard->GetPathName());
        Result->SetStringField(TEXT("blackboard_name"), NewBlackboard->GetName());
        Result->SetBoolField(TEXT("cleared"), false);
    }
    else
    {
        Result->SetBoolField(TEXT("cleared"), true);
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleAddBlackboardKey(const TSharedPtr<FJsonObject>& Params)
{
    FString ResolveError;
    UBlackboardData* BBData = ResolveBlackboardArg(Params, ResolveError);
    if (!BBData)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ResolveError);
    }

    FString KeyNameStr;
    if (!Params->TryGetStringField(TEXT("key_name"), KeyNameStr)
        && !Params->TryGetStringField(TEXT("name"), KeyNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'key_name' parameter"));
    }
    if (KeyNameStr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'key_name' must not be empty"));
    }
    const FName KeyName(*KeyNameStr);

    // Reject duplicates against this asset's own keys (parent chain
    // dupes are merged at runtime through UpdatePersistentKey, but we
    // surface the local conflict explicitly).
    for (const FBlackboardEntry& Existing : BBData->Keys)
    {
        if (Existing.EntryName == KeyName)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Blackboard '%s' already has a key '%s'"),
                    *BBData->GetName(), *KeyNameStr));
        }
    }

    FString KeyClassToken;
    if (!Params->TryGetStringField(TEXT("key_class"), KeyClassToken)
        && !Params->TryGetStringField(TEXT("class"), KeyClassToken)
        && !Params->TryGetStringField(TEXT("type"), KeyClassToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'key_class' parameter (bool / int / float / string / name / vector / rotator / object / class / enum / struct)"));
    }
    UClass* KeyTypeClass = ResolveBlackboardKeyTypeClass(KeyClassToken);
    if (!KeyTypeClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UBlackboardKeyType subclass '%s'"), *KeyClassToken));
    }
    if (KeyTypeClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is abstract"), *KeyTypeClass->GetPathName()));
    }

    // NewObject the typed key with the Blackboard as outer so it travels
    // with the asset on save.
    UBlackboardKeyType* NewKey = NewObject<UBlackboardKeyType>(BBData, KeyTypeClass, NAME_None, RF_Transactional);
    if (!NewKey)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to NewObject UBlackboardKeyType of class '%s'"), *KeyTypeClass->GetName()));
    }

    // Wire the inner type fields when the caller supplied a hint.
    FString BaseClassToken;
    if (Params->TryGetStringField(TEXT("base_class"), BaseClassToken)
        || Params->TryGetStringField(TEXT("base_class_path"), BaseClassToken))
    {
        if (UBlackboardKeyType_Object* AsObj = Cast<UBlackboardKeyType_Object>(NewKey))
        {
            if (UClass* BaseClass = ResolveAnyClass(BaseClassToken))
            {
                AsObj->BaseClass = BaseClass;
            }
        }
        else if (UBlackboardKeyType_Class* AsCls = Cast<UBlackboardKeyType_Class>(NewKey))
        {
            if (UClass* BaseClass = ResolveAnyClass(BaseClassToken))
            {
                AsCls->BaseClass = BaseClass;
            }
        }
    }
    FString EnumToken;
    if (Params->TryGetStringField(TEXT("enum_path"), EnumToken)
        || Params->TryGetStringField(TEXT("enum"), EnumToken))
    {
        if (UBlackboardKeyType_Enum* AsEnum = Cast<UBlackboardKeyType_Enum>(NewKey))
        {
            if (UEnum* EnumObj = ResolveEnumPath(EnumToken))
            {
                AsEnum->EnumType = EnumObj;
            }
        }
    }
    FString StructToken;
    if (Params->TryGetStringField(TEXT("struct_path"), StructToken)
        || Params->TryGetStringField(TEXT("struct"), StructToken))
    {
        if (UBlackboardKeyType_Struct* AsStruct = Cast<UBlackboardKeyType_Struct>(NewKey))
        {
            if (UScriptStruct* Struct = ResolveScriptStructPath(StructToken))
            {
                AsStruct->DefaultValue.InitializeAs(Struct);
            }
        }
    }

    FBlackboardEntry NewEntry;
    NewEntry.EntryName = KeyName;
    NewEntry.KeyType = NewKey;

    bool bInstanceSynced = false;
    if (Params->TryGetBoolField(TEXT("instance_synced"), bInstanceSynced))
    {
        NewEntry.bInstanceSynced = bInstanceSynced ? 1 : 0;
    }
#if WITH_EDITORONLY_DATA
    FString DescriptionStr;
    if (Params->TryGetStringField(TEXT("description"), DescriptionStr))
    {
        NewEntry.EntryDescription = DescriptionStr;
    }
    FString CategoryStr;
    if (Params->TryGetStringField(TEXT("category"), CategoryStr))
    {
        NewEntry.EntryCategory = FName(*CategoryStr);
    }
#endif
    BBData->Keys.Add(NewEntry);
    BBData->UpdateIfHasSynchronizedKeys();
    BBData->UpdateKeyIDs();
    BBData->PropagateKeyChangesToDerivedBlackboardAssets();

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    SaveBlackboardIfRequested(BBData, bSave);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_blackboard_key"));
    Result->SetStringField(TEXT("blackboard"), BBData->GetPathName());
    Result->SetStringField(TEXT("key_name"), KeyName.ToString());
    Result->SetStringField(TEXT("key_class"), KeyTypeClass->GetName());
    Result->SetStringField(TEXT("key_class_path"), KeyTypeClass->GetPathName());
    Result->SetNumberField(TEXT("key_index"), BBData->Keys.Num() - 1);
    Result->SetNumberField(TEXT("key_count"), BBData->Keys.Num());
    if (bInstanceSynced) Result->SetBoolField(TEXT("instance_synced"), true);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleRemoveBlackboardKey(const TSharedPtr<FJsonObject>& Params)
{
    FString ResolveError;
    UBlackboardData* BBData = ResolveBlackboardArg(Params, ResolveError);
    if (!BBData)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ResolveError);
    }

    FString KeyNameStr;
    if (!Params->TryGetStringField(TEXT("key_name"), KeyNameStr)
        && !Params->TryGetStringField(TEXT("name"), KeyNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'key_name' parameter"));
    }
    if (KeyNameStr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'key_name' must not be empty"));
    }
    const FName KeyName(*KeyNameStr);

    int32 RemoveIndex = INDEX_NONE;
    for (int32 Idx = 0; Idx < BBData->Keys.Num(); ++Idx)
    {
        if (BBData->Keys[Idx].EntryName == KeyName)
        {
            RemoveIndex = Idx;
            break;
        }
    }
    if (RemoveIndex == INDEX_NONE)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blackboard '%s' has no own key '%s'"),
                *BBData->GetName(), *KeyNameStr));
    }
    BBData->Keys.RemoveAt(RemoveIndex);
    BBData->UpdateIfHasSynchronizedKeys();
    BBData->UpdateKeyIDs();
    BBData->PropagateKeyChangesToDerivedBlackboardAssets();

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    SaveBlackboardIfRequested(BBData, bSave);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("remove_blackboard_key"));
    Result->SetStringField(TEXT("blackboard"), BBData->GetPathName());
    Result->SetStringField(TEXT("key_name"), KeyName.ToString());
    Result->SetNumberField(TEXT("removed_index"), RemoveIndex);
    Result->SetNumberField(TEXT("key_count"), BBData->Keys.Num());
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBehaviorTreeCommands::HandleRenameBlackboardKey(const TSharedPtr<FJsonObject>& Params)
{
    // Walks UBlackboardData::Keys, mutates the matching
    // FBlackboardEntry::EntryName, and then propagates the new name
    // to every UBTNode subobject that holds a FBlackboardKeySelector
    // referencing the old name. The propagation walks each Hard-
    // referencer package of the Blackboard, loads any asset that
    // implements IBlackboardAssetProvider and whose GetBlackboardAsset
    // matches the target, and updates each FStructProperty of the
    // FBlackboardKeySelector type whose SelectedKeyName equals the
    // old name. The walk matches the BT editor's
    // SBehaviorTreeBlackboardView::UpdateExternalBlackboardKeyReferences
    // path, re-implemented against the public AIModule + AssetRegistry
    // API so we do not pull the editor-only BehaviorTreeEditor module.
    FString ResolveError;
    UBlackboardData* BBData = ResolveBlackboardArg(Params, ResolveError);
    if (!BBData)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ResolveError);
    }

    FString OldNameStr;
    if (!Params->TryGetStringField(TEXT("old_name"), OldNameStr)
        && !Params->TryGetStringField(TEXT("old_key_name"), OldNameStr)
        && !Params->TryGetStringField(TEXT("from"), OldNameStr)
        && !Params->TryGetStringField(TEXT("key_name"), OldNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'old_name' parameter (the existing key name to rename)"));
    }
    FString NewNameStr;
    if (!Params->TryGetStringField(TEXT("new_name"), NewNameStr)
        && !Params->TryGetStringField(TEXT("new_key_name"), NewNameStr)
        && !Params->TryGetStringField(TEXT("to"), NewNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'new_name' parameter (the new FName to land on the entry)"));
    }
    if (OldNameStr.IsEmpty() || NewNameStr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("'old_name' and 'new_name' must both be non-empty"));
    }
    if (NewNameStr.Len() >= NAME_SIZE)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'new_name' too long (max %d chars)"), NAME_SIZE));
    }
    const FName OldName(*OldNameStr);
    const FName NewName(*NewNameStr);
    if (OldName == NewName)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("'old_name' and 'new_name' are identical; nothing to do"));
    }

    // Refuse duplicate names (own keys or parent-inherited keys).
    // Matches the editor's OnNameTextVerifyChanged guard.
    int32 RenameIndex = INDEX_NONE;
    for (int32 Idx = 0; Idx < BBData->Keys.Num(); ++Idx)
    {
        if (BBData->Keys[Idx].EntryName == OldName)
        {
            RenameIndex = Idx;
        }
        else if (BBData->Keys[Idx].EntryName == NewName)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Blackboard '%s' already has a key named '%s'"),
                    *BBData->GetName(), *NewNameStr));
        }
    }
    if (RenameIndex == INDEX_NONE)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blackboard '%s' has no own key '%s' to rename"),
                *BBData->GetName(), *OldNameStr));
    }
    for (const FBlackboardEntry& ParentKey : BBData->ParentKeys)
    {
        if (ParentKey.EntryName == NewName)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Blackboard '%s' inherits a parent key '%s'; rename refused"),
                    *BBData->GetName(), *NewNameStr));
        }
    }

    // Land the new name on the entry. The asset's PreEditChange /
    // PostEditChangeChainProperty pair refreshes the editor cache and
    // fires the FBlackboardDataChanged broadcast that key pickers
    // listen on; without that, BT pickers in an already-open BT
    // editor keep painting the old name.
    BBData->Modify();
    BBData->Keys[RenameIndex].EntryName = NewName;

#if WITH_EDITOR
    FProperty* KeysArrayProperty = FindFProperty<FProperty>(
        UBlackboardData::StaticClass(), GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
    FProperty* NameProperty = FindFProperty<FProperty>(
        FBlackboardEntry::StaticStruct(), GET_MEMBER_NAME_CHECKED(FBlackboardEntry, EntryName));
    if (KeysArrayProperty && NameProperty)
    {
        FEditPropertyChain PropertyChain;
        PropertyChain.AddHead(KeysArrayProperty);
        PropertyChain.AddTail(NameProperty);
        PropertyChain.SetActiveMemberPropertyNode(KeysArrayProperty);
        PropertyChain.SetActivePropertyNode(NameProperty);
        BBData->PreEditChange(PropertyChain);

        FPropertyChangedEvent PropertyChangedEvent(NameProperty, EPropertyChangeType::ValueSet);
        FPropertyChangedChainEvent PropertyChangedChainEvent(PropertyChain, PropertyChangedEvent);
        BBData->PostEditChangeChainProperty(PropertyChangedChainEvent);
    }
#endif

    // Per-BB house-keeping (matches the BT editor's flow after a key
    // mutation): cached IDs refresh + derived-BB asset chain stays
    // consistent so subclasses of this BB pick the new name up.
    BBData->UpdateIfHasSynchronizedKeys();
    BBData->UpdateKeyIDs();
    BBData->PropagateKeyChangesToDerivedBlackboardAssets();

    // Propagation walk: find every asset that Hard-references this
    // Blackboard package, load each one, and update any
    // FBlackboardKeySelector subobject that points at the old name.
    TArray<TSharedPtr<FJsonValue>> UpdatedAssetsJson;
    int32 TotalSelectorRewrites = 0;

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
    TArray<FName> ReferencerPackages;
    {
        UE::AssetRegistry::FDependencyQuery HardOnly;
        HardOnly.Required = UE::AssetRegistry::EDependencyProperty::Hard;
        AssetRegistry.GetReferencers(
            BBData->GetOutermost()->GetFName(),
            ReferencerPackages,
            UE::AssetRegistry::EDependencyCategory::Package,
            HardOnly);
    }

    // Pre-compute the set of UClass entries that implement
    // IBlackboardAssetProvider so we skip assets that cannot host a
    // BB selector. Matches the editor walk.
    TArray<const UClass*> BlackboardOwnerClasses;
    for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
    {
        UClass* Class = *ClassIt;
        if (Class && Class->ImplementsInterface(UBlackboardAssetProvider::StaticClass()))
        {
            BlackboardOwnerClasses.Add(Class);
        }
    }

    TSet<UObject*> AssetsToUpdate;
    for (const FName& ReferencerPackage : ReferencerPackages)
    {
        TArray<FAssetData> Assets;
        AssetRegistry.GetAssetsByPackageName(ReferencerPackage, Assets);
        for (const FAssetData& Asset : Assets)
        {
            if (BlackboardOwnerClasses.Find(Asset.GetClass()) == INDEX_NONE)
            {
                continue;
            }
            UObject* AssetObject = Asset.GetAsset();
            if (!AssetObject)
            {
                continue;
            }
            const IBlackboardAssetProvider* Provider = Cast<const IBlackboardAssetProvider>(AssetObject);
            if (Provider && Provider->GetBlackboardAsset() == BBData)
            {
                AssetsToUpdate.Add(AssetObject);
            }
        }
    }

    // For each candidate asset, walk every subobject under that
    // package and rewrite any FBlackboardKeySelector that selects the
    // old name. We compare against `FBlackboardKeySelector` via the
    // FStructProperty's `Struct` pointer (faster than the editor's
    // GetCPPType-substring check and immune to namespace renames).
    UScriptStruct* SelectorStruct = FBlackboardKeySelector::StaticStruct();
    for (UObject* Asset : AssetsToUpdate)
    {
        TArray<UObject*> Objects;
        GetObjectsWithOuter(Asset->GetOutermost(), Objects);
        int32 PerAssetRewrites = 0;
        for (UObject* SubObject : Objects)
        {
            if (!SubObject) continue;
            for (TFieldIterator<FStructProperty> It(SubObject->GetClass()); It; ++It)
            {
                if (It->Struct != SelectorStruct)
                {
                    continue;
                }
                FBlackboardKeySelector* SelectorPtr =
                    It->ContainerPtrToValuePtr<FBlackboardKeySelector>(SubObject);
                if (SelectorPtr && SelectorPtr->SelectedKeyName == OldName)
                {
                    SubObject->Modify();
                    SelectorPtr->SelectedKeyName = NewName;
                    ++PerAssetRewrites;
                    ++TotalSelectorRewrites;
                }
            }
        }
        if (PerAssetRewrites > 0)
        {
            Asset->MarkPackageDirty();
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("path"), Asset->GetPathName());
            Row->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
            Row->SetNumberField(TEXT("selector_rewrites"), PerAssetRewrites);
            UpdatedAssetsJson.Add(MakeShared<FJsonValueObject>(Row));
        }
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    SaveBlackboardIfRequested(BBData, bSave);
    if (bSave)
    {
        // Save the referencers we touched so the new selector value
        // sticks to disk.
        for (UObject* Asset : AssetsToUpdate)
        {
            if (Asset && Asset->GetOutermost()->IsDirty())
            {
                UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("rename_blackboard_key"));
    Result->SetStringField(TEXT("blackboard"), BBData->GetPathName());
    Result->SetStringField(TEXT("old_name"), OldName.ToString());
    Result->SetStringField(TEXT("new_name"), NewName.ToString());
    Result->SetNumberField(TEXT("renamed_index"), RenameIndex);
    Result->SetNumberField(TEXT("referencer_packages_scanned"), ReferencerPackages.Num());
    Result->SetNumberField(TEXT("referencer_assets_updated"), UpdatedAssetsJson.Num());
    Result->SetNumberField(TEXT("selector_rewrites"), TotalSelectorRewrites);
    Result->SetArrayField(TEXT("updated_assets"), UpdatedAssetsJson);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}
