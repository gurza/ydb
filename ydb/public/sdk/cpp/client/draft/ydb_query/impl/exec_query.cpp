#define INCLUDE_YDB_INTERNAL_H
#include "exec_query.h"

#include <ydb/public/sdk/cpp/client/impl/ydb_internal/make_request/make.h>
#include <ydb/public/sdk/cpp/client/ydb_common_client/impl/client.h>
#undef INCLUDE_YDB_INTERNAL_H

#include <ydb/public/sdk/cpp/client/ydb_proto/accessor.h>

namespace NYdb::NQuery {

using namespace NThreading;

class TExecuteQueryIterator::TReaderImpl {
public:
    using TSelf = TExecuteQueryIterator::TReaderImpl;
    using TResponse = Ydb::Query::ExecuteQueryResponsePart;
    using TStreamProcessorPtr = NGrpc::IStreamRequestReadProcessor<TResponse>::TPtr;
    using TReadCallback = NGrpc::IStreamRequestReadProcessor<TResponse>::TReadCallback;
    using TGRpcStatus = NGrpc::TGrpcStatus;
    using TBatchReadResult = std::pair<TResponse, TGRpcStatus>;

    TReaderImpl(TStreamProcessorPtr streamProcessor, const TString& endpoint)
        : StreamProcessor_(streamProcessor)
        , Finished_(false)
        , Endpoint_(endpoint)
    {}

    ~TReaderImpl() {
        StreamProcessor_->Cancel();
    }

    bool IsFinished() const {
        return Finished_;
    }

    TAsyncExecuteQueryPart ReadNext(std::shared_ptr<TSelf> self) {
        auto promise = NThreading::NewPromise<TExecuteQueryPart>();
        // Capture self - guarantee no dtor call during the read
        auto readCb = [self, promise](TGRpcStatus&& grpcStatus) mutable {
            if (!grpcStatus.Ok()) {
                self->Finished_ = true;
                promise.SetValue({TStatus(TPlainStatus(grpcStatus, self->Endpoint_))});
            } else {
                NYql::TIssues issues;
                NYql::IssuesFromMessage(self->Response_.issues(), issues);
                EStatus clientStatus = static_cast<EStatus>(self->Response_.status());
                // TODO: Add headers for streaming calls.
                TPlainStatus plainStatus{clientStatus, std::move(issues), self->Endpoint_, {}};
                TStatus status{std::move(plainStatus)};

                if (self->Response_.has_result_set()) {
                    promise.SetValue({
                        std::move(status),
                        TResultSet(std::move(*self->Response_.mutable_result_set())),
                        self->Response_.result_set_index()
                    });
                } else {
                    promise.SetValue({std::move(status)});
                }
            }
        };

        StreamProcessor_->Read(&Response_, readCb);
        return promise.GetFuture();
    }
private:
    TStreamProcessorPtr StreamProcessor_;
    TResponse Response_;
    bool Finished_;
    TString Endpoint_;
};

TAsyncExecuteQueryPart TExecuteQueryIterator::ReadNext() {
    if (ReaderImpl_->IsFinished()) {
        RaiseError("Attempt to perform read on invalid or finished stream");
    }

    return ReaderImpl_->ReadNext(ReaderImpl_);
}

using TExecuteQueryProcessorPtr = TExecuteQueryIterator::TReaderImpl::TStreamProcessorPtr;

struct TExecuteQueryBuffer : public TThrRefBase, TNonCopyable {
    using TPtr = TIntrusivePtr<TExecuteQueryBuffer>;

    TExecuteQueryBuffer(TExecuteQueryIterator&& iterator)
        : Promise_(NewPromise<TExecuteQueryResult>())
        , Iterator_(std::move(iterator)) {}

    TPromise<TExecuteQueryResult> Promise_;
    TExecuteQueryIterator Iterator_;
    TVector<NYql::TIssue> Issues_;
    TVector<Ydb::ResultSet> ResultSets_;

    void Next() {
        TPtr self(this);

        Iterator_.ReadNext().Subscribe([self](TAsyncExecuteQueryPart partFuture) mutable {
            auto part = partFuture.ExtractValue();

            if (!part.IsSuccess()) {
                if (part.EOS()) {
                    TVector<NYql::TIssue> issues;
                    TVector<Ydb::ResultSet> resultProtos;

                    std::swap(self->Issues_, issues);
                    std::swap(self->ResultSets_, resultProtos);

                    TVector<TResultSet> resultSets;
                    for (auto& proto : resultProtos) {
                        resultSets.emplace_back(std::move(proto));
                    }

                    self->Promise_.SetValue(TExecuteQueryResult(
                        TStatus(EStatus::SUCCESS, NYql::TIssues(std::move(issues))),
                        std::move(resultSets)
                    ));
                } else {
                    self->Promise_.SetValue(TExecuteQueryResult(std::move(part), {}));
                }

                return;
            }

            if (part.HasResultSet()) {
                auto inRs = part.ExtractResultSet();
                auto& inRsProto = TProtoAccessor::GetProto(inRs);

                // TODO: Use result sets metadata
                if (self->ResultSets_.size() <= part.GetResultSetIndex()) {
                    self->ResultSets_.resize(part.GetResultSetIndex() + 1);
                }

                auto& resultSet = self->ResultSets_[part.GetResultSetIndex()];
                if (resultSet.columns().empty()) {
                    resultSet.mutable_columns()->CopyFrom(inRsProto.columns());
                }

                resultSet.mutable_rows()->Add(inRsProto.rows().begin(), inRsProto.rows().end());
            }

            self->Next();
        });
    }
};

TFuture<std::pair<TPlainStatus, TExecuteQueryProcessorPtr>> StreamExecuteQueryImpl(
    const std::shared_ptr<TGRpcConnectionsImpl>& connections, const TDbDriverStatePtr& driverState,
    const TString& query, const TExecuteQuerySettings& settings)
{
    auto request = MakeRequest<Ydb::Query::ExecuteQueryRequest>();
    request.set_exec_mode(Ydb::Query::EXEC_MODE_EXECUTE);
    request.mutable_query_content()->set_text(query);

    auto promise = NewPromise<std::pair<TPlainStatus, TExecuteQueryProcessorPtr>>();

    connections->StartReadStream<
        Ydb::Query::V1::QueryService,
        Ydb::Query::ExecuteQueryRequest,
        Ydb::Query::ExecuteQueryResponsePart>
    (
        std::move(request),
        [promise] (TPlainStatus status, TExecuteQueryProcessorPtr processor) mutable {
            promise.SetValue(std::make_pair(status, processor));
        },
        &Ydb::Query::V1::QueryService::Stub::AsyncExecuteQuery,
        driverState,
        TRpcRequestSettings::Make(settings)
    );

    return promise.GetFuture();
}

TAsyncExecuteQueryIterator TExecQueryImpl::StreamExecuteQuery(const std::shared_ptr<TGRpcConnectionsImpl>& connections,
    const TDbDriverStatePtr& driverState, const TString& query, const TExecuteQuerySettings& settings)
{
    auto promise = NewPromise<TExecuteQueryIterator>();

    auto iteratorCallback = [promise](TFuture<std::pair<TPlainStatus, TExecuteQueryProcessorPtr>> future) mutable {
        Y_ASSERT(future.HasValue());
        auto pair = future.ExtractValue();
        promise.SetValue(TExecuteQueryIterator(
            pair.second
                ? std::make_shared<TExecuteQueryIterator::TReaderImpl>(pair.second, pair.first.Endpoint)
                : nullptr,
            std::move(pair.first))
        );
    };

    StreamExecuteQueryImpl(connections, driverState, query, settings).Subscribe(iteratorCallback);
    return promise.GetFuture();
}

TAsyncExecuteQueryResult TExecQueryImpl::ExecuteQuery(const std::shared_ptr<TGRpcConnectionsImpl>& connections,
    const TDbDriverStatePtr& driverState, const TString& query, const TExecuteQuerySettings& settings)
{
    return StreamExecuteQuery(connections, driverState, query, settings)
        .Apply([](TAsyncExecuteQueryIterator itFuture){
            auto it = itFuture.ExtractValue();

            if (!it.IsSuccess()) {
                return MakeFuture<TExecuteQueryResult>(std::move(it));
            }

            auto buffer = MakeIntrusive<TExecuteQueryBuffer>(std::move(it));
            buffer->Next();

            return buffer->Promise_.GetFuture();
        });
}

} // namespace NYdb::NQuery
