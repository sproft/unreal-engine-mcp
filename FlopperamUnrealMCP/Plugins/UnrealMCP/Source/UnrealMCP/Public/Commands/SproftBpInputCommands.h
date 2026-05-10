#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_input
 *
 * Manage Enhanced Input data assets and wire Enhanced Input event nodes into
 * Blueprint event graphs.
 *
 * Operations:
 *   - "create_input_action": create a UInputAction asset with a chosen
 *      EInputActionValueType (Boolean, Axis1D, Axis2D, Axis3D).
 *   - "create_input_mapping_context": create an empty UInputMappingContext.
 *   - "add_mapping": append one FEnhancedActionKeyMapping (action + key) to
 *      an existing IMC's default key-mapping list.
 *   - "add_action_event_node": spawn a UK2Node_EnhancedInputAction event
 *      node in a target Blueprint's event graph for a given UInputAction
 *      asset, and optionally MakeLinkTo from a chosen exec pin (default
 *      "Triggered") to a named function call on the same Blueprint.
 *   - "add_action_modifier": append a UInputModifier subobject to a
 *      mapping row's `Modifiers` array. The mapping row is found by
 *      `(action, key)` pair; the modifier class resolves through a short
 *      token (`negate` / `scalar` / `dead_zone` / `swizzle_axis`) or a
 *      full UInputModifier subclass path. An optional flat property dict
 *      lands on the new modifier instance through
 *      `FProperty::ImportText_InContainer`, so callers can write `Order`
 *      on a swizzle, `Scalar` on a scalar, `LowerThreshold` on a dead
 *      zone, etc., in the same call.
 *   - "add_action_trigger": append a UInputTrigger subobject to a
 *      mapping row's `Triggers` array. The mapping row is found by
 *      `(action, key)` pair (same shape as `add_action_modifier`);
 *      the trigger class resolves through a short token (`pressed`
 *      / `released` / `hold` / `hold_and_release` / `tap` / `pulse`
 *      / `chord` / `chord_action` / `down` / `repeated_tap` /
 *      `combo`) or a full UInputTrigger subclass path. An optional
 *      flat property dict applies through
 *      `FProperty::ImportText_InContainer`, so callers can write
 *      `HoldTimeThreshold` on a hold trigger, `TapReleaseTimeThreshold`
 *      on a tap trigger, `ChordAction` on a chord trigger, etc., in
 *      the same call.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UInputAction (UDataAsset subclass, EnhancedInput plugin)
 *   - UInputMappingContext::MapKey (the documented public binding entry point)
 *   - UInputMappingContext::GetMapping (non-const accessor for in-place
 *     `Modifiers` array writes; UnmapKey-then-rebuild is what the editor
 *     uses for full mapping replacement, but the FEnhancedActionKeyMapping
 *     surface is reflected so we can also attach modifiers in place)
 *   - UInputModifierNegate / UInputModifierScalar / UInputModifierDeadZone
 *     / UInputModifierSwizzleAxis (canonical modifier subclasses)
 *   - UK2Node_EnhancedInputAction (InputBlueprintNodes module)
 *   - UK2Node_CallFunction::SetFromFunction for the function-call follow-on
 *   - FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified +
 *     FKismetEditorUtilities::CompileBlueprint for the post-edit save path
 *   - FAssetRegistryModule::AssetCreated for content-browser registration
 *   - UEditorAssetLibrary for save / existence checks
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpInputCommands
{
public:
    FSproftBpInputCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpInput(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> CreateInputAction(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> CreateInputMappingContext(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddMapping(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddActionEventNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddActionModifier(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddActionTrigger(const TSharedPtr<FJsonObject>& Params);
};
