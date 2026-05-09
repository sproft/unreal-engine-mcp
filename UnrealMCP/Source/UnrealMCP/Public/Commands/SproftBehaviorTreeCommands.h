#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: behavior_tree (read + small edit slice)
 *
 * Multi-op tool keyed by `op`:
 *   - `inspect` (default): read-only structured dump of a
 *     UBehaviorTree asset. Returns the tree structure (composite
 *     root + recursive children + decorators + services), the linked
 *     Blackboard data asset path, and the Blackboard's key list.
 *   - `create_behavior_tree`: create a new UBehaviorTree asset at a
 *     `/Game/...` package path with an optional linked Blackboard.
 *   - `add_root_composite`: assign a `Selector` / `Sequence` /
 *     `SimpleParallel` composite as the tree's RootNode. Used right
 *     after `create_behavior_tree` to land a usable empty tree.
 *
 * Edit-slice future work (append child task / composite, insert
 * decorator, append service, blackboard key edits) stays on the
 * backlog.
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
 * Clean-room implementation derived from the public UE5 API:
 *   - UBehaviorTree::RootNode / RootDecorators / BlackboardAsset.
 *   - UBTCompositeNode::Children / Services /
 *     FBTCompositeChild::ChildComposite / ChildTask / Decorators.
 *   - UBTComposite_Selector / UBTComposite_Sequence /
 *     UBTComposite_SimpleParallel for the small set of root-composite
 *     classes the small variant ships.
 *   - UBTComposite_SimpleParallel::FinishMode for the parallel-mode
 *     composite-specific field.
 *   - UBlackboardData::Keys / FBlackboardEntry::EntryName /
 *     KeyType / bInstanceSynced for the inspect side.
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
};
