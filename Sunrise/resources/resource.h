#pragma once

/** The first module-local RCDATA identifier owns the bundled default settings document. */
#define IDR_DEFAULT_SETTINGS 101
/** The next module-local RCDATA identifier embeds the required Dear ImGui MIT notice. */
#define IDR_IMGUI_LICENSE 102
/** The next module-local RCDATA identifier embeds the required Microsoft Detours notice. */
#define IDR_DETOURS_LICENSE 103
/** The next module-local RCDATA identifier holds the animated logo sprite sheet, as a PNG. */
#define IDR_LOGO_SHEET 104
/** SQLite account schema retained for exact PR88 v1 migration. */
#define IDR_STATE_SCHEMA_V1 106
/** Additive bootstrap-state migration from account schema v1 to Sunrise schema v2. */
#define IDR_STATE_MIGRATION_V1_TO_V2 107
/** Canonical bootstrap seed generated from the checked-in version-8 defaults. */
#define IDR_DEFAULT_STATE_SEED 108
/** The compact project icon used by technical workspaces, as a PNG. */
#define IDR_INSPECTOR_ICON 105

/** The four numeric fields of the version resource, in FILEVERSION order. */
#define SUNRISE_VER_MAJOR 0
#define SUNRISE_VER_MINOR 3
#define SUNRISE_VER_PATCH 2
#define SUNRISE_VER_BUILD 0
/** The same version as display text. Windows shows this string, not the four fields. */
#define SUNRISE_VER_STRING "0.3.2.0"
