#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: behavior_tree (read-only slice)
 *
 * Read-only structured dump of a UBehaviorTree asset. Returns the tree
 * structure (composite root + recursive children + decorators +
 * services), the linked Blackboard data asset path, and the Blackboard's
 * key list (each name + type + sync flag + parent-inherited flag).
 *
 * The edit side (re-rooting, decorator insertion, blackboard key edits)
 * lives on the backlog. This slice covers the "what does this BT look
 * like" question that pairs with `niagara_inspect` for AI assets.
 *
 * Inputs:
 *   - tree:                short asset name or full `/Game/...` path of
 *                          a UBehaviorTree.
 *   - include_blackboard:  include the Blackboard key list. Default true.
 *   - include_decorators:  include each composite-child's decorator
 *                          chain. Default true.
 *   - include_services:    include each composite's service chain.
 *                          Default true.
 *   - max_depth:           how deep the tree walk recurses. Default 32
 *                          which is well above any production tree.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UBehaviorTree::RootNode / RootDecorators / BlackboardAsset.
 *   - UBTCompositeNode::Children / Services /
 *     FBTCompositeChild::ChildComposite / ChildTask / Decorators.
 *   - UBTNode::GetNodeName / GetExecutionIndex / GetTreeDepth.
 *   - UBlackboardData::Keys / FBlackboardEntry::EntryName /
 *     KeyType / bInstanceSynced.
 *   - UBTComposite_SimpleParallel::FinishMode for the parallel-mode
 *     composite-specific field.
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
};
