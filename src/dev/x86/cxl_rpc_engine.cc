/*
 * CXL RPC Engine implementation
 */

#include "dev/x86/cxl_rpc_engine.hh"

#include <algorithm>
#include <cstring>
#include <limits>

#include "base/trace.hh"
#include "debug/CXLRPCEngine.hh"
namespace gem5
{

namespace
{

constexpr Addr kDoorbellSlotStride = 0x40;
constexpr uint32_t kDoorbellReservedLines = 1;
constexpr Addr kMetadataPageSize = 0x1000;
constexpr Addr kMetadataPageMask = ~(kMetadataPageSize - 1);
constexpr uint32_t kBootstrapLenMagic = 0xFFFF0000u;
constexpr uint16_t kBootstrapOpRegisterDoorbell = 0x0001u;
constexpr uint16_t kBootstrapOpRegisterMetadataPageBase = 0x0100u;

inline uint32_t
doorbellSlotCount(const AddrRange& range)
{
    const uint64_t rangeSize = static_cast<uint64_t>(range.size());
    if (rangeSize < static_cast<uint64_t>(kDoorbellSlotStride)) {
        return 1;
    }

    uint64_t lines = rangeSize / static_cast<uint64_t>(kDoorbellSlotStride);
    if (lines <= static_cast<uint64_t>(kDoorbellReservedLines)) {
        return 1;
    }
    lines -= static_cast<uint64_t>(kDoorbellReservedLines);
    if (lines > std::numeric_limits<uint32_t>::max()) {
        lines = std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(lines);
}

inline bool
rangesOverlap(Addr aStart, Addr aEnd, Addr bStart, Addr bEnd)
{
    return aStart < bEnd && bStart < aEnd;
}

inline bool
isBootstrapLength(uint32_t length)
{
    return (length & 0xFFFF0000u) == kBootstrapLenMagic;
}

inline uint16_t
bootstrapOpcode(uint32_t length)
{
    return static_cast<uint16_t>(length & 0xFFFFu);
}

} // anonymous namespace

// DoorbellEntry methods
void DoorbellEntry::parseFromBuffer(const uint8_t* buf)
{
    uint64_t header = 0;
    std::memcpy(&header, buf, sizeof(header));
    method = static_cast<uint8_t>(header & 0x01u);
    is_inline = static_cast<uint8_t>((header >> 1) & 0x01u);
    phase = static_cast<uint8_t>((header >> 2) & 0x01u);
    length = static_cast<uint32_t>((header >> 3) & 0xFFFFFFFFu);
    node_id = static_cast<uint16_t>((header >> 35) & 0x3FFFu);
    rpc_id = static_cast<uint16_t>((header >> 49) & 0x7FFFu);

    // Bytes 8-15: data (64-bit, little endian)
    std::memcpy(&data, buf + 8, sizeof(data));
}

void DoorbellEntry::serializeToBuffer(uint8_t* buf) const
{
    uint64_t header = 0;
    header |= static_cast<uint64_t>(method & 0x01u);
    header |= static_cast<uint64_t>(is_inline & 0x01u) << 1;
    header |= static_cast<uint64_t>(phase & 0x01u) << 2;
    header |= static_cast<uint64_t>(length) << 3;
    header |= static_cast<uint64_t>(node_id & 0x3FFFu) << 35;
    header |= static_cast<uint64_t>(rpc_id & 0x7FFFu) << 49;
    std::memcpy(buf, &header, sizeof(header));

    // Bytes 8-15: data (64-bit, little endian)
    std::memcpy(buf + 8, &data, sizeof(data));
}

// CXLRPCEngine implementation
CXLRPCEngine::CXLRPCEngine(const Params& p)
    : SimObject(p),
      doorbellRange(p.doorbell_range),
      autoRegister(p.auto_register),
      defaultNodeId(p.default_node_id),
      defaultDoorbellAddr(p.default_doorbell_addr),
      defaultMetadataQueueAddr(p.default_metadata_queue_addr),
      defaultMetadataQueueEntries(p.default_metadata_queue_entries),
      defaultRequestDataAddr(p.default_request_data_addr),
      defaultRequestDataCapacity(p.default_request_data_capacity),
      defaultResponseDataAddr(p.default_response_data_addr),
      defaultResponseDataCapacity(p.default_response_data_capacity),
      defaultFlagAddr(p.default_flag_addr),
      stats(*this)
{
    for (size_t maskIndex = 0; maskIndex < remapByteEnableMasks.size();
         ++maskIndex) {
        auto &mask = remapByteEnableMasks[maskIndex];
        mask.assign(kDoorbellSlotStride, false);
        const size_t offset = maskIndex * 16u;
        for (size_t byte = 0; byte < 16u; ++byte) {
            mask[offset + byte] = true;
        }
    }

    DPRINTF(CXLRPCEngine, "CXLRPCEngine created with doorbell range [%#x, %#x]\n",
            doorbellRange.start(), doorbellRange.end());
}

void
CXLRPCEngine::startup()
{
    if (autoRegister) {
        DPRINTF(CXLRPCEngine, "Auto-registering default connection: "
                "node_id=%d, doorbell=%#x, metadata_queue=%#x\n",
                defaultNodeId, defaultDoorbellAddr,
                defaultMetadataQueueAddr);

        registerConnection(defaultNodeId,
                           defaultDoorbellAddr,
                           defaultMetadataQueueAddr,
                           defaultMetadataQueueEntries,
                           defaultRequestDataAddr,
                           defaultRequestDataCapacity,
                           defaultResponseDataAddr,
                           defaultResponseDataCapacity,
                           defaultFlagAddr);
    }

}

bool
CXLRPCEngine::isDoorbell(Addr addr) const
{
    if (singleDoorbellSegmentValid) {
        return addr >= singleDoorbellSegmentStart &&
               addr < singleDoorbellSegmentEnd;
    }

    // Once connections are available, classify doorbells via per-connection
    // contiguous doorbell segments (segment comparator), not per-slot scans.
    if (!connections.empty()) {
        const uint32_t slotsPerConn = doorbellSlotCount(doorbellRange);
        const Addr segmentSpan =
            static_cast<Addr>(slotsPerConn) * kDoorbellSlotStride;
        for (const auto& pair : connections) {
            const Addr segStart = pair.first;
            const Addr segEnd = segStart + segmentSpan;
            if (addr >= segStart && addr < segEnd) {
                return true;
            }
        }
        return false;
    }

    // Early-boot fallback before startup() registration.
    return doorbellRange.contains(addr);
}

void
CXLRPCEngine::rebuildDoorbellSlotIndex()
{
    singleDoorbellSegmentValid = false;
    singleDoorbellCanonicalAddr = 0;
    singleDoorbellSegmentStart = 0;
    singleDoorbellSegmentEnd = 0;

    if (connections.empty()) {
        return;
    }

    const uint32_t slotsPerConn = doorbellSlotCount(doorbellRange);
    const Addr segmentSpan =
        static_cast<Addr>(slotsPerConn) * kDoorbellSlotStride;

    if (connections.size() == 1) {
        const auto &pair = *connections.begin();
        singleDoorbellSegmentValid = true;
        singleDoorbellCanonicalAddr = pair.first;
        singleDoorbellSegmentStart = pair.first;
        singleDoorbellSegmentEnd = pair.first + segmentSpan;
    }
}

ClientConnection*
CXLRPCEngine::findConnectionForAddr(Addr addr, uint32_t size,
                                    Addr& base_addr, Addr* canonical_addr)
{
    return const_cast<ClientConnection *>(
        static_cast<const CXLRPCEngine *>(this)->findConnectionForAddr(
            addr, size, base_addr, canonical_addr));
}

const ClientConnection*
CXLRPCEngine::findConnectionForAddr(Addr addr, uint32_t size,
                                    Addr& base_addr,
                                    Addr* canonical_addr) const
{
    if (size == 0) {
        return nullptr;
    }

    const Addr pktStart = addr;
    Addr pktEnd = addr + static_cast<Addr>(size);
    if (pktEnd < addr) {
        pktEnd = MaxAddr;
    }

    if (singleDoorbellSegmentValid &&
        rangesOverlap(pktStart, pktEnd,
                      singleDoorbellSegmentStart,
                      singleDoorbellSegmentEnd)) {
        const Addr overlapStart = std::max(pktStart, singleDoorbellSegmentStart);
        const Addr slotIndex =
            (overlapStart - singleDoorbellSegmentStart) / kDoorbellSlotStride;
        base_addr =
            singleDoorbellSegmentStart + slotIndex * kDoorbellSlotStride;
        if (canonical_addr) {
            *canonical_addr = singleDoorbellCanonicalAddr;
        }
        auto connIt = connections.find(singleDoorbellCanonicalAddr);
        return (connIt == connections.end()) ? nullptr : &connIt->second;
    }

    const uint32_t slotsPerConn = doorbellSlotCount(doorbellRange);
    const Addr segmentSpan =
        static_cast<Addr>(slotsPerConn) * kDoorbellSlotStride;

    for (auto& pair : connections) {
        const Addr segStart = pair.first;
        Addr segEnd = segStart + segmentSpan;
        if (segEnd < segStart) {
            segEnd = MaxAddr;
        }
        if (!rangesOverlap(pktStart, pktEnd, segStart, segEnd)) {
            continue;
        }

        const Addr overlapStart = std::max(pktStart, segStart);
        const Addr slotIndex = (overlapStart - segStart) / kDoorbellSlotStride;
        base_addr = segStart + slotIndex * kDoorbellSlotStride;
        if (canonical_addr) {
            *canonical_addr = segStart;
        }
        return &pair.second;
    }

    return nullptr;
}

Addr
CXLRPCEngine::resolveMetadataSlotAddr(const ClientConnection& conn,
                                      uint32_t slot) const
{
    const Addr logicalBase = conn.metadata_queue_logical_base;
    const Addr logicalSlotAddr =
        logicalBase + static_cast<Addr>(slot) * static_cast<Addr>(16);
    const Addr logicalLine = logicalSlotAddr & ~Addr(0x3F);
    const Addr lineOffset = logicalSlotAddr - logicalLine;

    auto mapped = conn.metadata_line_addr_map.find(logicalLine);
    if (mapped != conn.metadata_line_addr_map.end()) {
        return mapped->second + lineOffset;
    }

    if (conn.metadata_queue.base_addr >= logicalBase) {
        return conn.metadata_queue.base_addr + (logicalSlotAddr - logicalBase);
    }

    return conn.metadata_queue.base_addr + static_cast<Addr>(slot) * 16;
}

DoorbellWriteProbe
CXLRPCEngine::probeDoorbellWrite(PacketPtr pkt) const
{
    DoorbellWriteProbe probe;

    if (!pkt || !pkt->isWrite()) {
        return probe;
    }

    const Addr addr = pkt->getAddr();
    const uint32_t size = pkt->getSize();
    if (size < 16) {
        return probe;
    }

    Addr pktEnd = addr + static_cast<Addr>(size);
    if (pktEnd < addr) {
        pktEnd = MaxAddr;
    }

    if (connections.empty()) {
        if ((addr < doorbellRange.end()) && (pktEnd > doorbellRange.start())) {
            probe.should_probe = true;
        }
        return probe;
    }

    Addr base_addr = 0;
    const ClientConnection* conn =
        findConnectionForAddr(addr, size, base_addr);
    if (conn) {
        probe.should_probe = true;
        probe.matched_connection = true;
        probe.connection = conn;
        probe.doorbell_addr = base_addr;
        if (pkt->hasData()) {
            const Addr dbStart = base_addr;
            const Addr dbEnd = base_addr + 16;
            if (addr <= dbStart && pktEnd >= dbEnd) {
                probe.covers_doorbell = true;
                probe.doorbell_offset =
                    static_cast<size_t>(dbStart - addr);
                probe.entry.parseFromBuffer(pkt->getConstPtr<uint8_t>() +
                                            probe.doorbell_offset);
                probe.parsed_entry = true;
            }
        }
    } else if (pkt->hasData() && (addr % kDoorbellSlotStride) == 0) {
        DoorbellEntry entry;
        const uint8_t* raw = pkt->getConstPtr<uint8_t>();
        uint64_t rawLo = 0;
        uint64_t rawHi = 0;
        entry.parseFromBuffer(raw);
        std::memcpy(&rawLo, raw, sizeof(rawLo));
        std::memcpy(&rawHi, raw + sizeof(rawLo), sizeof(rawHi));
        if (entry.method == METHOD_REQUEST && entry.rpc_id == 0) {
            DPRINTF(CXLRPCEngine,
                    "Unregistered aligned write candidate: addr=%#x size=%u "
                    "raw_lo=%#x raw_hi=%#x parsed method=%u inline=%u "
                    "phase=%u len=%#x node=%u rpc=%u\n",
                    addr, size, rawLo, rawHi,
                    static_cast<unsigned>(entry.method),
                    static_cast<unsigned>(entry.is_inline),
                    static_cast<unsigned>(entry.phase),
                    static_cast<unsigned>(entry.length),
                    static_cast<unsigned>(entry.node_id),
                    static_cast<unsigned>(entry.rpc_id));
        }
        if (isBootstrapRequest(entry)) {
            probe.should_probe = true;
        } else if (isBootstrapLength(entry.length) || entry.rpc_id == 0) {
            DPRINTF(CXLRPCEngine,
                    "Rejected bootstrap probe candidate: addr=%#x size=%u "
                    "method=%u inline=%u phase=%u len=%#x node=%u rpc=%u\n",
                    addr, size,
                    static_cast<unsigned>(entry.method),
                    static_cast<unsigned>(entry.is_inline),
                    static_cast<unsigned>(entry.phase),
                    static_cast<unsigned>(entry.length),
                    static_cast<unsigned>(entry.node_id),
                    static_cast<unsigned>(entry.rpc_id));
        }
    }

    return probe;
}

DoorbellHandleResult
CXLRPCEngine::handleDoorbellWrite(PacketPtr pkt,
                                  const DoorbellWriteProbe* probe)
{
    Addr addr = pkt->getAddr();
    uint32_t size = pkt->getSize();

    // Find the connection (supports writes within doorbell entry range)
    Addr base_addr = 0;
    ClientConnection* conn = nullptr;
    if (probe && probe->should_probe && probe->matched_connection) {
        base_addr = probe->doorbell_addr;
        conn = const_cast<ClientConnection*>(probe->connection);
    }
    if (!conn) {
        conn = findConnectionForAddr(addr, size, base_addr);
    }
    if (!conn) {
        if (tryLearnDoorbellTranslation(pkt)) {
            return DoorbellHandleResult::Consumed;
        }
        uint64_t first8 = 0;
        if (pkt->hasData() && pkt->getSize() >= 8) {
            memcpy(&first8, pkt->getConstPtr<uint8_t>(), sizeof(first8));
        }
        DPRINTF(CXLRPCEngine,
                "Doorbell write to unregistered address %#x size=%u first8=%#x\n",
                addr, pkt->getSize(), first8);
        return DoorbellHandleResult::NotHandled;
    }
    if (!pkt->hasData()) {
        return DoorbellHandleResult::NotHandled;
    }

    // Parse doorbell entry only when this write fully covers bytes [0..15]
    // of the logical doorbell. This supports cacheable 64B line writeback
    // while naturally ignoring partial sub-entry stores.
    const bool useProbeDoorbell =
        probe && probe->should_probe && probe->matched_connection &&
        probe->connection == conn && probe->doorbell_addr == base_addr;

    size_t dbOffset = 0;
    if (useProbeDoorbell && probe->covers_doorbell) {
        dbOffset = probe->doorbell_offset;
    } else {
        const Addr pktStart = addr;
        const Addr pktEnd = addr + static_cast<Addr>(size);
        const Addr dbStart = base_addr;
        const Addr dbEnd = base_addr + 16;
        if (pktStart > dbStart || pktEnd < dbEnd) {
            DPRINTF(CXLRPCEngine,
                    "Ignoring partial doorbell write: addr=%#x size=%u db=%#x\n",
                    addr, size, base_addr);
            return DoorbellHandleResult::NotHandled;
        }
        dbOffset = static_cast<size_t>(dbStart - pktStart);
    }

    const uint8_t* raw = pkt->getConstPtr<uint8_t>() + dbOffset;

    stats.doorbellWrites++;

    DoorbellEntry entry;
    if (useProbeDoorbell && probe->parsed_entry &&
        probe->doorbell_offset == dbOffset) {
        entry = probe->entry;
    } else {
        entry.parseFromBuffer(raw);
    }

    DPRINTF(CXLRPCEngine, "Doorbell write: method=%u, node_id=%u, "
            "rpc_id=%u, len=%u, inline=%u\n",
            static_cast<unsigned>(entry.method),
            static_cast<unsigned>(entry.node_id),
            static_cast<unsigned>(entry.rpc_id),
            static_cast<unsigned>(entry.length),
            static_cast<unsigned>(entry.is_inline));

    return processDoorbellEntry(*conn, base_addr, entry, pkt);
}

DoorbellHandleResult
CXLRPCEngine::processDoorbellEntry(
    ClientConnection& conn, Addr doorbell_addr,
    const DoorbellEntry& entry, PacketPtr pkt)
{
    switch (entry.method) {
        case METHOD_REQUEST:
            return processRequest(conn, doorbell_addr, entry, pkt);
        case METHOD_HEAD_UPDATE:
            return processHeadUpdate(conn, doorbell_addr, entry, pkt);
        default:
            DPRINTF(CXLRPCEngine, "Unknown method type: %d\n", entry.method);
            return DoorbellHandleResult::NotHandled;
    }
}

DoorbellHandleResult
CXLRPCEngine::processRequest(
    ClientConnection& conn, Addr doorbell_addr,
    const DoorbellEntry& entry, PacketPtr pkt)
{
    constexpr size_t kDoorbellEntrySize = 16;
    constexpr Addr kCacheLineMask = ~Addr(0x3F);

    if (isBootstrapRequest(entry)) {
        return processBootstrapRequest(conn, doorbell_addr, entry, pkt);
    }

    // Skip invalid doorbells (all-zero)
    // This can happen during initialization when doorbell memory is zero
    if (entry.rpc_id == 0 && entry.length == 0 &&
        entry.node_id == 0 && entry.data == 0) {
        DPRINTF(CXLRPCEngine, "Ignoring invalid all-zero doorbell\n");
        return DoorbellHandleResult::Handled;
    }

    MetadataQueue& queue = conn.metadata_queue;

    // Check if queue is full
    if (queue.isFull()) {
        stats.queueFullEvents++;
        DPRINTF(CXLRPCEngine,
                "Metadata queue full for node %u, requesting retry\n",
                static_cast<unsigned>(entry.node_id));
        return DoorbellHandleResult::Retry;
    }

    DPRINTF(CXLRPCEngine,
            "processRequest: pkt=%s isWrite=%d size=%u pkt_addr=%#x "
            "conn_db=%#x queue_tail=%u queue_head=%u phase=%u\n",
            pkt ? "yes" : "no",
            (pkt && pkt->isWrite()) ? 1 : 0,
            pkt ? pkt->getSize() : 0,
            pkt ? pkt->getAddr() : 0,
            doorbell_addr,
            queue.tail,
            queue.head_cached,
            queue.current_phase ? 1 : 0);

    const Addr pktStart = pkt->getAddr();
    const size_t pktSize = pkt->getSize();
    // Reserve slot 0 for server/head-update traffic so client request
    // doorbells start from slot 1.
    if (doorbell_addr == conn.doorbell_addr) {
        DPRINTF(CXLRPCEngine,
                "Request doorbell on reserved slot0 ignored: "
                "doorbell=%#x conn_db=%#x\n",
                doorbell_addr, conn.doorbell_addr);
        return DoorbellHandleResult::NotHandled;
    }

    const auto reservation = queue.reserveTail();
    const uint32_t slot = reservation.slot;
    const bool slotPhase = reservation.phase;
    const Addr slotAddr = resolveMetadataSlotAddr(conn, slot);

    auto *pktData = const_cast<uint8_t *>(pkt->getConstPtr<uint8_t>());
    Addr remapAddr = slotAddr;
    size_t payloadOffset = 0;

    if (pktSize > kDoorbellEntrySize) {
        const Addr lineAddr = slotAddr & kCacheLineMask;
        const size_t lineOffset = static_cast<size_t>(slotAddr - lineAddr);
        if (lineOffset + kDoorbellEntrySize <= pktSize) {
            remapAddr = lineAddr;
            payloadOffset = lineOffset;
        }

        const size_t maskIndex = payloadOffset / kDoorbellEntrySize;
        if (pktSize == kDoorbellSlotStride &&
            payloadOffset % kDoorbellEntrySize == 0 &&
            maskIndex < remapByteEnableMasks.size()) {
            pkt->req->setByteEnable(remapByteEnableMasks[maskIndex]);
        } else {
            if (remapByteEnableScratch.size() != pktSize) {
                remapByteEnableScratch.assign(pktSize, false);
            } else {
                std::fill(remapByteEnableScratch.begin(),
                          remapByteEnableScratch.end(),
                          false);
            }
            for (size_t i = 0; i < kDoorbellEntrySize; ++i) {
                remapByteEnableScratch[payloadOffset + i] = true;
            }
            pkt->req->setByteEnable(remapByteEnableScratch);
        }
    }

    const size_t doorbellOffset =
        static_cast<size_t>(doorbell_addr - pktStart);
    if (doorbellOffset != payloadOffset) {
        std::memmove(pktData + payloadOffset,
                     pktData + doorbellOffset,
                     kDoorbellEntrySize);
    }

    DoorbellEntry remapped = entry;
    remapped.phase = slotPhase ? 1u : 0u;
    remapped.serializeToBuffer(pktData + payloadOffset);

    pkt->setAddr(remapAddr);

    uint64_t metaLo = 0;
    uint64_t metaHi = 0;
    std::memcpy(&metaLo, pktData + payloadOffset, sizeof(metaLo));
    std::memcpy(&metaHi, pktData + payloadOffset + sizeof(metaLo),
                sizeof(metaHi));
    DPRINTF(CXLRPCEngine,
            "Request remapped to metadata queue: node=%u slot=%u "
            "slot_addr=%#x remap_addr=%#x offset=%u phase=%u rpc_id=%u "
            "meta_lo=%#x meta_hi=%#x\n",
            static_cast<unsigned>(remapped.node_id), slot, slotAddr, remapAddr,
            static_cast<unsigned>(payloadOffset), slotPhase ? 1 : 0,
            entry.rpc_id, metaLo, metaHi);
    return DoorbellHandleResult::Remapped;
}

DoorbellHandleResult
CXLRPCEngine::processHeadUpdate(
    ClientConnection& conn, Addr doorbell_addr,
    const DoorbellEntry& entry, PacketPtr pkt)
{
    // The data field contains the new head value
    uint32_t new_head = entry.data & 0xFFFFFFFFu;

    MetadataQueue& queue = conn.metadata_queue;
    if (queue.capacity == 0) {
        stats.invalidHeadUpdates++;
        DPRINTF(CXLRPCEngine,
                "HEAD_UPDATE ignored for node %u: capacity is zero\n",
                conn.node_id);
        return DoorbellHandleResult::Handled;
    }
    if (new_head >= queue.capacity) {
        stats.invalidHeadUpdates++;
        DPRINTF(CXLRPCEngine,
                "HEAD_UPDATE ignored for node %u: new_head=%u >= capacity=%u\n",
                conn.node_id, new_head, queue.capacity);
        return DoorbellHandleResult::Handled;
    }

    // HEAD_UPDATE only needs to update controller-side head state. Keep this
    // control-path update in-controller and skip downstream memory writes.
    queue.head_cached = new_head;
    stats.headUpdatesReceived++;

    if (pkt && pkt->isWrite()) {
        DPRINTF(CXLRPCEngine,
                "HEAD_UPDATE consumed in-controller: node=%u new_head=%u "
                "doorbell=%#x pkt_addr=%#x size=%u\n",
                conn.node_id, new_head, doorbell_addr,
                pkt->getAddr(), pkt->getSize());
        return DoorbellHandleResult::Consumed;
    }

    DPRINTF(CXLRPCEngine,
            "HEAD_UPDATE applied (internal): node=%u new_head=%u\n",
            conn.node_id, new_head);
    return DoorbellHandleResult::Handled;
}

bool
CXLRPCEngine::isBootstrapRequest(const DoorbellEntry& entry) const
{
    return entry.method == METHOD_REQUEST &&
           entry.is_inline == 1 &&
           entry.rpc_id == 0 &&
           isBootstrapLength(entry.length);
}

DoorbellHandleResult
CXLRPCEngine::processBootstrapRequest(
    ClientConnection& conn, Addr doorbell_addr,
    const DoorbellEntry& entry, PacketPtr pkt)
{
    const uint16_t opcode = bootstrapOpcode(entry.length);

    if (opcode == kBootstrapOpRegisterDoorbell) {
        DPRINTF(CXLRPCEngine,
                "Consumed bootstrap doorbell registration: node=%u "
                "doorbell=%#x pkt_addr=%#x size=%u\n",
                conn.node_id, doorbell_addr,
                pkt ? pkt->getAddr() : 0,
                pkt ? pkt->getSize() : 0);
        return DoorbellHandleResult::Consumed;
    }

    if ((opcode & 0xFF00u) == kBootstrapOpRegisterMetadataPageBase) {
        const uint32_t pageIndex = static_cast<uint32_t>(opcode & 0x00FFu);
        const Addr logicalBase = conn.metadata_queue_logical_base & kMetadataPageMask;
        const Addr logicalPage =
            logicalBase + static_cast<Addr>(pageIndex) * kMetadataPageSize;
        const Addr observedPage = static_cast<Addr>(entry.data) & kMetadataPageMask;
        const Addr queueBytes =
            static_cast<Addr>(conn.metadata_queue.capacity) * static_cast<Addr>(16);

        if (observedPage == 0 ||
            logicalPage < logicalBase ||
            (logicalPage - logicalBase) >= queueBytes) {
            DPRINTF(CXLRPCEngine,
                    "Ignoring bootstrap metadata-page registration: node=%u "
                    "page=%u logical=%#x observed=%#x queue_bytes=%u\n",
                    conn.node_id, pageIndex, logicalPage, observedPage,
                    static_cast<unsigned>(queueBytes));
            return DoorbellHandleResult::Consumed;
        }

        for (Addr off = 0; off < kMetadataPageSize; off += 64) {
            conn.metadata_line_addr_map[logicalPage + off] = observedPage + off;
        }

        if (pageIndex == 0) {
            conn.metadata_queue.base_addr =
                observedPage +
                (conn.metadata_queue_logical_base - logicalBase);
        }

        DPRINTF(CXLRPCEngine,
                "Registered metadata page translation: node=%u page=%u "
                "logical=%#x observed=%#x base=%#x\n",
                conn.node_id, pageIndex, logicalPage, observedPage,
                conn.metadata_queue.base_addr);
        return DoorbellHandleResult::Consumed;
    }

    DPRINTF(CXLRPCEngine,
            "Ignoring unknown bootstrap request: node=%u opcode=%#x "
            "doorbell=%#x pkt_addr=%#x\n",
            conn.node_id, opcode, doorbell_addr,
            pkt ? pkt->getAddr() : 0);
    return DoorbellHandleResult::Consumed;
}

bool
CXLRPCEngine::tryLearnDoorbellTranslation(PacketPtr pkt)
{
    if (!pkt || !pkt->isWrite() || !pkt->hasData() || pkt->getSize() < 16 ||
        connections.size() != 1 || (pkt->getAddr() % kDoorbellSlotStride) != 0) {
        return false;
    }

    DoorbellEntry entry;
    const uint8_t* raw = pkt->getConstPtr<uint8_t>();
    uint64_t rawLo = 0;
    uint64_t rawHi = 0;
    entry.parseFromBuffer(raw);
    std::memcpy(&rawLo, raw, sizeof(rawLo));
    std::memcpy(&rawHi, raw + sizeof(rawLo), sizeof(rawHi));
    if (!isBootstrapRequest(entry) ||
        bootstrapOpcode(entry.length) != kBootstrapOpRegisterDoorbell) {
        DPRINTF(CXLRPCEngine,
                "Unregistered write did not qualify for doorbell learning: "
                "addr=%#x size=%u raw_lo=%#x raw_hi=%#x method=%u inline=%u "
                "phase=%u len=%#x node=%u rpc=%u opcode=%#x\n",
                pkt->getAddr(), pkt->getSize(), rawLo, rawHi,
                static_cast<unsigned>(entry.method),
                static_cast<unsigned>(entry.is_inline),
                static_cast<unsigned>(entry.phase),
                static_cast<unsigned>(entry.length),
                static_cast<unsigned>(entry.node_id),
                static_cast<unsigned>(entry.rpc_id),
                static_cast<unsigned>(bootstrapOpcode(entry.length)));
        return false;
    }

    auto it = connections.begin();
    ClientConnection conn = it->second;
    const Addr observedCanonical = pkt->getAddr();
    const Addr logicalCanonical = it->first;
    if (observedCanonical == logicalCanonical) {
        return false;
    }

    connections.erase(it);
    conn.doorbell_addr = observedCanonical;
    connections[observedCanonical] = conn;
    rebuildDoorbellSlotIndex();

    DPRINTF(CXLRPCEngine,
            "Learned observed doorbell canonical address: logical=%#x "
            "observed=%#x node=%u\n",
            logicalCanonical, observedCanonical, conn.node_id);
    return true;
}

void
CXLRPCEngine::registerConnection(uint32_t node_id,
                                  Addr doorbell_addr,
                                  Addr metadata_queue_addr,
                                  uint32_t metadata_queue_entries,
                                  Addr request_data_addr,
                                  uint32_t request_data_capacity,
                                  Addr response_data_addr,
                                  uint32_t response_data_capacity,
                                  Addr flag_addr)
{
    ClientConnection conn;
    conn.node_id = node_id;
    conn.doorbell_addr = doorbell_addr;
    conn.request_data_addr = request_data_addr;
    conn.request_data_capacity = request_data_capacity;
    conn.response_data_addr = response_data_addr;
    conn.response_data_capacity = response_data_capacity;
    conn.flag_addr = flag_addr;

    conn.metadata_queue.base_addr = metadata_queue_addr;
    conn.metadata_queue_logical_base = metadata_queue_addr;
    conn.metadata_queue.capacity = metadata_queue_entries;
    conn.metadata_queue.tail = 0;
    conn.metadata_queue.head_cached = 0;
    // Initial metadata slots are zero (phase=0). Producer starts from phase=1.
    conn.metadata_queue.current_phase = true;
    conn.metadata_line_addr_map.clear();
    conn.metadata_line_addr_map[metadata_queue_addr & ~Addr(0x3F)] =
        metadata_queue_addr & ~Addr(0x3F);

    connections[doorbell_addr] = conn;
    rebuildDoorbellSlotIndex();

    DPRINTF(CXLRPCEngine, "Registered node %d: doorbell=%#x, "
            "metadata=%#x, request_data=%#x(cap=%u), "
            "response_data=%#x(cap=%u), flag=%#x\n",
            node_id, doorbell_addr, metadata_queue_addr,
            request_data_addr, request_data_capacity,
            response_data_addr, response_data_capacity, flag_addr);
}

void
CXLRPCEngine::accountRemappedDoorbellForward()
{
    stats.requestsForwarded++;
    stats.metadataQueueWrites++;
}

// Statistics
CXLRPCEngine::RPCEngineStats::RPCEngineStats(CXLRPCEngine& engine)
    : statistics::Group(&engine),
      ADD_STAT(doorbellWrites, statistics::units::Count::get(),
               "Number of doorbell writes received"),
      ADD_STAT(requestsForwarded, statistics::units::Count::get(),
               "Number of requests forwarded to metadata queue"),
      ADD_STAT(headUpdatesReceived, statistics::units::Count::get(),
               "Number of HEAD_UPDATE messages received"),
      ADD_STAT(invalidHeadUpdates, statistics::units::Count::get(),
               "Number of invalid HEAD_UPDATE values ignored"),
      ADD_STAT(queueFullEvents, statistics::units::Count::get(),
               "Number of times metadata queue was full"),
      ADD_STAT(metadataQueueWrites, statistics::units::Count::get(),
               "Number of metadata queue DMA writes")
{
}

} // namespace gem5
