# Common Change Recipes

Use these recipes as a starting checklist. They are not exhaustive, but they cover the file clusters that repeatedly matter for Better Timeline work.

## Add A New Built-In Track Type Or Clip Type

Touch these first:

- `source/blender/editors/space_better_timeline/better_timeline_types.cc`
- `source/blender/editors/include/ED_better_timeline.hh`
- `docs/better-timeline/architecture/data-model-and-types.md`

Also check:

- track add menu wiring in `better_timeline_ops_tracks.cc`
- track/clip visuals if the new type needs custom color/icon treatment
- any creation workflow that should expose the new type

Rule:

- compatibility must come from the registry, not ad hoc `if (type == ...)` checks in operators

## Add A New Persistent Common Field

Touch these first:

- `source/blender/makesdna/DNA_space_types.h`
- `source/blender/editors/space_better_timeline/better_timeline_data.cc`
- custom undo snapshot fields in `better_timeline_data.cc`

Then add RNA only if needed:

- `source/blender/makesrna/intern/rna_space.cc`
- `scripts/startup/bl_ui/space_better_timeline.py`

Rule:

- common fields belong in DNA structs only when they are truly common; type-specific payload still belongs in `properties`

## Add Or Change A Sidebar Property

Touch these first:

- `source/blender/makesrna/intern/rna_space.cc`
- `scripts/startup/bl_ui/space_better_timeline.py`

If the property changes clip placement or compatibility:

- route it through the same centralized validation used by drag/move/paste

Do not:

- bind the Python panel directly to blind DNA writes for `Start`, `End`, or `Duration`

## Change Track Or Clip Interaction Behavior

Usually touched files:

- `better_timeline_ops_tracks.cc` or `better_timeline_ops_clips.cc`
- `better_timeline_layout.cc`
- `better_timeline_intern.hh`
- `better_timeline_draw.cc` if there is a preview overlay
- `docs/better-timeline/ui/interactions.md`

Examples:

- new hit-test priority
- new drag mode
- new selection semantics
- new resize affordance

## Change Pane Layout Or Navigation

Usually touched files:

- `better_timeline_layout.cc`
- `better_timeline_draw.cc`
- `better_timeline_ops_view.cc`
- `space_better_timeline.cc` only if region setup changes
- `docs/better-timeline/ui/layout-and-regions.md`

Watch for:

- `View2D` persistence
- scrub clipping
- divider / scrollbar / add-button hit separation
- `v2d.oldwinx/oldwiny` refresh during splitter resize

## Change Persistence, Duplicate, Or Blend I/O

Touch these first:

- `better_timeline_data.cc`
- `DNA_space_types.h`
- `docs/better-timeline/architecture/persistence-and-runtime.md`

Checklist:

- free path
- duplicate path
- undo snapshot path
- blend read path
- blend write path
- post-read normalization if older or invalid state can exist

## Add A New Shortcut

Touch these first:

- operator registration in the relevant `better_timeline_ops_*.cc`
- keymap ensure logic in `better_timeline_ops_view.cc`
- `docs/better-timeline/ui/interactions.md`

Rules:

- use idempotent keymap ensure logic so rerunning setup does not duplicate bindings
- check conflicts against existing Better Timeline gestures
- use `OPTYPE_UNDO` if the operator changes persistent editor state

## Add A New Better Timeline C++ Module

Checklist:

- create the focused sibling file in `source/blender/editors/space_better_timeline/`
- declare shared helpers in `better_timeline_intern.hh` only if multiple modules need them
- add the file to `source/blender/editors/space_better_timeline/CMakeLists.txt`
- keep `space_better_timeline.cc` as glue, not the implementation home
