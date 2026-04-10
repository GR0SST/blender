# Docs Index

This is the canonical documentation entry point for the Better Timeline fork.

Use the docs in this order:

1. Read `/AGENTS.md` for non-negotiable architectural rules and product constraints.
2. Read this index to find the narrowest doc for the task.
3. Open the topic doc before touching code.

## Roles

- `AGENTS.md` = foundational invariants, design constraints, and rules that should not be broken.
- `CLAUDE.md` = short operational entry point: project purpose, build commands, and where to look first.
- `docs/` = working reference map for files, subsystems, workflows, and Blender integration touchpoints.

## Quick Navigation

| If you need to... | Read this first |
|---|---|
| understand the editor at a high level | `docs/better-timeline/README.md` |
| find the right C++/Python file fast | `docs/better-timeline/code-map/files-and-ownership.md` |
| understand typed tracks/clips and compatibility rules | `docs/better-timeline/architecture/data-model-and-types.md` |
| change persistence, blend I/O, runtime state, or undo | `docs/better-timeline/architecture/persistence-and-runtime.md` |
| change draw order, coordinate spaces, or GPU/UI APIs | `docs/better-timeline/ui/drawing.md` |
| change layout, splitter, scrollbar, regions, or View2D behavior | `docs/better-timeline/ui/layout-and-regions.md` |
| change hotkeys, hit-testing priority, or interaction flow | `docs/better-timeline/ui/interactions.md` |
| touch RNA, space registration, Python UI, or anim integration | `docs/better-timeline/code-map/blender-touchpoints.md` |
| debug object-slot binding, object selection sync, or Properties-pane object fields | `docs/better-timeline/code-map/blender-touchpoints.md` |
| add a feature and want a file-touch checklist | `docs/better-timeline/workflows/common-change-recipes.md` |

## Structure

```text
docs/
  README.md
  better-timeline/
    README.md
    architecture/
      overview.md
      data-model-and-types.md
      persistence-and-runtime.md
    ui/
      drawing.md
      interactions.md
      layout-and-regions.md
    code-map/
      files-and-ownership.md
      blender-touchpoints.md
    workflows/
      common-change-recipes.md
```

## Documentation Policy

- Keep `docs/README.md` as the single documentation index. Do not duplicate a second manual index in `CLAUDE.md`.
- Prefer responsibility-based filenames (`files-and-ownership`, `blender-touchpoints`) over vague buckets like `misc` or `notes`.
- Update docs only after the developer confirms the behavior works in their runtime build.
- When a session reveals a real codebase invariant or Blender-specific gotcha, add it to the narrowest relevant doc instead of bloating `AGENTS.md`.
