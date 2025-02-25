#include "interactive.h"
#include "recursive_remove.h"

#include <ydb/public/lib/ydb_cli/common/recursive_list.h>
#include <ydb/public/lib/ydb_cli/common/retry_func.h>

#include <util/string/builder.h>
#include <util/system/type_name.h>

namespace NYdb::NConsoleClient {

using namespace NScheme;
using namespace NTable;
using namespace NTopic;

TStatus RemoveDirectory(TSchemeClient& client, const TString& path, const TRemoveDirectorySettings& settings) {
    return RetryFunction([&]() -> TStatus {
        return client.RemoveDirectory(path, settings).ExtractValueSync();
    });
}

TStatus RemoveTable(TTableClient& client, const TString& path, const TDropTableSettings& settings) {
    return client.RetryOperationSync([path, settings](TSession session) {
        return session.DropTable(path, settings).ExtractValueSync();
    });
}

TStatus RemoveTopic(TTopicClient& client, const TString& path, const TDropTopicSettings& settings) {
    return RetryFunction([&]() -> TStatus {
        return client.DropTopic(path, settings).ExtractValueSync();
    });
}

NYql::TIssues MakeIssues(const TString& error) {
    NYql::TIssues issues;
    issues.AddIssue(NYql::TIssue(error));
    return issues;
}

bool Prompt(const TString& path, ESchemeEntryType type) {
    Cout << "Remove " << to_lower(ToString(type)) << " '" << path << "' (y/n)? ";
    return AskYesOrNo();
}

bool Prompt(ERecursiveRemovePrompt mode, const TString& path, NScheme::ESchemeEntryType type, bool first) {
    switch (mode) {
        case ERecursiveRemovePrompt::Always:
            return Prompt(path, type);
        case ERecursiveRemovePrompt::Once:
            if (first) {
                return Prompt(path, type);
            } else {
                return true;
            }
        case ERecursiveRemovePrompt::Never:
            return true;
    }
}

template <typename TClient, typename TSettings>
using TRemoveFunc = TStatus(*)(TClient&, const TString&, const TSettings&);

template <typename TClient, typename TSettings>
TStatus Remove(TRemoveFunc<TClient, TSettings> func, TClient* client, const TSchemeEntry& entry,
        ERecursiveRemovePrompt prompt, const TRemoveDirectorySettings& settings)
{
    if (!client) {
        return TStatus(EStatus::GENERIC_ERROR, MakeIssues(TStringBuilder()
            << TypeName<TClient>() << " not specified"));
    }

    if (Prompt(prompt, entry.Name, entry.Type, false)) {
        return func(*client, entry.Name, TSettings(settings));
    } else {
        return TStatus(EStatus::SUCCESS, {});
    }
}

TStatus RemoveDirectoryRecursive(
        TSchemeClient& schemeClient,
        TTableClient* tableClient,
        TTopicClient* topicClient,
        const TString& path,
        ERecursiveRemovePrompt prompt,
        const TRemoveDirectorySettings& settings,
        bool removeSelf)
{
    auto recursiveListResult = RecursiveList(schemeClient, path, {}, removeSelf);
    if (!recursiveListResult.Status.IsSuccess()) {
        return recursiveListResult.Status;
    }

    if (prompt == ERecursiveRemovePrompt::Once) {
        if (!Prompt(path, ESchemeEntryType::Directory)) {
            return TStatus(EStatus::SUCCESS, {});
        }
    }

    // output order is: Root, Recursive(children)...
    // we need to reverse it to delete recursively
    for (auto it = recursiveListResult.Entries.rbegin(); it != recursiveListResult.Entries.rend(); ++it) {
        const auto& entry = *it;
        switch (entry.Type) {
            case ESchemeEntryType::Directory:
                if (auto result = Remove(&RemoveDirectory, &schemeClient, entry, prompt, settings); !result.IsSuccess()) {
                    return result;
                }
                break;

            case ESchemeEntryType::ColumnTable:
            case ESchemeEntryType::Table:
                if (auto result = Remove(&RemoveTable, tableClient, entry, prompt, settings); !result.IsSuccess()) {
                    return result;
                }
                break;

            case ESchemeEntryType::Topic:
                if (auto result = Remove(&RemoveTopic, topicClient, entry, prompt, settings); !result.IsSuccess()) {
                    return result;
                }
                break;

            default:
                return TStatus(EStatus::UNSUPPORTED, MakeIssues(TStringBuilder()
                    << "Unsupported entry type: " << entry.Type));
        }
    }

    return TStatus(EStatus::SUCCESS, {});
}

TStatus RemoveDirectoryRecursive(
        TSchemeClient& schemeClient,
        TTableClient& tableClient,
        const TString& path,
        const TRemoveDirectorySettings& settings,
        bool removeSelf)
{
    return RemoveDirectoryRecursive(schemeClient, &tableClient, nullptr, path, ERecursiveRemovePrompt::Never, settings, removeSelf);
}

TStatus RemoveDirectoryRecursive(
        TSchemeClient& schemeClient,
        TTableClient& tableClient,
        TTopicClient& topicClient,
        const TString& path,
        ERecursiveRemovePrompt prompt,
        const TRemoveDirectorySettings& settings,
        bool removeSelf)
{
    return RemoveDirectoryRecursive(schemeClient, &tableClient, &topicClient, path, prompt, settings, removeSelf);
}

}
