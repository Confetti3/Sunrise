# Slophost Experiment — Archived

This branch is the archive for the Sunrise script-host experiment.

## Status

- **Archived:** development stopped on August 16, 2026.
- **Not for upstream:** this branch is not intended to be merged into, or used as the head of a pull request against, `stanuwu/Sunrise`.
- **Evidence boundary:** VM-demonstrated findings, clean-build-only hardening, and unresolved hypotheses are kept separate.
- **Original mission policy:** no claim is made that Bungie's original Red War mission scripts or host-side encounter policy were recovered.

## Useful retained results

- Homecoming (`arcade_homecoming`) was resolved at runtime to registered activity indices 37 and 38.
- Replacing the copied activity descriptor at the verified client boundary made Homecoming authoritative through the in-world start boundary.
- The native/C# pipe observed world phase and one real activity incident without exposing retained gameplay pointers.
- A direct operator probe changed gameplay-switch definition index 246 from 0 to 2, verified persistence, and rolled it back to 0.
- Objective consumer paths were localized, but no safe `objective.set` producer or Homecoming runtime-index mapping was recovered.
- Enemy actor creation, AI policy, health/death replication, and teardown were not implemented.

## Report

The plain-language findings explorer lives at [`docs/index.html`](docs/index.html). It is designed to be published from this branch's `/docs` directory with GitHub Pages.

## Branch provenance

This archive branch was created from the latest pushed script-host research line, `agent/0xf6-mutation-loop`, which already contained the earlier foundation, lab, and runtime-research history.

The later hardening line remained local-only at archive time:

- implementation: `c9a9b49` — `Harden script host foundation`
- evidence: `6b1e743` — `Record clean hardening build provenance`

Those local commits passed clean managed/native builds and tests, but did not complete the VM lifecycle matrix. Their results are summarized in the report without representing them as VM-tested.
