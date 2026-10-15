#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::diagnostics::studio::cui_wire {

/** CUI diagnostics use a separate local mapping and the ASCII magic SSCU. */
inline constexpr std::uint32_t kMagic = 0x55435353, kVersion = 1;
/** Studio names the mapping after the game process id. */
inline constexpr wchar_t kMappingFormat[] = L"Local\\SunriseStudio.Cui.%lu";
/** Capacity policy; a longer walk or text sets its truncated flag. */
inline constexpr std::size_t kScreenCapacity = 32, kWidgetCapacity = 8192;
inline constexpr std::size_t kSlotCount = 10, kTextCapacity = 256;
/** All-one values mean no parent row and no top active screen. */
inline constexpr std::uint16_t kNoRow = 0xFFFF;
inline constexpr std::uint32_t kNoFocus = 0xFFFFFFFFU;
/** Milliseconds: one capture per interval at most. */
inline constexpr std::uint64_t kCaptureInterval = 100;

/** Section placement; Studio copies each section by these offsets and sizes. */
inline constexpr std::size_t kControlBytes = 128, kScreenRowBytes = 32, kWidgetRowBytes = 56;
inline constexpr std::size_t kScreensOffset = 128, kScreensBytes = 1152;
inline constexpr std::size_t kDetailOffset = 1280, kDetailBytes = 640;
inline constexpr std::size_t kTreeOffset = 1920, kTreeHeaderBytes = 64;
inline constexpr std::size_t kMappingBytes =
    kTreeOffset + kTreeHeaderBytes + kWidgetCapacity * kWidgetRowBytes;

/** Control fields Studio reads or writes. */
inline constexpr std::size_t kMagicOffset = 0, kVersionOffset = 4;
inline constexpr std::size_t kBytesOffset = 8, kEditorPidOffset = 12;
inline constexpr std::size_t kHeartbeatOffset = 16, kClientVersionOffset = 24;
inline constexpr std::size_t kBridgeFlagsOffset = 28, kGamePidOffset = 32;
inline constexpr std::size_t kCaptureMicrosOffset = 36, kCheckedTimeOffset = 40;
inline constexpr std::size_t kWatchRequestOffset = 48, kWatchHierarchyOffset = 52;
inline constexpr std::size_t kWatchOrdinalOffset = 56, kWidgetRequestOffset = 60;
inline constexpr std::size_t kWidgetHierarchyOffset = 64, kWidgetOrdinalOffset = 68;
inline constexpr std::size_t kWidgetNodeOffset = 72;

/** Section-relative fields shared by every section. */
inline constexpr std::size_t kSequenceOffset = 0, kResponseOffset = 4;
/** Screens section fields. */
inline constexpr std::size_t kScreensCountOffset = 4, kScreensTimeOffset = 8;
inline constexpr std::size_t kScreensFlagsOffset = 16, kFocus0Offset = 20, kFocus1Offset = 24;
inline constexpr std::size_t kScreenRowsOffset = 32;
/** Screen row fields. */
inline constexpr std::size_t kScreenHierarchyOffset = 0, kScreenTagOffset = 4;
inline constexpr std::size_t kScreenViewOffset = 8, kScreenPageOffset = 12;
inline constexpr std::size_t kScreenFlagsOffset = 16, kScreenStateOffset = 20;
inline constexpr std::size_t kScreenPriorityOffset = 24, kScreenLayerOffset = 28;
inline constexpr std::size_t kScreenOrdinalOffset = 29;
/** Detail section fields. */
inline constexpr std::size_t kDetailStatusOffset = 8, kDetailFlagsOffset = 12;
inline constexpr std::size_t kDetailTimeOffset = 16, kDetailTypeOffset = 24;
inline constexpr std::size_t kDetailHierarchyOffset = 32, kDetailNameOffset = 36;
inline constexpr std::size_t kDetailNodeOffset = 40, kDetailParentOffset = 42;
inline constexpr std::size_t kDetailStateOffset = 44, kDetailSlotFlagsOffset = 48;
inline constexpr std::size_t kDetailObjectFlagsOffset = 52, kDetailChildCountOffset = 54;
inline constexpr std::size_t kDetailSlotsOffset = 56, kDetailTextHashOffset = 96;
inline constexpr std::size_t kDetailStyleOffset = 100, kDetailTextSourceOffset = 104;
inline constexpr std::size_t kDetailTextStateOffset = 105, kDetailTextLengthOffset = 106;
inline constexpr std::size_t kDetailTextModeOffset = 108, kDetailTextOffset = 112;
/** Tree section fields. */
inline constexpr std::size_t kTreeTimeOffset = 8, kTreeStatusOffset = 16;
inline constexpr std::size_t kTreeCountOffset = 20, kTreeFlagsOffset = 24;
inline constexpr std::size_t kTreeRevisionOffset = 28, kTreeHierarchyOffset = 32;
inline constexpr std::size_t kTreeScreenTagOffset = 36, kTreeViewOffset = 40;
inline constexpr std::size_t kTreeTotalOffset = 44;
/** Widget row fields. */
inline constexpr std::size_t kRowTypeOffset = 0, kRowHierarchyOffset = 8, kRowNameOffset = 12;
inline constexpr std::size_t kRowNodeOffset = 16, kRowParentOffset = 18, kRowStateOffset = 20;
inline constexpr std::size_t kRowFlagsOffset = 24, kRowTextHashOffset = 28;
inline constexpr std::size_t kRowVisibilityOffset = 32, kRowXOffset = 36, kRowYOffset = 40;
inline constexpr std::size_t kRowWidthOffset = 44, kRowHeightOffset = 48;

/** absent: no open screen or node matches. unavailable: no screen manager, or the walk faulted. */
enum class Status : std::uint32_t { pending, complete, absent, unavailable };
/** managerReady: the game has built its screen manager; it is clear early in boot. */
enum BridgeFlags : std::uint32_t { managerReady = 1 };
/** Flags of the screens and tree sections. */
enum WalkFlags : std::uint32_t { walkTruncated = 1, walkFaulted = 2 };
/** Native screen flag bit 0x10; only a sort key in the code read. */
enum ScreenFlags : std::uint32_t { screenSortLast = 1U << 4 };
/**
 * Native widget state bits. Visible is the widget's own bit, set by property 0x201; whether it
 * keeps a widget out of the draw is unverified. The capture skips a screen with stateSkipScreen.
 */
enum StateFlags : std::uint32_t { stateVisible = 1U << 1, stateSkipScreen = 1U << 15 };
/** DetailSection::flags: text widget, slots copied, text copied, text cut at kTextCapacity. */
enum DetailFlags : std::uint32_t {
    detailText = 1,
    detailSlotsRead = 2,
    detailTextCopied = 4,
    detailTextTruncated = 8,
};
/** WidgetRow::rowFlags: slots copied, textHash valid, stateFlags valid; a non-widget has none. */
enum RowFlags : std::uint32_t { rowSlotsRead = 1, rowText = 2, rowWidget = 4 };

/** Editor fields are written before their request id; game fields are written every capture. */
struct alignas(8) Control final {
    std::uint32_t magic{}, version{}, bytes{}, editorPid{};
    alignas(8) volatile std::int64_t heartbeat{};
    std::uint32_t clientVersion{}, bridgeFlags{}, gamePid{}, captureMicros{};
    alignas(8) volatile std::int64_t checkedTime{};
    alignas(4) volatile std::int32_t watchRequest{};
    std::uint32_t watchHierarchy{}, watchOrdinal{};
    alignas(4) volatile std::int32_t widgetRequest{};
    std::uint32_t widgetHierarchy{}, widgetOrdinal{}, widgetNode{};
    std::array<std::byte, 52> reserved{};
};

/** One open screen; ordinal counts earlier open screens with the same hierarchy tag. */
struct ScreenRow final {
    std::uint32_t hierarchyTag{}, screenTag{}, viewHash{}, page{};
    std::uint32_t screenFlags{}, stateFlags{};
    /** Signed; the native default is -1. */
    std::int32_t priority{};
    std::uint8_t layer{}, ordinal{};
    std::uint16_t reserved{};
};

/** Rows run layer 0 then layer 1, each in draw order from head.next. */
struct alignas(8) ScreensSection final {
    alignas(4) volatile std::int32_t sequence{};
    std::uint32_t count{};
    std::uint64_t time{};
    std::uint32_t flags{}, focus0{kNoFocus}, focus1{kNoFocus}, reserved{};
    std::array<ScreenRow, kScreenCapacity> rows{};
    std::array<std::byte, 96> padding{};
};

/**
 * One answer per widget request id; text is copied only while its owner block is alive or absent.
 * detailTextCopied: text holds textLength units, from either source; textSource tells which.
 * detailSlotsRead is set only when every copied slot is finite.
 */
struct alignas(8) DetailSection final {
    alignas(4) volatile std::int32_t sequence{};
    std::uint32_t response{};
    Status status{};
    std::uint32_t flags{};
    std::uint64_t time{}, typeKey{};
    std::uint32_t hierarchyTag{}, nameHash{};
    std::uint16_t nodeId{}, parentNode{kNoRow};
    std::uint32_t stateFlags{}, slotFlags{};
    std::uint16_t objectFlags{}, childCount{};
    std::array<float, kSlotCount> slots{};
    std::uint32_t textHash{}, styleKey{};
    std::uint8_t textSource{}, textState{};
    std::uint16_t textLength{};
    /** The text widget's byte at +512, passed to line layout; its meaning is unverified. */
    std::uint32_t textMode{};
    std::array<char16_t, kTextCapacity> text{};
    std::array<std::byte, 16> reserved{};
};

/**
 * One widget in depth-first pre-order; a parent row always comes before its children.
 * rowSlotsRead is set only when every copied slot is finite.
 */
struct alignas(8) WidgetRow final {
    std::uint64_t typeKey{};
    std::uint32_t hierarchyTag{}, nameHash{};
    std::uint16_t nodeId{}, parentRow{kNoRow};
    std::uint32_t stateFlags{}, rowFlags{}, textHash{};
    /** Slot 6, the 0x201 visibility value; then slots 1 to 4. */
    float visibility{}, x{}, y{}, width{}, height{};
    std::uint32_t reserved{};
};

/** The tree of the watched screen; republished only when its content or watch id changes. */
struct alignas(8) TreeSection final {
    alignas(4) volatile std::int32_t sequence{};
    std::uint32_t response{};
    std::uint64_t time{};
    Status status{};
    std::uint32_t count{}, flags{}, revision{};
    std::uint32_t hierarchyTag{}, screenTag{}, viewHash{}, total{};
    std::array<std::byte, 16> reserved{};
    std::array<WidgetRow, kWidgetCapacity> rows{};
};

/** Each section is written between two increments of its own sequence. */
struct alignas(8) SharedCui final {
    Control control{};
    ScreensSection screens{};
    DetailSection detail{};
    TreeSection tree{};
};

static_assert(sizeof(Control) == kControlBytes);
static_assert(offsetof(Control, magic) == kMagicOffset);
static_assert(offsetof(Control, version) == kVersionOffset);
static_assert(offsetof(Control, bytes) == kBytesOffset);
static_assert(offsetof(Control, editorPid) == kEditorPidOffset);
static_assert(offsetof(Control, heartbeat) == kHeartbeatOffset);
static_assert(offsetof(Control, clientVersion) == kClientVersionOffset);
static_assert(offsetof(Control, bridgeFlags) == kBridgeFlagsOffset);
static_assert(offsetof(Control, gamePid) == kGamePidOffset);
static_assert(offsetof(Control, captureMicros) == kCaptureMicrosOffset);
static_assert(offsetof(Control, checkedTime) == kCheckedTimeOffset);
static_assert(offsetof(Control, watchRequest) == kWatchRequestOffset);
static_assert(offsetof(Control, watchHierarchy) == kWatchHierarchyOffset);
static_assert(offsetof(Control, watchOrdinal) == kWatchOrdinalOffset);
static_assert(offsetof(Control, widgetRequest) == kWidgetRequestOffset);
static_assert(offsetof(Control, widgetHierarchy) == kWidgetHierarchyOffset);
static_assert(offsetof(Control, widgetOrdinal) == kWidgetOrdinalOffset);
static_assert(offsetof(Control, widgetNode) == kWidgetNodeOffset);

static_assert(sizeof(ScreenRow) == kScreenRowBytes);
static_assert(offsetof(ScreenRow, hierarchyTag) == kScreenHierarchyOffset);
static_assert(offsetof(ScreenRow, screenTag) == kScreenTagOffset);
static_assert(offsetof(ScreenRow, viewHash) == kScreenViewOffset);
static_assert(offsetof(ScreenRow, page) == kScreenPageOffset);
static_assert(offsetof(ScreenRow, screenFlags) == kScreenFlagsOffset);
static_assert(offsetof(ScreenRow, stateFlags) == kScreenStateOffset);
static_assert(offsetof(ScreenRow, priority) == kScreenPriorityOffset);
static_assert(offsetof(ScreenRow, layer) == kScreenLayerOffset);
static_assert(offsetof(ScreenRow, ordinal) == kScreenOrdinalOffset);

static_assert(sizeof(ScreensSection) == kScreensBytes);
static_assert(offsetof(ScreensSection, sequence) == kSequenceOffset);
static_assert(offsetof(ScreensSection, count) == kScreensCountOffset);
static_assert(offsetof(ScreensSection, time) == kScreensTimeOffset);
static_assert(offsetof(ScreensSection, flags) == kScreensFlagsOffset);
static_assert(offsetof(ScreensSection, focus0) == kFocus0Offset);
static_assert(offsetof(ScreensSection, focus1) == kFocus1Offset);
static_assert(offsetof(ScreensSection, rows) == kScreenRowsOffset);

static_assert(sizeof(DetailSection) == kDetailBytes);
static_assert(offsetof(DetailSection, sequence) == kSequenceOffset);
static_assert(offsetof(DetailSection, response) == kResponseOffset);
static_assert(offsetof(DetailSection, status) == kDetailStatusOffset);
static_assert(offsetof(DetailSection, flags) == kDetailFlagsOffset);
static_assert(offsetof(DetailSection, time) == kDetailTimeOffset);
static_assert(offsetof(DetailSection, typeKey) == kDetailTypeOffset);
static_assert(offsetof(DetailSection, hierarchyTag) == kDetailHierarchyOffset);
static_assert(offsetof(DetailSection, nameHash) == kDetailNameOffset);
static_assert(offsetof(DetailSection, nodeId) == kDetailNodeOffset);
static_assert(offsetof(DetailSection, parentNode) == kDetailParentOffset);
static_assert(offsetof(DetailSection, stateFlags) == kDetailStateOffset);
static_assert(offsetof(DetailSection, slotFlags) == kDetailSlotFlagsOffset);
static_assert(offsetof(DetailSection, objectFlags) == kDetailObjectFlagsOffset);
static_assert(offsetof(DetailSection, childCount) == kDetailChildCountOffset);
static_assert(offsetof(DetailSection, slots) == kDetailSlotsOffset);
static_assert(offsetof(DetailSection, textHash) == kDetailTextHashOffset);
static_assert(offsetof(DetailSection, styleKey) == kDetailStyleOffset);
static_assert(offsetof(DetailSection, textSource) == kDetailTextSourceOffset);
static_assert(offsetof(DetailSection, textState) == kDetailTextStateOffset);
static_assert(offsetof(DetailSection, textLength) == kDetailTextLengthOffset);
static_assert(offsetof(DetailSection, textMode) == kDetailTextModeOffset);
static_assert(offsetof(DetailSection, text) == kDetailTextOffset);

static_assert(sizeof(WidgetRow) == kWidgetRowBytes);
static_assert(offsetof(WidgetRow, typeKey) == kRowTypeOffset);
static_assert(offsetof(WidgetRow, hierarchyTag) == kRowHierarchyOffset);
static_assert(offsetof(WidgetRow, nameHash) == kRowNameOffset);
static_assert(offsetof(WidgetRow, nodeId) == kRowNodeOffset);
static_assert(offsetof(WidgetRow, parentRow) == kRowParentOffset);
static_assert(offsetof(WidgetRow, stateFlags) == kRowStateOffset);
static_assert(offsetof(WidgetRow, rowFlags) == kRowFlagsOffset);
static_assert(offsetof(WidgetRow, textHash) == kRowTextHashOffset);
static_assert(offsetof(WidgetRow, visibility) == kRowVisibilityOffset);
static_assert(offsetof(WidgetRow, x) == kRowXOffset);
static_assert(offsetof(WidgetRow, y) == kRowYOffset);
static_assert(offsetof(WidgetRow, width) == kRowWidthOffset);
static_assert(offsetof(WidgetRow, height) == kRowHeightOffset);

static_assert(offsetof(TreeSection, sequence) == kSequenceOffset);
static_assert(offsetof(TreeSection, response) == kResponseOffset);
static_assert(offsetof(TreeSection, time) == kTreeTimeOffset);
static_assert(offsetof(TreeSection, status) == kTreeStatusOffset);
static_assert(offsetof(TreeSection, count) == kTreeCountOffset);
static_assert(offsetof(TreeSection, flags) == kTreeFlagsOffset);
static_assert(offsetof(TreeSection, revision) == kTreeRevisionOffset);
static_assert(offsetof(TreeSection, hierarchyTag) == kTreeHierarchyOffset);
static_assert(offsetof(TreeSection, screenTag) == kTreeScreenTagOffset);
static_assert(offsetof(TreeSection, viewHash) == kTreeViewOffset);
static_assert(offsetof(TreeSection, total) == kTreeTotalOffset);
static_assert(offsetof(TreeSection, rows) == kTreeHeaderBytes);

static_assert(offsetof(SharedCui, screens) == kScreensOffset);
static_assert(offsetof(SharedCui, detail) == kDetailOffset);
static_assert(offsetof(SharedCui, tree) == kTreeOffset);
static_assert(sizeof(SharedCui) == kMappingBytes);
static_assert(kMappingBytes == 460736);

} // namespace sunrise::client::diagnostics::studio::cui_wire
