#pragma once

/** @file reader.hpp
 *  @brief The consumer side: raw record recovery (SegmentReader) and typed
 *         decoding (Decoder). Reads files; never talks to a producer.
 *
 *  Recovery policy is tolerate-corrupted-tail per chunk
 *  (docs/framing-and-recovery.md): a zero or untagged head word ends that
 *  chunk's decode, the remainder is counted, and reading resumes at the next
 *  chunk boundary. The unreadable count is part of the result, not a side
 *  print (R3.3, R9.2).
 */

#include "severity.hpp"
#include "wire.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace sub0log {

// ---------------------------------------------------------------------------
// Raw layer

/// A committed record as found in a chunk. Payload bytes are a view into the
/// caller's buffer; nothing is copied at this layer.
struct RecordView {
    wire::RecordHead head_{};
    std::span<const std::byte> payload_{};
    std::uint32_t chunkIndex_{};
    std::uint64_t ownerThread_{};
};

/// Why a segment could not be opened at all (damage *within* a valid segment
/// is not an error; it is counted).
enum class SegmentError : std::uint8_t {
    Ok = 0,
    TooSmall,
    BadMagic,
    UnknownVersion,   ///< Version gates everything after it (Kafka lesson).
    BadGeometry,      ///< Header geometry inconsistent with the file size.
};

/** Walks a segment image chunk by chunk, yielding committed records in
 *  chunk order (per-thread order; cross-thread order is the Merger's job).
 *
 *  Contract:
 *   - validate the SegmentHeader before touching any chunk; geometry from
 *     the header, never from defaults;
 *   - skip, and count in full, any chunk whose generation_ differs from the
 *     segment's (R3.4: positive evidence, not zero-fill inference);
 *   - per record: acquire-load the head word; stop the chunk on a missing
 *     commit tag; bounds-check payloadBytes against the chunk's remainder
 *     before touching the payload -- no length may steer the reader outside
 *     the chunk it was found in;
 *   - separate the two reasons a byte is not part of a record: space that
 *     was never written (a pre-sized segment is mostly this) versus bytes
 *     that could not be interpreted (damage). Only the second answers
 *     "how much did I lose?".
 */
class SegmentReader {
public:
    /// On failure the result has valid() false and error() saying why.
    [[nodiscard]] static SegmentReader open(std::span<const std::byte> image) noexcept;

    [[nodiscard]] bool valid() const noexcept { return error_ == SegmentError::Ok; }
    [[nodiscard]] SegmentError error() const noexcept { return error_; }

    [[nodiscard]] const wire::SegmentHeader& header() const noexcept { return header_; }

    /// Visits every committed record. Returns the damage count (also
    /// available afterwards via unreadableBytes()).
    ///
    /// The callback is a template parameter rather than a std::function so
    /// it inlines and never allocates for its captures: this runs once per
    /// record over a whole segment, and it is the streaming interface the
    /// typed layer is built on. Repeatable -- each call re-walks from the
    /// first chunk and resets the counters.
    template <typename OnRecord>
    std::uint64_t visit(OnRecord&& onRecord);

    /** Bytes that could not be interpreted: a torn record, a length that
     *  would leave its chunk, a chunk left by a previous generation, or a
     *  tail the file no longer has. This is the damage number, and on a
     *  healthy segment it is zero however little of the segment is used.
     *
     *  It deliberately excludes space that was simply never written --
     *  see unwrittenBytes(). Counting the two together, as this reader
     *  first did, makes the number useless for the question R3.3 asks:
     *  a fresh 8 MiB segment holding four records reported 8.3 million
     *  "unreadable" bytes, which reads as catastrophe and means nothing.
     */
    [[nodiscard]] std::uint64_t unreadableBytes() const noexcept
    {
        return unreadableBytes_;
    }

    /// Space the producer never wrote: chunks never claimed, and the unused
    /// remainder of chunks that were. Expected, not damage -- a segment is
    /// pre-sized, so this is normally most of it.
    [[nodiscard]] std::uint64_t unwrittenBytes() const noexcept
    {
        return unwrittenBytes_;
    }

    /// Every byte not part of a committed record, whatever the reason.
    [[nodiscard]] std::uint64_t accountedBytes() const noexcept
    {
        return unreadableBytes_ + unwrittenBytes_;
    }

private:
    SegmentReader() noexcept = default;

    std::span<const std::byte> image_{};
    wire::SegmentHeader header_{};
    SegmentError error_{SegmentError::TooSmall}; ///< Ok once open() validates.
    std::uint64_t unreadableBytes_{0};
    std::uint64_t unwrittenBytes_{0};
};

// ---------------------------------------------------------------------------
// Typed layer

/// An argument as the producer typed it (R2.1): an integer arrives as an
/// integer. Bytes/string views point into the segment image.
using DecodedArg = std::variant<bool, char, std::int64_t, std::uint64_t,
                                double, std::string_view>;

/// The constant half, reassembled from a SiteDefinition record.
struct DecodedSite {
    std::uint64_t siteId_{};
    SubsystemId subsystem_{};
    Severity severity_{};
    std::uint32_t line_{};
    std::string_view format_{};
    std::string_view file_{};
    std::vector<wire::TypeCode> argTypes_{};
};

/// One message, typed. Filterable on every field without touching text
/// (R2.2); a test asserts on fields, not substrings (R2.3).
struct DecodedRecord {
    const DecodedSite* site_{nullptr};
    std::uint64_t monoNs_{};
    std::uint64_t correlationId_{};
    std::uint64_t ownerThread_{};
    bool truncated_{};
    std::vector<DecodedArg> args_{};
};

/** Builds the site table from definitions as they arrive (they precede first
 *  use by construction) and turns Message records into DecodedRecords.
 *
 *  Site ids are scoped to one segment: one Decoder per segment.
 *
 *  **Lifetime, and the mistake it invites.** Nothing here copies: a
 *  DecodedSite's format_ and file_, every string argument on a
 *  DecodedRecord, and every name subsystemName() returns are views into the
 *  image passed to decodeAll's reader. The site table (and the subsystem
 *  name table beside it) additionally *keeps* what it parsed -- a site or a
 *  subsystem already known is not re-parsed -- so re-using one Decoder
 *  across a re-read into a different buffer leaves its earlier sites (and
 *  names) pointing into a buffer that no longer exists. A tailer that
 *  re-reads a growing segment must therefore either keep one image buffer
 *  alive and only grow it, or build a fresh Decoder for each fresh image.
 *  This cost an example author a segfault, so it is written down rather
 *  than left to be rediscovered.
 *
 *  Formatting is the one place text is made, and only on request: format()
 *  renders via std::format-style substitution of "{}" placeholders. A record
 *  whose definition is missing (pre-truncation damage) is counted
 *  undecodable, never dropped silently (R9.2).
 */
class Decoder {
public:
    /// Consumes every RecordView from a SegmentReader::visit pass, in order:
    /// definitions update the table, Message records become DecodedRecords.
    /// Damage never fails the call -- it is counted (unreadable bytes on the
    /// reader, undecodable records here).
    [[nodiscard]] std::vector<DecodedRecord> decodeAll(SegmentReader& reader);

    [[nodiscard]] const DecodedSite* siteFor(std::uint64_t siteId) const noexcept;

    /// The name a SubsystemDefinition record gave this subsystem, or an
    /// empty view when this segment never declared one -- the honest answer
    /// for a segment produced before subsystem declarations existed, rather
    /// than an invented placeholder. A duplicate declaration keeps the
    /// first, exactly as a duplicate SiteDefinition does.
    [[nodiscard]] std::string_view subsystemName(SubsystemId subsystem) const noexcept;

    /// Records whose site definition never appeared (damage upstream).
    [[nodiscard]] std::uint64_t undecodableRecords() const noexcept
    {
        return undecodable_;
    }

    /// Committed records this typed layer has no shape for yet -- the child
    /// kinds, Continuation and Blob. They are intact on the wire and
    /// readable through SegmentReader::visit; they are simply not Messages.
    /// Counted rather than passed over in silence, so "my records are
    /// missing" has an answer (R9.2).
    [[nodiscard]] std::uint64_t skippedRecords() const noexcept
    {
        return skipped_;
    }

    /// Renders one record's text: the site's format string with each "{}"
    /// replaced by the corresponding argument.
    [[nodiscard]] static std::string format(const DecodedRecord& record);

private:
    /// Parses a SiteDefinition payload, bounds-checking every embedded
    /// length against `payload` before it is used. Returns false (definition
    /// malformed, counted undecodable by the caller) rather than throwing.
    [[nodiscard]] static bool parseSiteDefinition(std::span<const std::byte> payload,
                                                   DecodedSite& out);

    /// Parses a SubsystemDefinition payload, bounds-checking the embedded
    /// name length against `payload` before it is used. Returns false
    /// (malformed, counted undecodable by the caller) rather than throwing
    /// -- the same contract as parseSiteDefinition.
    [[nodiscard]] static bool parseSubsystemDefinition(std::span<const std::byte> payload,
                                                        SubsystemId& id, std::string_view& name);

    /// Decodes one fixed-size argument at `p`. `code` must not be Bytes or
    /// Invalid, and the caller must have bounds-checked fixedSizeOf(code)
    /// bytes at `p` already.
    [[nodiscard]] static DecodedArg decodeFixedArg(wire::TypeCode code, const std::byte* p) noexcept;

    /// Appends one argument's text form to `out` (format()'s substitution).
    static void appendArgText(std::string& out, const DecodedArg& arg);

    std::unordered_map<std::uint64_t, DecodedSite> sites_{};
    std::unordered_map<std::uint32_t, std::string_view> subsystemNames_{};
    std::uint64_t undecodable_{0};
    std::uint64_t skipped_{0};
};

// ---------------------------------------------------------------------------
// SegmentReader implementation

inline SegmentReader SegmentReader::open(std::span<const std::byte> image) noexcept
{
    SegmentReader reader;
    reader.image_ = image;

    // Order matters: size before any field is read, magic before version,
    // version before geometry -- version gates how everything after it is
    // interpreted (the Kafka lesson, docs/framing-and-recovery.md).
    if (image.size() < wire::cSegmentHeaderBytes) {
        reader.error_ = SegmentError::TooSmall;
        return reader;
    }

    reader.header_ = wire::loadUnaligned<wire::SegmentHeader>(image.data());
    const wire::SegmentHeader& h = reader.header_;

    if (h.magic_ != wire::cMagic) {
        reader.error_ = SegmentError::BadMagic;
        return reader;
    }
    if (h.formatVersion_ != wire::cFormatVersion) {
        reader.error_ = SegmentError::UnknownVersion;
        return reader;
    }

    // Geometry is data, not convention (docs/architecture.md): trust the
    // file's own fields, checked for internal sanity and for consistency
    // with the image actually handed to us.
    const bool geometrySane =
        h.headerBytes_ >= sizeof(wire::SegmentHeader) &&
        h.chunkBytes_ > sizeof(wire::ChunkHeader) &&
        h.segmentBytes_ >= h.headerBytes_ &&
        h.headerBytes_ <= image.size();
    if (!geometrySane) {
        reader.error_ = SegmentError::BadGeometry;
        return reader;
    }

    reader.error_ = SegmentError::Ok;
    return reader;
}

template <typename OnRecord>
std::uint64_t SegmentReader::visit(OnRecord&& onRecord)
{
    // Reset so repeated calls are idempotent (the contract requires it).
    unreadableBytes_ = 0;
    unwrittenBytes_ = 0;
    if (!valid()) {
        return unreadableBytes_;
    }

    const std::uint64_t imageBytes = image_.size();
    const std::uint64_t segEnd = header_.segmentBytes_;
    std::uint64_t chunkStart = header_.headerBytes_;
    std::uint32_t chunkIndex = 0;

    while (chunkStart < segEnd) {
        if (chunkStart >= imageBytes) {
            // Everything from here to the declared end of the segment is
            // physically absent from the image we were handed -- a
            // truncated tail (R3.3). Counted in one arithmetic step rather
            // than walked chunk by chunk, so an untrusted segmentBytes_
            // cannot turn this into an unbounded loop.
            unreadableBytes_ += (segEnd - chunkStart);
            break;
        }

        const std::uint64_t nominalEnd = chunkStart + header_.chunkBytes_;
        const std::uint64_t chunkLogicalEnd = std::min(nominalEnd, segEnd);
        const std::uint64_t chunkNominalBytes = chunkLogicalEnd - chunkStart;
        const std::uint64_t presentEnd = std::min(chunkLogicalEnd, imageBytes);
        const std::uint64_t availableInChunk = presentEnd - chunkStart;
        const std::uint64_t missingTail = chunkNominalBytes - availableInChunk;

        if (availableInChunk < sizeof(wire::ChunkHeader)) {
            // Not even the chunk header survived the truncation: nothing in
            // this chunk is usable evidence, count the whole nominal chunk.
            unreadableBytes_ += chunkNominalBytes;
            chunkStart = nominalEnd;
            ++chunkIndex;
            continue;
        }

        const std::byte* const chunkPtr = image_.data() + chunkStart;
        const wire::ChunkHeader chunkHead = wire::loadUnaligned<wire::ChunkHeader>(chunkPtr);

        if (chunkHead.generation_ == 0u) {
            // Never claimed: a segment is created at full size, so this is
            // simply the part of it the producer had not reached. Nothing
            // was lost here, and calling it damage would drown the number
            // that matters.
            unwrittenBytes_ += chunkNominalBytes;
            chunkStart = nominalEnd;
            ++chunkIndex;
            continue;
        }

        if (chunkHead.generation_ != header_.generation_) {
            // A previous generation's chunk in recycled storage: positive
            // evidence, not zero-fill inference (R3.4). The header was
            // legible, the body is not ours to read.
            unreadableBytes_ += (availableInChunk - sizeof(wire::ChunkHeader)) + missingTail;
            chunkStart = nominalEnd;
            ++chunkIndex;
            continue;
        }

        const std::uint64_t bodyStart = chunkStart + sizeof(wire::ChunkHeader);
        const std::uint64_t bodyEnd = chunkStart + availableInChunk;
        std::uint64_t cursor = bodyStart;

        while (cursor < bodyEnd) {
            const std::uint64_t remaining = bodyEnd - cursor;
            if (remaining < sizeof(std::uint64_t)) {
                // Fewer than 8 bytes left: no head word can fit here, so
                // this is padding the writer could never have used.
                unwrittenBytes_ += remaining;
                break;
            }

            const std::uint64_t word = wire::loadUnaligned<std::uint64_t>(image_.data() + cursor);

            // The producer release-stores this word after its payload
            // (chunk.hpp, "commit last"); a reader that does not acquire
            // may observe the tag while the payload loads it guards are
            // hoisted above it. That is not theoretical here: the live-tail
            // path reads a segment a producer is still writing. The fence
            // pairs with that release and costs nothing per record on x86.
            std::atomic_thread_fence(std::memory_order_acquire);

            if (!wire::RecordHead::isCommitted(word)) {
                // The zero/non-zero split is the whole distinction: a zero
                // word is the chunk's unwritten remainder, while a non-zero
                // word without the commit tag is a record whose payload
                // landed and whose head word did not -- the torn write the
                // commit-last protocol exists to make visible. Either way
                // decoding of this chunk stops here.
                if (word == 0u) {
                    unwrittenBytes_ += remaining;
                } else {
                    unreadableBytes_ += remaining;
                }
                break;
            }

            const wire::RecordHead head = wire::RecordHead::unpack(word);
            const std::uint64_t afterHead = cursor + sizeof(std::uint64_t);
            const std::uint64_t spaceLeft = bodyEnd - afterHead;

            if (head.payloadBytes_ > spaceLeft) {
                // A bad length must never steer the reader outside the
                // chunk it was found in: treat it exactly like a torn
                // record and stop here, before touching the payload.
                unreadableBytes_ += remaining;
                break;
            }

            const RecordView view{
                head,
                std::span<const std::byte>(image_.data() + afterHead, head.payloadBytes_),
                chunkIndex,
                chunkHead.ownerThread_,
            };
            onRecord(view);

            cursor = afterHead + wire::paddedPayload(head.payloadBytes_);
        }

        unreadableBytes_ += missingTail;
        chunkStart = nominalEnd;
        ++chunkIndex;
    }

    return unreadableBytes_;
}

// ---------------------------------------------------------------------------
// Decoder implementation

inline bool Decoder::parseSiteDefinition(std::span<const std::byte> payload, DecodedSite& out)
{
    if (payload.size() < sizeof(wire::SiteDefinitionPayload)) {
        return false;
    }
    const auto prefix = wire::loadUnaligned<wire::SiteDefinitionPayload>(payload.data());
    std::size_t offset = sizeof(wire::SiteDefinitionPayload);

    const std::size_t argCount = prefix.argCount_;
    if (argCount > payload.size() - offset) {
        return false;
    }
    std::vector<wire::TypeCode> argTypes(argCount);
    for (std::size_t i = 0; i < argCount; ++i) {
        argTypes[i] = static_cast<wire::TypeCode>(std::to_integer<std::uint8_t>(payload[offset + i]));
    }
    offset += argCount;

    if (sizeof(std::uint16_t) > payload.size() - offset) {
        return false;
    }
    const std::uint16_t formatLen = wire::loadUnaligned<std::uint16_t>(payload.data() + offset);
    offset += sizeof(std::uint16_t);
    if (formatLen > payload.size() - offset) {
        return false;
    }
    const std::string_view format(reinterpret_cast<const char*>(payload.data() + offset), formatLen);
    offset += formatLen;

    if (sizeof(std::uint16_t) > payload.size() - offset) {
        return false;
    }
    const std::uint16_t fileLen = wire::loadUnaligned<std::uint16_t>(payload.data() + offset);
    offset += sizeof(std::uint16_t);
    if (fileLen > payload.size() - offset) {
        return false;
    }
    const std::string_view file(reinterpret_cast<const char*>(payload.data() + offset), fileLen);

    out.siteId_ = prefix.siteId_;
    out.subsystem_ = SubsystemId{prefix.subsystemId_};
    out.severity_ = static_cast<Severity>(prefix.severity_);
    out.line_ = prefix.line_;
    out.format_ = format;
    out.file_ = file;
    out.argTypes_ = std::move(argTypes);
    return true;
}

inline bool Decoder::parseSubsystemDefinition(std::span<const std::byte> payload, SubsystemId& id,
                                              std::string_view& name)
{
    if (payload.size() < sizeof(wire::SubsystemDefinitionPayload)) {
        return false;
    }
    const auto prefix = wire::loadUnaligned<wire::SubsystemDefinitionPayload>(payload.data());
    const std::size_t offset = sizeof(wire::SubsystemDefinitionPayload);
    if (prefix.nameLen_ > payload.size() - offset) {
        return false;
    }
    id = SubsystemId{prefix.subsystemId_};
    name = std::string_view(reinterpret_cast<const char*>(payload.data() + offset), prefix.nameLen_);
    return true;
}

inline DecodedArg Decoder::decodeFixedArg(const wire::TypeCode code, const std::byte* const p) noexcept
{
    switch (code) {
    case wire::TypeCode::Bool:
        return DecodedArg{wire::loadUnaligned<std::uint8_t>(p) != 0u};
    case wire::TypeCode::Char:
        return DecodedArg{static_cast<char>(wire::loadUnaligned<std::uint8_t>(p))};
    case wire::TypeCode::I8:
        return DecodedArg{static_cast<std::int64_t>(wire::loadUnaligned<std::int8_t>(p))};
    case wire::TypeCode::U8:
        return DecodedArg{static_cast<std::uint64_t>(wire::loadUnaligned<std::uint8_t>(p))};
    case wire::TypeCode::I16:
        return DecodedArg{static_cast<std::int64_t>(wire::loadUnaligned<std::int16_t>(p))};
    case wire::TypeCode::U16:
        return DecodedArg{static_cast<std::uint64_t>(wire::loadUnaligned<std::uint16_t>(p))};
    case wire::TypeCode::I32:
        return DecodedArg{static_cast<std::int64_t>(wire::loadUnaligned<std::int32_t>(p))};
    case wire::TypeCode::U32:
        return DecodedArg{static_cast<std::uint64_t>(wire::loadUnaligned<std::uint32_t>(p))};
    case wire::TypeCode::I64:
        return DecodedArg{wire::loadUnaligned<std::int64_t>(p)};
    case wire::TypeCode::U64:
        return DecodedArg{wire::loadUnaligned<std::uint64_t>(p)};
    case wire::TypeCode::F32:
        return DecodedArg{static_cast<double>(wire::loadUnaligned<float>(p))};
    case wire::TypeCode::F64:
        return DecodedArg{wire::loadUnaligned<double>(p)};
    case wire::TypeCode::Pointer:
        return DecodedArg{wire::loadUnaligned<std::uint64_t>(p)};
    case wire::TypeCode::Bytes:
    case wire::TypeCode::Invalid:
        break;
    }
    // Unreachable via the bounds-checked caller (Bytes/Invalid never route
    // here); a defined fallback keeps this function total regardless.
    return DecodedArg{std::uint64_t{0}};
}

inline std::vector<DecodedRecord> Decoder::decodeAll(SegmentReader& reader)
{
    // Two streaming passes, not one pass into a buffer. The buffer version
    // held a RecordView for every record in the segment before decoding
    // any of them -- several times the segment's own size in peak memory,
    // for a walk that visit() already streams. Two passes over mapped
    // memory cost a second walk and nothing else.
    //
    // Two passes rather than one because file order is not definition
    // order: threads claim chunks independently, so one thread's Message
    // can sit in an earlier chunk than another thread's SiteDefinition.
    // Every definition is therefore known before any message is decoded.
    std::size_t messageCount = 0;
    reader.visit([&](const RecordView& v) {
        switch (v.head_.kind_) {
        case wire::RecordKind::SiteDefinition: {
            DecodedSite site;
            if (!parseSiteDefinition(v.payload_, site)) {
                ++undecodable_;
                return;
            }
            // Duplicate siteId: keep the first (try_emplace no-ops otherwise).
            sites_.try_emplace(site.siteId_, std::move(site));
            return;
        }
        case wire::RecordKind::SubsystemDefinition: {
            SubsystemId id{};
            std::string_view name;
            if (!parseSubsystemDefinition(v.payload_, id, name)) {
                ++undecodable_;
                return;
            }
            // Duplicate id: keep the first, exactly as a duplicate
            // SiteDefinition is handled above.
            subsystemNames_.try_emplace(id.value_, name);
            return;
        }
        case wire::RecordKind::Message:
            ++messageCount;
            return;
        // Named rather than left to `default`, which would cover them
        // identically, because a consumer compiling this header with
        // -Wswitch-enum gets a warning out of it otherwise -- and a
        // header-only library that cannot be included quietly under a
        // strict warning set has made its own problem theirs.
        //
        // `default` stays regardless, and it is the load-bearing arm: a
        // kind this decoder has never heard of is exactly what lets the
        // format grow (a SubsystemDefinition read by a decoder built before
        // it existed lands here, counted rather than treated as damage).
        // kind_ is a uint8_t read off a file, so a value no enumerator
        // names is a real input, not a hypothetical.
        //
        // Which means the two warnings cannot both be satisfied:
        // -Wswitch-enum wants every enumerator listed, and clang's
        // -Wcovered-switch-default then calls the surviving `default`
        // redundant -- on the strength of an assumption a wire format
        // breaks. Listing them is the better half of that trade because
        // -Wswitch-enum turns up in real warning sets and
        // -Wcovered-switch-default essentially only inside -Weverything,
        // which clang itself advises against shipping with.
        case wire::RecordKind::Invalid:
        case wire::RecordKind::Continuation:
        case wire::RecordKind::Blob:
        case wire::RecordKind::ChildOutput:
        case wire::RecordKind::ChildStart:
        case wire::RecordKind::ChildExit:
        default:
            ++skipped_; // intact, just not a shape this layer decodes
            return;
        }
    });

    std::vector<DecodedRecord> result;
    result.reserve(messageCount); // exact: pass one counted them

    // Pass two: every Message becomes a DecodedRecord, or is counted
    // undecodable -- a missing site or a malformed argument never throws
    // and never drops silently (R9.2).
    reader.visit([&](const RecordView& v) {
        if (v.head_.kind_ != wire::RecordKind::Message) {
            return;
        }
        if (v.payload_.size() < sizeof(wire::MessagePayload)) {
            ++undecodable_;
            return;
        }
        const auto prefix = wire::loadUnaligned<wire::MessagePayload>(v.payload_.data());
        const DecodedSite* const site = siteFor(prefix.siteId_);
        if (site == nullptr) {
            ++undecodable_;
            return;
        }

        std::vector<DecodedArg> args;
        args.reserve(site->argTypes_.size());
        std::size_t offset = sizeof(wire::MessagePayload);
        bool ok = true;

        for (const wire::TypeCode code : site->argTypes_) {
            if (code == wire::TypeCode::Bytes) {
                if (sizeof(std::uint16_t) > v.payload_.size() - offset) {
                    ok = false;
                    break;
                }
                const std::uint16_t len =
                    wire::loadUnaligned<std::uint16_t>(v.payload_.data() + offset);
                offset += sizeof(std::uint16_t);
                if (len > v.payload_.size() - offset) {
                    ok = false;
                    break;
                }
                args.emplace_back(std::string_view(
                    reinterpret_cast<const char*>(v.payload_.data() + offset), len));
                offset += len;
                continue;
            }

            const std::uint32_t sz = wire::fixedSizeOf(code);
            if (sz == 0u || sz > v.payload_.size() - offset) {
                ok = false;
                break;
            }
            args.push_back(decodeFixedArg(code, v.payload_.data() + offset));
            offset += sz;
        }

        if (!ok) {
            ++undecodable_;
            return;
        }

        DecodedRecord rec;
        rec.site_ = site;
        rec.monoNs_ = prefix.monoNs_;
        rec.correlationId_ = prefix.correlationId_;
        rec.ownerThread_ = v.ownerThread_;
        rec.truncated_ = (v.head_.flags_ & wire::cFlagTruncated) != 0u;
        rec.args_ = std::move(args);
        result.push_back(std::move(rec));
    });

    return result;
}

inline const DecodedSite* Decoder::siteFor(std::uint64_t siteId) const noexcept
{
    const auto it = sites_.find(siteId);
    return it == sites_.end() ? nullptr : &it->second;
}

inline std::string_view Decoder::subsystemName(const SubsystemId subsystem) const noexcept
{
    const auto it = subsystemNames_.find(subsystem.value_);
    return it == subsystemNames_.end() ? std::string_view{} : it->second;
}

inline void Decoder::appendArgText(std::string& out, const DecodedArg& arg)
{
    std::visit(
        [&out](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, bool>) {
                out += value ? "true" : "false";
            } else if constexpr (std::is_same_v<T, char>) {
                out.push_back(value);
            } else if constexpr (std::is_same_v<T, std::string_view>) {
                out.append(value);
            } else {
                // int64_t, uint64_t, double: std::to_chars, no locale, no
                // allocation beyond the destination string's own growth.
                char buf[64];
                const auto res = std::to_chars(buf, buf + sizeof(buf), value);
                out.append(buf, res.ptr);
            }
        },
        arg);
}

inline std::string Decoder::format(const DecodedRecord& record)
{
    std::string out;
    if (record.site_ == nullptr) {
        return out;
    }

    const std::string_view fmt = record.site_->format_;
    out.reserve(fmt.size());
    std::size_t argIndex = 0;

    for (std::size_t i = 0; i < fmt.size();) {
        const char c = fmt[i];
        if (c == '{' && i + 1 < fmt.size() && fmt[i + 1] == '{') {
            out.push_back('{');
            i += 2;
            continue;
        }
        if (c == '}' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
            out.push_back('}');
            i += 2;
            continue;
        }
        if (c == '{' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
            if (argIndex < record.args_.size()) {
                appendArgText(out, record.args_[argIndex]);
                ++argIndex;
            } else {
                out += "{?}";
            }
            i += 2;
            continue;
        }
        out.push_back(c);
        ++i;
    }

    return out;
}

} // namespace sub0log
