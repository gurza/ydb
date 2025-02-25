#include "kqp_yql.h"

#include <ydb/library/yql/core/yql_expr_type_annotation.h>
#include <ydb/library/yql/core/expr_nodes/yql_expr_nodes.h>

namespace NYql {

using namespace NKikimr;
//using namespace NKikimr::NKqp;
using namespace NNodes;

static EPhysicalQueryType GetPhysicalQueryType(const TStringBuf& value) {
    if (value == "data_query") {
        return EPhysicalQueryType::Data;
    } else if (value == "scan_query") {
        return EPhysicalQueryType::Scan;
    } else if (value == "query") {
        return EPhysicalQueryType::Query;
    } else if (value == "federated_query") {
        return EPhysicalQueryType::FederatedQuery;
    } else {
        YQL_ENSURE(false, "Unknown physical query type: " << value);
    }
}

static TStringBuf PhysicalQueryTypeToString(EPhysicalQueryType type) {
    switch (type) {
        case EPhysicalQueryType::Unspecified:
            break;
        case EPhysicalQueryType::Data:
            return "data_query";
        case EPhysicalQueryType::Scan:
            return "scan_query";
        case EPhysicalQueryType::Query:
            return "query";
        case EPhysicalQueryType::FederatedQuery:
            return "federated_query";
    }

    YQL_ENSURE(false, "Unexpected physical query type: " << type);
}

TKqpPhyQuerySettings TKqpPhyQuerySettings::Parse(const TKqpPhysicalQuery& node) {
    TKqpPhyQuerySettings settings;

    for (const auto& tuple : node.Settings()) {
        auto name = tuple.Name().Value();
        if (name == TypeSettingName) {
            YQL_ENSURE(tuple.Value().Maybe<TCoAtom>());
            settings.Type = GetPhysicalQueryType(tuple.Value().Cast<TCoAtom>().Value());
        }
    }

    return settings;
}

NNodes::TCoNameValueTupleList TKqpPhyQuerySettings::BuildNode(TExprContext& ctx, TPositionHandle pos) const {
    TVector<TCoNameValueTuple> settings;

    if (Type) {
        settings.push_back(Build<TCoNameValueTuple>(ctx, pos)
            .Name().Build(TypeSettingName)
            .Value<TCoAtom>().Build(PhysicalQueryTypeToString(*Type))
            .Done());
    }

    return Build<TCoNameValueTupleList>(ctx, pos)
        .Add(settings)
        .Done();
}

static EPhysicalTxType GetPhysicalTxType(const TStringBuf& value) {
    if (value == "compute") {
        return EPhysicalTxType::Compute;
    } else if (value == "data") {
        return EPhysicalTxType::Data;
    } else if (value == "scan") {
        return EPhysicalTxType::Scan;
    } else if (value == "generic") {
        return EPhysicalTxType::Generic;
    } else {
        YQL_ENSURE(false, "Unknown physical tx type: " << value);
    }
}

static TStringBuf PhysicalTxTypeToString(EPhysicalTxType type) {
    switch (type) {
        case EPhysicalTxType::Unspecified:
            break;
        case EPhysicalTxType::Compute:
            return "compute";
        case EPhysicalTxType::Data:
            return "data";
        case EPhysicalTxType::Scan:
            return "scan";
        case EPhysicalTxType::Generic:
            return "generic";
    }

    YQL_ENSURE(false, "Unexpected physical tx type: " << type);
}

TKqpPhyTxSettings TKqpPhyTxSettings::Parse(const TKqpPhysicalTx& node) {
    TKqpPhyTxSettings settings;

    for (const auto& tuple : node.Settings()) {
        auto name = tuple.Name().Value();
        if (name == TypeSettingName) {
            YQL_ENSURE(tuple.Value().Maybe<TCoAtom>());
            settings.Type = GetPhysicalTxType(tuple.Value().Cast<TCoAtom>().Value());
        } else if (name == WithEffectsSettingName) {
            settings.WithEffects = true;
        }
    }

    return settings;
}

NNodes::TCoNameValueTupleList TKqpPhyTxSettings::BuildNode(TExprContext& ctx, TPositionHandle pos) const {
    TVector<TCoNameValueTuple> settings;
    settings.reserve(2);

    if (Type) {
        settings.emplace_back(Build<TCoNameValueTuple>(ctx, pos)
            .Name().Build(TypeSettingName)
            .Value<TCoAtom>().Build(PhysicalTxTypeToString(*Type))
            .Done());
    }

    if (WithEffects) {
        settings.emplace_back(Build<TCoNameValueTuple>(ctx, pos)
            .Name().Build(WithEffectsSettingName)
            .Done());
    }

    return Build<TCoNameValueTupleList>(ctx, pos)
        .Add(settings)
        .Done();
}

namespace {

TKqpReadTableSettings ParseInternal(const TCoNameValueTupleList& node) {
    TKqpReadTableSettings settings;

    for (const auto& tuple : node) {
        TStringBuf name = tuple.Name().Value();

        if (name == TKqpReadTableSettings::SkipNullKeysSettingName) {
            YQL_ENSURE(tuple.Value().Maybe<TCoAtomList>());
            for (const auto& key : tuple.Value().Cast<TCoAtomList>()) {
                settings.SkipNullKeys.emplace_back(TString(key.Value()));
            }
        } else if (name == TKqpReadTableSettings::ItemsLimitSettingName) {
            YQL_ENSURE(tuple.Value().IsValid());
            settings.ItemsLimit = tuple.Value().Cast().Ptr();
        } else if (name == TKqpReadTableSettings::ReverseSettingName) {
            YQL_ENSURE(tuple.Ref().ChildrenSize() == 1);
            settings.Reverse = true;
        } else if (name == TKqpReadTableSettings::SortedSettingName) {
            YQL_ENSURE(tuple.Ref().ChildrenSize() == 1);
            settings.Sorted = true;
        } else if (name == TKqpReadTableSettings::SequentialSettingName) {
            YQL_ENSURE(tuple.Ref().ChildrenSize() == 2);
            settings.SequentialHint = FromString<ui64>(tuple.Value().Cast<TCoAtom>().Value());
        } else {
            YQL_ENSURE(false, "Unknown KqpReadTable setting name '" << name << "'");
        }
    }

    return settings;
}

} // anonymous namespace end

TKqpReadTableSettings TKqpReadTableSettings::Parse(const NNodes::TCoNameValueTupleList& node) {
    return ParseInternal(node);
}

TKqpReadTableSettings TKqpReadTableSettings::Parse(const TKqlReadTableBase& node) {
    return TKqpReadTableSettings::Parse(node.Settings());
}

TKqpReadTableSettings TKqpReadTableSettings::Parse(const TKqlReadTableRangesBase& node) {
    return TKqpReadTableSettings::Parse(node.Settings());
}

NNodes::TCoNameValueTupleList TKqpReadTableSettings::BuildNode(TExprContext& ctx, TPositionHandle pos) const {
    TVector<TCoNameValueTuple> settings;
    settings.reserve(3);

    if (!SkipNullKeys.empty()) {
        TVector<TExprNodePtr> keys;
        keys.reserve(SkipNullKeys.size());
        for (auto& key: SkipNullKeys) {
            keys.emplace_back(ctx.NewAtom(pos, key));
        }

        settings.emplace_back(
            Build<TCoNameValueTuple>(ctx, pos)
                .Name()
                    .Build(SkipNullKeysSettingName)
                .Value<TCoAtomList>()
                    .Add(keys)
                    .Build()
                .Done());
    }

    if (ItemsLimit) {
        settings.emplace_back(
            Build<TCoNameValueTuple>(ctx, pos)
                .Name()
                    .Build(ItemsLimitSettingName)
                .Value(ItemsLimit)
                .Done());
    }

    if (Reverse) {
        settings.emplace_back(
            Build<TCoNameValueTuple>(ctx, pos)
                .Name()
                    .Build(ReverseSettingName)
                .Done());
    }

    if (Sorted) {
        settings.emplace_back(
            Build<TCoNameValueTuple>(ctx, pos)
                .Name()
                    .Build(SortedSettingName)
                .Done());
    }

    if (SequentialHint) {
        settings.emplace_back(
            Build<TCoNameValueTuple>(ctx, pos)
                .Name()
                    .Build(SequentialSettingName)
                .Value<TCoAtom>()
                    .Value(ToString(*SequentialHint))
                    .Build()
                .Done());
    }

    return Build<TCoNameValueTupleList>(ctx, pos)
        .Add(settings)
        .Done();
}

void TKqpReadTableSettings::AddSkipNullKey(const TString& key) {
    for (auto& k : SkipNullKeys) {
        if (k == key) {
            return;
        }
    }
    SkipNullKeys.emplace_back(key);
}

TKqpUpsertRowsSettings TKqpUpsertRowsSettings::Parse(const TKqpUpsertRows& node) {
    TKqpUpsertRowsSettings settings;

    for (const auto& tuple : node.Settings()) {
        TStringBuf name = tuple.Name().Value();

        if (name == TKqpUpsertRowsSettings::InplaceSettingName) {
            YQL_ENSURE(tuple.Ref().ChildrenSize() == 1);
            settings.Inplace = true;
        } else {
            YQL_ENSURE(false, "Unknown KqpUpsertRows setting name '" << name << "'");
        }
    }

    return settings;
}

NNodes::TCoNameValueTupleList TKqpUpsertRowsSettings::BuildNode(TExprContext& ctx, TPositionHandle pos) const {
    TVector<TCoNameValueTuple> settings;
    settings.reserve(1);

    if (Inplace) {
        settings.emplace_back(
            Build<TCoNameValueTuple>(ctx, pos)
                .Name().Build(InplaceSettingName)
                .Done());
    }

    return Build<TCoNameValueTupleList>(ctx, pos)
        .Add(settings)
        .Done();
}

TCoNameValueTupleList TKqpReadTableExplainPrompt::BuildNode(TExprContext& ctx, TPositionHandle pos) const {
    TVector<TCoNameValueTuple> prompt;
    prompt.reserve(2);

    TVector<TExprNodePtr> keys;
    keys.reserve(UsedKeyColumns.size());

    for (auto& key: UsedKeyColumns) {
        keys.emplace_back(ctx.NewAtom(pos, key));
    }

    prompt.emplace_back(
        Build<TCoNameValueTuple>(ctx, pos)
            .Name()
                .Build(UsedKeyColumnsName)
            .Value<TCoAtomList>()
                .Add(keys)
                .Build()
            .Done()
    );

    if (!ExpectedMaxRanges.empty()) {
        prompt.emplace_back(
            Build<TCoNameValueTuple>(ctx, pos)
                .Name()
                    .Build(ExpectedMaxRangesName)
                .Value<TCoAtom>()
                    .Build(ExpectedMaxRanges)
                .Done()
        );
    }

    return Build<TCoNameValueTupleList>(ctx, pos)
        .Add(prompt)
        .Done();
}

TKqpReadTableExplainPrompt TKqpReadTableExplainPrompt::Parse(const NNodes::TKqlReadTableRangesBase& node) {
    return TKqpReadTableExplainPrompt::Parse(node.ExplainPrompt());
}

TKqpReadTableExplainPrompt TKqpReadTableExplainPrompt::Parse(const NNodes::TCoNameValueTupleList& node) {
    TKqpReadTableExplainPrompt prompt;

    for (const auto& tuple : node) {
        TStringBuf name = tuple.Name().Value();

        if (name == TKqpReadTableExplainPrompt::UsedKeyColumnsName) {
            for (const auto& key : tuple.Value().template Cast<TCoAtomList>()) {
                prompt.UsedKeyColumns.emplace_back(TString(key.Value()));
            }

            continue;
        }

        if (name == TKqpReadTableExplainPrompt::ExpectedMaxRangesName) {
            prompt.ExpectedMaxRanges = TString(tuple.Value().template Cast<TCoAtom>());
             continue;
        }

        YQL_ENSURE(false, "Unknown KqpReadTableRanges explain prompt name '" << name << "'");
    }

    return prompt;
}

TString KqpExprToPrettyString(const TExprNode& expr, TExprContext& ctx) {
    try {
        TConvertToAstSettings settings;
        settings.NoInlineFunc = [] (const TExprNode& exprNode) {
            TExprBase node(&exprNode);

            if (node.Maybe<TDqStageBase>()) {
                return true;
            }

            if (node.Maybe<TDqConnection>()) {
                return true;
            }

            if (node.Maybe<TKqlReadTableBase>()) {
                return true;
            }

            if (node.Maybe<TKqlReadTableRangesBase>()) {
                return true;
            }

            return false;
        };

        auto ast = ConvertToAst(expr, ctx, settings);
        TStringStream exprStream;
        YQL_ENSURE(ast.Root);
        ast.Root->PrettyPrintTo(exprStream, NYql::TAstPrintFlags::PerLine | NYql::TAstPrintFlags::ShortQuote);
        TString exprText = exprStream.Str();

        return exprText;
    } catch (const std::exception& e) {
        return TStringBuilder() << "Failed to render expression to pretty string: " << e.what();
    }
}

TString KqpExprToPrettyString(const TExprBase& expr, TExprContext& ctx) {
    return KqpExprToPrettyString(expr.Ref(), ctx);
}

TString PrintKqpStageOnly(const TDqStageBase& stage, TExprContext& ctx) {
    if (stage.Inputs().Empty()) {
        return KqpExprToPrettyString(stage, ctx);
    }

    TNodeOnNodeOwnedMap replaces;
    for (ui64 i = 0; i < stage.Inputs().Size(); ++i) {
        auto input = stage.Inputs().Item(i);
        auto param = Build<TCoParameter>(ctx, input.Pos())
            .Name().Build(TStringBuilder() << "stage_input_" << i)
            .Type(ExpandType(input.Pos(), *input.Ref().GetTypeAnn(), ctx))
            .Done();

        replaces[input.Raw()] = param.Ptr();
    }

    auto newStage = ctx.ReplaceNodes(stage.Ptr(), replaces);
    return KqpExprToPrettyString(TExprBase(newStage), ctx);
}

} // namespace NYql
