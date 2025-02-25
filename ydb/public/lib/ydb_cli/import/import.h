#pragma once

#include <ydb/public/lib/ydb_cli/common/command.h>
#include <ydb/public/lib/ydb_cli/common/formats.h>
#include <ydb/public/sdk/cpp/client/ydb_types/status/status.h>
#include <ydb/public/sdk/cpp/client/ydb_types/fluent_settings_helpers.h>
#include <ydb/public/sdk/cpp/client/ydb_table/table.h>

#include <util/generic/size_literals.h>

namespace NYdb {

class TDriver;

namespace NOperation {
class TOperationClient;
}
namespace NScheme {
class TSchemeClient;
}
namespace NTable {
class TTableClient;
}
namespace NImport {
class TImportClient;
}

namespace NConsoleClient {

struct TImportFileSettings : public TOperationRequestSettings<TImportFileSettings> {
    using TSelf = TImportFileSettings;

    static constexpr ui64 MaxBytesPerRequest = 8_MB;
    static constexpr const char * DefaultDelimiter = ",";
    static constexpr ui32 OperationTimeoutSec = 5 * 60;
    static constexpr ui32 ClientTimeoutSec = OperationTimeoutSec + 5;
    static constexpr ui32 MaxRetries = 10000;

    // Allowed values: Csv, Tsv, JsonUnicode, JsonBase64. Default means Csv
    FLUENT_SETTING_DEFAULT(EOutputFormat, Format, EOutputFormat::Default);
    FLUENT_SETTING_DEFAULT(ui64, BytesPerRequest, 1_MB);
    FLUENT_SETTING_DEFAULT(ui64, FileBufferSize, 2_MB);
    FLUENT_SETTING_DEFAULT(ui64, MaxInFlightRequests, 100);
    // Settings below are for CSV format only
    FLUENT_SETTING_DEFAULT(ui32, SkipRows, 0);
    FLUENT_SETTING_DEFAULT(bool, Header, false);
    FLUENT_SETTING_DEFAULT(bool, NewlineDelimited, false);
    FLUENT_SETTING_DEFAULT(TString, HeaderRow, "");
    FLUENT_SETTING_DEFAULT(TString, Delimiter, DefaultDelimiter);
    FLUENT_SETTING_DEFAULT(TString, NullValue, "");
};

class TImportFileClient {
public:
    explicit TImportFileClient(const TDriver& driver, const TClientCommand::TConfig& rootConfig);
    TImportFileClient(const TImportFileClient&) = delete;

    // Ingest data from the input files to the database table.
    //   fsPaths: vector of paths to input files
    //   dbPath: full path to the database table, including the database path
    //   settings: input data format and operational settings
    TStatus Import(const TVector<TString>& fsPaths, const TString& dbPath, const TImportFileSettings& settings = {});

private:
    std::shared_ptr<NOperation::TOperationClient> OperationClient;
    std::shared_ptr<NScheme::TSchemeClient> SchemeClient;
    std::shared_ptr<NTable::TTableClient> TableClient;

    NTable::TBulkUpsertSettings UpsertSettings;
    NTable::TRetryOperationSettings RetrySettings;

    std::atomic<ui64> FilesCount;

    static constexpr ui32 VerboseModeReadSize = 1 << 27; // 100 MB

    void SetupUpsertSettingsCsv(const TImportFileSettings& settings);
    TStatus UpsertCsv(IInputStream& input, const TString& dbPath, const TImportFileSettings& settings);
    TStatus UpsertCsvByBlocks(const TString& filePath, const TString& dbPath, const TImportFileSettings& settings);
    TAsyncStatus UpsertCsvBuffer(const TString& dbPath, const TString& buffer);

    TStatus UpsertJson(IInputStream& input, const TString& dbPath, const TImportFileSettings& settings);
    TAsyncStatus UpsertJsonBuffer(const TString& dbPath, TValueBuilder& builder);
    TType GetTableType(const NTable::TTableDescription& tableDescription);

    TStatus UpsertParquet(const TString& filename, const TString& dbPath, const TImportFileSettings& settings);
    TAsyncStatus UpsertParquetBuffer(const TString& dbPath, const TString& buffer, const TString& strSchema);
};

}
}
