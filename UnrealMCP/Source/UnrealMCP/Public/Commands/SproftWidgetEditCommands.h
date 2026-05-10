#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: widget_edit
 *
 * A trimmed clone of the hosted Flop "widget_edit" surface. Operations:
 *   - "create_widget_blueprint": create a new UWidgetBlueprint with a parent
 *      class and an optional root panel widget class.
 *   - "add_child_widget": construct a named widget (Vertical Box, Horizontal
 *      Box, Progress Bar, Text Block, Button, Image) and attach it as a child
 *      of an existing widget inside a Widget Blueprint's tree.
 *   - "set_slot_property": apply a flat property dict to the UPanelSlot of
 *      an existing widget. Covers UCanvasPanelSlot anchors / offsets / size,
 *      UVerticalBoxSlot / UHorizontalBoxSlot padding / fill, and any other
 *      UPanelSlot-derived class without us spelling out each property by
 *      name. Properties go through `FProperty::ImportText_InContainer`.
 *   - "add_animation": append a new UWidgetAnimation to the
 *      `UWidgetBlueprint::Animations` array. The animation owns a fresh
 *      UMovieScene whose playback range covers `[0, duration]` seconds.
 *   - "add_animation_track": append a UMovieSceneTrack subclass against an
 *      existing animation's UMovieScene, scoped to the binding for a target
 *      widget by FName so the track resolves correctly at runtime.
 *   - "add_keyframe": write a key into a track's first section. Resolves
 *      the target track by index into the animation's MovieScene
 *      `GetTracks()` array. If the track has no section yet we spawn one
 *      through `UMovieSceneTrack::CreateNewSection` + `AddSection`; if it
 *      does, we expand the first section's range to cover the key time
 *      via `UMovieSceneSection::ExpandToFrame` and write the key through
 *      `FMovieSceneFloatChannel::AddCubicKey` (or
 *      `FMovieSceneDoubleChannel::AddCubicKey` for the 5.4+ vector track
 *      shape). Vector tracks accept an `[x, y, z, w?]` JSON array routed
 *      to channels 0..N-1; transform tracks take the same vector shape.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - FKismetEditorUtilities::CreateBlueprint for asset creation
 *   - UWidgetBlueprint / UWidgetBlueprintGeneratedClass for the asset class
 *   - UWidgetTree::ConstructWidget + UPanelWidget::AddChild for the tree
 *   - UWidget::Slot for the per-widget UPanelSlot pointer
 *   - FProperty::ImportText_InContainer for the property dict surface
 *   - UWidgetBlueprint::Animations + UMovieScene::SetPlaybackRange +
 *     UMovieScene::SetDisplayRate for the animation surface
 *   - UWidgetAnimation::AnimationBindings + UMovieScene::AddPossessable +
 *     UMovieScene::AddTrack for the track surface
 *   - FBlueprintEditorUtils::MarkBlueprintAsModified for change notification
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftWidgetEditCommands
{
public:
    FSproftWidgetEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleWidgetEdit(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> CreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddChildWidget(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetSlotProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddAnimation(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddAnimationTrack(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddAnimationKeyframe(const TSharedPtr<FJsonObject>& Params);
};
