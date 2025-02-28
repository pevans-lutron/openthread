/*
 *  Copyright (c) 2016, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file implements mesh forwarding of IPv6/6LoWPAN messages.
 */

#include "mesh_forwarder.hpp"

#include "common/code_utils.hpp"
#include "common/debug.hpp"
#include "common/encoding.hpp"
#include "common/instance.hpp"
#include "common/locator-getters.hpp"
#include "common/logging.hpp"
#include "common/message.hpp"
#include "common/random.hpp"
#include "net/ip6.hpp"
#include "net/ip6_filter.hpp"
#include "net/netif.hpp"
#include "net/tcp.hpp"
#include "net/udp6.hpp"
#include "phy/phy.hpp"
#include "thread/mle.hpp"
#include "thread/mle_router.hpp"
#include "thread/thread_netif.hpp"

using ot::Encoding::BigEndian::HostSwap16;

namespace ot {

MeshForwarder::MeshForwarder(Instance &aInstance)
    : InstanceLocator(aInstance)
    , mDiscoverTimer(aInstance, &MeshForwarder::HandleDiscoverTimer, this)
    , mUpdateTimer(aInstance, &MeshForwarder::HandleUpdateTimer, this)
    , mMessageNextOffset(0)
    , mSendMessage(NULL)
    , mSendMessageIsARetransmission(false)
    , mSendMessageMaxCsmaBackoffs(Mac::kMaxCsmaBackoffsDirect)
    , mSendMessageMaxFrameRetries(Mac::kMaxFrameRetriesDirect)
    , mMeshSource()
    , mMeshDest()
    , mAddMeshHeader(false)
    , mSendBusy(false)
    , mScheduleTransmissionTask(aInstance, ScheduleTransmissionTask, this)
    , mEnabled(false)
    , mScanChannels(0)
    , mScanChannel(0)
    , mMacRadioAcquisitionId(0)
    , mRestorePanId(Mac::kPanIdBroadcast)
    , mScanning(false)
#if OPENTHREAD_FTD
    , mSourceMatchController(aInstance)
    , mSendMessageFrameCounter(0)
    , mSendMessageKeyId(0)
    , mSendMessageDataSequenceNumber(0)
    , mIndirectStartingChild(NULL)
#endif
    , mDataPollSender(aInstance)
{
    mFragTag = Random::NonCrypto::GetUint16();

    mIpCounters.mTxSuccess = 0;
    mIpCounters.mRxSuccess = 0;
    mIpCounters.mTxFailure = 0;
    mIpCounters.mRxFailure = 0;

#if OPENTHREAD_FTD
    memset(mFragmentEntries, 0, sizeof(mFragmentEntries));
#endif
}

void MeshForwarder::Start(void)
{
    if (mEnabled == false)
    {
        Get<Mac::Mac>().SetRxOnWhenIdle(true);
        mEnabled = true;
    }
}

void MeshForwarder::Stop(void)
{
    Message *message;

    VerifyOrExit(mEnabled == true);

    mDataPollSender.StopPolling();
    mUpdateTimer.Stop();

    if (mScanning)
    {
        HandleDiscoverComplete();
    }

    while ((message = mSendQueue.GetHead()) != NULL)
    {
        mSendQueue.Dequeue(*message);
        message->Free();
    }

    while ((message = mReassemblyList.GetHead()) != NULL)
    {
        mReassemblyList.Dequeue(*message);
        message->Free();
    }

#if OPENTHREAD_FTD
    for (ChildTable::Iterator iter(GetInstance(), ChildTable::kInStateAnyExceptInvalid); !iter.IsDone(); iter++)
    {
        iter.GetChild()->SetIndirectMessage(NULL);
        Get<SourceMatchController>().ResetMessageCount(*iter.GetChild());
    }

    memset(mFragmentEntries, 0, sizeof(mFragmentEntries));
#endif

    mEnabled     = false;
    mSendMessage = NULL;
    Get<Mac::Mac>().SetRxOnWhenIdle(false);

exit:
    return;
}

void MeshForwarder::RemoveMessage(Message &aMessage)
{
#if OPENTHREAD_FTD
    for (ChildTable::Iterator iter(GetInstance(), ChildTable::kInStateAnyExceptInvalid); !iter.IsDone(); iter++)
    {
        IgnoreReturnValue(RemoveMessageFromSleepyChild(aMessage, *iter.GetChild()));
    }
#endif

    if (mSendMessage == &aMessage)
    {
        mSendMessage = NULL;
    }

    mSendQueue.Dequeue(aMessage);
    LogMessage(kMessageEvict, aMessage, NULL, OT_ERROR_NO_BUFS);
    aMessage.Free();
}

void MeshForwarder::ScheduleTransmissionTask(Tasklet &aTasklet)
{
    aTasklet.GetOwner<MeshForwarder>().ScheduleTransmissionTask();
}

void MeshForwarder::ScheduleTransmissionTask(void)
{
    VerifyOrExit(mSendBusy == false);

    mSendMessageIsARetransmission = false;

#if OPENTHREAD_FTD
    if (GetIndirectTransmission() == OT_ERROR_NONE)
    {
        ExitNow();
    }
#endif // OPENTHREAD_FTD

    if ((mSendMessage = GetDirectTransmission()) != NULL)
    {
        if (mSendMessage->GetOffset() == 0)
        {
            mSendMessage->SetTxSuccess(true);
        }

        mSendMessageMaxCsmaBackoffs = Mac::kMaxCsmaBackoffsDirect;
        mSendMessageMaxFrameRetries = Mac::kMaxFrameRetriesDirect;
        Get<Mac::Mac>().RequestFrameTransmission();
        ExitNow();
    }

exit:
    return;
}

otError MeshForwarder::PrepareDiscoverRequest(void)
{
    otError error = OT_ERROR_NONE;

    VerifyOrExit(!mScanning);

    mScanChannel  = Mac::ChannelMask::kChannelIteratorFirst;
    mRestorePanId = Get<Mac::Mac>().GetPanId();

    SuccessOrExit(error = Get<Mac::Mac>().AcquireRadioChannel(&mMacRadioAcquisitionId));

    mScanning = true;

    if (mScanChannels.GetNextChannel(mScanChannel) != OT_ERROR_NONE)
    {
        HandleDiscoverComplete();
        ExitNow(error = OT_ERROR_DROP);
    }

exit:
    return error;
}

Message *MeshForwarder::GetDirectTransmission(void)
{
    Message *curMessage, *nextMessage;
    otError  error = OT_ERROR_NONE;

    for (curMessage = mSendQueue.GetHead(); curMessage; curMessage = nextMessage)
    {
        if (curMessage->GetDirectTransmission() == false)
        {
            nextMessage = curMessage->GetNext();
            continue;
        }

        curMessage->SetDoNotEvict(true);

        switch (curMessage->GetType())
        {
        case Message::kTypeIp6:
            error = UpdateIp6Route(*curMessage);

            if (curMessage->GetSubType() == Message::kSubTypeMleDiscoverRequest)
            {
                error = PrepareDiscoverRequest();
            }

            break;

#if OPENTHREAD_FTD

        case Message::kType6lowpan:
            error = UpdateMeshRoute(*curMessage);
            break;

#endif

        default:
            error = OT_ERROR_DROP;
            break;
        }

        curMessage->SetDoNotEvict(false);

        // the next message may have been evicted during processing (e.g. due to Address Solicit)
        nextMessage = curMessage->GetNext();

        switch (error)
        {
        case OT_ERROR_NONE:
            ExitNow();

#if OPENTHREAD_FTD

        case OT_ERROR_ADDRESS_QUERY:
            mSendQueue.Dequeue(*curMessage);
            mResolvingQueue.Enqueue(*curMessage);
            continue;

#endif

        default:
            mSendQueue.Dequeue(*curMessage);
            LogMessage(kMessageDrop, *curMessage, NULL, error);
            curMessage->Free();
            continue;
        }
    }

exit:
    return curMessage;
}

otError MeshForwarder::UpdateIp6Route(Message &aMessage)
{
    Mle::MleRouter &mle   = Get<Mle::MleRouter>();
    otError         error = OT_ERROR_NONE;
    Ip6::Header     ip6Header;

    mAddMeshHeader = false;

    aMessage.Read(0, sizeof(ip6Header), &ip6Header);

    VerifyOrExit(!ip6Header.GetSource().IsMulticast(), error = OT_ERROR_DROP);

    // 1. Choose correct MAC Source Address.
    GetMacSourceAddress(ip6Header.GetSource(), mMacSource);

    // 2. Choose correct MAC Destination Address.
    if (mle.GetRole() == OT_DEVICE_ROLE_DISABLED || mle.GetRole() == OT_DEVICE_ROLE_DETACHED)
    {
        // Allow only for link-local unicasts and multicasts.
        if (ip6Header.GetDestination().IsLinkLocal() || ip6Header.GetDestination().IsLinkLocalMulticast())
        {
            GetMacDestinationAddress(ip6Header.GetDestination(), mMacDest);
        }
        else
        {
            error = OT_ERROR_DROP;
        }

        ExitNow();
    }

    if (ip6Header.GetDestination().IsMulticast())
    {
        // With the exception of MLE multicasts, a Thread End Device transmits multicasts,
        // as IEEE 802.15.4 unicasts to its parent.
        if (mle.GetRole() == OT_DEVICE_ROLE_CHILD && !aMessage.IsSubTypeMle())
        {
            mMacDest.SetShort(mle.GetNextHop(Mac::kShortAddrBroadcast));
        }
        else
        {
            mMacDest.SetShort(Mac::kShortAddrBroadcast);
        }
    }
    else if (ip6Header.GetDestination().IsLinkLocal())
    {
        GetMacDestinationAddress(ip6Header.GetDestination(), mMacDest);
    }
    else if (mle.IsMinimalEndDevice())
    {
        mMacDest.SetShort(mle.GetNextHop(Mac::kShortAddrBroadcast));
    }
    else
    {
#if OPENTHREAD_FTD
        error = UpdateIp6RouteFtd(ip6Header);
#else
        assert(false);
#endif
    }

exit:
    return error;
}

bool MeshForwarder::GetRxOnWhenIdle(void) const
{
    return Get<Mac::Mac>().GetRxOnWhenIdle();
}

void MeshForwarder::SetRxOnWhenIdle(bool aRxOnWhenIdle)
{
    Get<Mac::Mac>().SetRxOnWhenIdle(aRxOnWhenIdle);

    if (aRxOnWhenIdle)
    {
        mDataPollSender.StopPolling();
        Get<Utils::SupervisionListener>().Stop();
    }
    else
    {
        mDataPollSender.StartPolling();
        Get<Utils::SupervisionListener>().Start();
    }
}

void MeshForwarder::GetMacSourceAddress(const Ip6::Address &aIp6Addr, Mac::Address &aMacAddr)
{
    aIp6Addr.ToExtAddress(aMacAddr);

    if (aMacAddr.GetExtended() != Get<Mac::Mac>().GetExtAddress())
    {
        aMacAddr.SetShort(Get<Mac::Mac>().GetShortAddress());
    }
}

void MeshForwarder::GetMacDestinationAddress(const Ip6::Address &aIp6Addr, Mac::Address &aMacAddr)
{
    if (aIp6Addr.IsMulticast())
    {
        aMacAddr.SetShort(Mac::kShortAddrBroadcast);
    }
    else if (aIp6Addr.mFields.m16[0] == HostSwap16(0xfe80) && aIp6Addr.mFields.m16[1] == HostSwap16(0x0000) &&
             aIp6Addr.mFields.m16[2] == HostSwap16(0x0000) && aIp6Addr.mFields.m16[3] == HostSwap16(0x0000) &&
             aIp6Addr.mFields.m16[4] == HostSwap16(0x0000) && aIp6Addr.mFields.m16[5] == HostSwap16(0x00ff) &&
             aIp6Addr.mFields.m16[6] == HostSwap16(0xfe00))
    {
        aMacAddr.SetShort(HostSwap16(aIp6Addr.mFields.m16[7]));
    }
    else if (Get<Mle::MleRouter>().IsRoutingLocator(aIp6Addr))
    {
        aMacAddr.SetShort(HostSwap16(aIp6Addr.mFields.m16[7]));
    }
    else
    {
        aIp6Addr.ToExtAddress(aMacAddr);
    }
}

otError MeshForwarder::GetMeshHeader(const uint8_t *&aFrame, uint8_t &aFrameLength, Lowpan::MeshHeader &aMeshHeader)
{
    otError error;

    VerifyOrExit(aFrameLength >= 1 && reinterpret_cast<const Lowpan::MeshHeader *>(aFrame)->IsMeshHeader(),
                 error = OT_ERROR_NOT_FOUND);
    SuccessOrExit(error = aMeshHeader.Init(aFrame, aFrameLength));

exit:
    return error;
}

otError MeshForwarder::SkipMeshHeader(const uint8_t *&aFrame, uint8_t &aFrameLength)
{
    otError            error = OT_ERROR_NONE;
    Lowpan::MeshHeader meshHeader;

    VerifyOrExit(aFrameLength >= 1 && reinterpret_cast<const Lowpan::MeshHeader *>(aFrame)->IsMeshHeader());

    SuccessOrExit(error = meshHeader.Init(aFrame, aFrameLength));
    aFrame += meshHeader.GetHeaderLength();
    aFrameLength -= meshHeader.GetHeaderLength();

exit:
    return error;
}

otError MeshForwarder::GetFragmentHeader(const uint8_t *         aFrame,
                                         uint8_t                 aFrameLength,
                                         Lowpan::FragmentHeader &aFragmentHeader)
{
    otError error = OT_ERROR_NONE;

    VerifyOrExit(aFrameLength >= 1 && reinterpret_cast<const Lowpan::FragmentHeader *>(aFrame)->IsFragmentHeader(),
                 error = OT_ERROR_NOT_FOUND);

    SuccessOrExit(error = aFragmentHeader.Init(aFrame, aFrameLength));

exit:
    return error;
}

otError MeshForwarder::DecompressIp6Header(const uint8_t *     aFrame,
                                           uint8_t             aFrameLength,
                                           const Mac::Address &aMacSource,
                                           const Mac::Address &aMacDest,
                                           Ip6::Header &       aIp6Header,
                                           uint8_t &           aHeaderLength,
                                           bool &              aNextHeaderCompressed)
{
    otError                error = OT_ERROR_NONE;
    const uint8_t *        start = aFrame;
    Lowpan::FragmentHeader fragmentHeader;
    int                    headerLength;

    SuccessOrExit(error = SkipMeshHeader(aFrame, aFrameLength));

    if (GetFragmentHeader(aFrame, aFrameLength, fragmentHeader) == OT_ERROR_NONE)
    {
        // only the first fragment header is followed by a LOWPAN_IPHC header
        VerifyOrExit(fragmentHeader.GetDatagramOffset() == 0, error = OT_ERROR_NOT_FOUND);
        aFrame += fragmentHeader.GetHeaderLength();
        aFrameLength -= fragmentHeader.GetHeaderLength();
    }

    VerifyOrExit(aFrameLength >= 1 && Lowpan::Lowpan::IsLowpanHc(aFrame), error = OT_ERROR_NOT_FOUND);
    headerLength = Get<Lowpan::Lowpan>().DecompressBaseHeader(aIp6Header, aNextHeaderCompressed, aMacSource, aMacDest,
                                                              aFrame, aFrameLength);

    VerifyOrExit(headerLength > 0, error = OT_ERROR_PARSE);
    aHeaderLength = static_cast<uint8_t>(aFrame - start) + static_cast<uint8_t>(headerLength);

exit:
    return error;
}

otError MeshForwarder::HandleFrameRequest(Mac::Frame &aFrame)
{
    otError error = OT_ERROR_NONE;

    VerifyOrExit(mEnabled, error = OT_ERROR_ABORT);

    mSendBusy = true;

    if (mSendMessage == NULL)
    {
        SendEmptyFrame(aFrame, false);
        aFrame.SetIsARetransmission(false);
        aFrame.SetMaxCsmaBackoffs(Mac::kMaxCsmaBackoffsDirect);
        aFrame.SetMaxFrameRetries(Mac::kMaxFrameRetriesDirect);
        ExitNow();
    }

    switch (mSendMessage->GetType())
    {
    case Message::kTypeIp6:
        if (mSendMessage->GetSubType() == Message::kSubTypeMleDiscoverRequest)
        {
            SuccessOrExit(error = Get<Mac::Mac>().SetRadioChannel(mMacRadioAcquisitionId, mScanChannel));

            aFrame.SetChannel(mScanChannel);

            // In case a specific PAN ID of a Thread Network to be discovered is not known, Discovery
            // Request messages MUST have the Destination PAN ID in the IEEE 802.15.4 MAC header set
            // to be the Broadcast PAN ID (0xFFFF) and the Source PAN ID set to a randomly generated
            // value.
            if (mSendMessage->GetPanId() == Mac::kPanIdBroadcast && Get<Mac::Mac>().GetPanId() == Mac::kPanIdBroadcast)
            {
                uint16_t panid;

                do
                {
                    panid = Random::NonCrypto::GetUint16();
                } while (panid == Mac::kPanIdBroadcast);

                Get<Mac::Mac>().SetPanId(panid);
            }
        }

        error = SendFragment(*mSendMessage, aFrame);

        // `SendFragment()` fails with `NotCapable` error if the message is MLE (with
        // no link layer security) and also requires fragmentation.
        if (error == OT_ERROR_NOT_CAPABLE)
        {
            // Enable security and try again.
            mSendMessage->SetLinkSecurityEnabled(true);

            if (mSendMessage->GetSubType() == Message::kSubTypeMleChildIdRequest)
            {
                otLogNoteMac("Child ID Request requires fragmentation, aborting tx");
                mMessageNextOffset = mSendMessage->GetLength();
                error              = OT_ERROR_ABORT;
                ExitNow();
            }

            error = SendFragment(*mSendMessage, aFrame);
        }

        assert(aFrame.GetLength() != 7);
        break;

#if OPENTHREAD_FTD

    case Message::kType6lowpan:
        SendMesh(*mSendMessage, aFrame);
        break;

    case Message::kTypeSupervision:
        SendEmptyFrame(aFrame, kSupervisionMsgAckRequest);
        mMessageNextOffset = mSendMessage->GetLength();
        break;

#endif
    }

    assert(error == OT_ERROR_NONE);

    aFrame.SetIsARetransmission(mSendMessageIsARetransmission);
    aFrame.SetMaxCsmaBackoffs(mSendMessageMaxCsmaBackoffs);
    aFrame.SetMaxFrameRetries(mSendMessageMaxFrameRetries);

#if OPENTHREAD_FTD

    {
        Mac::Address macDest;
        Child *      child = NULL;

        if (mSendMessageIsARetransmission)
        {
            // If this is the re-transmission of an indirect frame to a sleepy child, we
            // ensure to use the same frame counter, key id, and data sequence number as
            // the last attempt.

            aFrame.SetSequence(mSendMessageDataSequenceNumber);

            if (aFrame.GetSecurityEnabled())
            {
                aFrame.SetFrameCounter(mSendMessageFrameCounter);
                aFrame.SetKeyId(mSendMessageKeyId);
            }
        }

        aFrame.GetDstAddr(macDest);

        // Set `FramePending` if there are more queued messages (excluding
        // the current one being sent out) for the child (note `> 1` check).
        // The case where the current message requires fragmentation is
        // already checked and handled in `SendFragment()` method.

        child = Get<ChildTable>().FindChild(macDest, ChildTable::kInStateValidOrRestoring);

        if ((child != NULL) && !child->IsRxOnWhenIdle() && (child->GetIndirectMessageCount() > 1))
        {
            aFrame.SetFramePending(true);
        }
    }

#endif

exit:
    return error;
}

otError MeshForwarder::SendFragment(Message &aMessage, Mac::Frame &aFrame)
{
    Mac::Address            meshDest, meshSource;
    uint16_t                fcf;
    Lowpan::FragmentHeader *fragmentHeader;
    uint8_t *               payload;
    uint8_t                 headerLength;
    uint16_t                payloadLength;
    uint16_t                fragmentLength;
    uint16_t                dstpan;
    uint8_t                 secCtl = Mac::Frame::kSecNone;
    otError                 error  = OT_ERROR_NONE;
#if OPENTHREAD_CONFIG_ENABLE_TIME_SYNC
    Mac::HeaderIe ieList[2];
#endif

    if (mAddMeshHeader)
    {
        meshSource.SetShort(mMeshSource);
        meshDest.SetShort(mMeshDest);
    }
    else
    {
        meshDest   = mMacDest;
        meshSource = mMacSource;
    }

    // initialize MAC header
    fcf = Mac::Frame::kFcfFrameData;

#if OPENTHREAD_CONFIG_ENABLE_TIME_SYNC

    if (aMessage.IsTimeSync())
    {
        fcf |= Mac::Frame::kFcfFrameVersion2015 | Mac::Frame::kFcfIePresent;
    }
    else
#endif
    {
        fcf |= Mac::Frame::kFcfFrameVersion2006;
    }

    fcf |= (mMacDest.IsShort()) ? Mac::Frame::kFcfDstAddrShort : Mac::Frame::kFcfDstAddrExt;
    fcf |= (mMacSource.IsShort()) ? Mac::Frame::kFcfSrcAddrShort : Mac::Frame::kFcfSrcAddrExt;

    // all unicast frames request ACK
    if (mMacDest.IsExtended() || !mMacDest.IsBroadcast())
    {
        fcf |= Mac::Frame::kFcfAckRequest;
    }

    if (aMessage.IsLinkSecurityEnabled())
    {
        fcf |= Mac::Frame::kFcfSecurityEnabled;

        switch (aMessage.GetSubType())
        {
        case Message::kSubTypeJoinerEntrust:
            secCtl = static_cast<uint8_t>(Mac::Frame::kKeyIdMode0);
            break;

        case Message::kSubTypeMleAnnounce:
            secCtl = static_cast<uint8_t>(Mac::Frame::kKeyIdMode2);
            break;

        default:
            secCtl = static_cast<uint8_t>(Mac::Frame::kKeyIdMode1);
            break;
        }

        secCtl |= Mac::Frame::kSecEncMic32;
    }

    dstpan = Get<Mac::Mac>().GetPanId();

    switch (aMessage.GetSubType())
    {
    case Message::kSubTypeMleAnnounce:
        aFrame.SetChannel(aMessage.GetChannel());
        dstpan = Mac::kPanIdBroadcast;
        break;

    case Message::kSubTypeMleDiscoverRequest:
    case Message::kSubTypeMleDiscoverResponse:
        dstpan = aMessage.GetPanId();
        break;

    default:
        break;
    }

    if (dstpan == Get<Mac::Mac>().GetPanId())
    {
#if OPENTHREAD_CONFIG_HEADER_IE_SUPPORT
        // Handle a special case in IEEE 802.15.4-2015, when Pan ID Compression is 0, but Src Pan ID is not present:
        //  Dest Address:       Extended
        //  Src Address:        Extended
        //  Dest Pan ID:        Present
        //  Src Pan ID:         Not Present
        //  Pan ID Compression: 0

        if ((fcf & Mac::Frame::kFcfFrameVersionMask) != Mac::Frame::kFcfFrameVersion2015 ||
            (fcf & Mac::Frame::kFcfDstAddrMask) != Mac::Frame::kFcfDstAddrExt ||
            (fcf & Mac::Frame::kFcfSrcAddrMask) != Mac::Frame::kFcfSrcAddrExt)
#endif
        {
            fcf |= Mac::Frame::kFcfPanidCompression;
        }
    }

    aFrame.InitMacHeader(fcf, secCtl);
    aFrame.SetDstPanId(dstpan);
    aFrame.SetSrcPanId(Get<Mac::Mac>().GetPanId());
    aFrame.SetDstAddr(mMacDest);
    aFrame.SetSrcAddr(mMacSource);

#if OPENTHREAD_CONFIG_ENABLE_TIME_SYNC

    if (aMessage.IsTimeSync())
    {
        Mac::TimeIe *ie;
        uint8_t *    cur = NULL;

        ieList[0].Init();
        ieList[0].SetId(Mac::Frame::kHeaderIeVendor);
        ieList[0].SetLength(sizeof(Mac::TimeIe));
        ieList[1].Init();
        ieList[1].SetId(Mac::Frame::kHeaderIeTermination2);
        ieList[1].SetLength(0);
        aFrame.AppendHeaderIe(ieList, 2);

        cur = aFrame.GetHeaderIe(Mac::Frame::kHeaderIeVendor);
        ie  = reinterpret_cast<Mac::TimeIe *>(cur + sizeof(Mac::HeaderIe));
        ie->Init();
    }

#endif

    payload = aFrame.GetPayload();

    headerLength = 0;

#if OPENTHREAD_FTD

    // initialize Mesh header
    if (mAddMeshHeader)
    {
        Mle::MleRouter &   mle = Get<Mle::MleRouter>();
        Lowpan::MeshHeader meshHeader;
        uint8_t            hopsLeft;

        if (mle.GetRole() == OT_DEVICE_ROLE_CHILD)
        {
            // REED sets hopsLeft to max (16) + 1. It does not know the route cost.
            hopsLeft = Mle::kMaxRouteCost + 1;
        }
        else
        {
            // Calculate the number of predicted hops.
            hopsLeft = mle.GetRouteCost(mMeshDest);

            if (hopsLeft != Mle::kMaxRouteCost)
            {
                hopsLeft += mle.GetLinkCost(Mle::Mle::GetRouterId(mle.GetNextHop(mMeshDest)));
            }
            else
            {
                // In case there is no route to the destination router (only link).
                hopsLeft = mle.GetLinkCost(Mle::Mle::GetRouterId(mMeshDest));
            }
        }

        // The hopsLft field MUST be incremented by one if the destination RLOC16
        // is not that of an active Router.
        if (!Mle::Mle::IsActiveRouter(mMeshDest))
        {
            hopsLeft += 1;
        }

        meshHeader.Init();
        meshHeader.SetHopsLeft(hopsLeft + Lowpan::MeshHeader::kAdditionalHopsLeft);
        meshHeader.SetSource(mMeshSource);
        meshHeader.SetDestination(mMeshDest);
        meshHeader.AppendTo(payload);
        payload += meshHeader.GetHeaderLength();
        headerLength += meshHeader.GetHeaderLength();
    }

#endif

    // copy IPv6 Header
    if (aMessage.GetOffset() == 0)
    {
        Lowpan::BufferWriter buffer(payload, aFrame.GetMaxPayloadLength() - headerLength -
                                                 Lowpan::FragmentHeader::kInitialHeaderSize);
        uint8_t              hcLength;

        error = Get<Lowpan::Lowpan>().Compress(aMessage, meshSource, meshDest, buffer);
        assert(error == OT_ERROR_NONE);

        hcLength = static_cast<uint8_t>(buffer.GetWritePointer() - payload);
        headerLength += hcLength;
        payloadLength  = aMessage.GetLength() - aMessage.GetOffset();
        fragmentLength = aFrame.GetMaxPayloadLength() - headerLength;

        if (payloadLength > fragmentLength)
        {
            if ((!aMessage.IsLinkSecurityEnabled()) && aMessage.IsSubTypeMle())
            {
                aMessage.SetOffset(0);
                ExitNow(error = OT_ERROR_NOT_CAPABLE);
            }

            // write Fragment header
            if (aMessage.GetDatagramTag() == 0)
            {
                // avoid using datagram tag value 0, which indicates the tag has not been set
                if (mFragTag == 0)
                {
                    mFragTag++;
                }

                aMessage.SetDatagramTag(mFragTag++);
            }

            memmove(payload + Lowpan::FragmentHeader::kInitialHeaderSize, payload, hcLength);

            fragmentHeader = reinterpret_cast<Lowpan::FragmentHeader *>(payload);
            fragmentHeader->Init();
            fragmentHeader->SetDatagramSize(aMessage.GetLength());
            fragmentHeader->SetDatagramTag(aMessage.GetDatagramTag());
            fragmentHeader->SetDatagramOffset(0);

            payload += fragmentHeader->GetHeaderLength();
            headerLength += fragmentHeader->GetHeaderLength();
            payloadLength = (aFrame.GetMaxPayloadLength() - headerLength) & ~0x7;
        }

        payload += hcLength;

        // copy IPv6 Payload
        aMessage.Read(aMessage.GetOffset(), payloadLength, payload);
        aFrame.SetPayloadLength(static_cast<uint8_t>(headerLength + payloadLength));

        mMessageNextOffset = aMessage.GetOffset() + payloadLength;
        aMessage.SetOffset(0);
    }
    else
    {
        payloadLength = aMessage.GetLength() - aMessage.GetOffset();

        // write Fragment header
        fragmentHeader = reinterpret_cast<Lowpan::FragmentHeader *>(payload);
        fragmentHeader->Init();
        fragmentHeader->SetDatagramSize(aMessage.GetLength());
        fragmentHeader->SetDatagramTag(aMessage.GetDatagramTag());
        fragmentHeader->SetDatagramOffset(aMessage.GetOffset());

        payload += fragmentHeader->GetHeaderLength();
        headerLength += fragmentHeader->GetHeaderLength();

        fragmentLength = (aFrame.GetMaxPayloadLength() - headerLength) & ~0x7;

        if (payloadLength > fragmentLength)
        {
            payloadLength = fragmentLength;
        }

        // copy IPv6 Payload
        aMessage.Read(aMessage.GetOffset(), payloadLength, payload);
        aFrame.SetPayloadLength(static_cast<uint8_t>(headerLength + payloadLength));

        mMessageNextOffset = aMessage.GetOffset() + payloadLength;
    }

    if (mMessageNextOffset < aMessage.GetLength())
    {
        aFrame.SetFramePending(true);
#if OPENTHREAD_CONFIG_ENABLE_TIME_SYNC
        aMessage.SetTimeSync(false);
#endif
    }

exit:

    return error;
}

void MeshForwarder::SendEmptyFrame(Mac::Frame &aFrame, bool aAckRequest)
{
    uint16_t     fcf;
    uint8_t      secCtl;
    Mac::Address macSource;

    macSource.SetShort(Get<Mac::Mac>().GetShortAddress());

    if (macSource.IsShortAddrInvalid())
    {
        macSource.SetExtended(Get<Mac::Mac>().GetExtAddress());
    }

    fcf = Mac::Frame::kFcfFrameData | Mac::Frame::kFcfFrameVersion2006;
    fcf |= (mMacDest.IsShort()) ? Mac::Frame::kFcfDstAddrShort : Mac::Frame::kFcfDstAddrExt;
    fcf |= (macSource.IsShort()) ? Mac::Frame::kFcfSrcAddrShort : Mac::Frame::kFcfSrcAddrExt;

    if (aAckRequest)
    {
        fcf |= Mac::Frame::kFcfAckRequest;
    }

    fcf |= Mac::Frame::kFcfSecurityEnabled;
    secCtl = Mac::Frame::kKeyIdMode1;
    secCtl |= Mac::Frame::kSecEncMic32;

    fcf |= Mac::Frame::kFcfPanidCompression;

    aFrame.InitMacHeader(fcf, secCtl);

    aFrame.SetDstPanId(Get<Mac::Mac>().GetPanId());
    aFrame.SetSrcPanId(Get<Mac::Mac>().GetPanId());
    aFrame.SetDstAddr(mMacDest);
    aFrame.SetSrcAddr(macSource);
    aFrame.SetPayloadLength(0);
    aFrame.SetFramePending(false);
}

Neighbor *MeshForwarder::UpdateNeighborOnSentFrame(Mac::Frame &aFrame, otError aError, const Mac::Address &aMacDest)
{
    Neighbor *neighbor = NULL;

    VerifyOrExit(mEnabled);

    neighbor = Get<Mle::MleRouter>().GetNeighbor(aMacDest);
    VerifyOrExit(neighbor != NULL);

    VerifyOrExit(aFrame.GetAckRequest());

    if (aError == OT_ERROR_NONE)
    {
        neighbor->ResetLinkFailures();
    }
    else if (aError == OT_ERROR_NO_ACK)
    {
        neighbor->IncrementLinkFailures();
        VerifyOrExit(Mle::Mle::IsActiveRouter(neighbor->GetRloc16()));

        if (neighbor->GetLinkFailures() >= Mle::kFailedRouterTransmissions)
        {
            Get<Mle::MleRouter>().RemoveNeighbor(*neighbor);
        }
    }

exit:
    return neighbor;
}

void MeshForwarder::HandleSentFrame(Mac::Frame &aFrame, otError aError)
{
    Neighbor *   neighbor = NULL;
    Mac::Address macDest;

    assert((aError == OT_ERROR_NONE) || (aError == OT_ERROR_CHANNEL_ACCESS_FAILURE) || (aError == OT_ERROR_ABORT) ||
           (aError == OT_ERROR_NO_ACK));

    mSendBusy = false;

    VerifyOrExit(mEnabled);

    aFrame.GetDstAddr(macDest);
    neighbor = UpdateNeighborOnSentFrame(aFrame, aError, macDest);

#if OPENTHREAD_FTD
    HandleSentFrameToChild(aFrame, aError, macDest);
#endif

    VerifyOrExit(mSendMessage != NULL);

    if (mSendMessage->GetDirectTransmission())
    {
        if (aError != OT_ERROR_NONE)
        {
            // If the transmission of any fragment frame fails,
            // the overall message transmission is considered
            // as failed

            mSendMessage->SetTxSuccess(false);

#if OPENTHREAD_CONFIG_DROP_MESSAGE_ON_FRAGMENT_TX_FAILURE

            // We set the NextOffset to end of message to avoid sending
            // any remaining fragments in the message.

            mMessageNextOffset = mSendMessage->GetLength();
#endif
        }

        if (mMessageNextOffset < mSendMessage->GetLength())
        {
            mSendMessage->SetOffset(mMessageNextOffset);
        }
        else
        {
            otError txError = aError;

            mSendMessage->ClearDirectTransmission();
            mSendMessage->SetOffset(0);

            if (neighbor != NULL)
            {
                neighbor->GetLinkInfo().AddMessageTxStatus(mSendMessage->GetTxSuccess());
            }

#if !OPENTHREAD_CONFIG_DROP_MESSAGE_ON_FRAGMENT_TX_FAILURE

            // When `CONFIG_DROP_MESSAGE_ON_FRAGMENT_TX_FAILURE` is
            // disabled, all fragment frames of a larger message are
            // sent even if the transmission of an earlier fragment fail.
            // Note that `GetTxSuccess() tracks the tx success of the
            // entire message, while `aError` represents the error
            // status of the last fragment frame transmission.

            if (!mSendMessage->GetTxSuccess() && (txError == OT_ERROR_NONE))
            {
                txError = OT_ERROR_FAILED;
            }
#endif

            LogMessage(kMessageTransmit, *mSendMessage, &macDest, txError);

            if (mSendMessage->GetType() == Message::kTypeIp6)
            {
                if (mSendMessage->GetTxSuccess())
                {
                    mIpCounters.mTxSuccess++;
                }
                else
                {
                    mIpCounters.mTxFailure++;
                }
            }
        }

        if (mSendMessage->GetSubType() == Message::kSubTypeMleDiscoverRequest)
        {
            mSendBusy = true;
            mDiscoverTimer.Start(static_cast<uint16_t>(Mac::kScanDurationDefault));
            ExitNow();
        }
    }

    if (mSendMessage->GetDirectTransmission() == false && mSendMessage->IsChildPending() == false)
    {
        if (mSendMessage->GetSubType() == Message::kSubTypeMleChildIdRequest && mSendMessage->IsLinkSecurityEnabled())
        {
            // If the Child ID Request requires fragmentation and therefore
            // link layer security, the frame transmission will be aborted.
            // When the message is being freed, we signal to MLE to prepare a
            // shorter Child ID Request message (by only including mesh-local
            // address in the Address Registration TLV).

            otLogInfoMac("Requesting shorter `Child ID Request`");
            Get<Mle::Mle>().RequestShorterChildIdRequest();
        }

        mSendQueue.Dequeue(*mSendMessage);
        mSendMessage->Free();
        mSendMessage       = NULL;
        mMessageNextOffset = 0;
    }

exit:

    if (mEnabled)
    {
        mScheduleTransmissionTask.Post();
    }
}

void MeshForwarder::SetDiscoverParameters(const Mac::ChannelMask &aScanChannels)
{
    uint32_t mask;
    uint32_t supportedMask = Get<Mac::Mac>().GetSupportedChannelMask().GetMask();

    mask = aScanChannels.IsEmpty() ? supportedMask : aScanChannels.GetMask();
    mScanChannels.SetMask(mask & supportedMask);
}

void MeshForwarder::HandleDiscoverTimer(Timer &aTimer)
{
    aTimer.GetOwner<MeshForwarder>().HandleDiscoverTimer();
}

void MeshForwarder::HandleDiscoverTimer(void)
{
    if (mScanChannels.GetNextChannel(mScanChannel) != OT_ERROR_NONE)
    {
        mSendQueue.Dequeue(*mSendMessage);
        mSendMessage->Free();
        mSendMessage = NULL;

        HandleDiscoverComplete();
        ExitNow();
    }

    mSendMessage->SetDirectTransmission();

exit:
    mSendBusy = false;
    mScheduleTransmissionTask.Post();
}

void MeshForwarder::HandleDiscoverComplete(void)
{
    assert(mScanning);

    if (mMacRadioAcquisitionId)
    {
        Get<Mac::Mac>().ReleaseRadioChannel();
        mMacRadioAcquisitionId = 0;
    }

    Get<Mac::Mac>().SetPanId(mRestorePanId);
    mScanning = false;
    Get<Mle::MleRouter>().HandleDiscoverComplete();
    mDiscoverTimer.Stop();
}

void MeshForwarder::HandleReceivedFrame(Mac::Frame &aFrame)
{
    otThreadLinkInfo linkInfo;
    Mac::Address     macDest;
    Mac::Address     macSource;
    uint8_t *        payload;
    uint8_t          payloadLength;
    otError          error = OT_ERROR_NONE;

    if (!mEnabled)
    {
        ExitNow(error = OT_ERROR_INVALID_STATE);
    }

    SuccessOrExit(error = aFrame.GetSrcAddr(macSource));
    SuccessOrExit(error = aFrame.GetDstAddr(macDest));

    aFrame.GetSrcPanId(linkInfo.mPanId);
    linkInfo.mChannel      = aFrame.GetChannel();
    linkInfo.mRss          = aFrame.GetRssi();
    linkInfo.mLqi          = aFrame.GetLqi();
    linkInfo.mLinkSecurity = aFrame.GetSecurityEnabled();
#if OPENTHREAD_CONFIG_ENABLE_TIME_SYNC
    linkInfo.mNetworkTimeOffset = aFrame.GetNetworkTimeOffset();
    linkInfo.mTimeSyncSeq       = aFrame.GetTimeSyncSeq();
#endif

    payload       = aFrame.GetPayload();
    payloadLength = aFrame.GetPayloadLength();

    Get<Utils::SupervisionListener>().UpdateOnReceive(macSource, linkInfo.mLinkSecurity);

    switch (aFrame.GetType())
    {
    case Mac::Frame::kFcfFrameData:
        if (payloadLength >= sizeof(Lowpan::MeshHeader) &&
            reinterpret_cast<Lowpan::MeshHeader *>(payload)->IsMeshHeader())
        {
#if OPENTHREAD_FTD
            HandleMesh(payload, payloadLength, macSource, linkInfo);
#endif
        }
        else if (payloadLength >= sizeof(Lowpan::FragmentHeader) &&
                 reinterpret_cast<Lowpan::FragmentHeader *>(payload)->IsFragmentHeader())
        {
            HandleFragment(payload, payloadLength, macSource, macDest, linkInfo);
        }
        else if (payloadLength >= 1 && Lowpan::Lowpan::IsLowpanHc(payload))
        {
            HandleLowpanHC(payload, payloadLength, macSource, macDest, linkInfo);
        }
        else
        {
            VerifyOrExit(payloadLength == 0, error = OT_ERROR_NOT_LOWPAN_DATA_FRAME);

            LogFrame("Received empty payload frame", aFrame, OT_ERROR_NONE);
        }

        break;

#if OPENTHREAD_FTD

    case Mac::Frame::kFcfFrameMacCmd:
    {
        uint8_t commandId;

        aFrame.GetCommandId(commandId);

        if (commandId == Mac::Frame::kMacCmdDataRequest)
        {
            HandleDataRequest(aFrame, macSource, linkInfo);
        }
        else
        {
            error = OT_ERROR_DROP;
        }

        break;
    }

#endif

    case Mac::Frame::kFcfFrameBeacon:
        break;

    default:
        error = OT_ERROR_DROP;
        break;
    }

exit:

    if (error != OT_ERROR_NONE)
    {
        LogFrame("Dropping rx frame", aFrame, error);
    }
}

void MeshForwarder::HandleFragment(uint8_t *               aFrame,
                                   uint8_t                 aFrameLength,
                                   const Mac::Address &    aMacSource,
                                   const Mac::Address &    aMacDest,
                                   const otThreadLinkInfo &aLinkInfo)
{
    otError                error = OT_ERROR_NONE;
    Lowpan::FragmentHeader fragmentHeader;
    Message *              message = NULL;
    int                    headerLength;

    // Check the fragment header
    VerifyOrExit(fragmentHeader.Init(aFrame, aFrameLength) == OT_ERROR_NONE, error = OT_ERROR_PARSE);
    aFrame += fragmentHeader.GetHeaderLength();
    aFrameLength -= fragmentHeader.GetHeaderLength();

    if (fragmentHeader.GetDatagramOffset() == 0)
    {
        uint8_t priority;

        SuccessOrExit(error = GetFramePriority(aFrame, aFrameLength, aMacSource, aMacDest, priority));
        VerifyOrExit((message = Get<MessagePool>().New(Message::kTypeIp6, 0, priority)) != NULL,
                     error = OT_ERROR_NO_BUFS);
        message->SetLinkSecurityEnabled(aLinkInfo.mLinkSecurity);
        message->SetPanId(aLinkInfo.mPanId);
        message->AddRss(aLinkInfo.mRss);
#if OPENTHREAD_CONFIG_ENABLE_TIME_SYNC
        message->SetTimeSyncSeq(aLinkInfo.mTimeSyncSeq);
        message->SetNetworkTimeOffset(aLinkInfo.mNetworkTimeOffset);
#endif
        headerLength = Get<Lowpan::Lowpan>().Decompress(*message, aMacSource, aMacDest, aFrame, aFrameLength,
                                                        fragmentHeader.GetDatagramSize());
        VerifyOrExit(headerLength > 0, error = OT_ERROR_PARSE);

        aFrame += headerLength;
        aFrameLength -= static_cast<uint8_t>(headerLength);

        VerifyOrExit(fragmentHeader.GetDatagramSize() >= message->GetOffset() + aFrameLength, error = OT_ERROR_PARSE);

        SuccessOrExit(error = message->SetLength(fragmentHeader.GetDatagramSize()));

        message->SetDatagramTag(fragmentHeader.GetDatagramTag());
        message->SetTimeout(kReassemblyTimeout);

        // copy Fragment
        message->Write(message->GetOffset(), aFrameLength, aFrame);
        message->MoveOffset(aFrameLength);

        // Security Check
        VerifyOrExit(Get<Ip6::Filter>().Accept(*message), error = OT_ERROR_DROP);

        // Allow re-assembly of only one message at a time on a SED by clearing
        // any remaining fragments in reassembly list upon receiving of a new
        // (secure) first fragment.

        if ((GetRxOnWhenIdle() == false) && message->IsLinkSecurityEnabled())
        {
            ClearReassemblyList();
        }

        mReassemblyList.Enqueue(*message);

        if (!mUpdateTimer.IsRunning())
        {
            mUpdateTimer.Start(kStateUpdatePeriod);
        }
    }
    else
    {
        for (message = mReassemblyList.GetHead(); message; message = message->GetNext())
        {
            // Security Check: only consider reassembly buffers that had the same Security Enabled setting.
            if (message->GetLength() == fragmentHeader.GetDatagramSize() &&
                message->GetDatagramTag() == fragmentHeader.GetDatagramTag() &&
                message->GetOffset() == fragmentHeader.GetDatagramOffset() &&
                message->GetOffset() + aFrameLength <= fragmentHeader.GetDatagramSize() &&
                message->IsLinkSecurityEnabled() == aLinkInfo.mLinkSecurity)
            {
                break;
            }
        }

        // For a sleepy-end-device, if we receive a new (secure) next fragment
        // with a non-matching fragmentation offset or tag, it indicates that
        // we have either missed a fragment, or the parent has moved to a new
        // message with a new tag. In either case, we can safely clear any
        // remaining fragments stored in the reassembly list.

        if (GetRxOnWhenIdle() == false)
        {
            if ((message == NULL) && (aLinkInfo.mLinkSecurity))
            {
                ClearReassemblyList();
            }
        }

        VerifyOrExit(message != NULL, error = OT_ERROR_DROP);

        // copy Fragment
        message->Write(message->GetOffset(), aFrameLength, aFrame);
        message->MoveOffset(aFrameLength);
        message->AddRss(aLinkInfo.mRss);
        message->SetTimeout(kReassemblyTimeout);
    }

exit:

    if (error == OT_ERROR_NONE)
    {
        if (message->GetOffset() >= message->GetLength())
        {
            mReassemblyList.Dequeue(*message);
            HandleDatagram(*message, aLinkInfo, aMacSource);
        }
    }
    else
    {
        LogFragmentFrameDrop(error, aFrameLength, aMacSource, aMacDest, fragmentHeader, aLinkInfo.mLinkSecurity);

        if (message != NULL)
        {
            message->Free();
        }
    }
}

void MeshForwarder::ClearReassemblyList(void)
{
    Message *message;
    Message *next;

    for (message = mReassemblyList.GetHead(); message; message = next)
    {
        next = message->GetNext();
        mReassemblyList.Dequeue(*message);

        LogMessage(kMessageReassemblyDrop, *message, NULL, OT_ERROR_NO_FRAME_RECEIVED);

        if (message->GetType() == Message::kTypeIp6)
        {
            mIpCounters.mRxFailure++;
        }

        message->Free();
    }
}

void MeshForwarder::HandleUpdateTimer(Timer &aTimer)
{
    aTimer.GetOwner<MeshForwarder>().HandleUpdateTimer();
}

void MeshForwarder::HandleUpdateTimer(void)
{
    bool shouldRun = false;

#if OPENTHREAD_FTD
    shouldRun = UpdateFragmentLifetime();
#endif

    if (UpdateReassemblyList() || shouldRun)
    {
        mUpdateTimer.Start(kStateUpdatePeriod);
    }
}

bool MeshForwarder::UpdateReassemblyList(void)
{
    Message *next = NULL;

    for (Message *message = mReassemblyList.GetHead(); message; message = next)
    {
        next = message->GetNext();

        if (message->GetTimeout() > 0)
        {
            message->DecrementTimeout();
        }
        else
        {
            mReassemblyList.Dequeue(*message);

            LogMessage(kMessageReassemblyDrop, *message, NULL, OT_ERROR_REASSEMBLY_TIMEOUT);
            if (message->GetType() == Message::kTypeIp6)
            {
                mIpCounters.mRxFailure++;
            }

            message->Free();
        }
    }

    return mReassemblyList.GetHead() != NULL;
}

void MeshForwarder::HandleLowpanHC(uint8_t *               aFrame,
                                   uint8_t                 aFrameLength,
                                   const Mac::Address &    aMacSource,
                                   const Mac::Address &    aMacDest,
                                   const otThreadLinkInfo &aLinkInfo)
{
    otError  error   = OT_ERROR_NONE;
    Message *message = NULL;
    int      headerLength;
    uint8_t  priority;

#if OPENTHREAD_FTD
    UpdateRoutes(aFrame, aFrameLength, aMacSource, aMacDest);
#endif

    SuccessOrExit(error = GetFramePriority(aFrame, aFrameLength, aMacSource, aMacDest, priority));
    VerifyOrExit((message = Get<MessagePool>().New(Message::kTypeIp6, 0, priority)) != NULL, error = OT_ERROR_NO_BUFS);
    message->SetLinkSecurityEnabled(aLinkInfo.mLinkSecurity);
    message->SetPanId(aLinkInfo.mPanId);
    message->AddRss(aLinkInfo.mRss);
#if OPENTHREAD_CONFIG_ENABLE_TIME_SYNC
    message->SetTimeSyncSeq(aLinkInfo.mTimeSyncSeq);
    message->SetNetworkTimeOffset(aLinkInfo.mNetworkTimeOffset);
#endif

    headerLength = Get<Lowpan::Lowpan>().Decompress(*message, aMacSource, aMacDest, aFrame, aFrameLength, 0);
    VerifyOrExit(headerLength > 0, error = OT_ERROR_PARSE);

    aFrame += headerLength;
    aFrameLength -= static_cast<uint8_t>(headerLength);

    SuccessOrExit(error = message->SetLength(message->GetLength() + aFrameLength));
    message->Write(message->GetOffset(), aFrameLength, aFrame);

    // Security Check
    VerifyOrExit(Get<Ip6::Filter>().Accept(*message), error = OT_ERROR_DROP);

exit:

    if (error == OT_ERROR_NONE)
    {
        HandleDatagram(*message, aLinkInfo, aMacSource);
    }
    else
    {
        LogLowpanHcFrameDrop(error, aFrameLength, aMacSource, aMacDest, aLinkInfo.mLinkSecurity);

        if (message != NULL)
        {
            message->Free();
        }
    }
}

otError MeshForwarder::HandleDatagram(Message &               aMessage,
                                      const otThreadLinkInfo &aLinkInfo,
                                      const Mac::Address &    aMacSource)
{
    ThreadNetif &netif = Get<ThreadNetif>();

    LogMessage(kMessageReceive, aMessage, &aMacSource, OT_ERROR_NONE);

    if (aMessage.GetType() == Message::kTypeIp6)
    {
        mIpCounters.mRxSuccess++;
    }

    return Get<Ip6::Ip6>().HandleDatagram(aMessage, &netif, &aLinkInfo, false);
}

otError MeshForwarder::GetFramePriority(const uint8_t *     aFrame,
                                        uint8_t             aFrameLength,
                                        const Mac::Address &aMacSource,
                                        const Mac::Address &aMacDest,
                                        uint8_t &           aPriority)
{
    otError        error = OT_ERROR_NONE;
    Ip6::Header    ip6Header;
    Ip6::UdpHeader udpHeader;
    uint8_t        headerLength;
    bool           nextHeaderCompressed;

    SuccessOrExit(error = DecompressIp6Header(aFrame, aFrameLength, aMacSource, aMacDest, ip6Header, headerLength,
                                              nextHeaderCompressed));
    aPriority = Ip6::Ip6::DscpToPriority(ip6Header.GetDscp());
    VerifyOrExit(ip6Header.GetNextHeader() == Ip6::kProtoUdp);

    aFrame += headerLength;
    aFrameLength -= headerLength;

    if (nextHeaderCompressed)
    {
        VerifyOrExit(Get<Lowpan::Lowpan>().DecompressUdpHeader(udpHeader, aFrame, aFrameLength) >= 0);
    }
    else
    {
        VerifyOrExit(aFrameLength >= sizeof(Ip6::UdpHeader), error = OT_ERROR_PARSE);
        memcpy(&udpHeader, aFrame, sizeof(Ip6::UdpHeader));
    }

    if (udpHeader.GetDestinationPort() == Mle::kUdpPort || udpHeader.GetDestinationPort() == kCoapUdpPort)
    {
        aPriority = Message::kPriorityNet;
    }

exit:
    return error;
}

#if (OPENTHREAD_CONFIG_LOG_LEVEL >= OT_LOG_LEVEL_NOTE) && (OPENTHREAD_CONFIG_LOG_MAC == 1)

otError MeshForwarder::ParseIp6UdpTcpHeader(const Message &aMessage,
                                            Ip6::Header &  aIp6Header,
                                            uint16_t &     aChecksum,
                                            uint16_t &     aSourcePort,
                                            uint16_t &     aDestPort)
{
    otError error = OT_ERROR_PARSE;
    union
    {
        Ip6::UdpHeader udp;
        Ip6::TcpHeader tcp;
    } header;

    aChecksum   = 0;
    aSourcePort = 0;
    aDestPort   = 0;

    VerifyOrExit(sizeof(Ip6::Header) == aMessage.Read(0, sizeof(Ip6::Header), &aIp6Header));
    VerifyOrExit(aIp6Header.IsVersion6());

    switch (aIp6Header.GetNextHeader())
    {
    case Ip6::kProtoUdp:
        VerifyOrExit(sizeof(Ip6::UdpHeader) == aMessage.Read(sizeof(Ip6::Header), sizeof(Ip6::UdpHeader), &header.udp));
        aChecksum   = header.udp.GetChecksum();
        aSourcePort = header.udp.GetSourcePort();
        aDestPort   = header.udp.GetDestinationPort();
        break;

    case Ip6::kProtoTcp:
        VerifyOrExit(sizeof(Ip6::TcpHeader) == aMessage.Read(sizeof(Ip6::Header), sizeof(Ip6::TcpHeader), &header.tcp));
        aChecksum   = header.tcp.GetChecksum();
        aSourcePort = header.tcp.GetSourcePort();
        aDestPort   = header.tcp.GetDestinationPort();
        break;

    default:
        break;
    }

    error = OT_ERROR_NONE;

exit:
    return error;
}

const char *MeshForwarder::MessageActionToString(MessageAction aAction, otError aError)
{
    const char *actionText = "";

    switch (aAction)
    {
    case kMessageReceive:
        actionText = "Received";
        break;

    case kMessageTransmit:
        actionText = (aError == OT_ERROR_NONE) ? "Sent" : "Failed to send";
        break;

    case kMessagePrepareIndirect:
        actionText = "Prepping indir tx";
        break;

    case kMessageDrop:
        actionText = "Dropping";
        break;

    case kMessageReassemblyDrop:
        actionText = "Dropping (reassembly queue)";
        break;

    case kMessageEvict:
        actionText = "Evicting";
        break;
    }

    return actionText;
}

const char *MeshForwarder::MessagePriorityToString(const Message &aMessage)
{
    const char *priorityText = "unknown";

    switch (aMessage.GetPriority())
    {
    case Message::kPriorityNet:
        priorityText = "net";
        break;

    case Message::kPriorityHigh:
        priorityText = "high";
        break;

    case Message::kPriorityNormal:
        priorityText = "normal";
        break;

    case Message::kPriorityLow:
        priorityText = "low";
        break;
    }

    return priorityText;
}

#if OPENTHREAD_CONFIG_LOG_SRC_DST_IP_ADDRESSES
void MeshForwarder::LogIp6SourceDestAddresses(Ip6::Header &aIp6Header,
                                              uint16_t     aSourcePort,
                                              uint16_t     aDestPort,
                                              otLogLevel   aLogLevel)
{
    if (aSourcePort != 0)
    {
        otLogMac(aLogLevel, "\tsrc:[%s]:%d", aIp6Header.GetSource().ToString().AsCString(), aSourcePort);
    }
    else
    {
        otLogMac(aLogLevel, "\tsrc:[%s]", aIp6Header.GetSource().ToString().AsCString());
    }

    if (aDestPort != 0)
    {
        otLogMac(aLogLevel, "\tdst:[%s]:%d", aIp6Header.GetDestination().ToString().AsCString(), aDestPort);
    }
    else
    {
        otLogMac(aLogLevel, "\tdst:[%s]", aIp6Header.GetDestination().ToString().AsCString());
    }
}
#else
void MeshForwarder::LogIp6SourceDestAddresses(Ip6::Header &, uint16_t, uint16_t, otLogLevel)
{
}
#endif

void MeshForwarder::LogIp6Message(MessageAction       aAction,
                                  const Message &     aMessage,
                                  const Mac::Address *aMacAddress,
                                  otError             aError,
                                  otLogLevel          aLogLevel)
{
    Ip6::Header ip6Header;
    uint16_t    checksum;
    uint16_t    sourcePort;
    uint16_t    destPort;
    bool        shouldLogRss;

    SuccessOrExit(ParseIp6UdpTcpHeader(aMessage, ip6Header, checksum, sourcePort, destPort));

    shouldLogRss = (aAction == kMessageReceive) || (aAction == kMessageReassemblyDrop);

    otLogMac(aLogLevel, "%s IPv6 %s msg, len:%d, chksum:%04x%s%s, sec:%s%s%s, prio:%s%s%s",
             MessageActionToString(aAction, aError), Ip6::Ip6::IpProtoToString(ip6Header.GetNextHeader()),
             aMessage.GetLength(), checksum,
             (aMacAddress == NULL) ? "" : ((aAction == kMessageReceive) ? ", from:" : ", to:"),
             (aMacAddress == NULL) ? "" : aMacAddress->ToString().AsCString(),
             aMessage.IsLinkSecurityEnabled() ? "yes" : "no", (aError == OT_ERROR_NONE) ? "" : ", error:",
             (aError == OT_ERROR_NONE) ? "" : otThreadErrorToString(aError), MessagePriorityToString(aMessage),
             shouldLogRss ? ", rss:" : "", shouldLogRss ? aMessage.GetRssAverager().ToString().AsCString() : "");

    if (aAction != kMessagePrepareIndirect)
    {
        LogIp6SourceDestAddresses(ip6Header, sourcePort, destPort, aLogLevel);
    }

exit:
    return;
}

void MeshForwarder::LogMessage(MessageAction       aAction,
                               const Message &     aMessage,
                               const Mac::Address *aMacAddress,
                               otError             aError)
{
    otLogLevel logLevel = OT_LOG_LEVEL_INFO;

    switch (aAction)
    {
    case kMessageReceive:
    case kMessageTransmit:
    case kMessagePrepareIndirect:
        logLevel = (aError == OT_ERROR_NONE) ? OT_LOG_LEVEL_INFO : OT_LOG_LEVEL_NOTE;
        break;

    case kMessageDrop:
    case kMessageReassemblyDrop:
    case kMessageEvict:
        logLevel = OT_LOG_LEVEL_NOTE;
        break;
    }

    VerifyOrExit(GetInstance().GetLogLevel() >= logLevel);

    switch (aMessage.GetType())
    {
    case Message::kTypeIp6:
        LogIp6Message(aAction, aMessage, aMacAddress, aError, logLevel);
        break;

#if OPENTHREAD_FTD
    case Message::kType6lowpan:
        LogMeshMessage(aAction, aMessage, aMacAddress, aError, logLevel);
        break;
#endif

    default:
        break;
    }

exit:
    return;
}

void MeshForwarder::LogFrame(const char *aActionText, const Mac::Frame &aFrame, otError aError)
{
    if (aError != OT_ERROR_NONE)
    {
        otLogNoteMac("%s, aError:%s, %s", aActionText, otThreadErrorToString(aError),
                     aFrame.ToInfoString().AsCString());
    }
    else
    {
        otLogInfoMac("%s, %s", aActionText, aFrame.ToInfoString().AsCString());
    }
}

void MeshForwarder::LogFragmentFrameDrop(otError                       aError,
                                         uint8_t                       aFrameLength,
                                         const Mac::Address &          aMacSource,
                                         const Mac::Address &          aMacDest,
                                         const Lowpan::FragmentHeader &aFragmentHeader,
                                         bool                          aIsSecure)
{
    otLogNoteMac("Dropping rx frag frame, error:%s, len:%d, src:%s, dst:%s, tag:%d, offset:%d, dglen:%d, sec:%s",
                 otThreadErrorToString(aError), aFrameLength, aMacSource.ToString().AsCString(),
                 aMacDest.ToString().AsCString(), aFragmentHeader.GetDatagramTag(), aFragmentHeader.GetDatagramOffset(),
                 aFragmentHeader.GetDatagramSize(), aIsSecure ? "yes" : "no");
}

void MeshForwarder::LogLowpanHcFrameDrop(otError             aError,
                                         uint8_t             aFrameLength,
                                         const Mac::Address &aMacSource,
                                         const Mac::Address &aMacDest,
                                         bool                aIsSecure)
{
    otLogNoteMac("Dropping rx lowpan HC frame, error:%s, len:%d, src:%s, dst:%s, sec:%s", otThreadErrorToString(aError),
                 aFrameLength, aMacSource.ToString().AsCString(), aMacDest.ToString().AsCString(),
                 aIsSecure ? "yes" : "no");
}

#else // #if (OPENTHREAD_CONFIG_LOG_LEVEL >= OT_LOG_LEVEL_INFO) && (OPENTHREAD_CONFIG_LOG_MAC == 1)

void MeshForwarder::LogMessage(MessageAction, const Message &, const Mac::Address *, otError)
{
}

void MeshForwarder::LogFrame(const char *, const Mac::Frame &, otError)
{
}

void MeshForwarder::LogFragmentFrameDrop(otError,
                                         uint8_t,
                                         const Mac::Address &,
                                         const Mac::Address &,
                                         const Lowpan::FragmentHeader &,
                                         bool)
{
}

void MeshForwarder::LogLowpanHcFrameDrop(otError, uint8_t, const Mac::Address &, const Mac::Address &, bool)
{
}

#endif // #if (OPENTHREAD_CONFIG_LOG_LEVEL >= OT_LOG_LEVEL_INFO) && (OPENTHREAD_CONFIG_LOG_MAC == 1)

} // namespace ot
