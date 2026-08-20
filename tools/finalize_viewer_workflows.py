"""Keep the Viewer build and generated-adapter workflows deterministic and non-racing."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".github/workflows/build.yml"
LEGACY_COMPLETION = ROOT / ".github/workflows/complete-viewer-catalogs.yml"

BUILD_CONTENT = """name: Build

permissions:
  contents: read

on:
  push:
    branches: [master, sunrise-inspector, sunrise-viewer]
  pull_request:
  workflow_dispatch:

jobs:
  release:
    name: Release x64
    # The image carries Visual Studio 2026, so the project's own v145 toolset and its
    # 10.0.26100.0 SDK are both present. No toolset override.
    runs-on: windows-2025-vs2026

    steps:
      - name: Checkout
        uses: actions/checkout@v4
        with:
          fetch-depth: 0

      - name: Verify Viewer diff hygiene
        if: github.ref == 'refs/heads/sunrise-viewer'
        shell: pwsh
        run: |
          git fetch --no-tags origin sunrise-inspector:refs/remotes/origin/sunrise-inspector
          git diff --check origin/sunrise-inspector...HEAD

      - name: Add MSBuild to PATH
        uses: microsoft/setup-msbuild@v2

      - name: Build
        run: >
          msbuild Sunrise.sln /m
          /p:Configuration=Release
          /p:Platform=x64
          /verbosity:minimal

      - name: Upload artifact
        uses: actions/upload-artifact@v4
        with:
          name: Sunrise-Release-x64
          path: |
            build/x64/Release/steam_api64.dll
            build/x64/Release/steam_api64.pdb
          if-no-files-found: error
"""


def main() -> int:
    BUILD.parent.mkdir(parents=True, exist_ok=True)
    BUILD.write_text(BUILD_CONTENT, encoding="utf-8")
    LEGACY_COMPLETION.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
