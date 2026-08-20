# Sunrise documentation

This directory contains implementation and research notes that are too detailed for the project
README.

## Viewer documentation

- [Using and maintaining Sunrise Viewer](sunrise-viewer.md) explains the UI, runtime data flow,
  lifecycle rules, supported coverage, validation commands, and known limitations.
- [Destiny 2 Viewer symbol research](research/2026-08-20-destiny2-viewer-symbols.md) records the
  evidence from the build-87221 executable/PDB pair and the verified symbol migrations to build
  86657.

The research report distinguishes offline symbol evidence from production hook readiness. A mapped
function address is not sufficient authorization to retain native pointers or install a hook.
