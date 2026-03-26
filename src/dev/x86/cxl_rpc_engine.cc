/*
 * CXL RPC Engine implementation
 */

#include "dev/x86/cxl_rpc_engine.hh"

#include <algorithm>
#include <cstring>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/CXLRPCEngine.hh"
namespace gem5
{

namespace
{

constexpr Addr kDoorbellSlotStride = 0x40;
constexpr Addr kDoorbellPageSize = 0x1000;
constexpr Addr kDoorbellPageMask = ~(kDoorbellPageSize - 1);
constexpr Addr kMetadataPageSize = 0x1000;
constexpr Addr kMetadataPageMask = ~(kMetadataPageSize - 1);
constexpr uint32_t kBootstrapLenMagic = 0xFFFF0000u;
constexpr uint16_t kBootstrapOpRegisterDoorbell = 0x0001u;
constexpr uint16_t kBootstrapOpRegisterDoorbellPageBase = 0x0200u;
constexpr uint16_t kBootstrapOpRegisterMetadataPageBase = 0x0100u;

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
      defaultFlagAddr(p.default_flag_addr)
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

void
CXLRPCEngine::bindDoorbellPage(ClientConnection& conn,
                               Addr logical_page,
                               Addr observed_page)
{
    auto existing = conn.doorbell_page_addr_map.find(logical_page);
    if (existing != conn.doorbell_page_addr_map.end() &&
        existing->second != observed_page) {
        auto observed = observedDoorbellPages.find(existing->second);
        if (observed != observedDoorbellPages.end() &&
            observed->second.connection == &conn &&
            observed->second.logicalPage == logical_page) {
            observedDoorbellPages.erase(observed);
        }
    }

    conn.doorbell_page_addr_map[logical_page] = observed_page;
    observedDoorbellPages[observed_page] =
        ObservedDoorbellPageBinding{logical_page, &conn};
}

void
CXLRPCEngine::unbindDoorbellPages(const ClientConnection& conn)
{
    for (const auto& doorbellPage : conn.doorbell_page_addr_map) {
        auto observed = observedDoorbellPages.find(doorbellPage.second);
        if (observed != observedDoorbellPages.end() &&
            observed->second.connection == &conn &&
            observed->second.logicalPage == doorbellPage.first) {
            observedDoorbellPages.erase(observed);
        }
    }
}

ClientConnection*
CXLRPCEngine::findConnectionForAddr(Addr addr, uint32_t size,
                                    Addr& base_addr)
{
    return const_cast<ClientConnection *>(
        static_cast<const CXLRPCEngine *>(this)->findConnectionForAddr(
            addr, size, base_addr));
}

const ClientConnection*
CXLRPCEngine::findConnectionForAddr(Addr addr, uint32_t size,
                                    Addr& base_addr) const
{
    if (size == 0) {
        return nullptr;
    }

    const Addr pktStart = addr;
    Addr pktEnd = addr + static_cast<Addr>(size);
    if (pktEnd < addr) {
        pktEnd = MaxAddr;
    }

    const Addr logicalDoorbellBytes = doorbellRange.size();
    if (observedDoorbellPages.empty()) {
        return nullptr;
    }

    Addr observedPage = pktStart & kDoorbellPageMask;
    while (true) {
        auto binding = observedDoorbellPages.find(observedPage);
        if (binding != observedDoorbellPages.end()) {
            const ClientConnection& conn = *binding->second.connection;
            const Addr logicalPage = binding->second.logicalPage;
            Addr observedPageEnd = observedPage + kDoorbellPageSize;
            if (observedPageEnd < observedPage) {
                observedPageEnd = MaxAddr;
            }
            if (rangesOverlap(pktStart, pktEnd, observedPage, observedPageEnd)) {
                const Addr overlapStart = std::max(pktStart, observedPage);
                const Addr logicalOverlap =
                    logicalPage + (overlapStart - observedPage);
                if (logicalOverlap >= conn.doorbell_addr) {
                    const Addr logicalOffset = logicalOverlap - conn.doorbell_addr;
                    if (logicalOffset < logicalDoorbellBytes) {
                        const uint32_t resolvedSlotIndex =
                            static_cast<uint32_t>(
                                logicalOffset / kDoorbellSlotStride);
                        const Addr resolvedLogicalAddr =
                            conn.doorbell_addr +
                            static_cast<Addr>(resolvedSlotIndex) *
                                kDoorbellSlotStride;
                        const Addr logicalPageBase =
                            resolvedLogicalAddr & kDoorbellPageMask;
                        const Addr pageSlotOffset =
                            resolvedLogicalAddr - logicalPageBase;
                        if (logicalPageBase == logicalPage &&
                            pageSlotOffset < kDoorbellPageSize) {
                            base_addr = observedPage + pageSlotOffset;
                            return &conn;
                        }
                    }
                }
            }
        }

        if (observedPage > MaxAddr - kDoorbellPageSize ||
            observedPage + kDoorbellPageSize >= pktEnd) {
            break;
        }
        observedPage += kDoorbellPageSize;
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

    panic("CXLRPCEngine missing metadata translation: node=%u slot=%u "
          "logical_slot=%#x logical_line=%#x logical_base=%#x\n",
          conn.node_id, slot, logicalSlotAddr, logicalLine, logicalBase);
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
        return probe;
    }

    Addr base_addr = 0;
    const ClientConnection* conn =
        findConnectionForAddr(addr, size, base_addr);
    if (conn) {
        probe.connection = conn;
        probe.doorbell_addr = base_addr;
        if (pkt->hasData()) {
            const Addr dbStart = base_addr;
            const Addr dbEnd = base_addr + 16;
            if (addr <= dbStart && pktEnd >= dbEnd) {
                const size_t doorbellOffset =
                    static_cast<size_t>(dbStart - addr);
                probe.entry.parseFromBuffer(pkt->getConstPtr<uint8_t>() +
                                            doorbellOffset);
                probe.should_probe = true;
            }
        }
    } else if (pkt->hasData() && (addr % kDoorbellSlotStride) == 0) {
        DoorbellEntry entry;
        const uint8_t* raw = pkt->getConstPtr<uint8_t>();
        entry.parseFromBuffer(raw);
        if (isBootstrapRequest(entry)) {
            probe.should_probe = true;
            probe.entry = entry;
        }
    }

    return probe;
}

DoorbellHandleResult
CXLRPCEngine::handleDoorbellWrite(PacketPtr pkt,
                                  const DoorbellWriteProbe* probe)
{
    if (!pkt || !probe || !probe->should_probe)
        return DoorbellHandleResult::NotHandled;

    if (!probe->connection) {
        if (consumeBootstrapDoorbellBinding(pkt, *probe)) {
            return DoorbellHandleResult::Consumed;
        }
        return DoorbellHandleResult::NotHandled;
    }

    ClientConnection* conn = const_cast<ClientConnection*>(probe->connection);

    DPRINTF(CXLRPCEngine, "Doorbell write: method=%u, node_id=%u, "
            "rpc_id=%u, len=%u, inline=%u\n",
            static_cast<unsigned>(probe->entry.method),
            static_cast<unsigned>(probe->entry.node_id),
            static_cast<unsigned>(probe->entry.rpc_id),
            static_cast<unsigned>(probe->entry.length),
            static_cast<unsigned>(probe->entry.is_inline));

    return processDoorbellEntry(*conn,
                                probe->doorbell_addr,
                                probe->entry,
                                pkt);
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
            return processHeadUpdate(conn, doorbell_addr,
                                     entry, pkt);
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

    const size_t pktSize = pkt->getSize();
    MetadataQueue& queue = conn.metadata_queue;

    // Check if queue is full only for validated request doorbells.
    if (queue.isFull()) {
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

    DoorbellEntry remapped = entry;
    remapped.phase = slotPhase ? 1u : 0u;
    // The remapped metadata entry is fully materialized here, so no source
    // payload shuffle is needed even when the packet spans a full cache line.
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
    // HEAD_UPDATE only needs to update controller-side head state. Keep this
    // control-path update in-controller and skip downstream memory writes.
    queue.head_cached = new_head;

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

    if ((opcode & 0xFF00u) == kBootstrapOpRegisterDoorbellPageBase) {
        const uint32_t pageIndex = static_cast<uint32_t>(opcode & 0x00FFu);
        const Addr logicalBase = conn.doorbell_addr & kDoorbellPageMask;
        const Addr logicalPage =
            logicalBase + static_cast<Addr>(pageIndex) * kDoorbellPageSize;
        const Addr observedPage =
            static_cast<Addr>(entry.data) & kDoorbellPageMask;
        const Addr queueBytes = doorbellRange.size();

        if (observedPage == 0 ||
            logicalPage < logicalBase ||
            (logicalPage - logicalBase) >= queueBytes) {
            DPRINTF(CXLRPCEngine,
                    "Ignoring bootstrap doorbell-page registration: node=%u "
                    "page=%u logical=%#x observed=%#x doorbell_bytes=%u\n",
                    conn.node_id, pageIndex, logicalPage, observedPage,
                    static_cast<unsigned>(queueBytes));
            return DoorbellHandleResult::Consumed;
        }

        bindDoorbellPage(conn, logicalPage, observedPage);

        DPRINTF(CXLRPCEngine,
                "Registered doorbell page translation: node=%u page=%u "
                "logical=%#x observed=%#x\n",
                conn.node_id, pageIndex, logicalPage, observedPage);
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
CXLRPCEngine::consumeBootstrapDoorbellBinding(PacketPtr pkt,
                                              const DoorbellWriteProbe& probe)
{
    if (!pkt || !pkt->isWrite() || !pkt->hasData() || pkt->getSize() < 16 ||
        connections.size() != 1 || (pkt->getAddr() % kDoorbellSlotStride) != 0) {
        return false;
    }

    if (!probe.should_probe || !isBootstrapRequest(probe.entry) ||
        bootstrapOpcode(probe.entry.length) != kBootstrapOpRegisterDoorbell) {
        return false;
    }

    auto it = connections.begin();
    ClientConnection& conn = it->second;
    const Addr observedCanonical = pkt->getAddr();
    const Addr logicalCanonical = conn.doorbell_addr;
    const Addr observedPage = observedCanonical & kDoorbellPageMask;
    const Addr logicalPage = logicalCanonical & kDoorbellPageMask;
    auto existing = conn.doorbell_page_addr_map.find(logicalPage);
    if (existing != conn.doorbell_page_addr_map.end() &&
        existing->second == observedPage) {
        return false;
    }

    bindDoorbellPage(conn, logicalPage, observedPage);

    DPRINTF(CXLRPCEngine,
            "Bound bootstrap doorbell canonical page: logical=%#x "
            "observed=%#x node=%u\n",
            logicalCanonical, observedPage, conn.node_id);
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
    auto existing = connections.find(doorbell_addr);
    if (existing != connections.end()) {
        unbindDoorbellPages(existing->second);
    }

    ClientConnection conn;
    conn.node_id = node_id;
    conn.doorbell_addr = doorbell_addr;
    conn.request_data_addr = request_data_addr;
    conn.request_data_capacity = request_data_capacity;
    conn.response_data_addr = response_data_addr;
    conn.response_data_capacity = response_data_capacity;
    conn.flag_addr = flag_addr;
    conn.doorbell_page_addr_map.clear();

    conn.metadata_queue.base_addr = 0;
    conn.metadata_queue_logical_base = metadata_queue_addr;
    conn.metadata_queue.capacity = metadata_queue_entries;
    conn.metadata_queue.tail = 0;
    conn.metadata_queue.head_cached = 0;
    // Initial metadata slots are zero (phase=0). Producer starts from phase=1.
    conn.metadata_queue.current_phase = true;
    conn.metadata_line_addr_map.clear();

    connections[doorbell_addr] = conn;

    DPRINTF(CXLRPCEngine, "Registered node %d: doorbell=%#x, "
            "metadata=%#x, request_data=%#x(cap=%u), "
            "response_data=%#x(cap=%u), flag=%#x\n",
            node_id, doorbell_addr, metadata_queue_addr,
            request_data_addr, request_data_capacity,
            response_data_addr, response_data_capacity, flag_addr);
}

} // namespace gem5
