from m5.params import *
from m5.SimObject import SimObject

class CXLRPCEngine(SimObject):
    type = 'CXLRPCEngine'
    cxx_header = "dev/x86/cxl_rpc_engine.hh"
    cxx_class = 'gem5::CXLRPCEngine'

    # Doorbell address range that this engine monitors.
    # The current public RPC layout reserves 16KiB at region-0 base so slot0
    # plus all logical client doorbells stay ahead of the metadata queue.
    doorbell_range = Param.AddrRange(AddrRange(0x100000000, size=0x4000),
        "Address range for doorbell writes")

    # Auto-register a default test connection on startup
    auto_register = Param.Bool(False,
        "Auto-register default connection on startup")
    default_node_id = Param.UInt32(0, "Default test node ID")
    default_doorbell_addr = Param.Addr(0x100000000, "Default doorbell address")
    default_metadata_queue_addr = Param.Addr(0x100004000,
        "Metadata queue base address")
    default_metadata_queue_entries = Param.UInt32(1024,
        "Metadata queue entries")
    default_request_data_addr = Param.Addr(0x100008000,
        "Request data base address")
    default_request_data_capacity = Param.UInt32(10 * 1024 * 1024,
        "Request data capacity in bytes")
    default_response_data_addr = Param.Addr(0x100A08000,
        "Response data base address")
    default_response_data_capacity = Param.UInt32(10 * 1024 * 1024,
        "Response data capacity in bytes")
    default_flag_addr = Param.Addr(0x101408000, "Flag address")
