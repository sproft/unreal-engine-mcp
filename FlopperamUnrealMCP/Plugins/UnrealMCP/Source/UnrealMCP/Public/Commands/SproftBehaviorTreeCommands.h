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
 *   - `add_service`: append a UBTService to a composite's service
 *     chain. Targets the composite by `GetNodeName()`. Optionally
 *     takes a flat property dict applied through `FProperty::ImportText`
 *     on the new service.
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
    TSharedPtr<FJsonObject> HandleAddService(const TSharedPtr<FJsonObject>& Params);
};
