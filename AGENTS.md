## Agent skills

### Building code

Try to use incremental builds always to save time.  When implementing and reviewing, only use the Release configuration to build and test.

### Testing

Only run tests via ctest or python scripts in Release builds, never in Debug, as they take too long.  Never run tests from submodules unless you have modified the submodules as part of the implementation.

Run tests in parallel using -j 16. 

Avoid re-running tests if the required data can be obtained in a different way.  ctest writes the full output of every run to build-linux/Testing/Temporary/LastTest.log and the failures to LastTestsFailed.log. Reading those found both failing checks without re-running anything. Worth knowing: that log survives the run, so grep on it is always cheaper than a re-run.

### Issue tracker

Issues and specs live as GitHub issues in this repo. See `docs/agents/issue-tracker.md`.

### Domain docs

Single-context repo: read `CONTEXT.md` at the root and relevant ADRs in `docs/adr/`. See `docs/agents/domain.md`.
