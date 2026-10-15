#include "studio_cui_reader.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <type_traits>

#include "../../memory/current_process_memory.h"

namespace sunrise::client::diagnostics::studio {
namespace {

namespace wire = cui_wire;

// --- Native layout ---

/** An intrusive list link; an empty list's head points at itself. */
struct NativeLink final {
    std::uintptr_t next{}, prev{};
};

/** One screen layer: screens in draw order, then the top active screen. */
constexpr std::size_t kLayerBytes = 64, kFocusOffset = 56;
struct NativeLayer final {
    NativeLink head{};
    std::array<std::byte, 40> reserved{};
    std::uintptr_t focus{};
};
static_assert(sizeof(NativeLayer) == kLayerBytes);
static_assert(offsetof(NativeLayer, focus) == kFocusOffset);

/** The screen manager holds its two layers at +208. */
constexpr std::size_t kLayerCount = 2, kLayersOffset = 208;
struct NativeScreenManager final {
    std::array<std::byte, kLayersOffset> reserved{};
    std::array<NativeLayer, kLayerCount> layers{};
};
static_assert(offsetof(NativeScreenManager, layers) == kLayersOffset);

/** A type descriptor names its parent type first, then its 64-bit type key. */
struct NativeDescriptor final {
    std::uintptr_t parent{};
    std::uint64_t key{};
};

/** The head every CUI object shares; children link through their sibling field. */
constexpr std::size_t kDescriptorOffset = 16, kTagOffset = 24, kNodeOffset = 28;
constexpr std::size_t kObjectFlagsOffset = 30, kNameOffset = 36, kParentOffset = 48;
constexpr std::size_t kSiblingOffset = 56, kChildrenOffset = 72;
struct NativeObject final {
    std::array<std::byte, kDescriptorOffset> head{};
    std::uintptr_t descriptor{};
    std::uint32_t hierarchyTag{};
    std::uint16_t nodeId{}, flags{};
    std::uint32_t reserved32{}, nameHash{};
    std::uintptr_t owner{}, parent{};
    NativeLink sibling{}, children{};
};
static_assert(offsetof(NativeObject, descriptor) == kDescriptorOffset);
static_assert(offsetof(NativeObject, hierarchyTag) == kTagOffset);
static_assert(offsetof(NativeObject, nodeId) == kNodeOffset);
static_assert(offsetof(NativeObject, flags) == kObjectFlagsOffset);
static_assert(offsetof(NativeObject, nameHash) == kNameOffset);
static_assert(offsetof(NativeObject, parent) == kParentOffset);
static_assert(offsetof(NativeObject, sibling) == kSiblingOffset);
static_assert(offsetof(NativeObject, children) == kChildrenOffset);

/** Widget fields: the float slot block, its indirect-slot bits, and the state word. */
constexpr std::size_t kSlotsOffset = 152, kSlotFlagsOffset = 160, kStateOffset = 168;
struct NativeWidget final {
    NativeObject object{};
    std::array<std::byte, kSlotsOffset - sizeof(NativeObject)> reserved{};
    std::uintptr_t slots{};
    std::uint32_t slotFlags{}, reserved164{}, state{};
};
static_assert(offsetof(NativeWidget, slots) == kSlotsOffset);
static_assert(offsetof(NativeWidget, slotFlags) == kSlotFlagsOffset);
static_assert(offsetof(NativeWidget, state) == kStateOffset);

/** Screen identity fields, the sort priority, and the layer list link. */
constexpr std::size_t kScreenTagOffset = 772, kViewOffset = 780, kPageOffset = 784;
constexpr std::size_t kScreenFlagsOffset = 796, kPriorityOffset = 1048, kScreenLinkOffset = 1536;
struct NativeScreenIdentity final {
    std::uint32_t screenTag{}, reserved776{}, viewHash{}, page{};
    std::array<std::uint32_t, 2> reserved788{};
    std::uint32_t flags{};
};
struct NativeScreen final {
    std::array<std::byte, kScreenTagOffset> head{};
    NativeScreenIdentity identity{};
    std::array<std::byte, kPriorityOffset - kScreenTagOffset - sizeof(NativeScreenIdentity)>
        reserved800{};
    std::int32_t priority{};
    std::array<std::byte, kScreenLinkOffset - kPriorityOffset - sizeof(std::int32_t)>
        reserved1052{};
    NativeLink link{};
};
static_assert(offsetof(NativeScreen, identity) + offsetof(NativeScreenIdentity, viewHash)
              == kViewOffset);
static_assert(offsetof(NativeScreen, identity) + offsetof(NativeScreenIdentity, page)
              == kPageOffset);
static_assert(offsetof(NativeScreen, identity) + offsetof(NativeScreenIdentity, flags)
              == kScreenFlagsOffset);
static_assert(offsetof(NativeScreen, priority) == kPriorityOffset);
static_assert(offsetof(NativeScreen, link) == kScreenLinkOffset);

/** Text widget fields from +288: state, source bits, string hash, and both text lanes. */
constexpr std::size_t kTextBlockOffset = 288, kTextStateOffset = 290, kTextSourceOffset = 291;
constexpr std::size_t kTextHashOffset = 308, kDynamicOwnerOffset = 312, kDynamicTextOffset = 320;
constexpr std::size_t kNarrowOwnerOffset = 336, kNarrowTextOffset = 344;
constexpr std::size_t kStyleOffset = 388, kTextModeOffset = 512;
struct NativeTextBlock final {
    std::uint16_t reserved288{};
    std::uint8_t state{}, source{};
    std::array<std::byte, 16> reserved292{};
    std::uint32_t hash{};
    std::uintptr_t dynamicOwner{}, dynamicText{}, reserved328{};
    std::uintptr_t narrowOwner{}, narrowText{};
};
struct NativeText final {
    std::array<std::byte, kTextBlockOffset> head{};
    NativeTextBlock block{};
    std::array<std::byte, kStyleOffset - kTextBlockOffset - sizeof(NativeTextBlock)> reserved{};
    std::uint32_t styleKey{};
    std::array<std::byte, kTextModeOffset - kStyleOffset - sizeof(std::uint32_t)> reserved392{};
    std::uint8_t mode{};
};
static_assert(offsetof(NativeText, block) == kTextBlockOffset);
static_assert(kTextBlockOffset + offsetof(NativeTextBlock, state) == kTextStateOffset);
static_assert(kTextBlockOffset + offsetof(NativeTextBlock, source) == kTextSourceOffset);
static_assert(kTextBlockOffset + offsetof(NativeTextBlock, hash) == kTextHashOffset);
static_assert(kTextBlockOffset + offsetof(NativeTextBlock, dynamicOwner) == kDynamicOwnerOffset);
static_assert(kTextBlockOffset + offsetof(NativeTextBlock, dynamicText) == kDynamicTextOffset);
static_assert(kTextBlockOffset + offsetof(NativeTextBlock, narrowOwner) == kNarrowOwnerOffset);
static_assert(kTextBlockOffset + offsetof(NativeTextBlock, narrowText) == kNarrowTextOffset);
static_assert(offsetof(NativeText, styleKey) == kStyleOffset);
static_assert(offsetof(NativeText, mode) == kTextModeOffset);

/** A text owner block whose byte at +30 is set no longer owns live text. */
constexpr std::size_t kOwnerDeadOffset = 30;
struct NativeTextOwner final {
    std::array<std::byte, kOwnerDeadOffset> head{};
    std::uint8_t dead{};
};
static_assert(offsetof(NativeTextOwner, dead) == kOwnerDeadOffset);

// --- Policy ---

/** Native type keys of the widget root type, c_text_widget and c_bitmap_widget. */
constexpr std::uint64_t kWidgetKey = 0xFF700E02, kTextKey = 0x0CE30D0B, kBitmapKey = 0x7D1BD221;
/** A descriptor chain deeper than this is treated as unreadable. */
constexpr std::size_t kDescriptorDepth = 16;
/** Every widget has at least 8 float slots; text and bitmap widgets have at least 10. */
constexpr std::size_t kBaseSlots = 8;
static_assert(kBaseSlots <= wire::kSlotCount);
/** Slot i holds a pointer to its value when bit i + 8 of the slot flags is set. */
constexpr std::uint32_t kIndirectShift = 8;
/** Float slots the tree rows copy. */
enum Slot : std::size_t { slotX = 1, slotY = 2, slotWidth = 3, slotHeight = 4, slotVisibility = 6 };
/** A walk stops after this many nodes; it bounds a corrupt or cyclic list. */
constexpr std::size_t kVisitLimit = 16384;
/** The walk descends at most this deep; deeper nodes are counted but not entered. */
constexpr std::size_t kMaxDepth = 256;
/** A layer longer than this is treated as corrupt. */
constexpr std::size_t kLayerLimit = 256;
/** The highest set source bit of 0x1F picks the text lane: 0x2 dynamic, 0x1 narrow. */
constexpr std::uint8_t kSourceMask = 0x1F, kDynamicSource = 0x2, kNarrowSource = 0x1;
/** Text reads stop at a 32-byte boundary, so a read never crosses a page it does not need. */
constexpr std::size_t kTextChunkBytes = 32;

std::uintptr_t g_manager{}, g_ready{};

/** A fault fails only this read. */
template <typename T> [[nodiscard]] bool copy(std::uintptr_t address, T& output) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    return memory::copy_current_process(
        nullptr, address, std::as_writable_bytes(std::span(&output, 1)));
}

/** What the descriptor chain says about one node. */
struct NodeType final {
    std::uint64_t key{};
    bool widget{}, text{}, tenSlots{};
};

/** @return False when the chain is unreadable or deeper than kDescriptorDepth. */
[[nodiscard]] bool read_type(std::uintptr_t descriptor, NodeType& type) noexcept {
    for (std::size_t depth = 0; descriptor != 0; ++depth) {
        NativeDescriptor native{};
        if (depth == kDescriptorDepth || !copy(descriptor, native)) {
            return false;
        }
        type.key = depth == 0 ? native.key : type.key;
        type.widget = type.widget || native.key == kWidgetKey;
        type.text = type.text || native.key == kTextKey;
        type.tenSlots = type.tenSlots || native.key == kTextKey || native.key == kBitmapKey;
        descriptor = native.parent;
    }
    return true;
}

/**
 * Copies the first output.size() float slots, following indirect slots to their value.
 * @return False when a read faults or a value is not finite.
 */
[[nodiscard]] bool read_slots(const NativeWidget& widget, std::span<float> output) noexcept {
    std::array<std::uint64_t, wire::kSlotCount> raw{};
    if (output.size() > raw.size()
        || !memory::copy_current_process(
            nullptr, widget.slots, std::as_writable_bytes(std::span(raw).first(output.size())))) {
        return false;
    }
    for (std::size_t i = 0; i < output.size(); ++i) {
        float value{};
        if (((widget.slotFlags >> (i + kIndirectShift)) & 1U) != 0) {
            if (!copy(static_cast<std::uintptr_t>(raw[i]), value)) {
                return false;
            }
        } else {
            std::memcpy(&value, &raw[i], sizeof value);
        }
        if (!std::isfinite(value)) {
            return false;
        }
        output[i] = value;
    }
    return true;
}

/**
 * Copies a NUL-terminated string in aligned chunks.
 * @param terminated Set when the NUL was found inside output.
 * @return Characters copied.
 */
template <typename Char>
[[nodiscard]] std::size_t
copy_string(std::uintptr_t address, std::span<Char> output, bool& terminated) noexcept {
    terminated = false;
    std::size_t length = 0;
    while (length < output.size()) {
        const std::uintptr_t at = address + length * sizeof(Char);
        const std::size_t room = (kTextChunkBytes - at % kTextChunkBytes) / sizeof(Char);
        const std::size_t units = std::min(std::max<std::size_t>(room, 1), output.size() - length);
        std::array<Char, kTextChunkBytes> chunk{};
        if (!memory::copy_current_process(
                nullptr, at, std::as_writable_bytes(std::span(chunk).first(units)))) {
            return length;
        }
        for (std::size_t i = 0; i < units; ++i) {
            if (chunk[i] == Char{}) {
                terminated = true;
                return length;
            }
            output[length++] = chunk[i];
        }
    }
    return length;
}

/** Copies the live text of one text widget while its owner block is alive or absent. */
void read_text(const NativeTextBlock& block, wire::DetailSection& output) noexcept {
    const auto lane = static_cast<std::uint8_t>(block.source & kSourceMask);
    const bool dynamic = (lane | kNarrowSource) == (kDynamicSource | kNarrowSource);
    const bool narrow = lane == kNarrowSource;
    const std::uintptr_t text = dynamic ? block.dynamicText : narrow ? block.narrowText : 0;
    const std::uintptr_t owner = dynamic ? block.dynamicOwner : block.narrowOwner;
    NativeTextOwner ownerBlock{};
    if (text == 0 || (owner != 0 && (!copy(owner, ownerBlock) || ownerBlock.dead != 0))) {
        return;
    }
    bool terminated = false;
    std::size_t length = 0;
    // A copy that stops short of the buffer without a NUL faulted, so it reports no text.
    if (dynamic) {
        length = copy_string<char16_t>(text, output.text, terminated);
        if (!terminated && length < output.text.size()) {
            output.text = {};
            return;
        }
    } else {
        std::array<char, wire::kTextCapacity> bytes{};
        const std::size_t count = copy_string<char>(text, bytes, terminated);
        if (!terminated && count < bytes.size()) {
            output.text = {};
            return;
        }
        // UTF-8 never needs more UTF-16 units than bytes, so the output always fits.
        length = count == 0 ? 0
                            : static_cast<std::size_t>(MultiByteToWideChar(
                                  CP_UTF8,
                                  0,
                                  bytes.data(),
                                  static_cast<int>(count),
                                  reinterpret_cast<wchar_t*>(output.text.data()),
                                  static_cast<int>(output.text.size())));
        if (count != 0 && length == 0) {
            output.text = {};
            return;
        }
    }
    output.textLength = static_cast<std::uint16_t>(length);
    output.flags |= wire::detailTextCopied | (terminated ? 0U : wire::detailTextTruncated);
}

/** @return False when a read faults; a row without finite slots keeps rowSlotsRead clear. */
[[nodiscard]] bool
describe_row(std::uintptr_t node, const NativeObject& object, wire::WidgetRow& row) noexcept {
    NodeType type{};
    if (!read_type(object.descriptor, type)) {
        return false;
    }
    row.typeKey = type.key;
    row.hierarchyTag = object.hierarchyTag;
    row.nameHash = object.nameHash;
    row.nodeId = object.nodeId;
    if (!type.widget) {
        return true;
    }
    row.rowFlags |= wire::rowWidget;
    NativeWidget widget{};
    if (!copy(node, widget)) {
        return false;
    }
    row.stateFlags = widget.state;
    std::array<float, kBaseSlots> slots{};
    if (read_slots(widget, slots)) {
        row.x = slots[slotX];
        row.y = slots[slotY];
        row.width = slots[slotWidth];
        row.height = slots[slotHeight];
        row.visibility = slots[slotVisibility];
        row.rowFlags |= wire::rowSlotsRead;
    }
    if (type.text && copy(node + kTextHashOffset, row.textHash)) {
        row.rowFlags |= wire::rowText;
    }
    return true;
}

// --- Walks ---

/** Visitor verdict for one node. */
enum class Step : std::uint8_t { next, stop, fault };

/** One level of the depth-first walk: the child list being read and the parent's row. */
struct WalkFrame final {
    std::uintptr_t head{}, link{};
    std::uint16_t parentRow{};
};

/**
 * Visits root, then its subtree in depth-first pre-order.
 * The visitor receives (node, object, parentRow, row&) and sets row to its output row or kNoRow.
 * @param flags Receives walkTruncated or walkFaulted.
 * @return False when a read faulted.
 */
template <typename Visitor>
[[nodiscard]] bool
walk_nodes(std::uintptr_t root, Visitor& visitor, std::uint32_t& flags) noexcept {
    std::array<WalkFrame, kMaxDepth> stack{};
    NativeObject object{};
    std::uint16_t row = wire::kNoRow;
    if (!copy(root, object)) {
        flags |= wire::walkFaulted;
        return false;
    }
    Step step = visitor(root, object, wire::kNoRow, row);
    std::size_t depth = 0;
    std::size_t visits = 1;
    if (step == Step::next) {
        stack[depth++] = {root + kChildrenOffset, object.children.next, row};
    }
    while (depth > 0 && step == Step::next) {
        WalkFrame& frame = stack[depth - 1];
        if (frame.link == frame.head) {
            --depth;
            continue;
        }
        if (visits == kVisitLimit) {
            flags |= wire::walkTruncated;
            return true;
        }
        const std::uintptr_t node = frame.link - kSiblingOffset;
        if (frame.link == 0 || !copy(node, object)) {
            step = Step::fault;
            break;
        }
        frame.link = object.sibling.next;
        ++visits;
        step = visitor(node, object, frame.parentRow, row);
        if (step != Step::next) {
            break;
        }
        if (depth == kMaxDepth) {
            flags |= wire::walkTruncated;
            continue;
        }
        stack[depth++] = {node + kChildrenOffset, object.children.next, row};
    }
    if (step == Step::fault) {
        flags |= wire::walkFaulted;
        return false;
    }
    return true;
}

/** Emits one tree row per visited node until the rows are full. */
struct TreeVisitor final {
    std::span<wire::WidgetRow> rows;
    CuiTree* tree{};

    /** A full span stops new rows but still counts every node in total. */
    Step operator()(std::uintptr_t node,
                    const NativeObject& object,
                    std::uint16_t parentRow,
                    std::uint16_t& row) noexcept {
        ++tree->total;
        row = wire::kNoRow;
        if (tree->count == rows.size()) {
            tree->flags |= wire::walkTruncated;
            return Step::next;
        }
        wire::WidgetRow output{};
        if (!describe_row(node, object, output)) {
            return Step::fault;
        }
        output.parentRow = parentRow;
        row = static_cast<std::uint16_t>(tree->count);
        rows[tree->count++] = output;
        return Step::next;
    }
};

/** Stops at the first node with the wanted hierarchy tag and id. */
struct NodeFinder final {
    std::uint32_t hierarchy{};
    std::uint16_t id{};
    std::uintptr_t found{};

    /** Node ids are unique within one hierarchy instantiation. */
    Step operator()(std::uintptr_t node,
                    const NativeObject& object,
                    std::uint16_t,
                    std::uint16_t& row) noexcept {
        row = wire::kNoRow;
        if (object.nodeId != id || object.hierarchyTag != hierarchy) {
            return Step::next;
        }
        found = node;
        return Step::stop;
    }
};

/** Open screens in row order, with the native screen of each row. */
struct ScreenList final {
    std::array<wire::ScreenRow, wire::kScreenCapacity> rows{};
    std::array<std::uintptr_t, wire::kScreenCapacity> screens{};
    CuiScreens header{};
};

/**
 * Reads one open screen into a row; the caller sets layer and ordinal.
 * @param next Receives the following list link.
 */
[[nodiscard]] bool
read_screen(std::uintptr_t screen, wire::ScreenRow& row, std::uintptr_t& next) noexcept {
    NativeWidget widget{};
    NativeScreenIdentity identity{};
    std::int32_t priority{};
    NativeLink link{};
    if (!copy(screen, widget) || !copy(screen + offsetof(NativeScreen, identity), identity)
        || !copy(screen + kPriorityOffset, priority) || !copy(screen + kScreenLinkOffset, link)) {
        return false;
    }
    row = {widget.object.hierarchyTag,
           identity.screenTag,
           identity.viewHash,
           identity.page,
           identity.flags,
           widget.state,
           priority};
    next = link.next;
    return true;
}

/** Walks layer 0 then layer 1 from head.next; a screen being destroyed is skipped. */
[[nodiscard]] bool list_screens(ScreenList& list) noexcept {
    auto& header = list.header;
    std::array<NativeLayer, kLayerCount> layers{};
    if (g_manager == 0 || !copy(g_manager + kLayersOffset, layers)) {
        header.flags |= wire::walkFaulted;
        return false;
    }
    for (std::size_t layer = 0; layer < kLayerCount; ++layer) {
        const std::uintptr_t head = g_manager + kLayersOffset + layer * sizeof(NativeLayer);
        std::uintptr_t link = layers[layer].head.next;
        for (std::size_t visits = 0; link != head; ++visits) {
            wire::ScreenRow row{};
            const std::uintptr_t screen = link - kScreenLinkOffset;
            if (visits == kLayerLimit || link == 0 || !read_screen(screen, row, link)) {
                header.flags |= wire::walkFaulted;
                return false;
            }
            if ((row.stateFlags & wire::stateSkipScreen) != 0) {
                continue;
            }
            if (header.count == list.rows.size()) {
                header.flags |= wire::walkTruncated;
                continue;
            }
            const auto first = list.rows.begin();
            const auto same = std::count_if(first, first + header.count, [&](const auto& other) {
                return other.hierarchyTag == row.hierarchyTag;
            });
            row.layer = static_cast<std::uint8_t>(layer);
            row.ordinal = static_cast<std::uint8_t>(same);
            if (screen == layers[layer].focus) {
                (layer == 0 ? header.focus0 : header.focus1) = header.count;
            }
            list.screens[header.count] = screen;
            list.rows[header.count++] = row;
        }
    }
    return true;
}

} // namespace

bool initialize_cui_reader(std::uintptr_t manager, std::uintptr_t ready) noexcept {
    g_manager = manager;
    g_ready = ready;
    return manager != 0 && ready != 0;
}

void clear_cui_reader() noexcept {
    g_manager = 0;
    g_ready = 0;
}

bool cui_manager_ready() noexcept {
    std::uint8_t ready{};
    return g_manager != 0 && copy(g_ready, ready) && ready != 0;
}

bool read_cui_screens(std::span<wire::ScreenRow> rows, CuiScreens& output) noexcept {
    ScreenList list{};
    const bool ok = list_screens(list);
    output = list.header;
    output.count = static_cast<std::uint32_t>(std::min<std::size_t>(output.count, rows.size()));
    std::copy_n(list.rows.begin(), output.count, rows.begin());
    return ok;
}

/** The screen pointer is valid only until the caller returns to the game. */
CuiLookup
find_cui_screen(std::uint32_t hierarchy, std::uint32_t ordinal, std::uintptr_t& screen) noexcept {
    ScreenList list{};
    if (!list_screens(list)) {
        return CuiLookup::faulted;
    }
    for (std::size_t i = 0; i < list.header.count; ++i) {
        if (list.rows[i].hierarchyTag == hierarchy && list.rows[i].ordinal == ordinal) {
            screen = list.screens[i];
            return CuiLookup::found;
        }
    }
    return CuiLookup::absent;
}

/** The header names the walked screen even when the walk itself faults later. */
bool walk_cui_tree(std::uintptr_t screen,
                   std::span<wire::WidgetRow> rows,
                   CuiTree& output) noexcept {
    output = {};
    NativeObject object{};
    NativeScreenIdentity identity{};
    if (!copy(screen, object) || !copy(screen + offsetof(NativeScreen, identity), identity)) {
        output.flags |= wire::walkFaulted;
        return false;
    }
    output.hierarchyTag = object.hierarchyTag;
    output.screenTag = identity.screenTag;
    output.viewHash = identity.viewHash;
    TreeVisitor visitor{rows, &output};
    return walk_nodes(screen, visitor, output.flags);
}

/** A walk that hit its visit limit before the node reports absent. */
CuiLookup find_cui_node(std::uintptr_t screen,
                        std::uint32_t hierarchy,
                        std::uint16_t nodeId,
                        std::uintptr_t& node) noexcept {
    NodeFinder finder{hierarchy, nodeId};
    std::uint32_t flags{};
    if (!walk_nodes(screen, finder, flags)) {
        return CuiLookup::faulted;
    }
    node = finder.found;
    return finder.found != 0 ? CuiLookup::found : CuiLookup::absent;
}

/** Reads the object head, then widget and text fields as the descriptor chain allows. */
bool read_cui_detail(std::uintptr_t node, wire::DetailSection& output) noexcept {
    NativeObject object{};
    NodeType type{};
    if (!copy(node, object) || !read_type(object.descriptor, type)) {
        return false;
    }
    output.typeKey = type.key;
    output.hierarchyTag = object.hierarchyTag;
    output.nameHash = object.nameHash;
    output.nodeId = object.nodeId;
    output.objectFlags = object.flags;
    NativeObject parent{};
    if (object.parent != 0 && !copy(object.parent, parent)) {
        return false;
    }
    output.parentNode = object.parent != 0 ? parent.nodeId : wire::kNoRow;
    std::uintptr_t link = object.children.next;
    std::size_t children = 0;
    for (; link != node + kChildrenOffset && children < kVisitLimit; ++children) {
        NativeLink next{};
        if (link == 0 || !copy(link, next)) {
            return false;
        }
        link = next.next;
    }
    output.childCount = static_cast<std::uint16_t>(children);
    if (!type.widget) {
        return true;
    }
    NativeWidget widget{};
    if (!copy(node, widget)) {
        return false;
    }
    output.stateFlags = widget.state;
    output.slotFlags = widget.slotFlags;
    const std::size_t slots = type.tenSlots ? wire::kSlotCount : kBaseSlots;
    if (read_slots(widget, std::span(output.slots).first(slots))) {
        output.flags |= wire::detailSlotsRead;
    } else {
        output.slots = {};
    }
    NativeText text{};
    if (!type.text || !copy(node + kTextBlockOffset, text.block)
        || !copy(node + kStyleOffset, text.styleKey) || !copy(node + kTextModeOffset, text.mode)) {
        return !type.text;
    }
    output.flags |= wire::detailText;
    output.textHash = text.block.hash;
    output.styleKey = text.styleKey;
    output.textSource = text.block.source;
    output.textState = text.block.state;
    output.textMode = text.mode;
    read_text(text.block, output);
    return true;
}

} // namespace sunrise::client::diagnostics::studio
