#include "console_endpoint.h"

#include <WinSock2.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../core/console/definition.h"
#include "../../core/console/parser/console_line_parse.h"
#include "../../core/console/queue/console_queue.h"
#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "definition.h"
#include "protocol/console_protocol.h"
#include "replies/console_reply_table.h"

namespace sunrise::server::console_endpoint {

namespace console = core::console;
namespace parser = core::console::parser;
namespace queue = core::console::queue;

namespace {

/**
 * Waiting connections the acceptor holds.
 *
 * One is enough. A second connection is taken only so it can be closed, and it has to reach the
 * accept queue for that to happen at all.
 */
constexpr int kBacklog = 1;

/**
 * Submitted lines that may be waiting for their results at once.
 *
 * An entry lives from the submission until the result is written back, so it is only resolvable
 * while the reply table still holds that result. The table keeps `kReplyCapacity` and slides past
 * it, so tracking more than that would be tracking tickets whose results have already been
 * dropped. The console queue is the same depth, which puts this bound and the queue's refusal at
 * the same point rather than one silently shadowing the other.
 */
constexpr std::size_t kPendingCapacity = replies::kReplyCapacity;

/**
 * Bytes the outbound frame buffer holds.
 *
 * Either encoder writes into it, so it takes the wider of the two, plus the one byte this file
 * appends as the frame terminator.
 */
constexpr std::size_t kOutputCapacity =
    (kResponseCapacity > kRegistryCapacity ? kResponseCapacity : kRegistryCapacity) + 1;

/**
 * Correlation id used to answer something that never parsed far enough to carry one.
 *
 * `decode_request` refuses a zero id, so no caller can send one, which makes it unambiguous: a
 * response on this id says "a request arrived and could not be read at all". Two cases reach it and
 * no others — an object with no readable id in it, and a line so long the bytes that carried its id
 * were thrown away before the newline arrived. Anything else that fails to read is answered on the
 * id the caller sent, because `decode_request` hands that back even when it refuses.
 */
constexpr std::uint64_t kUnknownId = 0;

/**
 * Bytes one structured log line here needs.
 *
 * The widest is the startup failure, not the success: `ev=console_endpoint stage=listen
 * result=fail call=ioctlsocket error=-2147483648` is 79 bytes, being 44 of fixed text, the longest
 * call name at 11, and a signed 32-bit error at its widest 11, plus the two field names. Eighty
 * bytes and its terminator is the true floor; 96 is that rounded up so a call name a few characters
 * longer does not silently start cutting the error code off the end of the one line a failed
 * startup produces.
 */
constexpr std::size_t kLogLineCapacity = 96;

/**
 * Milliseconds a half-closed connection keeps being serviced before it is closed regardless.
 *
 * A peer that shut down only its send direction is still owed the answers to what it already sent,
 * and the drain needs a frame or two per line to produce them — so the flag alone would be enough
 * if every caller read what it asked for. What this bounds is the caller that does not: with a full
 * receive window `flush_peer` reports would-block forever, and because one caller is served at a
 * time that connection would hold the endpoint for the rest of the session. The wait is orders of
 * magnitude above what a drain needs and short enough that a human retrying at the keyboard is not
 * waiting on it.
 */
constexpr std::uint64_t kHalfClosedGraceMs = 10000;
/** Maximum time a normal peer may hold the single endpoint without sending a complete request. */
constexpr std::uint64_t kPeerIdleTimeoutMs = 30000;

/** One submitted line, kept until its result is written back. */
struct PendingRequest {
    std::uint64_t id{};
    std::uint64_t ticket{};
};

/** Nonblocking endpoint state, touched only while the lifecycle lock is held. */
struct Endpoint {
    bool active{};
    bool winsockOwned{};
    SOCKET acceptor{INVALID_SOCKET};
    SOCKET peer{INVALID_SOCKET};
    std::array<char, kRequestCapacity> request{};
    std::size_t requestSize{};
    /**
     * Set while the rest of an over-long line is being thrown away.
     *
     * It has to outlive one `recv`. A 4 KiB line arrives across several slices, and a flag reset
     * between them would take the tail of that line for a fresh request and run it.
     */
    bool discarding{};
    /**
     * Set once the peer has shut down its send direction.
     *
     * A zero-length `recv` says the caller will send nothing more, not that it has gone: send, then
     * half-close, then read is the ordinary request/response idiom, and treating it as a death
     * throws away the very answer the caller is waiting on. So the connection stays open, out of
     * the read set, answering what it already accepted, until it owes nothing.
     */
    bool peerClosed{};
    /** Tick past which a half-closed peer is closed even with answers still owed to it. */
    std::uint64_t peerClosedDeadline{};
    /** Tick past which an ordinary idle or partial-line peer releases the single connection. */
    std::uint64_t peerIdleDeadline{};
    std::size_t outputOffset{};
    std::size_t outputSize{};
    std::array<PendingRequest, kPendingCapacity> pending{};
    std::size_t pendingCount{};
};

Endpoint g_endpoint;
/**
 * The one response being written back.
 *
 * File-static rather than a local: a registry listing is 128 KiB, and a frame that size on a game
 * thread is a stack overflow waiting for whatever call happens to sit below it. One buffer also
 * means one response is staged at a time, which is what the slice below is built around.
 */
std::array<char, kOutputCapacity> g_output{};
SRWLOCK g_endpointLock{SRWLOCK_INIT};

/**
 * Tickets this endpoint submitted, newest last, the oldest overwritten in place.
 *
 * The drain reports every ticket it runs, including the lines a player types into the game's own
 * console. Those must not be remembered: the reply table holds exactly as many results as this
 * endpoint may have requests in flight, so a single overlay line stored there slides that table
 * and evicts a result a caller is still waiting for. Nothing in this system times out, so that
 * caller would wait for the rest of the session. This ring is what tells the two apart.
 *
 * `kPendingCapacity` slots is exactly enough and not one more, because no ticket is ever evicted
 * before the drain has reported it. Two things hold that up. The guard in `handle_request` refuses
 * a submission once `pendingCount` reaches `kPendingCapacity`, so at most that many of this
 * endpoint's tickets are unreported at any moment; and the queue is first-in first-out with a
 * single drainer, so tickets are reported in the order they were issued. Submission *N* therefore
 * lands in slot *N* mod `kPendingCapacity`, which last held *N* minus `kPendingCapacity` — and that
 * one cannot still be unreported, or the guard would have refused *N*. Change either premise and
 * this array has to grow with it.
 */
std::array<std::uint64_t, kPendingCapacity> g_submitted{};
/** Slot the next submitted ticket is written to. */
std::size_t g_submittedNext{};
/**
 * Guards the ticket ring, and nothing else.
 *
 * Deliberately not `g_endpointLock`. The observer below runs on the thread that draws frames,
 * while the slice runs on the thread that services the Server and holds `g_endpointLock` across
 * its socket work; an observer waiting on that lock would charge the frame rate for a `select` and
 * a `send`. It cannot decline to wait either, the way `service` skips a busy slice — a dropped
 * observation loses the result for good and hangs the caller. So the one thing the two threads
 * share sits behind a lock of its own.
 *
 * **Never acquire this while holding the console queue's lock.** `submit_tracked` below takes this
 * one and then blocks in `queue::submit` on the queue's, deliberately, to close the publication
 * race; the reverse order on any thread is an ABBA deadlock that hangs the game process. What makes
 * that deliberate order safe is a guarantee the queue gives and writes down — `queue::drain`
 * invokes the observer with **no queue lock held** (`console_queue.h:67-69`,
 * `console_queue.cpp:89-92`) — so the draining thread's `on_drained` takes this lock alone and
 * never nests the two. Hoisting the observer call into a queue critical section would break this
 * from the other side of the tree.
 */
SRWLOCK g_submittedLock{SRWLOCK_INIT};

/**
 * Writes one already-formatted line to the server channel, sized by what fits.
 *
 * The reported count is clamped rather than trusted: `snprintf` returns the length it *would* have
 * written, so a format that outgrew the buffer would otherwise hand the log a span running past the
 * end of it.
 *
 * @param level Severity to record at.
 * @param line Buffer `snprintf` has just written into.
 * @param written Whatever `snprintf` returned.
 */
void write_log_line(core::log::Level level,
                    const std::array<char, kLogLineCapacity>& line,
                    int written) noexcept {
    if (written <= 0) {
        return;
    }
    const auto produced = static_cast<std::size_t>(written);
    const std::size_t length = produced < line.size() ? produced : line.size() - 1;
    core::log::write(core::log::Channel::server, level, {line.data(), length});
}

/**
 * Reports one failed startup call by name.
 *
 * The runtime logs a single `stage=listen result=fail` when this returns false, and that line is
 * the whole diagnostic a keyboard operator gets. It cannot tell a port already held from a firewall
 * or a layered service provider refusing the socket, which are the two likeliest causes and want
 * opposite responses. Naming the call and its error code is what separates them.
 *
 * @param call Winsock function that failed, as it is spelled in the SDK.
 * @param error Its error code, read before any cleanup could overwrite it.
 */
void log_listen_failure(const char* call, int error) noexcept {
    std::array<char, kLogLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=console_endpoint stage=listen result=fail call=%s "
                                      "error=%d",
                                      call,
                                      error);
    write_log_line(core::log::Level::warn, line, written);
}

/** Makes one socket nonblocking. @return True when it can no longer block its caller. */
[[nodiscard]] bool make_nonblocking(SOCKET socket) noexcept {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) != SOCKET_ERROR;
}

/** Forgets every tracked ticket, so no result still running is taken for this endpoint's. */
void forget_submitted() noexcept {
    AcquireSRWLockExclusive(&g_submittedLock);
    g_submitted = {};
    g_submittedNext = 0;
    ReleaseSRWLockExclusive(&g_submittedLock);
}

/**
 * Submits one invocation and claims its ticket before the drain can report it.
 *
 * The claim is written while the lock taken before the submission is still held. A drain that
 * finishes the line first therefore blocks in `on_drained` until the ticket has been claimed,
 * rather than reading a ring that does not name it yet and discarding the result as somebody
 * else's. Publishing the ticket first is not an option: the queue is what issues it.
 *
 * @param invocation Checked invocation to run.
 * @return Its ticket, or `queue::kNoTicket` when the queue refused it.
 */
[[nodiscard]] std::uint64_t submit_tracked(const parser::Invocation& invocation) noexcept {
    AcquireSRWLockExclusive(&g_submittedLock);
    const std::uint64_t ticket = queue::submit(invocation);
    if (ticket != queue::kNoTicket) {
        g_submitted[g_submittedNext] = ticket;
        g_submittedNext = (g_submittedNext + 1) % kPendingCapacity;
    }
    ReleaseSRWLockExclusive(&g_submittedLock);
    return ticket;
}

/**
 * Keeps a drained result when it answers a line this endpoint submitted.
 *
 * Runs on whichever thread drains the console queue, which is the one that draws frames, and may
 * run while the slice is mid-`select` on the other. That is safe because it reads only the ticket
 * ring and writes only the reply table, each behind its own lock, and touches nothing the slice
 * owns. It outliving `shutdown` is safe for the same reason: both live as long as the process.
 *
 * @param ticket Ticket the drain finished.
 * @param result What the handler reported.
 */
void on_drained(std::uint64_t ticket, const console::Result& result) noexcept {
    if (ticket == queue::kNoTicket) {
        return;
    }
    // Hold the ownership lock through publication. Shutdown/close takes it exclusively before
    // clearing replies, so a callback already in flight either publishes before that clear or sees
    // an empty ownership table. It cannot repopulate stale replies afterward.
    AcquireSRWLockShared(&g_submittedLock);
    bool owned = false;
    for (const std::uint64_t known : g_submitted) {
        owned = owned || known == ticket;
    }
    if (owned) {
        static_cast<void>(replies::remember(ticket, result));
    }
    ReleaseSRWLockShared(&g_submittedLock);
}

/** Closes the connection and forgets everything that belonged to it. */
void close_peer() noexcept {
    if (g_endpoint.peer != INVALID_SOCKET) {
        closesocket(g_endpoint.peer);
        g_endpoint.peer = INVALID_SOCKET;
    }
    g_endpoint.requestSize = 0;
    g_endpoint.discarding = false;
    g_endpoint.peerClosed = false;
    g_endpoint.peerClosedDeadline = 0;
    g_endpoint.peerIdleDeadline = 0;
    g_endpoint.outputOffset = 0;
    g_endpoint.outputSize = 0;
    g_endpoint.pendingCount = 0;
    // Results the departed caller asked for answer nobody now. Dropping them here is what keeps
    // the next connection from being handed replies to lines it never sent. The claims go first,
    // not second: once the ring is empty no drain can decide a still-running line was ours, so
    // nothing new can be written into a table that is about to be emptied. What the reverse order
    // left open was a whole two-statement window where the ring still named a ticket the table no
    // longer had room for it.
    forget_submitted();
    replies::clear();
}

/**
 * Takes one waiting connection, or turns a second one away.
 * @return True when a connection was taken, so its first bytes may be asked for at once.
 */
[[nodiscard]] bool accept_connection(std::uint64_t now) noexcept {
    const SOCKET accepted = accept(g_endpoint.acceptor, nullptr, nullptr);
    if (accepted == INVALID_SOCKET) {
        return false;
    }
    if (g_endpoint.peer != INVALID_SOCKET) {
        // One caller at a time: two agents driving the same console would produce a state neither
        // of them could explain. The second is accepted and then closed rather than left waiting
        // in the backlog, because a client blocked on a connect that never completes has nothing
        // to read and no way to learn why.
        closesocket(accepted);
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=console_endpoint stage=accept result=busy");
        return false;
    }
    if (!make_nonblocking(accepted)) {
        closesocket(accepted);
        return false;
    }
    g_endpoint.peer = accepted;
    g_endpoint.requestSize = 0;
    g_endpoint.discarding = false;
    g_endpoint.peerClosed = false;
    g_endpoint.peerClosedDeadline = 0;
    g_endpoint.peerIdleDeadline = now + kPeerIdleTimeoutMs;
    g_endpoint.outputOffset = 0;
    g_endpoint.outputSize = 0;
    g_endpoint.pendingCount = 0;
    // A previous caller may have left results behind. They belong to lines this one never sent.
    // Claims first, results second, for the reason `close_peer` gives.
    forget_submitted();
    replies::clear();
    core::log::write(core::log::Channel::server,
                     core::log::Level::info,
                     "ev=console_endpoint stage=accept result=ok");
    return true;
}

/**
 * Sends at most once from the staged response.
 * @return True while the connection remains usable.
 */
[[nodiscard]] bool flush_peer() noexcept {
    if (g_endpoint.outputSize == 0) {
        return true;
    }
    const std::size_t remaining = g_endpoint.outputSize - g_endpoint.outputOffset;
    const int sent = send(
        g_endpoint.peer, g_output.data() + g_endpoint.outputOffset, static_cast<int>(remaining), 0);
    if (sent > 0) {
        // A nonblocking send takes what fits and no more, and a registry listing is far wider than
        // any socket buffer, so what it did not take is carried into the next slice rather than
        // assumed gone.
        const auto accepted = static_cast<std::size_t>(sent);
        if (accepted > remaining) {
            return false;
        }
        g_endpoint.outputOffset += accepted;
        if (g_endpoint.outputOffset == g_endpoint.outputSize) {
            g_endpoint.outputOffset = 0;
            g_endpoint.outputSize = 0;
        }
        return true;
    }
    // Nothing was ready to leave, which is not a failure. Anything else is.
    return sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK;
}

/**
 * Reads at most once into the bounded request buffer.
 * @param now Monotonic tick count, used to date a half-close.
 */
void receive_peer(std::uint64_t now) noexcept {
    const std::size_t free = kRequestCapacity - g_endpoint.requestSize;
    if (free == 0) {
        return;
    }
    const int received = recv(g_endpoint.peer,
                              g_endpoint.request.data() + g_endpoint.requestSize,
                              static_cast<int>(free),
                              0);
    if (received > 0) {
        g_endpoint.requestSize += static_cast<std::size_t>(received);
        g_endpoint.peerIdleDeadline = now + kPeerIdleTimeoutMs;
        return;
    }
    if (received == 0) {
        // The peer shut down its send direction, which is the ordinary way a client says "that is
        // my whole request" — send, half-close, read. Closing here would destroy the pending entry
        // and the ticket claim for a line already submitted, and the caller would see a clean close
        // and no bytes. The connection stays open instead, out of the read set, until it owes
        // nothing.
        g_endpoint.peerClosed = true;
        g_endpoint.peerClosedDeadline = now + kHalfClosedGraceMs;
        return;
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
        close_peer();
    }
}

/** @return The room an encoder may use, holding back the byte the frame terminator needs. */
[[nodiscard]] std::span<char> encode_span() noexcept {
    return std::span<char>{g_output.data(), g_output.size() - 1};
}

// The `length == 0` branch in `stage_written` below would strand a result `stage_ready_reply` has
// already taken out of the table and un-pended, leaving that id unanswered for the session. An
// encoder reports zero only when its span is under the protocol's own floor, so this is what pins
// that branch unreachable — at compile time, rather than leaving it to whoever next changes a
// capacity to notice.
static_assert(kOutputCapacity - 1 >= protocol::kMinimumCapacity);

/**
 * Frames whatever an encoder just wrote and stages it to be sent.
 * @param length Bytes the encoder reported writing.
 */
void stage_written(std::size_t length) noexcept {
    g_endpoint.outputOffset = 0;
    if (length == 0) {
        // An encoder given too little room writes nothing rather than a fragment, so there is
        // nothing to send. Unreachable at these capacities, and answered with silence rather than
        // with half an object a caller could neither parse nor correlate.
        g_endpoint.outputSize = 0;
        return;
    }
    // The encoders never write a terminator and never write a newline of their own, which is what
    // lets one go here and frame the response. The length is theirs, never a scan of the buffer:
    // the bytes past it are still the tail of whatever longer response came before.
    g_output[length] = '\n';
    g_endpoint.outputSize = length + 1;
}

/**
 * Stages one result as this connection's next response.
 * @param id Correlation id to echo back.
 * @param result Result to report.
 */
void stage_result(std::uint64_t id, const console::Result& result) noexcept {
    std::size_t length = 0;
    protocol::encode_result(id, result, encode_span(), length);
    stage_written(length);
}

/**
 * Stages the whole registry listing as this connection's next response.
 * @param id Correlation id to echo back.
 */
void stage_registry_response(std::uint64_t id) noexcept {
    std::size_t length = 0;
    protocol::encode_registry(id, encode_span(), length);
    stage_written(length);
}

/**
 * Stages one bare status with the sentence that explains it.
 * @param id Correlation id to echo back.
 * @param status Outcome to report.
 * @param summary Sentence a reader of the response sees.
 */
void stage_status(std::uint64_t id, console::Status status, std::string_view summary) noexcept {
    console::Result result{};
    result.status = status;
    console::set_summary(result, summary);
    stage_result(id, result);
}

/** Drops one pending entry, keeping the rest in the order they were submitted in. */
void forget_pending(std::size_t index) noexcept {
    for (std::size_t move = index; move + 1 < g_endpoint.pendingCount; ++move) {
        g_endpoint.pending[move] = g_endpoint.pending[move + 1];
    }
    --g_endpoint.pendingCount;
}

/**
 * Stages the first finished result among the requests still waiting.
 * @return True when one was staged.
 */
[[nodiscard]] bool stage_ready_reply() noexcept {
    for (std::size_t index = 0; index < g_endpoint.pendingCount; ++index) {
        console::Result result{};
        if (!replies::take(g_endpoint.pending[index].ticket, result)) {
            continue;
        }
        const std::uint64_t id = g_endpoint.pending[index].id;
        forget_pending(index);
        stage_result(id, result);
        return true;
    }
    return false;
}

/**
 * Answers or submits one whole request line.
 *
 * Nothing runs here. Every path either writes a response this thread already knows, or hands a
 * checked invocation to the queue for the drain to run somewhere else.
 *
 * @param text One request object, without its terminator.
 */
[[nodiscard]] bool pending_id(std::uint64_t id) noexcept {
    for (std::size_t index = 0; index < g_endpoint.pendingCount; ++index) {
        if (g_endpoint.pending[index].id == id) {
            return true;
        }
    }
    return false;
}

void handle_request(std::string_view text) noexcept {
    protocol::Request request{};
    if (!protocol::decode_request(text, request)) {
        // Answered on the caller's own id whenever the object got far enough to carry one, and on
        // `kUnknownId` only when it did not. A refusal on zero is a refusal nobody can match to
        // what they sent, and with no timeout anywhere that request is simply never resolved.
        stage_status(request.id, console::Status::badArgument, "The request object did not read.");
        return;
    }
    if (pending_id(request.id)) {
        stage_status(request.id,
                     console::Status::refused,
                     "This request id is already waiting for a result.");
        return;
    }
    if (request.describe) {
        // A describe reads the registry and answers on the spot. It runs no handler, so putting it
        // through the queue would cost it a frame and buy nothing.
        stage_registry_response(request.id);
        return;
    }

    const parser::Outcome outcome = parser::parse_line({request.line.data(), request.lineLength});
    if (outcome.status != console::Status::ok) {
        // A line that did not read cannot run, so it never reaches the queue. Answering it here is
        // also what keeps a rejected line from consuming a ticket the caller would wait on.
        console::Result rejected{};
        rejected.status = outcome.status;
        if (outcome.status == console::Status::unknownName && !outcome.requestedName.empty()) {
            // The row the overlay reports for the same failure, so a caller reading either surface
            // is told which name was refused rather than only that one was.
            console::Value named{};
            named.type = console::Type::text;
            console::store_text(outcome.requestedName, named.text, named.textLength);
            static_cast<void>(console::add_row(rejected, "name", named));
        }
        stage_result(request.id, rejected);
        return;
    }
    if (g_endpoint.pendingCount == kPendingCapacity) {
        // Refusing is what makes this bound visible. Submitting anyway and forgetting the ticket
        // would leave the caller waiting on a response that was never going to be written.
        stage_status(request.id,
                     console::Status::refused,
                     "Too many requests are already waiting for their results.");
        return;
    }
    const std::uint64_t ticket = submit_tracked(outcome.invocation);
    if (ticket == queue::kNoTicket) {
        // A full queue is an outcome the caller has to be able to tell apart from a line that ran,
        // so it is reported rather than retried or dropped.
        stage_status(request.id, console::Status::refused, "Too much is already waiting to run.");
        return;
    }
    g_endpoint.pending[g_endpoint.pendingCount] = PendingRequest{request.id, ticket};
    ++g_endpoint.pendingCount;
}

/**
 * Takes at most one whole line out of the request buffer.
 * @return True when a line was taken, so the caller may look for the next one.
 */
[[nodiscard]] bool consume_request() noexcept {
    std::size_t newline = g_endpoint.requestSize;
    for (std::size_t index = 0; index < g_endpoint.requestSize; ++index) {
        if (g_endpoint.request[index] == '\n') {
            newline = index;
            break;
        }
    }
    if (newline == g_endpoint.requestSize) {
        if (g_endpoint.requestSize < kRequestCapacity) {
            return false;
        }
        // Full with no terminator: the line is longer than anything this answers, and the buffer
        // does not grow to meet it. What arrived is dropped and the rest is thrown away as it
        // arrives, so no part of an over-long line is ever taken for a request of its own.
        g_endpoint.requestSize = 0;
        if (g_endpoint.discarding) {
            return false;
        }
        g_endpoint.discarding = true;
        stage_status(kUnknownId,
                     console::Status::badArgument,
                     "The request line is longer than the endpoint accepts.");
        return true;
    }

    if (g_endpoint.discarding) {
        // The tail of an over-long line, which was answered when it overflowed. Only its
        // terminator matters: it is where the next real request begins.
        g_endpoint.discarding = false;
    } else {
        handle_request(std::string_view{g_endpoint.request.data(), newline});
    }
    const std::size_t consumed = newline + 1;
    const std::size_t remaining = g_endpoint.requestSize - consumed;
    for (std::size_t index = 0; index < remaining; ++index) {
        g_endpoint.request[index] = g_endpoint.request[consumed + index];
    }
    g_endpoint.requestSize = remaining;
    return true;
}

} // namespace

/** Binds the loopback console endpoint when the settings enable it. */
bool initialize() noexcept {
    const core::settings::server::ConsoleEndpointSettings& configured =
        core::settings::get().server.consoleEndpoint;
    if (!configured.enabled) {
        // Off is the default, and an endpoint nobody asked for is not a failure. Nothing is bound
        // and this still succeeds, so the runtime does not log a warning about a switch that is
        // simply off.
        return true;
    }
    AcquireSRWLockExclusive(&g_endpointLock);
    if (g_endpoint.active) {
        ReleaseSRWLockExclusive(&g_endpointLock);
        return true;
    }
    // Winsock is reference counted, so starting it here is right even though the BAP listener
    // already did: each owner ends its own reference and neither can pull the floor from the other.
    WSADATA winsock{};
    const int started = WSAStartup(MAKEWORD(2, 2), &winsock);
    if (started != 0) {
        // `WSAStartup` reports through its return value and not through `WSAGetLastError`, which is
        // documented as unusable until a startup has succeeded — so the code logged here is the one
        // it returned.
        log_listen_failure("WSAStartup", started);
        ReleaseSRWLockExclusive(&g_endpointLock);
        return false;
    }
    g_endpoint.winsockOwned = true;
    g_endpoint.acceptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_endpoint.acceptor == INVALID_SOCKET) {
        // Read before the cleanup below, which sets its own last error over this one.
        const int error = WSAGetLastError();
        WSACleanup();
        g_endpoint.winsockOwned = false;
        log_listen_failure("socket", error);
        ReleaseSRWLockExclusive(&g_endpointLock);
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(configured.port);
    // Loopback, never a wildcard bind: whatever the port is set to, nothing off this machine
    // reaches the console through it.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // The interface is local but mutable. Exclusive ownership prevents another local process from
    // sharing or hijacking the configured port while Sunrise holds it.
    BOOL exclusive = TRUE;
    const char* failedCall = nullptr;
    if (setsockopt(g_endpoint.acceptor,
                   SOL_SOCKET,
                   SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive),
                   sizeof exclusive)
        == SOCKET_ERROR) {
        failedCall = "setsockopt";
    } else if (!make_nonblocking(g_endpoint.acceptor)) {
        failedCall = "ioctlsocket";
    } else if (bind(
                   g_endpoint.acceptor, reinterpret_cast<const sockaddr*>(&address), sizeof address)
               == SOCKET_ERROR) {
        failedCall = "bind";
    } else if (listen(g_endpoint.acceptor, kBacklog) == SOCKET_ERROR) {
        failedCall = "listen";
    }
    if (failedCall != nullptr) {
        // Read before the teardown, which would replace it with its own.
        const int error = WSAGetLastError();
        closesocket(g_endpoint.acceptor);
        g_endpoint.acceptor = INVALID_SOCKET;
        WSACleanup();
        g_endpoint.winsockOwned = false;
        log_listen_failure(failedCall, error);
        ReleaseSRWLockExclusive(&g_endpointLock);
        return false;
    }
    g_endpoint.active = true;
    // Nothing runs on this thread, so this is the only way a result ever comes back. Registered
    // after the bind rather than before it, so an endpoint that failed to listen leaves the queue
    // reporting to nobody, and a disabled one never reaches here at all.
    queue::observe(&on_drained);
    std::array<char, kLogLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=console_endpoint stage=listen result=ok port=%u",
                                      static_cast<unsigned>(configured.port));
    write_log_line(core::log::Level::info, line, written);
    ReleaseSRWLockExclusive(&g_endpointLock);
    return true;
}

/** Runs one bounded endpoint slice on the caller thread. */
void service(std::uint64_t now) noexcept {
    // No poll interval, which is where this parts company with `bap_listener`. That listener owes
    // the Server periodic pushes and throttles them to `kServiceIntervalMs`; this is a
    // request/response loop where a caller is blocked on every line it sends, so an interval here
    // would be latency charged to every command rather than work saved. The tick is read only to
    // date the grace a half-closed connection gets.
    if (!TryAcquireSRWLockExclusive(&g_endpointLock)) {
        return;
    }
    if (!g_endpoint.active) {
        ReleaseSRWLockExclusive(&g_endpointLock);
        return;
    }

    fd_set readable;
    fd_set writable;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    // The acceptor stays in the read set even while a caller is connected: a second connection has
    // to be taken before it can be turned away.
    FD_SET(g_endpoint.acceptor, &readable);
    if (g_endpoint.peer != INVALID_SOCKET) {
        // A half-closed peer will never send again, so asking would only ever return the same zero.
        // Leaving it out of the read set is also what makes the wind-down finite: nothing more can
        // enter the request buffer, so the work still owed can only shrink.
        if (!g_endpoint.peerClosed && g_endpoint.requestSize < kRequestCapacity) {
            FD_SET(g_endpoint.peer, &readable);
        }
        if (g_endpoint.outputSize != 0) {
            FD_SET(g_endpoint.peer, &writable);
        }
    }
    timeval timeout{};
    if (select(0, &readable, &writable, nullptr, &timeout) == SOCKET_ERROR) {
        ReleaseSRWLockExclusive(&g_endpointLock);
        return;
    }

    bool justAccepted = false;
    if (FD_ISSET(g_endpoint.acceptor, &readable)) {
        justAccepted = accept_connection(now);
    }
    if (g_endpoint.peer == INVALID_SOCKET) {
        ReleaseSRWLockExclusive(&g_endpointLock);
        return;
    }
    // What was staged before this slice leaves first. One response is staged at a time, so the
    // buffer has to empty before anything new can be written into it.
    if (g_endpoint.outputSize != 0 && FD_ISSET(g_endpoint.peer, &writable) && !flush_peer()) {
        close_peer();
        ReleaseSRWLockExclusive(&g_endpointLock);
        return;
    }
    // A connection taken in this slice was not in the read set, so its first bytes are asked for
    // directly rather than a frame later.
    if (!g_endpoint.peerClosed && (justAccepted || FD_ISSET(g_endpoint.peer, &readable))) {
        receive_peer(now);
    }
    if (g_endpoint.peer == INVALID_SOCKET) {
        ReleaseSRWLockExclusive(&g_endpointLock);
        return;
    }

    // Replies before requests. A result the drain finished before this slice would otherwise wait
    // a whole frame, while a request read in this slice cannot have a result yet — so reading
    // first would buy nothing and cost that frame. The loop stops as soon as a response is staged,
    // and every turn either stages one or takes a whole line out of a buffer that only shrinks, so
    // it cannot spin.
    while (g_endpoint.outputSize == 0) {
        if (stage_ready_reply()) {
            break;
        }
        if (!consume_request()) {
            break;
        }
    }

    // A response produced in this slice goes out in this slice. `select` reported writability
    // before that response existed, so this asks the socket itself and reads a would-block as
    // "the rest next time".
    if (g_endpoint.outputSize != 0 && !flush_peer()) {
        close_peer();
        ReleaseSRWLockExclusive(&g_endpointLock);
        return;
    }
    if (!g_endpoint.peerClosed && now >= g_endpoint.peerIdleDeadline) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=console_endpoint stage=peer result=idle_timeout");
        close_peer();
        ReleaseSRWLockExclusive(&g_endpointLock);
        return;
    }
    if (g_endpoint.peerClosed
        && ((g_endpoint.pendingCount == 0 && g_endpoint.outputSize == 0)
            || now >= g_endpoint.peerClosedDeadline)) {
        // Everything the caller was owed has left, or it stopped reading long enough that nothing
        // more is going to. Either way this is where the half-close finally becomes a close, and
        // where the port is handed back to the next caller.
        close_peer();
    }
    ReleaseSRWLockExclusive(&g_endpointLock);
}

/** Closes the endpoint's sockets. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_endpointLock);
    if (!g_endpoint.active) {
        ReleaseSRWLockExclusive(&g_endpointLock);
        return;
    }
    g_endpoint.active = false;
    // Withdrawn first, so nothing reported after this point is stored for a caller that is going
    // away. A drain already past the read keeps a valid pointer: `on_drained` touches only the two
    // file-static tables below, never the socket state this goes on to tear down.
    queue::observe(nullptr);
    if (g_endpoint.acceptor != INVALID_SOCKET) {
        closesocket(g_endpoint.acceptor);
        g_endpoint.acceptor = INVALID_SOCKET;
    }
    // This also drops the reply table, so nothing survives that was addressed to a caller the
    // endpoint can no longer answer.
    close_peer();
    if (g_endpoint.winsockOwned) {
        WSACleanup();
        g_endpoint.winsockOwned = false;
    }
    ReleaseSRWLockExclusive(&g_endpointLock);
}

} // namespace sunrise::server::console_endpoint
