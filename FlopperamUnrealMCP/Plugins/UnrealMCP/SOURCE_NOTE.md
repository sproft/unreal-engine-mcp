# Bundled UnrealMCP plugin source

The canonical UnrealMCP plugin source lives at `<repo-root>/UnrealMCP/Source/`
(plus the matching `<repo-root>/UnrealMCP/UnrealMCP.uplugin`). The bundled
copy under this directory is a snapshot taken from that canonical tree.

When the bundled project here drifts behind the canonical tree (either
because we ship new Sproft commands in the canonical tree first or because
we refactor the upstream Epic source), copy the canonical `Source/` folder
plus `UnrealMCP.uplugin` over this directory before opening the
`.uproject`. A drift-checker is on the backlog.
