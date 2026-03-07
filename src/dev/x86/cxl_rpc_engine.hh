/*
 * CXL RPC Engine - Doorbell interception and metadata queue management
 *
 * This component is responsible for:
 * 1. Intercepting doorbell writes from clients
 * 2. Filling phase bits for metadata queue wrap-around detection
 * 3. Writing to metadata queue for server consumption
 * 4. Handling HEAD_UPDATE messages from server
 */

#ifndef __DEV_X86_CXL_RPC_ENGINE_HH__
#define __DEV_X86_CXL_RPC_ENGINE_HH__

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "base/addr_range.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/packet.hh"
#include "params/CXLRPCEngine.hh"
#include "sim/sim_object.hh"

namespace gem5
{

/**
 * Doorbell entry structure (16 bytes)
 *
 * Byte 0:    method(2b) + inline(1b) + phase_bit(1b) + reserved(4b)
 * Byte 1-2:  service_id (16b)
 * Byte 3-4:  method_id (16b)
 * Byte 5-6:  request_id (16b)
 * Byte 7:    payload_len (8b)
 * Byte 8-15: inline message or request_data address
 */
struct DoorbellEntry {
    // Parsed fields (populated by parseFromBuffer)
    uint8_t method;       // 0=request, 1=response, 2=HEAD_UPDATE
    uint8_t is_inline;    // 1=inline message, 0=pointer to request_data
    uint8_t phase_bit;    // Phase bit for wrap-around detection
    uint16_t service_id;
    uint16_t method_id;
    uint32_t request_id;  // Only low 16 bits used
    uint8_t payload_len;
    uint64_t data;        // Inline message or address

    DoorbellEntry() : method(0), is_inline(0), phase_bit(0),
                      service_id(0), method_id(0), request_id(0),
                      payload_len(0), data(0) {}

    // Parse from raw 16-byte buffer
    void parseFromBuffer(const uint8_t* buf);

    // Serialize to raw 16-byte buffer
    void serializeToBuffer(uint8_t* buf) const;
};

// Method types
enum DoorbellMethod {
    METHOD_REQUEST = 0,
    METHOD_RESPONSE = 1,
    METHOD_HEAD_UPDATE = 2,
    METHOD_CONTROL = 3
};

// Result of processing a doorbell write.
enum class DoorbellHandleResult {
    NotHandled = 0,   // Not a registered doorbell/entry.
    Handled = 1,      // Observed/handled in software, still forward pkt.
    Remapped = 2,     // Doorbell rewritten to metadata target.
    Consumed = 3,     // Fully consumed in-controller, do not forward pkt.
    Retry = 4         // Temporarily backpressure and ask sender to retry.
};

/**
 * Per-client metadata queue state
 */
struct MetadataQueue {
    struct TailReservation {
        uint32_t slot;
        bool phase;
    };

    Addr base_addr;          // Base address of metadata queue in shared memory
    uint32_t capacity;       // Queue capacity (number of entries)
    uint32_t tail;           // Tail index (controller writes here)
    uint32_t head_cached;    // Cached head from server (for flow control)
    bool current_phase;      // Current phase bit value

    MetadataQueue() : base_addr(0), capacity(0), tail(0),
                      head_cached(0), current_phase(true) {}

    // Check if queue is full (for flow control)
    bool isFull() const {
        if (capacity == 0) return true;  // Prevent division by zero
        uint32_t next_tail = (tail + 1) % capacity;
        return (next_tail == head_cached);
    }

    // Producer phase bit semantics:
    //   - Metadata memory is zero-initialized at startup (entry phase=0).
    //   - First pass writes use phase=1 (current_phase=true).
    //   - On wrap-around, phase toggles.
    // This matches consumer logic: expected phase starts at 1 and toggles
    // whenever server head wraps to slot 0.
    // Reserve current tail slot and return the phase value that must be written
    // into that slot. The phase flips only for subsequent wraps.
    TailReservation reserveTail() {
        if (capacity == 0) return TailReservation{0, current_phase};
        const bool slot_phase = current_phase;
        uint32_t slot = tail;
        tail = (tail + 1) % capacity;
        if (tail == 0) {
            current_phase = !current_phase;
        }
        return TailReservation{slot, slot_phase};
    }
};

/**
 * Per-client connection state
 */
struct ClientConnection {
    uint32_t client_id;

    // Client-side memory regions
    Addr request_data_addr;
    Addr response_data_addr;
    Addr flag_addr;

    // Server-side resources for this client
    Addr doorbell_addr;
    MetadataQueue metadata_queue;
    Addr metadata_queue_logical_base;  // Untranslated logical MQ base addr
    std::unordered_map<Addr, Addr> metadata_line_addr_map;  // logical line -> observed line

    // Capacity information
    uint32_t request_data_capacity;
    uint32_t response_data_capacity;

    ClientConnection() : client_id(0), request_data_addr(0),
                         response_data_addr(0), flag_addr(0),
                         doorbell_addr(0),
                         metadata_queue_logical_base(0),
                         request_data_capacity(0),
                         response_data_capacity(0) {}
};

/**
 * CXL RPC Engine
 *
 * Intercepts doorbell writes and manages metadata queues for RPC communication.
 * This object is a control-plane helper invoked by CXLMemCtrl.
 */
class CXLRPCEngine : public SimObject
{
  private:
    // Doorbell address range fallback (used before connections are ready).
    AddrRange doorbellRange;

    // Registered client connections (doorbell_addr -> connection)
    std::unordered_map<Addr, ClientConnection> connections;
    // Fast lookup index built from connections:
    // - exact slot start -> canonical doorbell address
    std::unordered_map<Addr, Addr> doorbellSlotToCanonical;

    // Auto-register parameters
    bool autoRegister;
    uint32_t defaultClientId;
    Addr defaultDoorbellAddr;
    Addr defaultMetadataQueueAddr;
    uint32_t defaultMetadataQueueEntries;
    Addr defaultRequestDataAddr;
    uint32_t defaultRequestDataCapacity;
    Addr defaultResponseDataAddr;
    uint32_t defaultResponseDataCapacity;
    Addr defaultFlagAddr;

    // Find connection whose doorbell slot overlaps [addr, addr + size).
    // base_addr is set to the matched slot start address.
    ClientConnection* findConnectionForAddr(
        Addr addr, uint32_t size, Addr& base_addr);
    // Resolve a logical/observed doorbell address to a connection, including
    // per-client slot aliases inside the configured doorbell segment.
    ClientConnection* findConnectionForDoorbellAddr(
        Addr doorbell_addr, Addr* canonical_addr = nullptr);
    const ClientConnection* findConnectionForDoorbellAddr(
        Addr doorbell_addr, Addr* canonical_addr = nullptr) const;
    void rebuildDoorbellSlotIndex();
    Addr resolveMetadataSlotAddr(const ClientConnection& conn,
                                 uint32_t slot) const;

    // Per-packet bookkeeping for request-doorbell remap path.
    struct RemappedDoorbellState {
        bool countAsMetadataWrite = true;
    };
    std::unordered_map<PacketPtr, RemappedDoorbellState> remappedDoorbells;
    std::vector<bool> remapByteEnableScratch;

    // Dynamic GPA translation learned from the first control-doorbell write.
    bool addrTranslationLearned = false;

    // Statistics
    struct RPCEngineStats : public statistics::Group
    {
        RPCEngineStats(CXLRPCEngine& engine);

        statistics::Scalar doorbellWrites;
        statistics::Scalar requestsForwarded;
        statistics::Scalar headUpdatesReceived;
        statistics::Scalar invalidHeadUpdates;
        statistics::Scalar queueFullEvents;
        statistics::Scalar metadataQueueWrites;
    };

    RPCEngineStats stats;

  public:
    PARAMS(CXLRPCEngine);
    CXLRPCEngine(const Params& p);

    void startup() override;

    /**
     * Check if an address targets a registered doorbell segment.
     *
     * When connections are registered, this checks each connection's
     * contiguous doorbell segment (base + slot-stride range). Before startup
     * registration, it falls back to the configured doorbell range.
     */
    bool isDoorbell(Addr addr) const;

    /**
     * Fast pre-filter: returns true if this write MUST be forwarded to
     * handleDoorbellWrite(), false when it is definitely a plain data store
     * that can be skipped entirely.
     *
     * During bootstrap this still passes control/marker writes needed by
     * tryLearnAddressTranslation(), while dropping obvious non-doorbell
     * payload writes before the expensive parse/remap path.
     */
    bool mustProbeWrite(PacketPtr pkt) const;

    /**
     * Handle a doorbell write from client
     *
     * @param pkt The packet containing the doorbell data
     * @return Handling result (handled / not handled / backpressure)
     */
    DoorbellHandleResult handleDoorbellWrite(PacketPtr pkt);

    /**
     * Finalize a request-doorbell remap in timing/atomic modes.
     */
    void commitRemappedDoorbell(PacketPtr pkt);

    /**
     * Register a new client connection
     */
    void registerConnection(uint32_t client_id,
                            Addr doorbell_addr,
                            Addr metadata_queue_addr,
                            uint32_t metadata_queue_size,
                            Addr request_data_addr,
                            uint32_t request_data_capacity,
                            Addr response_data_addr,
                            uint32_t response_data_capacity,
                            Addr flag_addr);

  private:
    /**
     * Process a complete doorbell entry (dispatch by method type)
     */
    DoorbellHandleResult processDoorbellEntry(
        ClientConnection& conn, Addr doorbell_addr,
        const DoorbellEntry& entry, PacketPtr pkt);

    /**
     * Process a request doorbell
     * @return Handling result (handled or backpressure)
     */
    DoorbellHandleResult processRequest(
        ClientConnection& conn, Addr doorbell_addr,
        const DoorbellEntry& entry, PacketPtr pkt);

    /**
     * Process a HEAD_UPDATE doorbell
     */
    DoorbellHandleResult processHeadUpdate(
        ClientConnection& conn, Addr doorbell_addr,
        const DoorbellEntry& entry, PacketPtr pkt);
    DoorbellHandleResult processControl(
        ClientConnection& conn, const DoorbellEntry& entry);

    /**
     * Apply a single address delta to all registered connection regions.
     * This is used when software logical addresses differ from observed
     * packet GPAs under NUMA/KM allocation paths.
     */
    void applyAddressTranslationDelta(int64_t delta);

    /**
     * Best-effort bootstrap: for unregistered-address writes, detect a
     * REGISTER_TRANSLATION control doorbell and infer address delta once.
     *
     * @return true if translation was applied.
     */
    bool tryLearnAddressTranslation(PacketPtr pkt);
};

} // namespace gem5

#endif // __DEV_X86_CXL_RPC_ENGINE_HH__
