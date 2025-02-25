#pragma once

#include "defs.h"
#include "dsproxy_blackboard.h"

#include <ydb/core/blobstorage/groupinfo/blobstorage_groupinfo_sets.h>

namespace NKikimr {

    class TMirror3dcBasicGetStrategy : public IStrategy {
        const TNodeLayoutInfoPtr& NodeLayout;
        const bool PhantomCheck;

        static constexpr ui32 NumRings = 3;
        static constexpr ui32 NumFailDomainsPerRing = 3;

    public:
        TMirror3dcBasicGetStrategy(const TNodeLayoutInfoPtr& nodeLayout, bool phantomCheck)
            : NodeLayout(nodeLayout)
            , PhantomCheck(phantomCheck)
        {}

        bool DoRequestDisk(TBlobState& state, TGroupDiskRequests& groupDiskRequests, ui32 diskIdx) {
            TBlobState::TDisk& disk = state.Disks[diskIdx];
            // calculate part number from disk; ring always matches PartIdx
            const ui32 partIdx = diskIdx % NumRings;
            TBlobState::TDiskPart& diskPart = disk.DiskParts[partIdx];
            switch (diskPart.Situation) {
                case TBlobState::ESituation::Unknown: {
                    // get the request -- all the needed parts except already got and already requested
                    TIntervalSet<i32> request(state.Whole.Needed);
                    request.Subtract(state.Whole.Here);
                    // remove parts that were already requested, but not yet answered
                    request.Subtract(diskPart.Requested);
                    if (!request.IsEmpty()) {
                        TLogoBlobID id(state.Id, partIdx + 1);
                        groupDiskRequests.AddGet(disk.OrderNumber, id, request);
                        diskPart.Requested.Add(request);
                    } else {
                        // ensure that we are waiting for some data to come
                        Y_VERIFY(!diskPart.Requested.IsEmpty());
                    }
                    // return true indicating that we have a request that is not yet satisfied
                    return true;
                }
                case TBlobState::ESituation::Present:
                    break;
                case TBlobState::ESituation::Error:
                    break;
                case TBlobState::ESituation::Absent:
                    break;
                case TBlobState::ESituation::Lost:
                    break;
                case TBlobState::ESituation::Sent:
                    Y_FAIL("unexpected state");
            }

            return false;
        }

        EStrategyOutcome Process(TLogContext& logCtx, TBlobState& state, const TBlobStorageGroupInfo& info,
                TBlackboard &blackboard, TGroupDiskRequests& groupDiskRequests) override {
            if (state.WholeSituation == TBlobState::ESituation::Present) {
                return EStrategyOutcome::DONE;
            }

            const ui32 totalPartCount = info.Type.TotalPartCount();

            // merge found data parts in our blob
            for (ui32 partIdx = 0; partIdx < totalPartCount; ++partIdx) {
                const TBlobState::TState& part = state.Parts[partIdx];

                // check if we can obtain some _new_ data from the part
                if (!part.Here.IsSubsetOf(state.Whole.Here)) {
                    // scan through all the intervals
                    for (const auto& range : part.Here) {
                        ui64 begin = range.first;
                        const ui64 end = range.second;
                        TIntervalVec<i32> interval(begin, end);
                        // check if this interval contains some data which is not in state.Whole.Here
                        if (!interval.IsSubsetOf(state.Whole.Here)) {
                            char buffer[4096];
                            while (begin != end) {
                                const ui64 len = Min<ui64>(sizeof(buffer), end - begin);
                                part.Data.Read(begin, buffer, len);
                                state.Whole.Data.Write(begin, buffer, len);
                                begin += len;
                            }
                            state.Whole.Here.Add(interval);
                        }
                    }
                }
            }
            if (state.Whole.Needed.IsSubsetOf(state.Whole.Here)) {
                // we are not going to restore this blob and we have all required data read, so we can exit now
                state.WholeSituation = TBlobState::ESituation::Present;
                return EStrategyOutcome::DONE;
            }

            // issue request for a specific disk; returns true if the request was issued and not yet completed, otherwise
            // false

            // find the slowest disk and mark it
            switch (blackboard.AccelerationMode) {
                case TBlackboard::AccelerationModeSkipOneSlowest: {
                    i32 worstSubgroupIdx = -1;
                    ui64 worstPredictedNs = 0;
                    ui64 nextToWorstPredictedNs = 0;
                    state.GetWorstPredictedDelaysNs(info, *blackboard.GroupQueues,
                            HandleClassToQueueId(blackboard.GetHandleClass),
                            &worstPredictedNs, &nextToWorstPredictedNs, &worstSubgroupIdx);

                    // Check if the slowest disk exceptionally slow, or just not very fast
                    i32 slowDiskSubgroupIdx = -1;
                    if (nextToWorstPredictedNs > 0 && worstPredictedNs > nextToWorstPredictedNs * 2) {
                        slowDiskSubgroupIdx = worstSubgroupIdx;
                    }

                    // Mark single slow disk
                    for (size_t diskIdx = 0; diskIdx < state.Disks.size(); ++diskIdx) {
                        state.Disks[diskIdx].IsSlow = false;
                    }
                    if (slowDiskSubgroupIdx >= 0) {
                        state.Disks[slowDiskSubgroupIdx].IsSlow = true;
                    }
                    break;
                }
                case TBlackboard::AccelerationModeSkipMarked:
                    // The slowest disk is already marked!
                    break;
            }

            // create an array defining order in which we traverse the disks
            TStackVec<ui32, 32> diskIdxList;
            for (ui32 i = 0; i < state.Disks.size(); ++i) {
                diskIdxList.push_back(i);
            }

            // calculate distance (in relative units) to the disk from our node
            // non-main replicas get +1 second-level score
            // marked slow get +2 second-level score
            // sort according to this distance high part indicates fail
            // domain -- we want to scan for main replicas first, then scan handoff
            auto distance = [&](ui32 diskIdx) {
                const bool isMain = diskIdx < NumRings;
                const bool isSlow = state.Disks[diskIdx].IsSlow;
                ui64 score = static_cast<ui64>((isMain ? 0 : 1) + (isSlow ? 2 : 0)) << 32;

                if (NodeLayout) {
                    const ui32 orderNumber = state.Disks[diskIdx].OrderNumber;
                    const auto& diskItems = NodeLayout->LocationPerOrderNumber[orderNumber].GetItems();
                    const auto& selfItems = NodeLayout->SelfLocation.GetItems();
                    i64 commonPrefixKey = Min<int>();
                    for (auto diskIt = diskItems.begin(), selfIt = selfItems.begin();; ++diskIt, ++selfIt) {
                        if (diskIt == diskItems.end() || selfIt == selfItems.end() || *diskIt != *selfIt) {
                            break;
                        }
                        commonPrefixKey = diskIt->first;
                    }
                    score += Max<int>() - commonPrefixKey;
                }

                return score;
            };
            auto compare = [&](ui32 x, ui32 y) {
                return distance(x) < distance(y);
            };
            std::sort(diskIdxList.begin(), diskIdxList.end(), compare);


            // scan all disks and try to generate new request
            bool requested = false; // was the new request generated or not
            for (ui32 diskIdx : diskIdxList) {
                if ((requested = DoRequestDisk(state, groupDiskRequests, diskIdx))) {
                    break;
                }
            }

            TBlobStorageGroupInfo::TSubgroupVDisks failed(&info.GetTopology()), possiblyWritten(&info.GetTopology());
            TStackVec<TBlobState::ESituation, NumRings * NumFailDomainsPerRing> situations;
            for (ui32 diskIdx : diskIdxList) {
                TBlobState::TDisk& disk = state.Disks[diskIdx];
                const ui32 partIdx = diskIdx % NumRings;
                const TBlobState::TDiskPart& diskPart = disk.DiskParts[partIdx];
                switch (diskPart.Situation) {
                    case TBlobState::ESituation::Error:
                        failed += TBlobStorageGroupInfo::TSubgroupVDisks(&info.GetTopology(), diskIdx);
                        [[fallthrough]];
                    case TBlobState::ESituation::Lost:
                        possiblyWritten += TBlobStorageGroupInfo::TSubgroupVDisks(&info.GetTopology(), diskIdx);
                        break;
                    default:
                        break;
                }
                situations.push_back(diskPart.Situation);
            }

            if (!info.GetQuorumChecker().CheckFailModelForSubgroup(failed)) {
                return EStrategyOutcome::Error("TMirror3dcBasicGetStrategy failed the Fail Model check");
            } else if (requested) {
                // we can't finish request now, because the VGet was just issued or still being executed, so we
                // drop status to UNKNOWN
                return EStrategyOutcome::IN_PROGRESS;
            } else if (!state.Whole.Needed.IsSubsetOf(state.Whole.Here)) {
                // we haven't requested anything, but there is no required data in buffer, so blob is lost
                R_LOG_WARN_SX(logCtx, "BPG48", "missing blob# " << state.Id.ToString() << " state# " << state.ToString());
                state.WholeSituation = TBlobState::ESituation::Absent;
                state.LooksLikePhantom = true;
                if (PhantomCheck || info.GetQuorumChecker().CheckQuorumForSubgroup(possiblyWritten)) {
                    // this blob is either:
                    // 1. Has full quorum of Lost & Error replies
                    // 2. Is checked for being phantom during replication
                    // in both cases we return Absent only when there are only Lost and Absent replies from the disks,
                    // otherwise we return ERROR assuming this blob could be restored
                    for (const TBlobState::ESituation situation : situations) {
                        switch (situation) {
                            case TBlobState::ESituation::Absent:
                            case TBlobState::ESituation::Lost:
                                // these statuses do not lead to error as they represent missing blob data
                                break;

                            case TBlobState::ESituation::Unknown:
                            case TBlobState::ESituation::Present:
                            case TBlobState::ESituation::Sent:
                                // unexpected state
                                Y_VERIFY_DEBUG(false);
                                [[fallthrough]];
                            case TBlobState::ESituation::Error:
                                state.WholeSituation = TBlobState::ESituation::Error;
                                break;
                        }
                    }
                }
                return EStrategyOutcome::DONE;
            } else {
                Y_FAIL("must not reach this point");
            }
        }
    };

} // NKikimr
