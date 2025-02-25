#include "blob_cache.h"
#include "columnshard.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/blobstorage.h>
#include <ydb/core/base/tablet_pipe.h>

#include <library/cpp/actors/core/actor.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/cache/cache.h>

#include <util/string/vector.h>
#include <tuple>

namespace NKikimr::NBlobCache {
namespace {

using namespace NActors;

class TBlobCache: public TActorBootstrapped<TBlobCache> {
private:
    struct TReadInfo {
        bool Cache;                 // Put in cache after read?
        TList<TActorId> Waiting;    // List of readers

        TReadInfo()
            : Cache(true)
        {}
    };

    struct TReadItem : public TReadBlobRangeOptions {
        enum class EReadVariant {
            FAST = 0,
            DEFAULT,
            DEFAULT_NO_DEADLINE,
        };

        TBlobRange BlobRange;

        TReadItem(const TReadBlobRangeOptions& opts, const TBlobRange& blobRange)
            : TReadBlobRangeOptions(opts)
            , BlobRange(blobRange)
        {
            Y_VERIFY(blobRange.BlobId.IsValid());
        }

        bool PromoteInCache() const {
            return CacheAfterRead;
        }

        static NKikimrBlobStorage::EGetHandleClass ReadClass(EReadVariant readVar) {
            return (readVar == EReadVariant::FAST)
                ? NKikimrBlobStorage::FastRead
                : NKikimrBlobStorage::AsyncRead;
        }

        EReadVariant ReadVariant() const {
            return IsBackgroud
                ? (WithDeadline ? EReadVariant::DEFAULT : EReadVariant::DEFAULT_NO_DEADLINE)
                : EReadVariant::FAST;
        }

        // Blobs with same tagret can be read in a single request
        // (e.g. DS blobs from the same tablet residing on the same DS group, or 2 small blobs from the same tablet)
        std::tuple<ui64, ui32, EReadVariant> BlobSource() const {
            const TUnifiedBlobId& blobId = BlobRange.BlobId;

            Y_VERIFY(blobId.IsValid());

            if (blobId.IsDsBlob()) {
                // Tablet & group restriction
                return {blobId.GetTabletId(), blobId.GetDsGroup(), ReadVariant()};
            } else if (blobId.IsSmallBlob()) {
                // Tablet restriction, no group restrictions
                return {blobId.GetTabletId(), 0, ReadVariant()};
            }

            return {0, 0, EReadVariant::FAST};
        }
    };

    static constexpr i64 MAX_IN_FLIGHT_BYTES = 250ll << 20;
    static constexpr i64 MAX_IN_FLIGHT_FALLBACK_BYTES = 100ll << 20;
    static constexpr i64 MAX_REQUEST_BYTES = 8ll << 20;
    static constexpr TDuration DEFAULT_READ_DEADLINE = TDuration::Seconds(30);
    static constexpr TDuration FAST_READ_DEADLINE = TDuration::Seconds(10);

    TLRUCache<TBlobRange, TString> Cache;
    THashMap<TUnifiedBlobId, THashSet<TBlobRange>> CachedRanges;   // List of cached ranges by blob id
                                                            // It is used to remove all blob ranges from cache when
                                                            // it gets a notification that a blob has been deleted
    TControlWrapper MaxCacheDataSize;
    TControlWrapper MaxInFlightDataSize;
    TControlWrapper MaxFallbackDataSize; // It's expected to be less then MaxInFlightDataSize
    i64 CacheDataSize;              // Current size of all blobs in cache
    ui64 ReadCookie;
    THashMap<ui64, std::vector<TBlobRange>> CookieToRange;  // All in-flight requests
    THashMap<TBlobRange, TReadInfo> OutstandingReads;   // All in-flight and enqueued reads
    TDeque<TReadItem> ReadQueue;    // Reads that are waiting to be sent
                                    // TODO: Consider making per-group queues
    i64 InFlightDataSize;           // Current size of all in-flight blobs
    i64 FallbackDataSize;           // Current size of in-flight fallback blobs

    THashMap<ui64, TActorId> ShardPipes;    // TabletId -> PipeClient for small blob read requests
    THashMap<ui64, THashSet<ui64>> InFlightTabletRequests;  // TabletId -> list to read cookies

    using TCounterPtr = ::NMonitoring::TDynamicCounters::TCounterPtr;
    const TCounterPtr SizeBytes;
    const TCounterPtr SizeBlobs;
    const TCounterPtr Hits;
    const TCounterPtr Misses;
    const TCounterPtr Evictions;
    const TCounterPtr Adds;
    const TCounterPtr Forgets;
    const TCounterPtr HitsBytes;
    const TCounterPtr EvictedBytes;
    const TCounterPtr ReadBytes;
    const TCounterPtr AddBytes;
    const TCounterPtr ForgetBytes;
    const TCounterPtr SizeBytesInFlight;
    const TCounterPtr SizeBlobsInFlight;
    const TCounterPtr ReadRequests;
    const TCounterPtr ReadsInQueue;

public:
    static constexpr auto ActorActivityType() {
        return NKikimrServices::TActivity::BLOB_CACHE_ACTOR;
    }

public:
    explicit TBlobCache(ui64 maxSize, TIntrusivePtr<::NMonitoring::TDynamicCounters> counters)
        : TActorBootstrapped<TBlobCache>()
        , Cache(SIZE_MAX)
        , MaxCacheDataSize(maxSize, 0, 1ull << 40)
        , MaxInFlightDataSize(Min<i64>(MaxCacheDataSize, MAX_IN_FLIGHT_BYTES), 0, 10ull << 30)
        , MaxFallbackDataSize(Min<i64>(MaxCacheDataSize / 2, MAX_IN_FLIGHT_FALLBACK_BYTES), 0, 5ull << 30)
        , CacheDataSize(0)
        , ReadCookie(1)
        , InFlightDataSize(0)
        , FallbackDataSize(0)
        , SizeBytes(counters->GetCounter("SizeBytes"))
        , SizeBlobs(counters->GetCounter("SizeBlobs"))
        , Hits(counters->GetCounter("Hits", true))
        , Misses(counters->GetCounter("Misses", true))
        , Evictions(counters->GetCounter("Evictions", true))
        , Adds(counters->GetCounter("Adds", true))
        , Forgets(counters->GetCounter("Forgets", true))
        , HitsBytes(counters->GetCounter("HitsBytes", true))
        , EvictedBytes(counters->GetCounter("EvictedBytes", true))
        , ReadBytes(counters->GetCounter("ReadBytes", true))
        , AddBytes(counters->GetCounter("AddBytes", true))
        , ForgetBytes(counters->GetCounter("ForgetBytes", true))
        , SizeBytesInFlight(counters->GetCounter("SizeBytesInFlight"))
        , SizeBlobsInFlight(counters->GetCounter("SizeBlobsInFlight"))
        , ReadRequests(counters->GetCounter("ReadRequests", true))
        , ReadsInQueue(counters->GetCounter("ReadsInQueue"))
    {}

    void Bootstrap(const TActorContext& ctx) {
        auto& icb = AppData(ctx)->Icb;
        icb->RegisterSharedControl(MaxCacheDataSize, "BlobCache.MaxCacheDataSize");
        icb->RegisterSharedControl(MaxInFlightDataSize, "BlobCache.MaxInFlightDataSize");
        icb->RegisterSharedControl(MaxFallbackDataSize, "BlobCache.MaxFallbackDataSize");

        LOG_S_NOTICE("MaxCacheDataSize: " << (i64)MaxCacheDataSize
            << " MaxFallbackDataSize: " << (i64)MaxFallbackDataSize
            << " InFlightDataSize: " << (i64)InFlightDataSize);

        Become(&TBlobCache::StateFunc);
        ScheduleWakeup();
    }

private:
    STFUNC(StateFunc) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvents::TEvPoisonPill, Handle);
            HFunc(TEvents::TEvWakeup, Handle);
            HFunc(TEvBlobCache::TEvReadBlobRange, Handle);
            HFunc(TEvBlobCache::TEvReadBlobRangeBatch, Handle);
            HFunc(TEvBlobCache::TEvCacheBlobRange, Handle);
            HFunc(TEvBlobCache::TEvForgetBlob, Handle);
            HFunc(TEvBlobStorage::TEvGetResult, Handle);
            HFunc(TEvTabletPipe::TEvClientConnected, Handle);
            HFunc(TEvTabletPipe::TEvClientDestroyed, Handle);
            HFunc(TEvColumnShard::TEvReadBlobRangesResult, Handle);
        default:
            LOG_S_WARN("Unhandled event type: " << ev->GetTypeRewrite()
                       << " event: " << ev->ToString());
            Send(IEventHandle::ForwardOnNondelivery(ev, TEvents::TEvUndelivered::ReasonActorUnknown));
            break;
        };
    }

    void ScheduleWakeup() {
        Schedule(TDuration::MilliSeconds(100), new TEvents::TEvWakeup());
    }

    void Handle(TEvents::TEvWakeup::TPtr& ev, const TActorContext& ctx) {
        Y_UNUSED(ev);
        Evict(ctx);         // Max cache size might have changed
        ScheduleWakeup();
    }

    void Handle(TEvents::TEvPoisonPill::TPtr& ev, const TActorContext& ctx) {
        Y_UNUSED(ev);
        Die(ctx);
    }

    void Handle(TEvBlobCache::TEvReadBlobRange::TPtr& ev, const TActorContext& ctx) {
        const TBlobRange& blobRange = ev->Get()->BlobRange;
        const bool promote = (i64)MaxCacheDataSize && ev->Get()->ReadOptions.CacheAfterRead;
        const bool fallback = ev->Get()->ReadOptions.ForceFallback;

        LOG_S_DEBUG("Read request: " << blobRange << " cache: " << (ui32)promote << " fallback: " << (ui32)fallback << " sender:" << ev->Sender);

        TReadItem readItem(ev->Get()->ReadOptions, blobRange);
        HandleSingleRangeRead(std::move(readItem), ev->Sender, ctx);

        MakeReadRequests(ctx);
    }

    void HandleSingleRangeRead(TReadItem&& readItem, const TActorId& sender, const TActorContext& ctx) {
        const TBlobRange& blobRange = readItem.BlobRange;

        // Is in cache?
        auto it = readItem.PromoteInCache() ? Cache.Find(blobRange) : Cache.FindWithoutPromote(blobRange);
        if (it != Cache.End()) {
            Hits->Inc();
            HitsBytes->Add(blobRange.Size);
            return SendResult(sender, blobRange, NKikimrProto::OK, it.Value(), ctx, true);
        }

        LOG_S_DEBUG("Miss cache: " << blobRange << " sender:" << sender);
        Misses->Inc();

        // Prevent full cache flushing by exported blobs. Decrease propability of caching depending on cache size.
        // TODO: better cache strategy
        if (readItem.ForceFallback && readItem.CacheAfterRead) {
            if (CacheDataSize > (MaxCacheDataSize / 4) * 3) {
                readItem.CacheAfterRead = !(ReadCookie % 256);
            } else if (CacheDataSize > (MaxCacheDataSize / 2)) {
                readItem.CacheAfterRead = !(ReadCookie % 32);
            }
        }

        // Is outstanding?
        auto readIt = OutstandingReads.find(blobRange);
        if (readIt != OutstandingReads.end()) {
            readIt->second.Waiting.push_back(sender);
            readIt->second.Cache |= readItem.PromoteInCache();
            return;
        }

        EnqueueRead(std::move(readItem), sender);
    }

    void Handle(TEvBlobCache::TEvReadBlobRangeBatch::TPtr& ev, const TActorContext& ctx) {
        const auto& ranges = ev->Get()->BlobRanges;
        LOG_S_DEBUG("Batch read request: " << JoinStrings(ranges.begin(), ranges.end(), " "));

        auto& readOptions = ev->Get()->ReadOptions;
        readOptions.CacheAfterRead = (i64)MaxCacheDataSize && readOptions.CacheAfterRead;

        for (const auto& blobRange : ranges) {
            TReadItem readItem(readOptions, blobRange);
            HandleSingleRangeRead(std::move(readItem), ev->Sender, ctx);
        }

        MakeReadRequests(ctx);
    }

    void Handle(TEvBlobCache::TEvCacheBlobRange::TPtr& ev, const TActorContext& ctx) {
        const auto& blobRange = ev->Get()->BlobRange;
        const auto& data = ev->Get()->Data;

        if (blobRange.Size != data.size()) {
            LOG_S_ERROR("Trying to add invalid data for range: " << blobRange << " size: " << data.size());
            return;
        }

        Adds->Inc();

        if (OutstandingReads.contains(blobRange)) {
            // Don't bother if there is already a read request for this range
            return;
        }

        LOG_S_DEBUG("Adding range: " << blobRange);

        AddBytes->Add(blobRange.Size);

        InsertIntoCache(blobRange, data);

        Evict(ctx);
    }

    void Handle(TEvBlobCache::TEvForgetBlob::TPtr& ev, const TActorContext&) {
        const TUnifiedBlobId& blobId = ev->Get()->BlobId;

        LOG_S_INFO("Forgetting blob: " << blobId);

        Forgets->Inc();

        auto blobIdIt = CachedRanges.find(blobId);
        if (blobIdIt == CachedRanges.end()) {
            return;
        }

        // Remove all ranges of this blob that are present in cache
        for (const auto& blobRange: blobIdIt->second) {
            auto rangeIt = Cache.FindWithoutPromote(blobRange);
            if (rangeIt == Cache.End()) {
                continue;
            }

            Cache.Erase(rangeIt);
            CacheDataSize -= blobRange.Size;
            SizeBytes->Sub(blobRange.Size);
            SizeBlobs->Dec();
            ForgetBytes->Add(blobRange.Size);
        }

        CachedRanges.erase(blobIdIt);
    }

    void EnqueueRead(TReadItem&& readItem, const TActorId& sender) {
        const auto& blobRange = readItem.BlobRange;
        TReadInfo& blobInfo = OutstandingReads[blobRange];
        blobInfo.Waiting.push_back(sender);
        blobInfo.Cache = readItem.PromoteInCache();

        LOG_S_DEBUG("Enqueue read range: " << blobRange);

        ReadQueue.emplace_back(std::move(readItem));
        ReadsInQueue->Set(ReadQueue.size());
    }

    void SendBatchReadRequestToDS(const std::vector<TBlobRange>& blobRanges, const ui64 cookie,
        ui32 dsGroup, TReadItem::EReadVariant readVariant, const TActorContext& ctx)
    {
        LOG_S_DEBUG("Sending read from DS: group: " << dsGroup
            << " ranges: " << JoinStrings(blobRanges.begin(), blobRanges.end(), " ")
            << " cookie: " << cookie);

        TArrayHolder<TEvBlobStorage::TEvGet::TQuery> queires(new TEvBlobStorage::TEvGet::TQuery[blobRanges.size()]);
        for (size_t i = 0; i < blobRanges.size(); ++i) {
            Y_VERIFY(dsGroup == blobRanges[i].BlobId.GetDsGroup());
            queires[i].Set(blobRanges[i].BlobId.GetLogoBlobId(), blobRanges[i].Offset, blobRanges[i].Size);
        }

        NKikimrBlobStorage::EGetHandleClass readClass = TReadItem::ReadClass(readVariant);
        TInstant deadline = ReadDeadline(readVariant);
        SendToBSProxy(ctx,
                dsGroup,
                new TEvBlobStorage::TEvGet(queires, blobRanges.size(), deadline, readClass, false),
                cookie);

        ReadRequests->Inc();
    }

    static TInstant ReadDeadline(TReadItem::EReadVariant variant) {
        if (variant == TReadItem::EReadVariant::FAST) {
            return TAppData::TimeProvider->Now() + FAST_READ_DEADLINE;
        } else if (variant == TReadItem::EReadVariant::DEFAULT) {
            return TAppData::TimeProvider->Now() + DEFAULT_READ_DEADLINE;
        }
        return TInstant::Max(); // EReadVariant::DEFAULT_NO_DEADLINE
    }

    void MakeReadRequests(const TActorContext& ctx, THashMap<TUnifiedBlobId, std::vector<TBlobRange>>&& fallbackRanges = {}) {
        THashMap<std::tuple<ui64, ui32, TReadItem::EReadVariant>, std::vector<TBlobRange>> groupedBlobRanges;

        while (!ReadQueue.empty()) {
            const auto& readItem = ReadQueue.front();
            const TBlobRange& blobRange = readItem.BlobRange;

            // NOTE: if queue is not empty, at least 1 in-flight request is allowed
            if (InFlightDataSize && InFlightDataSize >= MaxInFlightDataSize) {
                break;
            }
            InFlightDataSize += blobRange.Size;
            SizeBytesInFlight->Add(blobRange.Size);
            SizeBlobsInFlight->Inc();

            if (readItem.ForceFallback) {
                Y_VERIFY(blobRange.BlobId.IsDsBlob());

                if (FallbackDataSize && FallbackDataSize >= MaxFallbackDataSize) {
                    // 1. Do not block DS reads by fallbacks (fallback reads form S3 could be much slower then DS ones)
                    // 2. Limit max fallback data in flight
                    // Requires MaxFallbackDataSize < MaxInFlightDataSize
                    ReadQueue.push_back(readItem);
                } else {
                    // Tablet cannot read different blobs in fallback now. Group reads by blobId.
                    fallbackRanges[blobRange.BlobId].push_back(blobRange);
                    FallbackDataSize += blobRange.Size;
                }
            } else {
                auto blobSrc = readItem.BlobSource();
                groupedBlobRanges[blobSrc].push_back(blobRange);
            }

            ReadQueue.pop_front();
        }

        ReadsInQueue->Set(ReadQueue.size());

        // We might need to free some space to accommodate the results of new reads
        Evict(ctx);

        std::vector<ui64> tabletReads;
        tabletReads.reserve(groupedBlobRanges.size() + fallbackRanges.size());

        for (auto& [blobId, ranges] : fallbackRanges) {
            Y_VERIFY(blobId.IsDsBlob());

            ui64 cookie = ++ReadCookie;
            CookieToRange[cookie] = std::move(ranges);
            tabletReads.push_back(cookie);
        }

        ui64 cookie = ++ReadCookie;

        // TODO: fix small blobs mix with dsGroup == 0 (it could be zero in tests)
        for (auto& [target, rangesGroup] : groupedBlobRanges) {
            ui64 requestSize = 0;
            ui32 dsGroup = std::get<1>(target);
            TReadItem::EReadVariant readVariant = std::get<2>(target);
            bool isDS = rangesGroup.begin()->BlobId.IsDsBlob();

            std::vector<ui64> dsReads;

            for (auto& blobRange : rangesGroup) {
                if (requestSize && (requestSize + blobRange.Size > MAX_REQUEST_BYTES)) {
                    if (isDS) {
                        dsReads.push_back(cookie);
                    } else {
                        tabletReads.push_back(cookie);
                    }
                    cookie = ++ReadCookie;
                    requestSize = 0;
                }

                requestSize += blobRange.Size;
                CookieToRange[cookie].emplace_back(std::move(blobRange));
            }
            if (requestSize) {
                if (isDS) {
                    dsReads.push_back(cookie);
                } else {
                    tabletReads.push_back(cookie);
                }
                cookie = ++ReadCookie;
                requestSize = 0;
            }

            for (ui64 cookie : dsReads) {
                SendBatchReadRequestToDS(CookieToRange[cookie], cookie, dsGroup, readVariant, ctx);
            }
        }

        for (ui64 cookie : tabletReads) {
            SendBatchReadRequestToTablet(CookieToRange[cookie], cookie, ctx);
        }
    }

    void SendResult(const TActorId& to, const TBlobRange& blobRange, NKikimrProto::EReplyStatus status,
                    const TString& data, const TActorContext& ctx, const bool fromCache = false) {
        LOG_S_DEBUG("Send result: " << blobRange << " to: " << to << " status: " << status);

        ctx.Send(to, new TEvBlobCache::TEvReadBlobRangeResult(blobRange, status, data, fromCache));
    }

    void Handle(TEvBlobStorage::TEvGetResult::TPtr& ev, const TActorContext& ctx) {
        const ui64 readCookie = ev->Cookie;

        if (ev->Get()->ResponseSz < 1) {
            Y_FAIL("Unexpected reply from blobstorage");
        }

        if (ev->Get()->Status != NKikimrProto::EReplyStatus::OK) {
            LOG_S_WARN("Read failed: " << ev->Get()->ToString());
        }

        auto cookieIt = CookieToRange.find(readCookie);
        if (cookieIt == CookieToRange.end()) {
            // This shouldn't happen
            LOG_S_CRIT("Unknown read result cookie: " << readCookie);
            return;
        }

        std::vector<TBlobRange> blobRanges = std::move(cookieIt->second);
        CookieToRange.erase(readCookie);

        Y_VERIFY(blobRanges.size() == ev->Get()->ResponseSz, "Mismatched number of results for read request!");

        // We could find blob ranges evicted (NODATA). Try to fallback them to tablet.
        THashMap<TUnifiedBlobId, std::vector<TBlobRange>> fallbackRanges;
        for (size_t i = 0; i < ev->Get()->ResponseSz; ++i) {
            const auto& res = ev->Get()->Responses[i];
            if (res.Status == NKikimrProto::EReplyStatus::NODATA) {
                fallbackRanges[blobRanges[i].BlobId].emplace_back(std::move(blobRanges[i]));
            } else {
                ProcessSingleRangeResult(blobRanges[i], readCookie, res.Status, res.Buffer, ctx);
            }
        }

        MakeReadRequests(ctx, std::move(fallbackRanges));
    }

    void ProcessSingleRangeResult(const TBlobRange& blobRange, const ui64 readCookie,
        ui32 status, const TString& data, const TActorContext& ctx)
    {
        auto readIt = OutstandingReads.find(blobRange);
        if (readIt == OutstandingReads.end()) {
            // This shouldn't happen
            LOG_S_CRIT("Unknown read result key: " << blobRange << " cookie: " << readCookie);
            return;
        }

        SizeBytesInFlight->Sub(blobRange.Size);
        SizeBlobsInFlight->Dec();
        InFlightDataSize -= blobRange.Size;

        Y_VERIFY(Cache.Find(blobRange) == Cache.End(),
            "Range %s must not be already in cache", blobRange.ToString().c_str());

        if (status == NKikimrProto::EReplyStatus::OK) {
            Y_VERIFY(blobRange.Size == data.size(),
                "Read %s, size %" PRISZT, blobRange.ToString().c_str(), data.size());
            ReadBytes->Add(blobRange.Size);

            if (readIt->second.Cache) {
                InsertIntoCache(blobRange, data);
            }
        } else {
            LOG_S_WARN("Read failed for range: " << blobRange
                << " status: " << NKikimrProto::EReplyStatus_Name(status));
        }

        // Send results to all waiters
        for (const auto& to : readIt->second.Waiting) {
            SendResult(to, blobRange, (NKikimrProto::EReplyStatus)status, data, ctx);
        }

        OutstandingReads.erase(readIt);
    }

    void SendBatchReadRequestToTablet(const std::vector<TBlobRange>& blobRanges,
        const ui64 cookie, const TActorContext& ctx)
    {
        Y_VERIFY(!blobRanges.empty());
        ui64 tabletId = blobRanges.front().BlobId.GetTabletId();

        LOG_S_INFO("Sending read from Tablet: " << tabletId
            << " ranges: " << JoinStrings(blobRanges.begin(), blobRanges.end(), " ")
            << " cookie: " << cookie);

        if (!ShardPipes.contains(tabletId)) {
            NTabletPipe::TClientConfig clientConfig;
            clientConfig.AllowFollower = false;
            clientConfig.CheckAliveness = true;
            clientConfig.RetryPolicy = {
                .RetryLimitCount = 10,
                .MinRetryTime = TDuration::MilliSeconds(5),
            };
            ShardPipes[tabletId] = ctx.Register(NTabletPipe::CreateClient(ctx.SelfID, tabletId, clientConfig));
        }

        auto ev = std::make_unique<TEvColumnShard::TEvReadBlobRanges>(blobRanges);

        InFlightTabletRequests[tabletId].insert(cookie);
        NTabletPipe::SendData(ctx, ShardPipes[tabletId], ev.release(), cookie);

        ReadRequests->Inc();
    }

    // Frogets the pipe to the tablet and fails all in-flight requests to it
    void DestroyPipe(ui64 tabletId, const TActorContext& ctx) {
        ShardPipes.erase(tabletId);
        // Send errors for in-flight requests
        auto cookies = std::move(InFlightTabletRequests[tabletId]);
        InFlightTabletRequests.erase(tabletId);
        for (ui64 readCookie : cookies) {
            auto cookieIt = CookieToRange.find(readCookie);
            if (cookieIt == CookieToRange.end()) {
                // This might only happen in case fo race between response and pipe close
                LOG_S_NOTICE("Unknown read result cookie: " << readCookie);
                return;
            }

            std::vector<TBlobRange> blobRanges = std::move(cookieIt->second);
            CookieToRange.erase(readCookie);

            for (size_t i = 0; i < blobRanges.size(); ++i) {
                Y_VERIFY(blobRanges[i].BlobId.GetTabletId() == tabletId);
                ProcessSingleRangeResult(blobRanges[i], readCookie, NKikimrProto::EReplyStatus::NOTREADY, {}, ctx);
            }
        }

        MakeReadRequests(ctx);
    }

    void Handle(TEvTabletPipe::TEvClientConnected::TPtr& ev, const TActorContext& ctx) {
        TEvTabletPipe::TEvClientConnected* msg = ev->Get();
        const ui64 tabletId = msg->TabletId;
        Y_VERIFY(tabletId != 0);
        if (msg->Status == NKikimrProto::OK) {
            LOG_S_DEBUG("Pipe connected to tablet: " << tabletId);
        } else {
            LOG_S_DEBUG("Pipe connection to tablet: " << tabletId << " failed with status: " << msg->Status);
            DestroyPipe(tabletId, ctx);
        }
    }

    void Handle(TEvTabletPipe::TEvClientDestroyed::TPtr& ev, const TActorContext& ctx) {
        const ui64 tabletId = ev->Get()->TabletId;
        Y_VERIFY(tabletId != 0);

        LOG_S_DEBUG("Closed pipe connection to tablet: " << tabletId);
        DestroyPipe(tabletId, ctx);
    }

    void Handle(TEvColumnShard::TEvReadBlobRangesResult::TPtr& ev, const TActorContext& ctx) {
        const auto& record = ev->Get()->Record;
        ui64 tabletId = record.GetTabletId();
        ui64 readCookie = ev->Cookie;
        LOG_S_INFO("Got read result from tablet: " << tabletId);

        auto cookieIt = CookieToRange.find(readCookie);
        if (cookieIt == CookieToRange.end()) {
            // This might only happen in case fo race between response and pipe close
            LOG_S_NOTICE("Unknown read result cookie: " << readCookie);
            return;
        }

        std::vector<TBlobRange> blobRanges = std::move(cookieIt->second);

        Y_VERIFY(record.ResultsSize(), "Zero results for read request!");
        Y_VERIFY(blobRanges.size() >= record.ResultsSize(), "Mismatched number of results for read request");

        if (blobRanges.size() == record.ResultsSize()) {
            InFlightTabletRequests[tabletId].erase(readCookie);
            CookieToRange.erase(readCookie);
        } else {
            // Extract blobRanges for returned blobId. Keep others ordered.
            TString strReturnedBlobId = record.GetResults(0).GetBlobRange().GetBlobId();
            std::vector<TBlobRange> same;
            std::vector<TBlobRange> others;
            same.reserve(record.ResultsSize());
            others.reserve(blobRanges.size() - record.ResultsSize());

            for (auto&& blobRange : blobRanges) {
                TString strBlobId = blobRange.BlobId.ToStringNew();
                if (strBlobId == strReturnedBlobId) {
                    same.emplace_back(std::move(blobRange));
                } else {
                    others.emplace_back(std::move(blobRange));
                }
            }
            blobRanges.swap(same);

            CookieToRange[readCookie] = std::move(others);
        }

        for (size_t i = 0; i < record.ResultsSize(); ++i) {
            const auto& res = record.GetResults(i);
            const auto& blobRange = blobRanges[i];
            if (!blobRange.BlobId.IsSmallBlob()) {
                FallbackDataSize -= blobRange.Size;
            }

            Y_VERIFY(blobRange.BlobId.ToStringNew() == res.GetBlobRange().GetBlobId());
            Y_VERIFY(blobRange.Offset == res.GetBlobRange().GetOffset());
            Y_VERIFY(blobRange.Size == res.GetBlobRange().GetSize());
            ProcessSingleRangeResult(blobRange, readCookie, res.GetStatus(), res.GetData(), ctx);
        }

        MakeReadRequests(ctx);
    }

    void InsertIntoCache(const TBlobRange& blobRange, TString data) {
        CacheDataSize += blobRange.Size;
        SizeBytes->Add(blobRange.Size);
        SizeBlobs->Inc();

        // Shrink the buffer if it has to much extra capacity
        if (data.capacity() > data.size() * 1.1) {
            data = TString(data.begin(), data.end());
        }

        Cache.Insert(blobRange, data);
        CachedRanges[blobRange.BlobId].insert(blobRange);
    }

    void Evict(const TActorContext&) {
        while (CacheDataSize + InFlightDataSize > MaxCacheDataSize) {
            auto it = Cache.FindOldest();
            if (it == Cache.End()) {
                break;
            }

            LOG_S_DEBUG("Evict: " << it.Key()
                << " CacheDataSize: " << CacheDataSize
                << " InFlightDataSize: " << (i64)InFlightDataSize
                << " MaxCacheDataSize: " << (i64)MaxCacheDataSize
                << " MaxFallbackDataSize: " << (i64)MaxFallbackDataSize);

            {
                // Remove the range from list of ranges by blob id
                auto blobIdIt = CachedRanges.find(it.Key().BlobId);
                if (blobIdIt != CachedRanges.end()) {
                    blobIdIt->second.erase(it.Key());
                    if (blobIdIt->second.empty()) {
                        CachedRanges.erase(blobIdIt);
                    }
                }
            }

            Evictions->Inc();
            EvictedBytes->Add(it.Key().Size);

            CacheDataSize -= it.Key().Size;
            Cache.Erase(it);

            SizeBytes->Set(CacheDataSize);
            SizeBlobs->Set(Cache.Size());
        }
    }
};

} // namespace

NActors::IActor* CreateBlobCache(ui64 maxBytes, TIntrusivePtr<::NMonitoring::TDynamicCounters> counters) {
    return new TBlobCache(maxBytes, counters);
}

void AddRangeToCache(const TBlobRange& blobRange, const TString& data) {
    TlsActivationContext->Send(
        new IEventHandle(MakeBlobCacheServiceId(), TActorId(), new TEvBlobCache::TEvCacheBlobRange(blobRange, data)));
}

void ForgetBlob(const TUnifiedBlobId& blobId) {
    TlsActivationContext->Send(
        new IEventHandle(MakeBlobCacheServiceId(), TActorId(), new TEvBlobCache::TEvForgetBlob(blobId)));
}

}
