/*
 * CXL RPC Engine implementation
 */

#include "dev/x86/cxl_rpc_engine.hh"

#include <algorithm>
#include <cstring>
#include <limits>

#include "base/trace.hh"
#include "debug/CXLRPCEngine.hh"
#include "mem/request.hh"

namespace gem5
{

namespace
{

constexpr uint32_t kControlResetClientState = 1u;
constexpr uint16_t kControlInternalService = 0x07A1u;
constexpr uint16_t kControlRegisterTranslation = 0x0001u;
constexpr uint16_t kControlRegisterMetadataPageTranslation = 0x0002u;
constexpr Addr kDoorbellSlotStride = 0x40;
constexpr uint32_t kDoorbellReservedLines = 1;

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

inline Addr
addSignedDelta(Addr base, int64_t delta)
{
    if (delta >= 0) {
        return base + static_cast<Addr>(delta);
    }
    return base - static_cast<Addr>(-delta);
}

inline bool
rangesOverlap(Addr aStart, Addr aEnd, Addr bStart, Addr bEnd)
{
    return aStart < bEnd && bStart < aEnd;
}

} // anonymous namespace

// DoorbellEntry methods
void DoorbellEntry::parseFromBuffer(const uint8_t* buf)
{
    uint64_t header = 0;
    std::memcpy(&header, buf, sizeof(header));
    method = static_cast<uint8_t>(header & 0x03u);
    is_inline = static_cast<uint8_t>((header >> 2) & 0x01u);
    phase_bit = static_cast<uint8_t>((header >> 3) & 0x01u);
    payload_len = static_cast<uint32_t>((header >> 4) & 0xFFFFFu);
    service_id = static_cast<uint16_t>((header >> 24) & 0x0FFFu);
    method_id = static_cast<uint16_t>((header >> 36) & 0x0FFFu);
    request_id = static_cast<uint32_t>((header >> 48) & 0xFFFFu);

    // Bytes 8-15: data (64-bit, little endian)
    std::memcpy(&data, buf + 8, sizeof(data));
}

void DoorbellEntry::serializeToBuffer(uint8_t* buf) const
{
    uint64_t header = 0;
    header |= static_cast<uint64_t>(method & 0x03u);
    header |= static_cast<uint64_t>(is_inline & 0x01u) << 2;
    header |= static_cast<uint64_t>(phase_bit & 0x01u) << 3;
    header |= static_cast<uint64_t>(payload_len & 0xFFFFFu) << 4;
    header |= static_cast<uint64_t>(service_id & 0x0FFFu) << 24;
    header |= static_cast<uint64_t>(method_id & 0x0FFFu) << 36;
    header |= static_cast<uint64_t>(request_id & 0xFFFFu) << 48;
    std::memcpy(buf, &header, sizeof(header));

    // Bytes 8-15: data (64-bit, little endian)
    std::memcpy(buf + 8, &data, sizeof(data));
}

// CXLRPCEngine implementation
CXLRPCEngine::CXLRPCEngine(const Params& p)
    : SimObject(p),
      doorbellRange(p.doorbell_range),
      autoRegister(p.auto_register),
      defaultClientId(p.default_client_id),
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
    DPRINTF(CXLRPCEngine, "CXLRPCEngine created with doorbell range [%#x, %#x]\n",
            doorbellRange.start(), doorbellRange.end());
}

void
CXLRPCEngine::startup()
{
    if (autoRegister) {
        DPRINTF(CXLRPCEngine, "Auto-registering default connection: "
                "client_id=%d, doorbell=%#x, metadata_queue=%#x\n",
                defaultClientId, defaultDoorbellAddr,
                defaultMetadataQueueAddr);

        registerConnection(defaultClientId,
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
    doorbellSlotToCanonical.clear();

    if (connections.empty()) {
        return;
    }

    const uint32_t slotsPerConn = doorbellSlotCount(doorbellRange);
    const size_t slotCount =
        static_cast<size_t>(connections.size()) * slotsPerConn;
    doorbellSlotToCanonical.reserve(slotCount);

    for (const auto& pair : connections) {
        const Addr canonical = pair.first;
        for (uint32_t i = 0; i < slotsPerConn; ++i) {
            const Addr slot =
                canonical + static_cast<Addr>(i) * kDoorbellSlotStride;
            doorbellSlotToCanonical[slot] = canonical;
        }
    }
}

ClientConnection*
CXLRPCEngine::findConnectionForAddr(Addr addr, uint32_t size, Addr& base_addr)
{
    if (size == 0) {
        return nullptr;
    }

    const Addr pktStart = addr;
    Addr pktEnd = addr + static_cast<Addr>(size);
    if (pktEnd < addr) {
        pktEnd = MaxAddr;
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
        return &pair.second;
    }

    return nullptr;
}

ClientConnection*
CXLRPCEngine::findConnectionForDoorbellAddr(Addr doorbell_addr,
                                            Addr* canonical_addr)
{
    auto slotIt = doorbellSlotToCanonical.find(doorbell_addr);
    if (slotIt == doorbellSlotToCanonical.end()) {
        return nullptr;
    }
    if (canonical_addr) {
        *canonical_addr = slotIt->second;
    }
    auto connIt = connections.find(slotIt->second);
    if (connIt == connections.end()) {
        return nullptr;
    }
    return &connIt->second;
}

const ClientConnection*
CXLRPCEngine::findConnectionForDoorbellAddr(Addr doorbell_addr,
                                            Addr* canonical_addr) const
{
    auto slotIt = doorbellSlotToCanonical.find(doorbell_addr);
    if (slotIt == doorbellSlotToCanonical.end()) {
        return nullptr;
    }
    if (canonical_addr) {
        *canonical_addr = slotIt->second;
    }
    auto connIt = connections.find(slotIt->second);
    if (connIt == connections.end()) {
        return nullptr;
    }
    return &connIt->second;
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

bool
CXLRPCEngine::mustProbeWrite(PacketPtr pkt) const
{
    if (!pkt || !pkt->isWrite()) {
        return false;
    }

    const Addr addr = pkt->getAddr();
    const uint32_t size = pkt->getSize();
    if (size < 16) {
        return false;
    }

    Addr pktEnd = addr + static_cast<Addr>(size);
    if (pktEnd < addr) {
        pktEnd = MaxAddr;
    }

    // No connections registered yet — use the configured broad fallback range.
    if (connections.empty()) {
        return (addr < doorbellRange.end()) && (pktEnd > doorbellRange.start());
    }

    // Post-registration fast path: comparator over per-connection
    // doorbell segments.
    const uint32_t slotsPerConn = doorbellSlotCount(doorbellRange);
    const Addr segmentSpan =
        static_cast<Addr>(slotsPerConn) * kDoorbellSlotStride;
    for (const auto& pair : connections) {
        const Addr segStart = pair.first;
        Addr segEnd = segStart + segmentSpan;
        if (segEnd < segStart) {
            segEnd = MaxAddr;
        }
        if (rangesOverlap(addr, pktEnd, segStart, segEnd)) {
            return true;
        }
    }

    // Before address translation is learned we only probe control writes,
    // and learn delta via REGISTER_TRANSLATION when present.
    if (!addrTranslationLearned) {
        if (!pkt->hasData()) {
            return false;
        }
        const uint8_t* payload = pkt->getConstPtr<uint8_t>();
        if (!payload) {
            return false;
        }

        const uint8_t flags = payload[0];
        const uint8_t method = flags & 0x03;
        return (method == METHOD_CONTROL);
    }

    return false;
}

DoorbellHandleResult
CXLRPCEngine::handleDoorbellWrite(PacketPtr pkt)
{
    Addr addr = pkt->getAddr();
    uint32_t size = pkt->getSize();

    // Find the connection (supports writes within doorbell entry range)
    Addr base_addr = 0;
    ClientConnection* conn = findConnectionForAddr(addr, size, base_addr);
    if (!conn && tryLearnAddressTranslation(pkt)) {
        conn = findConnectionForAddr(addr, size, base_addr);
    }
    if (!conn) {
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

    const size_t dbOffset = static_cast<size_t>(dbStart - pktStart);
    const uint8_t* raw = pkt->getConstPtr<uint8_t>() + dbOffset;

    stats.doorbellWrites++;

    DoorbellEntry entry;
    entry.parseFromBuffer(raw);

    DPRINTF(CXLRPCEngine, "Doorbell write: method=%u, service=%u, method_id=%u, "
            "request_id=%u, len=%u, inline=%u\n",
            static_cast<unsigned>(entry.method),
            static_cast<unsigned>(entry.service_id),
            static_cast<unsigned>(entry.method_id),
            static_cast<unsigned>(entry.request_id),
            static_cast<unsigned>(entry.payload_len),
            static_cast<unsigned>(entry.is_inline));

    return processDoorbellEntry(*conn, base_addr, entry, pkt);
}

DoorbellHandleResult
CXLRPCEngine::processDoorbellEntry(
    ClientConnection& conn, Addr doorbell_addr,
    const DoorbellEntry& entry, PacketPtr pkt)
{
    // Runtime doorbell hot path only needs:
    //   1) METHOD_REQUEST    (client -> controller -> metadata queue)
    //   2) METHOD_HEAD_UPDATE(server -> controller head sync)
    // METHOD_CONTROL is retained for one-time bootstrap/registration.
    switch (entry.method) {
        case METHOD_REQUEST:
            return processRequest(conn, doorbell_addr, entry, pkt);
        case METHOD_HEAD_UPDATE:
            return processHeadUpdate(conn, doorbell_addr, entry, pkt);
        case METHOD_CONTROL:
            return processControl(conn, entry);
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
    // Skip invalid doorbells (all-zero)
    // This can happen during initialization when doorbell memory is zero
    if (entry.request_id == 0 && entry.service_id == 0 &&
        entry.method_id == 0 && entry.data == 0) {
        DPRINTF(CXLRPCEngine, "Ignoring invalid all-zero doorbell\n");
        return DoorbellHandleResult::Handled;
    }

    MetadataQueue& queue = conn.metadata_queue;

    // Check if queue is full
    if (queue.isFull()) {
        stats.queueFullEvents++;
        DPRINTF(CXLRPCEngine,
                "Metadata queue full for client %u, requesting retry\n",
                conn.client_id);
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

    // For doorbell packets in timing/atomic paths, remap the packet to
    // metadata-queue tail so it follows the normal memory pipeline.
    //
    // Cacheable doorbells commonly arrive as a 64B writeback. Keep the packet
    // size unchanged (for timing realism), but mask bytes [16..size) so only
    // one 16B metadata entry is committed.
    const Addr pktStart = pkt ? pkt->getAddr() : 0;
    const Addr pktEnd = pktStart + (pkt ? static_cast<Addr>(pkt->getSize()) : 0);
    const Addr dbStart = doorbell_addr;
    const Addr dbEnd = doorbell_addr + 16;
    const bool coversDoorbell =
        pkt && pkt->isWrite() && pkt->getSize() >= 16 &&
        rangesOverlap(pktStart, pktEnd, dbStart, dbEnd) &&
        pktStart <= dbStart && pktEnd >= dbEnd;

    if (coversDoorbell) {
        const uint32_t slotsPerConn = doorbellSlotCount(doorbellRange);
        uint32_t observedClientId = conn.client_id;
        if (doorbell_addr >= conn.doorbell_addr) {
            const Addr delta = doorbell_addr - conn.doorbell_addr;
            if ((delta % kDoorbellSlotStride) == 0) {
                const uint32_t slotClient =
                    static_cast<uint32_t>(delta / kDoorbellSlotStride);
                // Reserve slot 0 for server/control traffic so client request
                // doorbells start from slot 1.
                if (slotClient == 0) {
                    DPRINTF(CXLRPCEngine,
                            "Request doorbell on reserved slot0 ignored: "
                            "doorbell=%#x conn_db=%#x\n",
                            doorbell_addr, conn.doorbell_addr);
                    return DoorbellHandleResult::NotHandled;
                }
                if (slotClient < slotsPerConn) {
                    observedClientId = slotClient - 1;
                }
            }
        }

        auto& state = remappedDoorbells[pkt];
        state.countAsMetadataWrite = true;

        const auto reservation = queue.reserveTail();
        const uint32_t slot = reservation.slot;
        const bool slotPhase = reservation.phase;
        const Addr slotAddr = resolveMetadataSlotAddr(conn, slot);

        /*
         * Keep large writebacks (typically 64B) but align the packet address to
         * cacheline start and place metadata at the slot offset within that
         * line. This avoids interleaving boundary crossing when slotAddr is not
         * 64B-aligned (slot granularity is 16B).
         */
        constexpr size_t kDoorbellEntrySize = 16;
        Addr remapAddr = slotAddr;
        size_t payloadOffset = 0;
        const size_t pktSize = pkt->getSize();
        if (pktSize > 16) {
            const Addr lineAddr = slotAddr & ~Addr(0x3F);
            const size_t lineOffset =
                static_cast<size_t>(slotAddr - lineAddr);
            if (lineOffset + kDoorbellEntrySize <= pktSize) {
                remapAddr = lineAddr;
                payloadOffset = lineOffset;
            }

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

        auto *pktData = const_cast<uint8_t *>(pkt->getConstPtr<uint8_t>());
        const size_t doorbellOffset = static_cast<size_t>(dbStart - pktStart);
        if (doorbellOffset != payloadOffset) {
            std::memmove(pktData + payloadOffset,
                         pktData + doorbellOffset,
                         kDoorbellEntrySize);
        }

        // Metadata entry wire format is doorbell format + producer phase bit.
        pktData[payloadOffset] =
            static_cast<uint8_t>((pktData[payloadOffset] & ~0x08) |
                                 ((slotPhase ? 1u : 0u) << 3));

        pkt->setAddr(remapAddr);

        uint64_t metaLo = 0;
        uint64_t metaHi = 0;
        std::memcpy(&metaLo, pktData + payloadOffset, sizeof(metaLo));
        std::memcpy(&metaHi, pktData + payloadOffset + sizeof(metaLo), sizeof(metaHi));
        DPRINTF(CXLRPCEngine,
                "Request remapped to metadata queue: client=%u slot=%u "
                "slot_addr=%#x remap_addr=%#x offset=%u phase=%u req_id=%u "
                "meta_lo=%#x meta_hi=%#x\n",
                observedClientId, slot, slotAddr, remapAddr,
                static_cast<unsigned>(payloadOffset), slotPhase ? 1 : 0,
                entry.request_id, metaLo, metaHi);
        return DoorbellHandleResult::Remapped;
    }

    DPRINTF(CXLRPCEngine,
            "processRequest fallback: pkt_valid=%d isWrite=%d size_ok=%d "
            "covers_doorbell=%d (pkt_addr=%#x size=%u conn_db=%#x)\n",
            pkt ? 1 : 0,
            (pkt && pkt->isWrite()) ? 1 : 0,
            (pkt && pkt->getSize() >= 16) ? 1 : 0,
            coversDoorbell ? 1 : 0,
            pkt ? pkt->getAddr() : 0,
            pkt ? pkt->getSize() : 0,
            doorbell_addr);

    // Request doorbells that are not remappable must not take a functional
    // side path. Let them pass through unchanged.
    DPRINTF(CXLRPCEngine,
            "processRequest: unable to remap packet, returning NotHandled\n");
    return DoorbellHandleResult::NotHandled;
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
                "HEAD_UPDATE ignored for client %u: capacity is zero\n",
                conn.client_id);
        return DoorbellHandleResult::Handled;
    }
    if (new_head >= queue.capacity) {
        stats.invalidHeadUpdates++;
        DPRINTF(CXLRPCEngine,
                "HEAD_UPDATE ignored for client %u: new_head=%u >= capacity=%u\n",
                conn.client_id, new_head, queue.capacity);
        return DoorbellHandleResult::Handled;
    }

    // HEAD_UPDATE only needs to update controller-side head state. Keep this
    // control-path update in-controller and skip downstream memory writes.
    queue.head_cached = new_head;
    stats.headUpdatesReceived++;

    if (pkt && pkt->isWrite()) {
        DPRINTF(CXLRPCEngine,
                "HEAD_UPDATE consumed in-controller: client=%u new_head=%u "
                "doorbell=%#x pkt_addr=%#x size=%u\n",
                conn.client_id, new_head, doorbell_addr,
                pkt->getAddr(), pkt->getSize());
        return DoorbellHandleResult::Consumed;
    }

    DPRINTF(CXLRPCEngine,
            "HEAD_UPDATE applied (internal): client=%u new_head=%u\n",
            conn.client_id, new_head);
    return DoorbellHandleResult::Handled;
}

DoorbellHandleResult
CXLRPCEngine::processControl(ClientConnection& conn, const DoorbellEntry& entry)
{
    if (entry.service_id == kControlInternalService &&
        entry.method_id == kControlRegisterTranslation) {
        DPRINTF(CXLRPCEngine,
                "Control REGISTER_TRANSLATION for client %u (doorbell=%#x)\n",
                conn.client_id, static_cast<Addr>(entry.data));
        return DoorbellHandleResult::Handled;
    }

    if (entry.service_id == kControlInternalService &&
        entry.method_id == kControlRegisterMetadataPageTranslation) {
        constexpr Addr kMetadataPageSize = 0x1000;
        constexpr Addr kMetadataPageMask = ~(kMetadataPageSize - 1);
        const uint64_t pageIndex =
            (static_cast<uint64_t>(entry.request_id & 0xFFFFu)) |
            (static_cast<uint64_t>(entry.payload_len & 0xFFFFFu) << 16);
        const Addr logicalPage =
            (conn.metadata_queue_logical_base & kMetadataPageMask) +
            (static_cast<Addr>(pageIndex) * kMetadataPageSize);
        const Addr observedPage = static_cast<Addr>(entry.data) & kMetadataPageMask;

        for (Addr off = 0; off < kMetadataPageSize; off += 64) {
            conn.metadata_line_addr_map[logicalPage + off] = observedPage + off;
        }

        DPRINTF(CXLRPCEngine,
                "Control REGISTER_METADATA_PAGE_TRANSLATION for client %u "
                "page=%#llx logical=%#x observed=%#x\n",
                conn.client_id,
                static_cast<unsigned long long>(pageIndex),
                logicalPage, observedPage);
        return DoorbellHandleResult::Handled;
    }

    uint32_t control = static_cast<uint32_t>(entry.data & 0xFFFFFFFFu);
    if (control != kControlResetClientState) {
        DPRINTF(CXLRPCEngine,
                "Unknown control op=%u for client %u\n",
                control, conn.client_id);
        return DoorbellHandleResult::Handled;
    }

    DPRINTF(CXLRPCEngine,
            "Control RESET_CLIENT_STATE for client %u\n",
            conn.client_id);

    // Reset metadata queue producer-side state.
    conn.metadata_queue.tail = 0;
    conn.metadata_queue.head_cached = 0;
    conn.metadata_queue.current_phase = true;

    return DoorbellHandleResult::Handled;
}

void
CXLRPCEngine::applyAddressTranslationDelta(int64_t delta)
{
    if (connections.empty()) {
        return;
    }

    std::unordered_map<Addr, ClientConnection> translated;
    translated.reserve(connections.size());

    for (const auto& pair : connections) {
        ClientConnection conn = pair.second;
        conn.doorbell_addr = addSignedDelta(conn.doorbell_addr, delta);
        const Addr logicalMqLine = conn.metadata_queue_logical_base & ~Addr(0x3F);
        const Addr observedMqLine = addSignedDelta(logicalMqLine, delta);
        const Addr logicalMqOffset = conn.metadata_queue_logical_base - logicalMqLine;
        conn.metadata_queue.base_addr = observedMqLine + logicalMqOffset;
        conn.request_data_addr = addSignedDelta(conn.request_data_addr, delta);
        conn.response_data_addr = addSignedDelta(conn.response_data_addr, delta);
        conn.flag_addr = addSignedDelta(conn.flag_addr, delta);
        conn.metadata_line_addr_map.clear();
        conn.metadata_line_addr_map[logicalMqLine] = observedMqLine;
        translated[conn.doorbell_addr] = conn;
    }

    connections.swap(translated);
    rebuildDoorbellSlotIndex();
    remappedDoorbells.clear();
    addrTranslationLearned = true;

    DPRINTF(CXLRPCEngine,
            "Applied RPC address translation delta=%#llx to %u connection(s)\n",
            static_cast<long long>(delta),
            static_cast<unsigned>(connections.size()));
}

bool
CXLRPCEngine::tryLearnAddressTranslation(PacketPtr pkt)
{
    if (!pkt || !pkt->isWrite() || !pkt->hasData())
        return false;

    const uint8_t* payload = pkt->getConstPtr<uint8_t>();
    if (!payload)
        return false;

    DoorbellEntry entry;
    uint8_t raw[16];
    Addr observedAddr = pkt->getAddr();

    if (pkt->getSize() < 16)
        return false;
    memcpy(raw, payload, sizeof(raw));
    entry.parseFromBuffer(raw);

    if (entry.method == METHOD_CONTROL) {
        DPRINTF(CXLRPCEngine,
                "tryLearn control candidate: addr=%#x size=%u svc=%u mid=%u "
                "inline=%u req_id=%u len=%u data=%#x\n",
                observedAddr, pkt->getSize(), entry.service_id, entry.method_id,
                entry.is_inline, entry.request_id, entry.payload_len, entry.data);
    }

    if (entry.method != METHOD_CONTROL || entry.is_inline != 1)
        return false;

    if (entry.service_id == kControlInternalService &&
        entry.method_id == kControlRegisterTranslation) {
        const Addr logicalDoorbell = static_cast<Addr>(entry.data);
        if (logicalDoorbell == 0)
            return false;

        int64_t delta = 0;
        if (observedAddr >= logicalDoorbell) {
            delta = static_cast<int64_t>(observedAddr - logicalDoorbell);
        } else {
            delta = -static_cast<int64_t>(logicalDoorbell - observedAddr);
        }

        Addr canonicalDoorbell = 0;
        if (findConnectionForDoorbellAddr(logicalDoorbell,
                                          &canonicalDoorbell) != nullptr) {
            DPRINTF(CXLRPCEngine,
                    "Learning RPC address translation from REGISTER_TRANSLATION: "
                    "logical=%#x canonical=%#x observed=%#x delta=%#llx\n",
                    logicalDoorbell, canonicalDoorbell, observedAddr,
                    static_cast<long long>(delta));
            applyAddressTranslationDelta(delta);
            return true;
        }

        DPRINTF(CXLRPCEngine,
                "REGISTER_TRANSLATION ignored: logical=%#x is outside "
                "statically registered doorbell slots\n",
                logicalDoorbell);
        return false;
    }

    return false;
}

void
CXLRPCEngine::registerConnection(uint32_t client_id,
                                  Addr doorbell_addr,
                                  Addr metadata_queue_addr,
                                  uint32_t metadata_queue_size,
                                  Addr request_data_addr,
                                  uint32_t request_data_capacity,
                                  Addr response_data_addr,
                                  uint32_t response_data_capacity,
                                  Addr flag_addr)
{
    ClientConnection conn;
    conn.client_id = client_id;
    conn.doorbell_addr = doorbell_addr;
    conn.request_data_addr = request_data_addr;
    conn.request_data_capacity = request_data_capacity;
    conn.response_data_addr = response_data_addr;
    conn.response_data_capacity = response_data_capacity;
    conn.flag_addr = flag_addr;

    conn.metadata_queue.base_addr = metadata_queue_addr;
    conn.metadata_queue_logical_base = metadata_queue_addr;
    conn.metadata_queue.capacity = metadata_queue_size;
    conn.metadata_queue.tail = 0;
    conn.metadata_queue.head_cached = 0;
    // Initial metadata slots are zero (phase=0). Producer starts from phase=1.
    conn.metadata_queue.current_phase = true;
    conn.metadata_line_addr_map.clear();
    conn.metadata_line_addr_map[metadata_queue_addr & ~Addr(0x3F)] =
        metadata_queue_addr & ~Addr(0x3F);

    connections[doorbell_addr] = conn;
    rebuildDoorbellSlotIndex();

    DPRINTF(CXLRPCEngine, "Registered client %d: doorbell=%#x, "
            "metadata=%#x, request_data=%#x(cap=%u), "
            "response_data=%#x(cap=%u), flag=%#x\n",
            client_id, doorbell_addr, metadata_queue_addr,
            request_data_addr, request_data_capacity,
            response_data_addr, response_data_capacity, flag_addr);
}

void
CXLRPCEngine::commitRemappedDoorbell(PacketPtr pkt)
{
    auto it = remappedDoorbells.find(pkt);
    if (it == remappedDoorbells.end()) {
        return;
    }

    if (it->second.countAsMetadataWrite) {
        stats.requestsForwarded++;
        stats.metadataQueueWrites++;
    }

    remappedDoorbells.erase(it);
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
