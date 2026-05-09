#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_function_create
 *
 * Declarative one-call wrapper around `FBlueprintEditorUtils::CreateNewGraph`
 * + `FBlueprintEditorUtils::AddFunctionGraph` plus typed input / output
 * pin creation on the FunctionEntry / FunctionResult nodes. Sits on top
 * of the existing per-call `create_function` + `add_function_input` +
 * `add_function_output` helpers in the local repo; what this tool adds
 * is laying the function down with its full signature in a single round
 * trip.
 *
 * Operation: single op (`create_function`, default).
 *
 * Inputs:
 *   - `blueprint`: short asset name or full `/Game/...` Blueprint path.
 *   - `function_name`: FName for the new graph. Validated against the
 *     same character class as the existing `create_function` op.
 *   - `inputs`: optional array of `{name, type}` entries for input pins
 *     placed on the FunctionEntry node. Each entry may also include
 *     `is_array` (bool) and `is_reference` (bool, sets `bIsReference`
 *     on the pin type).
 *   - `outputs`: optional array of `{name, type}` entries for output
 *     pins placed on the FunctionResult node. Same extra fields as
 *     `inputs`.
 *   - `pure`: optional bool; when true marks the function as Pure
 *     (`UK2Node_FunctionEntry::SetExtraFlags(FUNC_BlueprintPure)`).
 *   - `category`: optional string assigned to the function's
 *     `Category` metadata.
 *   - `keywords`: optional string assigned to the function's
 *     `Keywords` metadata.
 *   - `tooltip`: optional string assigned to the function's `tooltip`
 *     metadata.
 *   - `access_specifier`: optional `public` / `protected` / `private`
 *     mapped onto the FunctionEntry's access metadata.
 *   - `compile`, `save`: standard post-op toggles (default true).
 *
 * Type token resolver mirrors `bp_variable`: scalar tokens (bool, int,
 * int64, byte, float, double, string, name, text), built-in structs
 * (vector, vector2d, rotator, transform, color, linear_color), object
 * refs through `/Script/Module.ClassName` paths, `/Game/...` Blueprint
 * class paths (auto-suffixed with `_C`), and `struct:/...` UScriptStruct
 * paths.
 *
 * Returns:
 *   - `function_name`: the requested name.
 *   - `graph_name`: actual FName of the created `UEdGraph` (Unreal
 *     auto-suffixes when a collision occurs).
 *   - `entry_node`: FName of the `UK2Node_FunctionEntry`.
 *   - `result_node`: FName of the `UK2Node_FunctionResult`, populated
 *     iff at least one output pin was declared.
 *   - `inputs_added`: list of `{name, type, success?}` per requested
 *     input, plus failures with reason text.
 *   - `outputs_added`: same shape for outputs.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - FBlueprintEditorUtils::CreateNewGraph + AddFunctionGraph
 *   - UK2Node_FunctionEntry::CreateUserDefinedPin (input pin lives on
 *     the entry node)
 *   - UK2Node_FunctionResult::CreateUserDefinedPin (output pin lives on
 *     the result node)
 *   - UK2Node_FunctionEntry::MetaData / SetExtraFlags(FUNC_BlueprintPure)
 *   - FKismetEditorUtilities::CompileBlueprint
 *   - UEditorAssetLibrary::SaveAsset
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpFunctionCreateCommands
{
public:
    FSproftBpFunctionCreateCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleFunctionCreate(const TSharedPtr<FJsonObject>& Params);
};
