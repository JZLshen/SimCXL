#include <optional>

#include "base/trace.hh"
#include "dev/x86/cxl_mem_ctrl.hh"
#include "debug/CXLMemCtrl.hh"
#include "debug/CXLRange.hh"
#include "debug/CXLRPCEngine.hh"

namespace gem5
{

namespace
{

bool
isConsumedHeadUpdateCandidate(PacketPtr pkt, const DoorbellWriteProbe& probe)
{
    if (!pkt || !pkt->isWrite() || !pkt->hasData() ||
        !probe.matched_connection || !probe.covers_doorbell ||
        !probe.parsed_entry) {
        return false;
    }

    return probe.entry.method == METHOD_HEAD_UPDATE;
}

} // anonymous namespace

CXLMemCtrl::CXLResponsePort::CXLResponsePort(const std::string& _name,
                                        CXLMemCtrl& _ctrl,
                                        CXLRequestPort& _memReqPort,
                                        Cycles _protoProcLat, int _resp_limit,
                                        AddrRange _devMemRange)
    : ResponsePort(_name), ctrl(_ctrl),
    memReqPort(_memReqPort), protoProcLat(_protoProcLat),
    devMemRange(_devMemRange),
    outstandingResponses(0),
    retryReq(false),
    respQueueLimit(_resp_limit),
    sendEvent([this]{ trySendTiming(); }, _name)
{
}

CXLMemCtrl::CXLRequestPort::CXLRequestPort(const std::string& _name,
                                    CXLMemCtrl& _ctrl,
                                    CXLResponsePort& _cxlRspPort,
                                    Cycles _protoProcLat, int _req_limit)
    : RequestPort(_name), ctrl(_ctrl),
    cxlRspPort(_cxlRspPort),
    protoProcLat(_protoProcLat), reqQueueLimit(_req_limit),
    sendEvent([this]{ trySendTiming(); }, _name)
{
}

CXLMemCtrl::CXLMemCtrl(const Params &p)
    : PciDevice(p),
    cxlRspPort(p.name + ".cxl_rsp_port", *this, memReqPort,
            ticksToCycles(p.proto_proc_lat), p.rsp_size, p.cxl_mem_range),
    memReqPort(p.name + ".mem_req_port", *this, cxlRspPort,
            ticksToCycles(p.proto_proc_lat), p.req_size),
    preRspTick(-1),
    stats(*this),
    rpcEngine(p.rpc_engine)
    {
        DPRINTF(CXLMemCtrl, "CXL mem range: [%#x, %#x)\n",
                p.cxl_mem_range.start(), p.cxl_mem_range.end());
        if (rpcEngine) {
            DPRINTF(CXLMemCtrl, "RPC Engine enabled\n");
        }
    }

CXLMemCtrl::CXLCtrlStats::CXLCtrlStats(CXLMemCtrl &_ctrl)
    : statistics::Group(&_ctrl),

      ADD_STAT(reqQueFullEvents, statistics::units::Count::get(),
               "Number of times the request queue has become full"),
      ADD_STAT(reqRetryCounts, statistics::units::Count::get(),
               "Number of times the request was sent for retry"),
      ADD_STAT(rspQueFullEvents, statistics::units::Count::get(),
               "Number of times the response queue has become full"),
      ADD_STAT(reqSendFailed, statistics::units::Count::get(),
               "Number of times the request send failed"),
      ADD_STAT(rspSendFailed, statistics::units::Count::get(),
               "Number of times the response send failed"),
      ADD_STAT(reqSendSucceed, statistics::units::Count::get(),
               "Number of times the request send succeeded"),
      ADD_STAT(rspSendSucceed, statistics::units::Count::get(),
               "Number of times the response send succeeded"),
      ADD_STAT(reqQueueLenDist, "Request queue length distribution (Count)"),
      ADD_STAT(rspQueueLenDist, "Response queue length distribution (Count)"),
      ADD_STAT(rspOutStandDist, "outstandingResponses distribution (Count)"),
      ADD_STAT(reqQueueLatDist, "Response queue latency distribution (Tick)"),
      ADD_STAT(rspQueueLatDist, "Response queue latency distribution (Tick)"),
      ADD_STAT(memToCXLCtrlRsp, "Distribution of the time intervals between "
               "consecutive mem responses from the memory media to the CXLCtrl (Cycle)")
{
    reqQueueLenDist
        .init(0, 49, 10)
        .flags(statistics::nozero);
    rspQueueLenDist
        .init(0, 49, 10)
        .flags(statistics::nozero);
    rspOutStandDist
        .init(0, 49, 10)
        .flags(statistics::nozero);
    reqQueueLatDist
        .init(12000, 41999, 1000)
        .flags(statistics::nozero);
    rspQueueLatDist
        .init(12000, 41999, 1000)
        .flags(statistics::nozero);
    memToCXLCtrlRsp
        .init(0, 299, 10)
        .flags(statistics::nozero);
}

Port & 
CXLMemCtrl::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cxl_rsp_port")
        return cxlRspPort;
    else if (if_name == "mem_req_port")
        return memReqPort;
    else if (if_name == "dma")
        return dmaPort;
    else
        return PioDevice::getPort(if_name, idx);
}

void
CXLMemCtrl::init()
{
    if (!cxlRspPort.isConnected() || !memReqPort.isConnected()
         || !pioPort.isConnected())
        panic("CXL port of %s not connected to anything!", name());

    pioPort.sendRangeChange();
    cxlRspPort.sendRangeChange();
}

Tick
CXLMemCtrl::read(PacketPtr pkt)
{
    Addr addr = pkt->getAddr();
    DPRINTF(CXLRange, "PIO read addr %#lx size %u\n",
            addr, pkt->getSize());

    pkt->setUintX(0, ByteOrder::little);
    pkt->makeResponse();
    return pioDelay;
}

Tick
CXLMemCtrl::write(PacketPtr pkt)
{
    Addr addr = pkt->getAddr();
    uint64_t data = pkt->getUintX(ByteOrder::little);

    DPRINTF(CXLRange, "PIO write addr %#lx data %#lx\n",
            addr, data);

    pkt->makeResponse();
    return pioDelay;
}

AddrRangeList
CXLMemCtrl::getAddrRanges() const
{
    DPRINTF(CXLRange, "PIO base AddrRanges:\n");
    AddrRangeList ranges = PciDevice::getAddrRanges();
    for (const auto &r : ranges) {
        DPRINTF(CXLRange,
                "  range [%#lx - %#lx) size %#lx\n",
                r.start(), r.end(), r.size());
    }
    return ranges;
}

bool
CXLMemCtrl::CXLResponsePort::respQueueFull() const
{
    if (outstandingResponses == respQueueLimit) {
        ctrl.stats.rspQueFullEvents++;
        return true;
    } else {
        return false;
    }
}

bool
CXLMemCtrl::CXLRequestPort::reqQueueFull() const
{
    if (transmitList.size() == reqQueueLimit) {
        ctrl.stats.reqQueFullEvents++;
        return true;
    } else {
        return false;
    }
}

bool
CXLMemCtrl::CXLRequestPort::recvTimingResp(PacketPtr pkt)
{
    // all checks are done when the request is accepted on the response
    // side, so we are guaranteed to have space for the response
    DPRINTF(CXLMemCtrl, "recvTimingResp: %s addr 0x%x\n",
            pkt->cmdString(), pkt->getAddr());

    DPRINTF(CXLMemCtrl, "Request queue size: %d\n", transmitList.size());

    if (ctrl.preRspTick == -1) {
        ctrl.preRspTick = ctrl.clockEdge();
    } else {
        ctrl.stats.memToCXLCtrlRsp.sample(
            ctrl.ticksToCycles(ctrl.clockEdge() - ctrl.preRspTick));
        ctrl.preRspTick = ctrl.clockEdge();
    }

    // technically the packet only reaches us after the header delay,
    // and typically we also need to deserialise any payload
    Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;

    cxlRspPort.schedTimingResp(pkt, ctrl.clockEdge(protoProcLat) +
                              receive_delay);

    return true;
}

bool
CXLMemCtrl::CXLResponsePort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(CXLMemCtrl, "recvTimingReq: %s addr 0x%x\n",
            pkt->cmdString(), pkt->getAddr());

    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    DPRINTF(CXLMemCtrl, "Response queue size: %d outresp: %d\n",
            transmitList.size(), outstandingResponses);

    DoorbellWriteProbe doorbellProbe;
    const bool probeDoorbell = pkt->isWrite() && ctrl.rpcEngine;
    if (probeDoorbell) {
        doorbellProbe = ctrl.rpcEngine->probeDoorbellWrite(pkt);
    }
    const bool headUpdateBypassCandidate =
        doorbellProbe.should_probe &&
        isConsumedHeadUpdateCandidate(pkt, doorbellProbe);

    auto consumeHeadUpdateDoorbell =
        [&](const char *reason) -> std::optional<bool> {
        if (!headUpdateBypassCandidate) {
            return std::nullopt;
        }

        auto dbResult = ctrl.rpcEngine->handleDoorbellWrite(pkt, &doorbellProbe);
        if (dbResult != DoorbellHandleResult::Consumed) {
            DPRINTF(CXLMemCtrl,
                    "Stalled %s path saw non-consumed HEAD_UPDATE candidate at %#x "
                    "(result=%d)\n",
                    reason, pkt->getAddr(), static_cast<int>(dbResult));
            return false;
        }

        DPRINTF(CXLMemCtrl,
                "Processing HEAD_UPDATE doorbell while stalled (%s) at %#x\n",
                reason, pkt->getAddr());

        Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
        pkt->headerDelay = pkt->payloadDelay = 0;
        if (pkt->needsResponse()) {
            pkt->makeResponse();
            schedTimingResp(
                pkt,
                ctrl.clockEdge(protoProcLat + protoProcLat) + receive_delay);
        } else {
            pendingDelete.reset(pkt);
        }

        retryStalledReq();
        return true;
    };

    if (retryReq) {
        if (auto handled = consumeHeadUpdateDoorbell("retry-pending")) {
            return *handled;
        }
        return false;
    }

    // Admission first: only admitted packets pay doorbell handling costs.
    if (memReqPort.reqQueueFull()) {
        if (auto handled = consumeHeadUpdateDoorbell("request-queue-full")) {
            return *handled;
        }
        DPRINTF(CXLMemCtrl, "Request queue full\n");
        retryReq = true;
        return false;
    }

    bool expects_response = pkt->needsResponse();
    if (expects_response) {
        if (respQueueFull()) {
            if (auto handled = consumeHeadUpdateDoorbell("response-queue-full")) {
                return *handled;
            }
            DPRINTF(CXLMemCtrl, "Response queue full\n");
            retryReq = true;
            return false;
        }

        DPRINTF(CXLMemCtrl, "Reserving space for response\n");
        assert(outstandingResponses != respQueueLimit);
        ++outstandingResponses;
        ctrl.stats.rspOutStandDist.sample(outstandingResponses);
    }

    // Only pay doorbell handling costs after admission succeeds.
    DoorbellHandleResult doorbellResult = DoorbellHandleResult::NotHandled;
    bool remappedDoorbell = false;
    if (doorbellProbe.should_probe) {
        auto dbResult = ctrl.rpcEngine->handleDoorbellWrite(pkt, &doorbellProbe);
        doorbellResult = dbResult;
        if (dbResult != DoorbellHandleResult::NotHandled) {
            DPRINTF(CXLRPCEngine, "Intercepting doorbell write at %#x\n",
                    pkt->getAddr());
            remappedDoorbell = (dbResult == DoorbellHandleResult::Remapped);
        }
    }

    if (doorbellResult == DoorbellHandleResult::Retry) {
        if (expects_response) {
            assert(outstandingResponses != 0);
            --outstandingResponses;
            ctrl.stats.rspOutStandDist.sample(outstandingResponses);
        }
        retryReq = true;
        return false;
    }

    Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;

    if (doorbellResult == DoorbellHandleResult::Consumed) {
        if (expects_response) {
            pkt->makeResponse();
            schedTimingResp(
                pkt,
                ctrl.clockEdge(protoProcLat + protoProcLat) + receive_delay);
        } else {
            // Hold and delete after unwinding request processing stack.
            pendingDelete.reset(pkt);
        }
        retryStalledReq();
        return !retryReq;
    }

    memReqPort.schedTimingReq(pkt, ctrl.clockEdge(protoProcLat) + receive_delay);
    if (remappedDoorbell && ctrl.rpcEngine) {
        ctrl.rpcEngine->accountRemappedDoorbellForward();
    }

    return !retryReq;
}

void
CXLMemCtrl::CXLResponsePort::retryStalledReq()
{
    if (retryReq) {
        DPRINTF(CXLMemCtrl, "Request waiting for retry, now retrying\n");
        retryReq = false;
        sendRetryReq();
        ctrl.stats.reqRetryCounts++;
    }
}

void
CXLMemCtrl::CXLRequestPort::schedTimingReq(PacketPtr pkt, Tick when)
{
    // If we're about to put this packet at the head of the queue, we
    // need to schedule an event to do the transmit.  Otherwise there
    // should already be an event scheduled for sending the head
    // packet.
    if (transmitList.empty()) {
        ctrl.schedule(sendEvent, when);
    }

    assert(transmitList.size() != reqQueueLimit);

    transmitList.emplace_back(pkt, when);

    ctrl.stats.reqQueueLenDist.sample(transmitList.size());
}

void
CXLMemCtrl::CXLResponsePort::schedTimingResp(PacketPtr pkt, Tick when)
{
    if (transmitList.empty()) {
        ctrl.schedule(sendEvent, when);
    }

    transmitList.emplace_back(pkt, when);

    ctrl.stats.rspQueueLenDist.sample(transmitList.size());
}

void
CXLMemCtrl::CXLRequestPort::trySendTiming()
{
    assert(!transmitList.empty());

    DeferredPacket req = transmitList.front();

    assert(req.tick <= curTick());

    PacketPtr pkt = req.pkt;

    DPRINTF(CXLMemCtrl, "trySend request addr 0x%x, queue size %d\n",
            pkt->getAddr(), transmitList.size());

    if (sendTimingReq(pkt)) {
        // send successful
        ctrl.stats.reqSendSucceed++;
        ctrl.stats.reqQueueLatDist.sample(curTick() - req.entryTime);

        transmitList.pop_front();

        ctrl.stats.reqQueueLenDist.sample(transmitList.size());
        DPRINTF(CXLMemCtrl, "trySend request successful\n");

        // If there are more packets to send, schedule event to try again.
        if (!transmitList.empty()) {
            DeferredPacket next_req = transmitList.front();
            DPRINTF(CXLMemCtrl, "Scheduling next send\n");
            ctrl.schedule(sendEvent, std::max(next_req.tick,
                                                ctrl.clockEdge()));
        }
        cxlRspPort.retryStalledReq();

    } else {
        ctrl.stats.reqSendFailed++;
    }

    // if the send failed, then we try again once we receive a retry,
    // and therefore there is no need to take any action
}

void
CXLMemCtrl::CXLResponsePort::trySendTiming()
{
    assert(!transmitList.empty());

    DeferredPacket resp = transmitList.front();

    assert(resp.tick <= curTick());

    PacketPtr pkt = resp.pkt;

    DPRINTF(CXLMemCtrl, "trySend response addr 0x%x, outstanding %d\n",
            pkt->getAddr(), outstandingResponses);

    if (sendTimingResp(pkt)) {
        // send successful
        ctrl.stats.rspSendSucceed++;
        ctrl.stats.rspQueueLatDist.sample(curTick() - resp.entryTime);

        transmitList.pop_front();

        ctrl.stats.rspQueueLenDist.sample(transmitList.size());
        DPRINTF(CXLMemCtrl, "trySend response successful\n");

        assert(outstandingResponses != 0);
        --outstandingResponses;

        ctrl.stats.rspOutStandDist.sample(outstandingResponses);

        // If there are more packets to send, schedule event to try again.
        if (!transmitList.empty()) {
            DeferredPacket next_resp = transmitList.front();
            DPRINTF(CXLMemCtrl, "Scheduling next send\n");
            ctrl.schedule(sendEvent, std::max(next_resp.tick,
                                                ctrl.clockEdge()));
        }
        if (!memReqPort.reqQueueFull() && retryReq) {
            DPRINTF(CXLMemCtrl, "Request waiting for retry, now retrying\n");
            retryReq = false;
            sendRetryReq();
            ctrl.stats.reqRetryCounts++;
        }

    } else {
        ctrl.stats.rspSendFailed++;
    }

    // if the send failed, then we try again once we receive a retry,
    // and therefore there is no need to take any action
}

void
CXLMemCtrl::CXLRequestPort::recvReqRetry()
{
    trySendTiming();
}

void
CXLMemCtrl::CXLResponsePort::recvRespRetry()
{
    trySendTiming();
}

void
CXLMemCtrl::CXLResponsePort::recvFunctional(PacketPtr pkt)
{
    ctrl.memReqPort.sendFunctional(pkt);
}

Tick
CXLMemCtrl::CXLResponsePort::recvAtomic(PacketPtr pkt)
{
    DPRINTF(CXLMemCtrl, "CXLMemCtrl recvAtomic: %s AddrRange: %s\n",
            pkt->cmdString(), pkt->getAddrRange().to_string());
    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    // Check if this is a doorbell write for RPC (same as timing path).
    DoorbellHandleResult atomicDoorbellResult = DoorbellHandleResult::NotHandled;
    DoorbellWriteProbe doorbellProbe;
    const bool probeDoorbell =
        pkt->isWrite() && ctrl.rpcEngine;
    if (probeDoorbell) {
        doorbellProbe = ctrl.rpcEngine->probeDoorbellWrite(pkt);
    }
    if (doorbellProbe.should_probe) {
        auto dbResult = ctrl.rpcEngine->handleDoorbellWrite(pkt, &doorbellProbe);
        atomicDoorbellResult = dbResult;
        if (dbResult != DoorbellHandleResult::NotHandled) {
            DPRINTF(CXLRPCEngine, "Intercepting doorbell write (atomic) at %#x\n",
                    pkt->getAddr());
            // Fall through to normal DRAM access regardless
        }
    }

    Cycles delay = processCXLMem(pkt);

    if (atomicDoorbellResult == DoorbellHandleResult::Retry) {
        panic("CXL RPC metadata queue retry is unsupported in atomic mode.");
    }

    if (atomicDoorbellResult == DoorbellHandleResult::Consumed) {
        if (pkt->needsResponse()) {
            pkt->makeResponse();
        }
        return delay * ctrl.clockPeriod();
    }

    Tick access_delay = memReqPort.sendAtomic(pkt);

    if (atomicDoorbellResult == DoorbellHandleResult::Remapped && ctrl.rpcEngine) {
        ctrl.rpcEngine->accountRemappedDoorbellForward();
    }

    DPRINTF(CXLMemCtrl, "access_delay=%ld, proto_proc_lat=%ld, total=%ld\n",
            access_delay, delay, delay * ctrl.clockPeriod() + access_delay);
    return delay * ctrl.clockPeriod() + access_delay;
}

Tick
CXLMemCtrl::CXLResponsePort::recvAtomicBackdoor(
    PacketPtr pkt, MemBackdoorPtr &backdoor)
{
    // Check if this is a doorbell write for RPC (same as recvAtomic path).
    DoorbellHandleResult backdoorDoorbellResult = DoorbellHandleResult::NotHandled;
    DoorbellWriteProbe doorbellProbe;
    const bool probeDoorbell =
        pkt->isWrite() && ctrl.rpcEngine;
    if (probeDoorbell) {
        doorbellProbe = ctrl.rpcEngine->probeDoorbellWrite(pkt);
    }
    if (doorbellProbe.should_probe) {
        auto dbResult = ctrl.rpcEngine->handleDoorbellWrite(pkt, &doorbellProbe);
        backdoorDoorbellResult = dbResult;
        if (dbResult != DoorbellHandleResult::NotHandled) {
            DPRINTF(CXLRPCEngine,
                    "Intercepting doorbell write (atomic backdoor) at %#x\n",
                    pkt->getAddr());
            // Fall through to normal DRAM access
        }
    }

    Cycles delay = processCXLMem(pkt);

    if (backdoorDoorbellResult == DoorbellHandleResult::Retry) {
        panic("CXL RPC metadata queue retry is unsupported in atomic backdoor mode.");
    }

    if (backdoorDoorbellResult == DoorbellHandleResult::Consumed) {
        backdoor = nullptr;
        if (pkt->needsResponse()) {
            pkt->makeResponse();
        }
        return delay * ctrl.clockPeriod();
    }

    Tick access_delay = memReqPort.sendAtomicBackdoor(pkt, backdoor);

    if (backdoorDoorbellResult == DoorbellHandleResult::Remapped &&
        ctrl.rpcEngine) {
        ctrl.rpcEngine->accountRemappedDoorbellForward();
    }

    return delay * ctrl.clockPeriod() + access_delay;
}

Cycles
CXLMemCtrl::CXLResponsePort::processCXLMem(PacketPtr pkt) {
    if (pkt->cxl_cmd == MemCmd::M2SReq) {
        assert(pkt->isRead());
    } else if (pkt->cxl_cmd == MemCmd::M2SRwD) {
        assert(pkt->isWrite());
    }
    return protoProcLat + protoProcLat;
}

AddrRangeList
CXLMemCtrl::CXLResponsePort::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(devMemRange);
    DPRINTF(CXLRange, "CXLResponsePort AddrRanges:\n");
    for (const auto &r : ranges) {
        DPRINTF(CXLRange,
                "  range [%#lx - %#lx) size %#lx\n",
                r.start(), r.end(), r.size());
    }
    return ranges;
}

} // namespace gem5
