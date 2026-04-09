# Better Timeline Docs

Use this folder when you already know the task is Better Timeline-specific and need the shortest path to the right subsystem.

## Read By Task

- Start with `architecture/overview.md` if you need a mental model of the editor.
- Open `code-map/files-and-ownership.md` if you need to know which file owns a behavior.
- Open `code-map/blender-touchpoints.md` if the task crosses out of the editor folder into DNA, RNA, keymaps, undo, or Python UI.
- Open `architecture/data-model-and-types.md` before changing tracks, clips, compatibility, or type registration.
- Open `architecture/persistence-and-runtime.md` before changing DNA fields, runtime state, duplication, blend file I/O, or undo.
- Open `ui/layout-and-regions.md` before changing pane geometry, scrollbar logic, splitter behavior, or View2D setup.
- Open `ui/drawing.md` before changing visual layers, scissoring, GPU immediate drawing, icons, or text.
- Open `ui/interactions.md` before changing hit-testing order, gestures, or Better Timeline keybindings.
- Open `workflows/common-change-recipes.md` when you want a quick checklist for a class of change.

## Section Summary

- `architecture/` explains what state exists, who owns it, and what invariants the model relies on.
- `ui/` explains how the editor is laid out and how it draws and reacts to input.
- `code-map/` explains where responsibilities live in the codebase and what non-obvious Blender files participate.
- `workflows/` gives practical edit recipes so agents do not have to rediscover the same cross-file touchpoints.

## Rules Of Thumb

- Treat `SpaceBetterTimeline::tracks` and per-track `clips` as the authoritative model.
- Treat the registry API in `ED_better_timeline.hh` as the only compatibility source of truth.
- Keep `space_better_timeline.cc` thin; substantial work belongs in sibling modules.
- Do not trust generic Blender editor behavior by default. Better Timeline has several explicit opt-ins and opt-outs documented in the touchpoint docs.
