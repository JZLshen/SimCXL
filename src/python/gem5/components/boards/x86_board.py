# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


from typing import (
    List,
    Sequence,
)

from m5.objects import (
    Addr,
    AddrRange,
    BaseXBar,
    Bridge,
    CXLBridge,
    CXLMemCtrl,
    CowDiskImage,
    IdeDisk,
    IOXBar,
    CopyEngine,
    Pc,
    Port,
    RawDiskImage,
    X86E820Entry,
    X86FsLinux,
    X86IntelMPBus,
    X86IntelMPBusHierarchy,
    X86IntelMPIOAPIC,
    X86IntelMPIOIntAssignment,
    X86IntelMPProcessor,
    X86SMBiosBiosInformation,
)
from m5.objects.PciDevice import PciMemBar
from m5.objects.CXLRPCEngine import CXLRPCEngine
from m5.params import Latency
from m5.util.convert import toMemorySize

from ...isas import ISA
from ...resources.resource import AbstractResource
from ...utils.override import overrides
from ..cachehierarchies.abstract_cache_hierarchy import AbstractCacheHierarchy
from ..memory.abstract_memory_system import AbstractMemorySystem
from ..processors.abstract_processor import AbstractProcessor
from .abstract_system_board import AbstractSystemBoard
from .kernel_disk_workload import KernelDiskWorkload

_COPY_ENGINE_GENERAL_REG_SIZE = 0x80
_COPY_ENGINE_CHANNEL_REG_SIZE = 0x80
_COPY_ENGINE_LEGACY_BAR0_SIZE = 1024
_CXL_RPC_BASE_ADDR = 0x100000000
_CXL_RPC_PUBLIC_CXL_SIZE = 0x200000000
_CXL_RPC_CLIENT_REGION_SIZE = 0x02000000
_CXL_RPC_TOTAL_REGIONS = _CXL_RPC_PUBLIC_CXL_SIZE // _CXL_RPC_CLIENT_REGION_SIZE
_CXL_RPC_MAX_CLIENTS = _CXL_RPC_TOTAL_REGIONS - 1
_CXL_RPC_DOORBELL_SLOT_BYTES = 0x40
_CXL_RPC_RESERVED_DOORBELL_SLOTS = 1
_CXL_RPC_METADATA_Q_ENTRIES = 1024
_CXL_RPC_METADATA_ENTRY_BYTES = 16
_CXL_RPC_METADATA_Q_BYTES = (
    _CXL_RPC_METADATA_Q_ENTRIES * _CXL_RPC_METADATA_ENTRY_BYTES
)
_CXL_RPC_REQUEST_DATA_BYTES = 10 * 1024 * 1024
_CXL_RPC_RESPONSE_DATA_BYTES = 10 * 1024 * 1024
_CXL_RPC_FLAG_BYTES = 64
#
# Keep CopyEngines on bus 0 / function 0 only.
# Skip:
# - dev 0 to avoid colliding with the conventional host-bridge slot
# - dev 4 which is already used by the IDE controller
# - dev 6 which is already used by the CXL memory controller
_COPY_ENGINE_PCI_DEVS = tuple([1, 2, 3, 5] + list(range(7, 32)))


def _next_power_of_two(size: int) -> int:
    if size <= 1:
        return 1
    return 1 << (size - 1).bit_length()


def _align_up(value: int, align: int) -> int:
    return (value + align - 1) & ~(align - 1)


def _get_copy_engine_bar0_size(channel_count: int) -> int:
    required_size = _COPY_ENGINE_GENERAL_REG_SIZE + (
        channel_count * _COPY_ENGINE_CHANNEL_REG_SIZE
    )
    return max(
        _COPY_ENGINE_LEGACY_BAR0_SIZE,
        _next_power_of_two(required_size),
    )


def _get_public_cxl_rpc_layout() -> dict[str, int]:
    doorbell_region_bytes = _align_up(
        (_CXL_RPC_MAX_CLIENTS + _CXL_RPC_RESERVED_DOORBELL_SLOTS)
        * _CXL_RPC_DOORBELL_SLOT_BYTES,
        0x1000,
    )
    metadata_offset = doorbell_region_bytes
    request_data_offset = _align_up(
        metadata_offset + _CXL_RPC_METADATA_Q_BYTES, 0x1000
    )
    response_data_offset = _align_up(
        request_data_offset + _CXL_RPC_REQUEST_DATA_BYTES, 0x1000
    )
    flag_offset = _align_up(
        response_data_offset + _CXL_RPC_RESPONSE_DATA_BYTES, 0x1000
    )

    if flag_offset + _CXL_RPC_FLAG_BYTES > _CXL_RPC_CLIENT_REGION_SIZE:
        raise ValueError("Public CXL RPC layout no longer fits in one client region")

    return {
        "doorbell_offset": 0,
        "doorbell_region_bytes": doorbell_region_bytes,
        "metadata_offset": metadata_offset,
        "metadata_entries": _CXL_RPC_METADATA_Q_ENTRIES,
        "metadata_bytes": _CXL_RPC_METADATA_Q_BYTES,
        "request_data_offset": request_data_offset,
        "request_data_bytes": _CXL_RPC_REQUEST_DATA_BYTES,
        "response_data_offset": response_data_offset,
        "response_data_bytes": _CXL_RPC_RESPONSE_DATA_BYTES,
        "flag_offset": flag_offset,
    }


_CXL_RPC_PUBLIC_LAYOUT = _get_public_cxl_rpc_layout()


class X86Board(AbstractSystemBoard, KernelDiskWorkload):
    """
    A board capable of full system simulation for X86.

    **Limitations**
    * Currently, this board's memory is hardcoded to 3GB.
    * Much of the I/O subsystem is hard coded.
    """

    def __init__(
        self,
        clk_freq: str,
        processor: AbstractProcessor,
        memory: AbstractMemorySystem,
        cache_hierarchy: AbstractCacheHierarchy,
        cxl_memory: AbstractMemorySystem,
        is_asic: bool = True,
        num_copy_engines: int = 1,
        copy_engine_channels: int = 1,
        copy_engine_xfercap: str = "4KiB",
    ) -> None:
        if num_copy_engines < 1:
            raise ValueError("X86Board requires at least one CopyEngine.")
        if copy_engine_channels < 1:
            raise ValueError(
                "X86Board requires at least one CopyEngine channel."
            )
        if copy_engine_channels > 64:
            raise ValueError(
                "X86Board only supports up to 64 CopyEngine channels."
            )
        if num_copy_engines > len(_COPY_ENGINE_PCI_DEVS):
            raise ValueError(
                "X86Board only supports up to "
                f"{len(_COPY_ENGINE_PCI_DEVS)} single-function CopyEngines "
                "on PCI bus 0."
            )

        self._cxl_memory_ptr = cxl_memory
        self._is_asic = is_asic
        self._num_copy_engines = num_copy_engines
        self._copy_engine_channels = copy_engine_channels
        self._copy_engine_xfercap = copy_engine_xfercap

        super().__init__(
            clk_freq=clk_freq,
            processor=processor,
            memory=memory,
            cache_hierarchy=cache_hierarchy,
        )
        self.cxl_memory = self._cxl_memory_ptr

        if self.get_processor().get_isa() != ISA.X86:
            raise Exception(
                "The X86Board requires a processor using the X86 "
                f"ISA. Current processor ISA: '{processor.get_isa().name}'."
            )

    @overrides(AbstractSystemBoard)
    def _setup_board(self) -> None:
        self.pc = Pc()
        # cxl_device is dynamically initialized and attached
        self.pc.south_bridge.cxl_device = CXLMemCtrl(pci_func=0, pci_dev=6, pci_bus=0)

        selected_copy_engine_pci_devs = _COPY_ENGINE_PCI_DEVS[
            : self._num_copy_engines
        ]
        copy_engines = [
            CopyEngine(
                pci_func=0,
                pci_dev=pci_dev,
                pci_bus=0,
                ChanCnt=self._copy_engine_channels,
                XferCap=self._copy_engine_xfercap,
                BAR0=PciMemBar(
                    size=f"{_get_copy_engine_bar0_size(self._copy_engine_channels)}B"
                ),
            )
            for pci_dev in selected_copy_engine_pci_devs
        ]
        self.pc.south_bridge.copy_engines = copy_engines
        object.__setattr__(self, "copy_engines", copy_engines)

        self.workload = X86FsLinux()

        # North Bridge
        self.iobus = IOXBar()

        # Set up all of the I/O.
        self._setup_io_devices()

        self.m5ops_base = 0xFFFF0000

    def _get_copy_engine_dma_ports(self) -> List[Port]:
        return [
            copy_engine.dma[channel_index]
            for copy_engine in self.copy_engines
            for channel_index in range(copy_engine.ChanCnt)
        ]

    def _attach_copy_engines_to_iobus(self, connect_dma: bool) -> None:
        for copy_engine in self.copy_engines:
            copy_engine.pio = self.get_io_bus().mem_side_ports
            if connect_dma:
                copy_engine.dma = [
                    self.get_io_bus().cpu_side_ports
                    for _ in range(copy_engine.ChanCnt)
                ]

    def _setup_io_devices(self):
        """Sets up the x86 IO devices.

        .. note::

            This is mostly copy-paste from prior X86 FS setups. Some of it
            may not be documented and there may be bugs.
        """

        # Constants similar to x86_traits.hh
        IO_address_space_base = 0x8000000000000000
        pci_config_address_space_base = 0xC000000000000000
        interrupts_address_space_base = 0xA000000000000000
        APIC_range_size = 1 << 12

        # Configure CXL Device
        cxl_dram = self._cxl_memory_ptr
        # Place CXL memory starting at 4GB and keep software-visible addresses
        # aligned to this direct range (NUMA/system-memory path).
        cxl_mem_size = cxl_dram.get_size()
        cxl_mem_range = AddrRange(Addr(_CXL_RPC_BASE_ADDR), size=cxl_mem_size)
        cxl_dram.set_memory_range([cxl_mem_range])
        cxl_mem_ctrl = self.pc.south_bridge.cxl_device
        cxl_mem_ctrl.connectMemory(cxl_mem_range, cxl_dram)
        cxl_abstract_mems = []
        for mc in cxl_dram.get_memory_controllers():
            cxl_abstract_mems.append(mc.dram)
        self.memories.extend(cxl_abstract_mems)

        if self._is_asic:
            cxl_mem_ctrl.configCXL(Latency("15ns"), 48)
        else:
            cxl_mem_ctrl.configCXL(Latency("60ns"), 36)

        rpc_layout = _CXL_RPC_PUBLIC_LAYOUT
        rpc_base = _CXL_RPC_BASE_ADDR
        metadata_addr = rpc_base + rpc_layout["metadata_offset"]
        doorbell_range = AddrRange(
            rpc_base + rpc_layout["doorbell_offset"],
            size=rpc_layout["doorbell_region_bytes"],
        )
        if rpc_base + rpc_layout["doorbell_region_bytes"] > metadata_addr:
            raise ValueError(
                "RPC doorbell region overlaps metadata queue in board layout"
            )

        self.rpc_engine = CXLRPCEngine()
        self.rpc_engine.doorbell_range = doorbell_range
        self.rpc_engine.auto_register = True
        self.rpc_engine.default_node_id = 0
        self.rpc_engine.default_doorbell_addr = (
            rpc_base + rpc_layout["doorbell_offset"]
        )
        self.rpc_engine.default_metadata_queue_addr = metadata_addr
        self.rpc_engine.default_metadata_queue_entries = rpc_layout[
            "metadata_entries"
        ]
        self.rpc_engine.default_request_data_addr = (
            rpc_base + rpc_layout["request_data_offset"]
        )
        self.rpc_engine.default_request_data_capacity = rpc_layout[
            "request_data_bytes"
        ]
        self.rpc_engine.default_response_data_addr = (
            rpc_base + rpc_layout["response_data_offset"]
        )
        self.rpc_engine.default_response_data_capacity = rpc_layout[
            "response_data_bytes"
        ]
        self.rpc_engine.default_flag_addr = rpc_base + rpc_layout["flag_offset"]

        # Attach to CXLMemCtrl
        cxl_mem_ctrl.rpc_engine = self.rpc_engine

        # Setup memory system specific settings.
        is_ruby = self.get_cache_hierarchy().is_ruby()
        if is_ruby:
            self.pc.attachIO(
                self.get_io_bus(),
                [
                    self.pc.south_bridge.ide.dma,
                    cxl_mem_ctrl.dma,
                    *self._get_copy_engine_dma_ports(),
                ],
            )
            # Ruby path passes cxl DMA explicitly, so SouthBridge.attachIO does
            # not auto-wire cxl_rsp_port. Connect it explicitly.
            cxl_mem_ctrl.cxl_rsp_port = self.get_io_bus().mem_side_ports
        else:
            # Configure CXLBridge
            self.bridge = CXLBridge(bridge_lat="50ns", proto_proc_lat="12ns", req_fifo_depth=128, resp_fifo_depth=128)
            self.bridge.mem_side_port = self.get_io_bus().cpu_side_ports
            self.bridge.cpu_side_port = (
                self.get_cache_hierarchy().get_mem_side_port()
            )

            # Keep CXL bridge routing aligned with the configured CXL memory
            # aperture so CXL traffic follows the same path as the original
            # Type-3 board setup.
            self.bridge.ranges = [
                AddrRange(0xC0000000, 0xFFFF0000),
                AddrRange(
                    IO_address_space_base, interrupts_address_space_base - 1
                ),
                AddrRange(pci_config_address_space_base, Addr.max),
                cxl_mem_range
            ]

            self.apicbridge = Bridge(delay="50ns")
            self.apicbridge.cpu_side_port = self.get_io_bus().mem_side_ports
            self.apicbridge.mem_side_port = (
                self.get_cache_hierarchy().get_cpu_side_port()
            )
            self.apicbridge.ranges = [
                AddrRange(
                    interrupts_address_space_base,
                    interrupts_address_space_base
                    + self.get_processor().get_num_cores() * APIC_range_size
                    - 1,
                )
            ]

            self.pc.attachIO(self.get_io_bus())

        # SouthBridge.attachIO does not wire CopyEngine instances.
        # Explicitly connect their BAR MMIO window, and on non-Ruby systems
        # connect each DMA channel to the I/O bus.
        self._attach_copy_engines_to_iobus(connect_dma=not is_ruby)

        # Add in a Bios information structure.
        self.workload.smbios_table.structures = [X86SMBiosBiosInformation()]

        # Set up the Intel MP table
        base_entries = []
        ext_entries = []
        for i in range(self.get_processor().get_num_cores()):
            bp = X86IntelMPProcessor(
                local_apic_id=i,
                local_apic_version=0x14,
                enable=True,
                bootstrap=(i == 0),
            )
            base_entries.append(bp)
        io_apic = X86IntelMPIOAPIC(
            id=self.get_processor().get_num_cores(),
            version=0x11,
            enable=True,
            address=0xFEC00000,
        )

        self.pc.south_bridge.io_apic.apic_id = io_apic.id
        base_entries.append(io_apic)
        pci_bus = X86IntelMPBus(bus_id=0, bus_type="PCI   ")
        base_entries.append(pci_bus)
        isa_bus = X86IntelMPBus(bus_id=1, bus_type="ISA   ")
        base_entries.append(isa_bus)
        connect_busses = X86IntelMPBusHierarchy(
            bus_id=1, subtractive_decode=True, parent_bus=0
        )
        ext_entries.append(connect_busses)

        pci_dev4_inta = X86IntelMPIOIntAssignment(
            interrupt_type="INT",
            polarity="ConformPolarity",
            trigger="ConformTrigger",
            source_bus_id=0,
            source_bus_irq=0 + (4 << 2),
            dest_io_apic_id=io_apic.id,
            dest_io_apic_intin=16,
        )

        base_entries.append(pci_dev4_inta)

        def assignISAInt(irq, apicPin):
            assign_8259_to_apic = X86IntelMPIOIntAssignment(
                interrupt_type="ExtInt",
                polarity="ConformPolarity",
                trigger="ConformTrigger",
                source_bus_id=1,
                source_bus_irq=irq,
                dest_io_apic_id=io_apic.id,
                dest_io_apic_intin=0,
            )
            base_entries.append(assign_8259_to_apic)

            assign_to_apic = X86IntelMPIOIntAssignment(
                interrupt_type="INT",
                polarity="ConformPolarity",
                trigger="ConformTrigger",
                source_bus_id=1,
                source_bus_irq=irq,
                dest_io_apic_id=io_apic.id,
                dest_io_apic_intin=apicPin,
            )
            base_entries.append(assign_to_apic)

        assignISAInt(0, 2)
        assignISAInt(1, 1)

        for i in range(3, 15):
            assignISAInt(i, i)

        self.workload.intel_mp_table.base_entries = base_entries
        self.workload.intel_mp_table.ext_entries = ext_entries

        entries = [
            # Mark the first megabyte of memory as reserved
            X86E820Entry(addr=0, size="639kB", range_type=1),
            X86E820Entry(addr=0x9FC00, size="385kB", range_type=2),
            # Mark the rest of physical memory as available
            X86E820Entry(
                addr=0x100000,
                size=f"{self.mem_ranges[0].size() - 0x100000:d}B",
                range_type=1,
            ),
        ]

        # Reserve the last 16kB of the 32-bit address space for m5ops
        entries.append(
            X86E820Entry(addr=0xFFFF0000, size="64kB", range_type=2)
        )

        # Expose CXL memory as a discoverable system memory range.
        entries.append(
            X86E820Entry(
                addr=0x100000000,
                size=f"{cxl_mem_range.size()}B",
                range_type=20,
            )
        )

        self.workload.e820_table.entries = entries

    @overrides(AbstractSystemBoard)
    def has_io_bus(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_io_bus(self) -> BaseXBar:
        return self.iobus

    @overrides(AbstractSystemBoard)
    def has_dma_ports(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_dma_ports(self) -> Sequence[Port]:
        dma_ports = [
            self.pc.south_bridge.ide.dma,
            self.iobus.mem_side_ports,
            self.pc.south_bridge.cxl_device.dma,
        ]
        dma_ports.extend(self._get_copy_engine_dma_ports())
        return dma_ports

    @overrides(AbstractSystemBoard)
    def has_coherent_io(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_mem_side_coherent_io_port(self) -> Port:
        return self.iobus.mem_side_ports

    @overrides(AbstractSystemBoard)
    def _setup_memory_ranges(self):
        memory = self.get_memory()

        if memory.get_size() > toMemorySize("3GB"):
            raise Exception(
                "X86Board currently only supports memory sizes up "
                "to 3GB because of the I/O hole."
            )
        data_range = AddrRange(memory.get_size())
        memory.set_memory_range([data_range])
        cpu_abstract_mems = []
        for mc in memory.get_memory_controllers():
            cpu_abstract_mems.append(mc.dram)
        self.memories = cpu_abstract_mems

        # Add the address range for the IO
        self.mem_ranges = [
            data_range,  # All data
            AddrRange(0xC0000000, size=0x100000),  # For I/0
        ]

    @overrides(KernelDiskWorkload)
    def get_disk_device(self):
        return "/dev/sda1"

    @overrides(KernelDiskWorkload)
    def _add_disk_to_board(self, disk_image: AbstractResource):
        ide_disk = IdeDisk()
        ide_disk.driveID = "device0"
        ide_disk.image = CowDiskImage(
            child=RawDiskImage(read_only=True), read_only=False
        )
        ide_disk.image.child.image_file = disk_image.get_local_path()

        # Attach the SimObject to the system.
        self.pc.south_bridge.ide.disks = [ide_disk]

    @overrides(KernelDiskWorkload)
    def get_default_kernel_args(self) -> List[str]:
        return [
            "earlyprintk=ttyS0",
            "console=ttyS0",
            "lpj=7999923",
            "root={root_value}",
            "disk_device={disk_device}",
        ]
