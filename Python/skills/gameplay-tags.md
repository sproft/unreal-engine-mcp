# Gameplay Tags

When to use this skill: any time a UE5 project needs hierarchical
typed identifiers that can be tested for membership and inheritance.
Reach for them on damage types (`Damage.Physical.Slashing`), AI
states (`AI.State.Combat.Aggressive`), ability cost categories
(`Ability.Cost.Stamina`), gameplay events
(`GameplayCue.Footstep.Wood`), and anywhere we would otherwise reach
for a string enum that callers need to extend without recompiling.
For a fixed enumeration with no hierarchy, a regular UENUM is
simpler.

The canonical UE5 path: tags live in
`Config/DefaultGameplayTags.ini` (or any `Config/*Tags.ini` source
file) and are loaded into the in-process `UGameplayTagsManager`
singleton on startup. New tags are written through the editor's
Project Settings -> Gameplay Tags panel, which routes through
`IGameplayTagsEditorModule::AddNewGameplayTagToINI` and broadcasts a
refresh so live tag pickers update. At runtime tags are queried
through the `FGameplayTag` struct (single tag) or
`FGameplayTagContainer` (set). Membership tests use `MatchesTag`
(child checks parent), `MatchesTagExact` (exact match only),
`HasTag` and `HasAnyExact` (containers). GameplayAbilities + Effects
read tags off the asset CDO and route through their own
`AssetTags` / `GrantedTags` / `BlockAbilitiesWithTag` containers,
each backed by a GE component subclass since 5.3.

Our wrappers in this fork: `tag_registry_edit` is the dedicated
registry tool. Three ops: `add_tag` (new tag + dev comment routed
through the editor module), `remove_tag` (removes from the chosen
ini through `DeleteTagFromINI`), and `list_tags` (read-only substring
search returning each tag's owning source name + ini path + dev
comment). `gas_edit` covers the GameplayAbility / GameplayEffect tag
side: the `set_gameplay_tags` op writes UGameplayAbility's reflected
tag fields directly and routes UGameplayEffect through
`FindOrAddComponent<UAssetTagsGameplayEffectComponent>` /
`UTargetTagsGameplayEffectComponent` /
`UBlockAbilityTagsGameplayEffectComponent` so the cached tag
snapshot on the GE refreshes. `bp_brief` and `bp_export` include any
`FGameplayTag` / `FGameplayTagContainer` fields in their variable
dumps.

Gotchas: tags are case-sensitive under the hood, even though the
editor's autocomplete is case-insensitive. The dev-comment field is
editor-only and never ships to a cooked build. A tag added at
runtime through the editor module is held in memory until the next
reload; the .ini write happens immediately but live ability /
effect assets do not auto-refresh their cached containers without a
GE component `SetAndApply` call (`gas_edit` handles this). When
removing a tag, search for asset references first - removing an
in-use tag silently breaks any container that referenced it.
