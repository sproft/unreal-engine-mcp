#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: behavior_tree (read + edit slice)
 *
 * Multi-op tool keyed by `op`:
 *   - `inspect` (default): read-only structured dump of a
 *     UBehaviorTree asset.
 *   - `create_behavior_tree`: create a new UBehaviorTree asset at a
 *     `/Game/...` package path with an optional linked Blackboard.
 *   - `add_root_composite`: assign a `Selector` / `Sequence` /
 *     `SimpleParallel` composite as the tree's RootNode.
 *   - `add_child_task`: append a UBTTaskNode (or any UBTNode subclass
 *     including a nested composite) as a child slot under a named
 *     composite. Resolves the parent by `GetNodeName()` substring
 *     match through a recursive walk from `RootNode`. The new node
 *     is outered to the tree asset and wired through
 *     `InitializeFromAsset`; its FBTCompositeChild slot is appended
 *     to `Children`.
 *   - `add_decorator`: append a UBTDecorator to a composite child's
 *     decorator chain. Targets a child slot by the child's
 *     `GetNodeName()` (composite walk). Optionally takes a flat
 *     property dict applied through `FProperty::ImportText` on the
 *     new decorator.
 *   - `set_root_decorator` (aliases `add_root_decorator`): append a
 *     UBTDecorator to the tree-level `RootDecorators` array on the
 *     UBehaviorTree asset itself (the same chain the BT editor
 *     surfaces when the user right-clicks the root composite and
 *     chooses "Add Decorator"). The decorator outers under the tree
 *     and wires through `InitializeFromAsset`; the class must derive
 *     from `UBTDecorator`. Optionally takes a flat property dict
 *     applied through `FProperty::ImportText` on the new decorator.
 *   - `add_blackboard_decorator`: declarative one-call shortcut for
 *     the Blackboard decorator the editor's add-decorator picker
 *     spawns most often. Spawns a UBTDecorator_Blackboard under a
 *     target child slot, wires the FBlackboardKeySelector against a
 *     named Blackboard key, picks the right operation family
 *     (Basic / Arithmetic / Text) from the resolved key's type, and
 *     writes the matching EBasicKeyOperation / EArithmeticKeyOperation
 *     / ETextKeyOperation row plus the comparison payload field
 *     (IntValue / FloatValue / StringValue) through reflection so
 *     the protected UPROPERTY surface lands without us touching
 *     engine private headers. Conditions: `IsSet` / `IsNotSet`
 *     (Basic family) or `IsEqualTo` / `IsNotEqualTo` (Arithmetic
 *     family for int / float / bool / enum keys; Text family for
 *     FName / FString keys). Returns the resolved key + classified
 *     family + decorator FName so a follow-up call can address the
 *     spawned decorator.
 *   - `add_service`: append a UBTService to a composite's service
 *     chain. Targets the composite by `GetNodeName()`. Optionally
 *     takes a flat property dict applied through `FProperty::ImportText`
 *     on the new service.
 *   - `set_blackboard`: rebind the tree's Blackboard asset. Accepts a
 *     `/Game/...` UBlackboardData path or short asset name. Pass
 *     `clear=true` to unbind the asset.
 *   - `add_blackboard_key`: append a key to a target Blackboard's
 *     `Keys` array. Resolves the key class from the short token
 *     (`bool` / `int` / `float` / `string` / `name` / `vector` /
 *     `rotator` / `object` / `class` / `enum` / `struct`), instantiates
 *     a UBlackboardKeyType subclass outered to the Blackboard, and
 *     wires inner type fields (BaseClass on Object / Class keys,
 *     EnumType on Enum keys, DefaultValue.GetScriptStruct() on Struct
 *     keys) when the caller passes the corresponding parameter.
 *   - `remove_blackboard_key`: remove a key from a target
 *     Blackboard's `Keys` array by FName.
 *   - `rename_blackboard_key`: rename a key on a target Blackboard
 *     by FName. Walks `UBlackboardData::Keys`, mutates the matching
 *     `FBlackboardEntry::EntryName`, and then runs the documented
 *     key-rename propagation so every UBTNode subobject that holds a
 *     `FBlackboardKeySelector` referencing the old key name lands on
 *     the new key name. The walk pulls each Hard-referencer package
 *     of the Blackboard through `IAssetRegistry::GetReferencers`,
 *     loads any asset that implements `IBlackboardAssetProvider` and
 *     whose `GetBlackboardAsset()` matches the target Blackboard,
 *     iterates every subobject under that package, and updates each
 *     `FStructProperty` of the FBlackboardKeySelector type whose
 *     `SelectedKeyName` equals the old name. Refuses duplicate
 *     names (own keys or parent-inherited keys) and refuses an
 *     empty new name. `UpdateIfHasSynchronizedKeys` + `UpdateKeyIDs`
 *     + `PropagateKeyChangesToDerivedBlackboardAssets` fix-up runs
 *     after the rename so derived Blackboards stay consistent.
 *
 * Inputs (inspect):
 *   - tree:                short asset name or full `/Game/...` path
 *                          of a UBehaviorTree.
 *   - include_blackboard:  include the Blackboard key list. Default
 *                          true.
 *   - include_decorators:  include each composite-child's decorator
 *                          chain. Default true.
 *   - include_services:    include each composite's service chain.
 *                          Default true.
 *   - max_depth:           how deep the tree walk recurses. Default
 *                          32.
 *
 * Inputs (create_behavior_tree):
 *   - tree:                target `/Game/...` package path. Required.
 *   - blackboard:          optional `/Game/...` UBlackboardData path
 *                          to link as the new tree's BlackboardAsset.
 *   - overwrite:           replace an existing asset at the path.
 *                          Default false.
 *   - save:                save the new package after creation.
 *                          Default true.
 *
 * Inputs (add_root_composite):
 *   - tree:                short asset name or full `/Game/...` path
 *                          of a UBehaviorTree.
 *   - composite_class:     one of `selector` / `sequence` /
 *                          `simple_parallel` (case-insensitive). The
 *                          full UClass path is also accepted for any
 *                          UBTCompositeNode subclass.
 *   - replace:             replace an existing RootNode. Default
 *                          false; the call errors out if a RootNode
 *                          is already present and `replace=false`.
 *   - save:                save the asset after the edit. Default
 *                          true.
 *
 * Inputs (add_child_task):
 *   - tree:                BT asset path / short name. Required.
 *   - parent:              parent composite by `GetNodeName()` (case-
 *                          insensitive substring) or the sentinel
 *                          token `root` to target `RootNode`. Default
 *                          `root`.
 *   - task_class:          UClass for the child node. Accepts a short
 *                          token (e.g. `wait`, `move_to`, `selector`,
 *                          `sequence`) plus `/Script/Module.ClassName`
 *                          and `/Game/...` Blueprint paths. Required.
 *   - node_name:           optional friendly name written to
 *                          `UBTNode::NodeName`. Defaults to the class
 *                          display name.
 *   - save:                save the asset after the edit. Default
 *                          true.
 *
 * Inputs (add_decorator):
 *   - tree:                BT asset path / short name. Required.
 *   - target:              child node by `GetNodeName()` substring.
 *                          Required (decorators attach to a composite
 *                          child slot, not the composite itself).
 *   - decorator_class:     UClass for the decorator. Short token
 *                          (e.g. `blackboard`, `cooldown`, `loop`)
 *                          plus full path forms. Required.
 *   - properties:          optional flat property dict applied
 *                          through `FProperty::ImportText` on the new
 *                          decorator.
 *   - save:                save the asset after the edit. Default
 *                          true.
 *
 * Inputs (set_root_decorator):
 *   - tree:                BT asset path / short name. Required.
 *   - decorator_class:     UClass for the decorator. Short token
 *                          (e.g. `blackboard`, `cooldown`, `loop`)
 *                          plus full path forms. Required.
 *   - properties:          optional flat property dict applied
 *                          through `FProperty::ImportText` on the new
 *                          decorator.
 *   - save:                save the asset after the edit. Default
 *                          true.
 *
 * Inputs (add_service):
 *   - tree:                BT asset path / short name. Required.
 *   - target:              composite or task by `GetNodeName()` substring.
 *                          Sentinel `root` targets `RootNode`. Default
 *                          `root`.
 *   - service_class:       UClass for the service. Short token plus
 *                          full path forms. Required.
 *   - properties:          optional flat property dict applied
 *                          through `FProperty::ImportText` on the new
 *                          service.
 *   - save:                save the asset after the edit. Default
 *                          true.
 *
 * Inputs (set_blackboard):
 *   - tree:                BT asset path / short name. Required.
 *   - blackboard:          `/Game/...` UBlackboardData path or short
 *                          asset name. Required unless `clear=true`.
 *   - clear:               unbind the BlackboardAsset slot. Default
 *                          false.
 *   - save:                save the asset after the edit. Default
 *                          true.
 *
 * Inputs (add_blackboard_key):
 *   - blackboard:          `/Game/...` UBlackboardData path or short
 *                          asset name. Required. (When `tree` is set
 *                          but `blackboard` is omitted, the BT's
 *                          BlackboardAsset is used.)
 *   - tree:                optional BT asset path / short name. When
 *                          set without `blackboard`, the BT's
 *                          BlackboardAsset is targeted.
 *   - key_name:            new key's FName. Required.
 *   - key_class:           short type token. One of `bool` / `int` /
 *                          `float` / `string` / `name` / `vector` /
 *                          `rotator` / `object` / `class` / `enum` /
 *                          `struct`. Full `/Script/AIModule.UBlackboardKeyType_X`
 *                          paths and the short class names also
 *                          work.
 *   - base_class:          optional `/Script/...` or short class
 *                          name applied to UBlackboardKeyType_Object
 *                          / UBlackboardKeyType_Class as `BaseClass`.
 *   - enum_path:           optional `/Script/...` or `/Game/...`
 *                          UEnum path applied to UBlackboardKeyType_Enum
 *                          as `EnumType`.
 *   - struct_path:         optional `/Script/...` or `/Game/...`
 *                          UScriptStruct path. Wires the
 *                          UBlackboardKeyType_Struct's
 *                          `DefaultValue.InitializeAs(Struct)`.
 *   - instance_synced:     optional bool. Sets the entry's
 *                          `bInstanceSynced` flag.
 *   - description:         optional string (editor-only).
 *   - category:            optional FName (editor-only).
 *   - save:                save the Blackboard asset after the edit.
 *                          Default true.
 *
 * Inputs (remove_blackboard_key):
 *   - blackboard:          UBlackboardData path or short name (or
 *                          omit when `tree` is set, same fallback).
 *   - tree:                optional BT asset path / short name.
 *   - key_name:            FName of the key to remove. Required.
 *   - save:                save the Blackboard asset after the edit.
 *                          Default true.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UBehaviorTree::RootNode / RootDecorators / BlackboardAsset.
 *   - UBTCompositeNode::Children / Services /
 *     FBTCompositeChild::ChildComposite / ChildTask / Decorators.
 *   - UBTNode::InitializeFromAsset for the child / decorator / service
 *     wiring on append.
 *   - UBTComposite_Selector / UBTComposite_Sequence /
 *     UBTComposite_SimpleParallel for the small set of root-composite
 *     classes the small variant ships.
 *   - UBlackboardData::Keys / FBlackboardEntry for the inspect side.
 *   - CreatePackage / NewObject / FAssetRegistryModule::AssetCreated /
 *     UEditorAssetLibrary::SaveAsset for the create / save side.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBehaviorTreeCommands
{
public:
    FSproftBehaviorTreeCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBehaviorTree(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreateBehaviorTree(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddRootComposite(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddChildTask(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddDecorator(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetRootDecorator(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlackboardDecorator(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddService(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetBlackboard(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlackboardKey(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveBlackboardKey(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRenameBlackboardKey(const TSharedPtr<FJsonObject>& Params);
};
