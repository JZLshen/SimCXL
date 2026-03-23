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

#include <array>
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
 * Bit 0:       method (0=request, 1=head_update)
 * Bit 1:       inline (1b)
 * Bit 2:       phase (1b)
 * Bits 3-34:   length (32b)
 * Bits 35-48:  node_id (14b)
 * Bits 49-63:  rpc_id (15b)
 * Byte 8-15: inline message or request_data address
 */
struct DoorbellEntry {
    // Parsed fields (populated by parseFromBuffer)
    uint8_t method;       // 0=request, 1=head_update
    uint8_t is_inline;    // 1=inline message, 0=pointer to request_data
    uint8_t phase;        // Valid/phase bit used by metadata queue
    uint16_t node_id;     // low 14 bits used
    uint16_t rpc_id;      // low 15 bits used, 0 is invalid
    uint32_t length;      // full 32-bit request length
    uint64_t data;        // Inline message or address

    DoorbellEntry() : method(0), is_inline(0), phase(0),
                      node_id(0), rpc_id(0), length(0), data(0) {}

    // Parse from raw 16-byte buffer
    void parseFromBuffer(const uint8_t* buf);

    // Serialize to raw 16-byte buffer
    void serializeToBuffer(uint8_t* buf) const;
};

// Method types
enum DoorbellMethod {
    METHOD_REQUEST = 0,
    METHOD_HEAD_UPDATE = 1
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
    uint32_t node_id;

    // Client-side memory regions
    Addr request_data_addr;
    Addr response_data_addr;
    Addr flag_addr;

    // Server-side resources for this client
    Addr doorbell_addr;
    std::unordered_map<Addr, Addr> doorbell_page_addr_map;  // logical page -> observed page
    MetadataQueue metadata_queue;
    Addr metadata_queue_logical_base;  // Untranslated logical MQ base addr
    std::unordered_map<Addr, Addr> metadata_line_addr_map;  // logical line -> observed line

    // Capacity information
    uint32_t request_data_capacity;
    uint32_t response_data_capacity;

    ClientConnection() : node_id(0), request_data_addr(0),
                         response_data_addr(0), flag_addr(0),
                         doorbell_addr(0),
                         metadata_queue_logical_base(0),
                         request_data_capacity(0),
                         response_data_capacity(0) {}
};

struct DoorbellWriteProbe {
    bool should_probe = false;
    bool matched_connection = false;
    bool covers_doorbell = false;
    bool parsed_entry = false;
    Addr doorbell_addr = 0;
    Addr logical_doorbell_addr = 0;
    size_t doorbell_offset = 0;
    uint32_t slot_index = 0;
    const ClientConnection* connection = nullptr;
    DoorbellEntry entry;
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
    // Logical public doorbell span configured by the board layout.
    AddrRange doorbellRange;

    // Registered client connections (doorbell_addr -> connection)
    std::unordered_map<Addr, ClientConnection> connections;

    // Auto-register parameters
    bool autoRegister;
    uint32_t defaultNodeId;
    Addr defaultDoorbellAddr;
    Addr defaultMetadataQueueAddr;
    uint32_t defaultMetadataQueueEntries;
    Addr defaultRequestDataAddr;
    uint32_t defaultRequestDataCapacity;
    Addr defaultResponseDataAddr;
    uint32_t defaultResponseDataCapacity;
    Addr defaultFlagAddr;

    // Find connection whose doorbell slot overlaps [addr, addr + size).
    // base_addr is set to the matched observed slot start address.
    // logical_addr is set to the matched logical slot start address.
    ClientConnection* findConnectionForAddr(
        Addr addr, uint32_t size, Addr& base_addr,
        Addr* logical_addr = nullptr, uint32_t* slot_index = nullptr);
    const ClientConnection* findConnectionForAddr(
        Addr addr, uint32_t size, Addr& base_addr,
        Addr* logical_addr = nullptr, uint32_t* slot_index = nullptr) const;
    Addr resolveMetadataSlotAddr(const ClientConnection& conn,
                                 uint32_t slot) const;
    std::array<std::vector<bool>, 4> remapByteEnableMasks;
    std::vector<bool> remapByteEnableScratch;

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
     * Classify a write packet for doorbell handling. The result can be passed
     * to handleDoorbellWrite() to avoid repeating steady-state connection
     * lookup and doorbell slot derivation on the same packet.
     *
     * This is the one shared classification step for the controller hot path.
     * It identifies whether the write overlaps any registered doorbell slot
     * and, if so, returns the resolved slot start address plus any already
     * parsed 16B doorbell payload for reuse by handleDoorbellWrite().
     */
    DoorbellWriteProbe probeDoorbellWrite(PacketPtr pkt) const;

    /**
     * Handle a doorbell write from client
     *
     * @param pkt The packet containing the doorbell data
     * @return Handling result (handled / not handled / backpressure)
     */
    DoorbellHandleResult handleDoorbellWrite(
        PacketPtr pkt, const DoorbellWriteProbe* probe = nullptr);

    /**
     * Account for a remapped request doorbell that was successfully forwarded
     * downstream as one metadata queue write.
     */
    void accountRemappedDoorbellForward();

    /**
     * Register a new client connection
     */
    void registerConnection(uint32_t node_id,
                            Addr doorbell_addr,
                            Addr metadata_queue_addr,
                            uint32_t metadata_queue_entries,
                            Addr request_data_addr,
                            uint32_t request_data_capacity,
                            Addr response_data_addr,
                            uint32_t response_data_capacity,
                            Addr flag_addr);

  private:
    bool isBootstrapRequest(const DoorbellEntry& entry) const;
    DoorbellHandleResult processBootstrapRequest(
        ClientConnection& conn, Addr doorbell_addr,
        Addr logical_doorbell_addr, uint32_t slot_index,
        const DoorbellEntry& entry, PacketPtr pkt);
    bool consumeBootstrapDoorbellBinding(PacketPtr pkt);

    /**
     * Process a complete doorbell entry (dispatch by method type)
     */
    DoorbellHandleResult processDoorbellEntry(
        ClientConnection& conn, Addr doorbell_addr,
        Addr logical_doorbell_addr, uint32_t slot_index,
        const DoorbellEntry& entry, PacketPtr pkt);

    /**
     * Process a request doorbell
     * @return Handling result (handled or backpressure)
     */
    DoorbellHandleResult processRequest(
        ClientConnection& conn, Addr doorbell_addr,
        Addr logical_doorbell_addr, uint32_t slot_index,
        const DoorbellEntry& entry, PacketPtr pkt);

    /**
     * Process a HEAD_UPDATE doorbell
     */
    DoorbellHandleResult processHeadUpdate(
        ClientConnection& conn, Addr doorbell_addr,
        Addr logical_doorbell_addr, uint32_t slot_index,
        const DoorbellEntry& entry, PacketPtr pkt);
};

} // namespace gem5

#endif // __DEV_X86_CXL_RPC_ENGINE_HH__
