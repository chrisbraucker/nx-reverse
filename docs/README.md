# Docs Workspace

This directory holds the publishable part of the reversing workspace: derived
analysis, plans, methods, and setup documentation.

Primary documents:

- [ROADMAP.md](/workspaces/nx-reversing.git/docs/20.5.0/ROADMAP.md:1)
- [TRACE_SCHEMA.md](/workspaces/nx-reversing.git/docs/methods/TRACE_SCHEMA.md:1)

Initial target firmware:

- `20.5.0`

Repository split:

1. keep publishable notes and methodology in `docs/`
2. keep local firmware inputs and all persistent reversing state under `workspace/`
3. keep raw local captures under `traces/<version>/local/`

When module identity is uncertain, preserve the original local filename under
`workspace/` and document the mapping in the relevant note.
