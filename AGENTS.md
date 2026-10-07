## Agent skills

### Building code

Try to use incremental builds always to same time.  When implementing and reviewing, only use the Release configuration to build and test.

### Testing

Only run tests via ctest or python scripts in Release builds, never in Debug, as they take too long.

### Issue tracker

Issues and specs live as GitHub issues in this repo. See `docs/agents/issue-tracker.md`.

### Domain docs

Single-context repo: read `CONTEXT.md` at the root and relevant ADRs in `docs/adr/`. See `docs/agents/domain.md`.
