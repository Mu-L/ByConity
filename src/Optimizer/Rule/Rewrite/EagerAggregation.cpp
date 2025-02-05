#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <Optimizer/Rule/Rewrite/EagerAggregation.h>

#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <Core/SortDescription.h>
#include <Interpreters/join_common.h>
#include <Optimizer/DistinctOutputUtil.h>
#include <Optimizer/PredicateUtils.h>
#include <Optimizer/Rule/Patterns.h>
#include <Optimizer/Utils.h>
#include <Parsers/ASTIdentifier.h>
#include <QueryPlan/AggregatingStep.h>
#include <QueryPlan/GraphvizPrinter.h>
#include <QueryPlan/JoinStep.h>
#include <QueryPlan/ProjectionStep.h>
#include <QueryPlan/SymbolMapper.h>
#include <QueryPlan/ValuesStep.h>
#include <fmt/format.h>
#include <incubator-brpc/src/butil/file_util.h>
#include <Poco/Logger.h>
#include <Poco/String.h>
#include <Poco/StringTokenizer.h>
#include "Core/NameToType.h"
#include "Core/Names.h"
#include "Interpreters/AggregateDescription.h"
#include "Interpreters/Context_fwd.h"
#include "Optimizer/CardinalityEstimate/CardinalityEstimator.h"
#include "Optimizer/Rule/Rule.h"
#include "Optimizer/SymbolUtils.h"
#include "Optimizer/SymbolsExtractor.h"
#include "Parsers/ASTFunction.h"
#include "QueryPlan/IQueryPlanStep.h"
#include <DataTypes/DataTypeCustomSimpleAggregateFunction.h>

namespace DB
{

ConstRefPatternPtr EagerAggregation::getPattern() const
{
    static auto pattern = Patterns::aggregating()
        .matchingStep<AggregatingStep>([](const AggregatingStep & s) { return s.isNormal() && !s.getKeys().empty(); })
        .result();
    return pattern;
}

enum class AggFuncClass
{
    BASIC,
    NEED_MERGE,
    CLASS_C,
    CLASS_D,
    UNKNOWN
};

static AggFuncClass getClassOfAggFunc(String name)
{
    name = Poco::toLower(name);

    static const std::unordered_set<std::string> simple_aggregate_functions = {
        "any",
        "anyLast",
        "min",
        "max",
        "sum",
        "sumWithOverflow",
        "groupBitAnd",
        "groupBitOr",
        "groupBitXor",
        "sumMap",
        "minMap",
        "maxMap",
        "groupArrayArray",
        "groupArrayLastArray",
        "groupUniqArrayArray",
        "sumMappedArrays",
        "minMappedArrays",
        "maxMappedArrays"
    };

    if (simple_aggregate_functions.contains(name))
        return AggFuncClass::BASIC;
    if (name == "uniqexact" || name == "count")
        return AggFuncClass::NEED_MERGE;
    // if (name == "sum" || name == "count")
    //     return AggFuncClass::CLASS_C;
    // if (name == "sumdistinct" || name == "uniqexact" || name == "avg" || name == "min" || name == "max")
    //     return AggFuncClass::CLASS_D;
    return AggFuncClass::UNKNOWN;
}

// Split any function in clickhouse to state + merge:
// sum -split to-> sumState + sumMerge.
// Sometimes it is necessary to further split the intermediate function:
// sumState -split to-> sumState + sumStateMerge.
// sumMerge -split to-> sumStateMerge + sumMerge.
// sumStateMerge -split to-> sumStateMerge + sumStateMerge.
static String getStateName(const String & func_name)
{
    return func_name + "State";
}

static String getMergeName(const String & func_name)
{
    return func_name + "Merge";
}

static bool decomposeAggJoin(
    const AggregateDescriptions & agg_descs,
    const NameSet & group_by_keys,
    const NameSet & names_from_left,
    const NameSet & names_from_right,
    AggregateDescriptions & composed_aggregates,
    AggregateDescriptions & s1,
    AggregateDescriptions & s2,
    Names & g1,
    Names & g2)
{
    for (const auto & aggregator : agg_descs)
    {
        auto function_type = getClassOfAggFunc(aggregator.function->getName());
        if (function_type == AggFuncClass::UNKNOWN)
            return false;
        if (SymbolUtils::containsAll(names_from_left, aggregator.argument_names))
        {
            if (aggregator.argument_names.size() == 1 && !group_by_keys.contains(aggregator.argument_names[0]))
                s1.emplace_back(aggregator);
        }
        else if (SymbolUtils::containsAll(names_from_right, aggregator.argument_names))
        {
            if (aggregator.argument_names.size() == 1 && !group_by_keys.contains(aggregator.argument_names[0]))
                s2.emplace_back(aggregator);
        }
        else
        {
            composed_aggregates.emplace_back(aggregator);
        }
    }

    for (const auto & group_key : group_by_keys)
    {
        if (names_from_left.contains(group_key))
            g1.push_back(group_key);
        else if (names_from_right.contains(group_key))
            g2.push_back(group_key);
        else
            return false;
    }

    return true;
}

static bool decomposeProjection(
    const ProjectionStep & projection_step,
    const AggregateDescriptions & composed_aggregates,
    const NameSet & group_by_keys,
    const NameSet & names_from_left,
    const NameSet & names_from_right,
    NameToNameMap & global_argument_name_to_local_from_left,
    NameToNameMap & global_argument_name_to_local_from_right,
    AggregateDescriptions & s1,
    AggregateDescriptions & s2,
    NameOrderedSet & projection_require_symbols,
    NameSet & projection_gene_symbols,
    const SymbolAllocatorPtr & symbol_allocator)
{
    bool deep_parse_success = false;
    const Assignments & assignments = projection_step.getAssignments();

    // the projection where a new sub agg can be extracted.
    if (!composed_aggregates.empty())
    {
        for (auto agg_desc : composed_aggregates)
        {
            if (agg_desc.argument_names.size() == 1)
            {
                String the_only_argument_name = agg_desc.argument_names[0];
                if (assignments.contains(the_only_argument_name))
                {
                    ConstASTPtr ast = assignments.at(the_only_argument_name)->clone();
                    if (const auto * func = ast->as<ASTFunction>(); func && Poco::toLower(func->name) == "multiif")
                    {
                        const auto * expr_list = func->children[0]->as<ASTExpressionList>();
                        if (expr_list && expr_list->children.size() > 2)
                        {
                            if (const auto * child = expr_list->children[1]->as<ASTIdentifier>())
                            {
                                String decomposed_argument_name = child->name();

                                if (!global_argument_name_to_local_from_left.contains(decomposed_argument_name)
                                    && !global_argument_name_to_local_from_right.contains(decomposed_argument_name)
                                    && !group_by_keys.contains(decomposed_argument_name)) // Avoid producing duplicate sum entries in local aggregate.
                                {
                                    String new_decomposed_argument_name = symbol_allocator->newSymbol("inter#" + decomposed_argument_name);

                                    deep_parse_success = true;

                                    agg_desc.argument_names[0] = decomposed_argument_name;
                                    agg_desc.column_name = new_decomposed_argument_name;

                                    if (names_from_left.contains(decomposed_argument_name))
                                    {
                                        s1.emplace_back(agg_desc);
                                        global_argument_name_to_local_from_left.emplace(
                                            decomposed_argument_name, new_decomposed_argument_name);
                                    }
                                    if (names_from_right.contains(decomposed_argument_name))
                                    {
                                        s2.emplace_back(agg_desc);
                                        global_argument_name_to_local_from_right.emplace(
                                            decomposed_argument_name, new_decomposed_argument_name);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // projection that can be fully pushed down to the join side.
    if (!deep_parse_success)
    {
        int left_cnt = 0, right_cnt = 0, total_cnt = 0;
        for (const auto & assignment : assignments)
        {
            if (Utils::isIdentity(assignment))
                continue;
            ++total_cnt;

            auto symbols = SymbolsExtractor::extract(assignment.second);
            if (SymbolUtils::containsAll(names_from_left, symbols))
                ++left_cnt;
            else if (SymbolUtils::containsAll(names_from_right, symbols))
                ++right_cnt;
            if (left_cnt > 0 && right_cnt > 0)
                break;
            projection_require_symbols.insert(symbols.begin(), symbols.end());
            projection_gene_symbols.insert(assignment.first);
        }

        if (left_cnt != total_cnt && right_cnt != total_cnt)
        {
            projection_require_symbols.clear();
            projection_gene_symbols.clear();
            return false;
        }

        for (const auto & agg_desc : composed_aggregates)
        {
            String the_only_argument_name = agg_desc.argument_names[0];
            if (assignments.contains(the_only_argument_name))
            {
                if (!global_argument_name_to_local_from_left.contains(the_only_argument_name)
                    && !global_argument_name_to_local_from_right.contains(
                        the_only_argument_name)) // Avoid producing duplicate sum entries in local aggregate.
                {
                    if (left_cnt > 0)
                    {
                        s1.emplace_back(agg_desc);
                    }
                    if (right_cnt > 0)
                    {
                        s2.emplace_back(agg_desc);
                    }
                }
            }
        }
        return true;
    }


    return deep_parse_success;
}

struct AggregationsAndKeys
{
public:
    AggregateDescriptions descriptions;
    Names keys;
    bool invalid = false;

    operator bool() const { return !invalid; }
};
AggregationsAndKeys AggregationsAndKeysInvalidFlag{{}, {}, true};

static AggregationsAndKeys updateAggS0AndG0(NameSet names_from_one_side, const NameSet & projection_gene_symbols, const AggregateDescriptions & s0, const Names & g0)
{
    names_from_one_side.insert(projection_gene_symbols.begin(), projection_gene_symbols.end());

    AggregateDescriptions new_s0;
    for (const auto & agg : s0)
    {
        auto function_type = getClassOfAggFunc(agg.function->getName());

        // argument_names cannot be empty, otherwise it is not possible to tell whether to push down to the left or the right
        if (function_type != AggFuncClass::UNKNOWN && agg.argument_names.size() == 1 && SymbolUtils::containsAll(names_from_one_side, agg.argument_names))
            new_s0.push_back(agg);
        else
            return AggregationsAndKeysInvalidFlag;
    }

    Names new_g0;
    for (const auto & group_key : g0)
    {
        if (names_from_one_side.contains(group_key))
            new_g0.push_back(group_key);
    }

    // LOG_DEBUG(
    //     getLogger("test"),
    //     "names_from_one_side={}, g0={}, new_g0={}, s0={}, new_s0={}",
    //     fmt::join(names_from_one_side, ","),
    //     fmt::join(g0, ","),
    //     fmt::join(new_g0, ","),
    //     formatS0(s0),
    //     formatS0(new_s0));

    return {new_s0, new_g0};
}

static LocalGroupByTargetMap determineBottomJoin(
    const PlanNodePtr & parent_of_first_join,
    const PlanNodePtr & projection,
    const AggregateDescriptions & init_s0,
    const Names & init_g0,
    const NameOrderedSet & projection_require_symbols,
    const NameSet & projection_gene_symbols,
    const NameSet & init_require_output_names_from_local_agg,
    const NameToNameMap & global_argument_name_to_local_from_projection,
    RuleContext & context)
{
    LocalGroupByTargetMap result;

    String global_argument_name_to_local_from_projection_str;
    for (const auto & [a, b] : global_argument_name_to_local_from_projection)
        global_argument_name_to_local_from_projection_str += a + ", " + b + "\n";

    LOG_DEBUG(
        getLogger("test"),
        "\tinto determineBottomJoin, init_s0={}, init_g0={}, projection_gene_symbols={}, projection_gene_symbols={}, "
        "init_require_output_names_from_local_agg={}, global_argument_name_to_local_from_projection_str={}",
        formatS0(init_s0),
        fmt::join(init_g0, ","),
        fmt::join(projection_require_symbols, ","),
        fmt::join(projection_gene_symbols, ","),
        fmt::join(init_require_output_names_from_local_agg, ","),
        global_argument_name_to_local_from_projection_str);

    bool has_visit_first_join = false;

    std::function<void(NameSet, PlanNodePtr, int, AggregateDescriptions, Names, int, std::unordered_map<String, String>)> find_bottom_join
        = [&](NameSet require_output_names_from_local_agg, PlanNodePtr join, int index, AggregateDescriptions s0, Names g0, int join_layer, std::unordered_map<String, String> proj_expr_to_origin_column) {

        if (join->getChildren()[index]->getType() == IQueryPlanStep::Type::Projection
            && join->getChildren()[index]->getChildren()[0]->getType() == IQueryPlanStep::Type::Join
            && proj_expr_to_origin_column.empty()) // try to push agg through projection
        {
            const auto & projection_step = dynamic_cast<const ProjectionStep &>(*join->getChildren()[index]->getStep());
            const auto & next_join_node = join->getChildren()[index]->getChildren()[0];

            for (const auto & [name, ast] : projection_step.getAssignments())
            {
                if (!Utils::isIdentity(name, ast))
                {
                    auto names = SymbolsExtractor::extract(ast);
                    if (names.size() != 1)
                    {
                        proj_expr_to_origin_column.clear();
                        break;
                    }
                    proj_expr_to_origin_column.emplace(name, *names.begin());
                }
            }
            if (!proj_expr_to_origin_column.empty())
            {
                const auto & second_join_step = dynamic_cast<const JoinStep &>(*next_join_node->getStep());
                if (second_join_step.getFilter())
                {
                    auto symbols = SymbolsExtractor::extract(second_join_step.getFilter());
                    require_output_names_from_local_agg.insert(symbols.begin(), symbols.end());
                }
                require_output_names_from_local_agg.insert(second_join_step.getLeftKeys().begin(), second_join_step.getLeftKeys().end());
                require_output_names_from_local_agg.insert(second_join_step.getRightKeys().begin(), second_join_step.getRightKeys().end());
                auto second_names_from_left = next_join_node->getChildren()[0]->getCurrentDataStream().header.getNameSet();
                auto second_names_from_right = next_join_node->getChildren()[1]->getCurrentDataStream().header.getNameSet();

                auto old_result_size = result.size();
                if (auto new_sg = updateAggS0AndG0(second_names_from_left, projection_gene_symbols, s0, g0))
                {
                    find_bottom_join(require_output_names_from_local_agg, next_join_node, 0, new_sg.descriptions, new_sg.keys, join_layer, proj_expr_to_origin_column);
                }

                if (old_result_size == result.size())
                {
                    if (auto new_sg = updateAggS0AndG0(second_names_from_right, projection_gene_symbols, s0, g0))
                    {
                        find_bottom_join(require_output_names_from_local_agg, next_join_node, 1, new_sg.descriptions, new_sg.keys, join_layer, proj_expr_to_origin_column);
                    }
                }
                return;
            }
        }

        if (join->getChildren()[index]->getType() != IQueryPlanStep::Type::Join || has_visit_first_join)
        {
            Names c1;
            if (projection_gene_symbols.empty())
                c1 = join->getChildren()[index]->getCurrentDataStream().header.getNames();
            else
            {
                c1 = join->getChildren()[index]->getCurrentDataStream().header.getNames();
                const auto & proj_step = static_cast<const ProjectionStep &>(*projection->getStep());
                for (const auto & assignment : proj_step.getAssignments())
                {
                    if (!Utils::isIdentity(assignment))
                        c1.push_back(assignment.first);
                }
            }


            String str;
            for (const auto & [a, b] : proj_expr_to_origin_column)
                str += a + ", " + b + "\n";

            LOG_WARNING(getLogger("test"), "before proj_expr_to_origin_column={}", str);

            require_output_names_from_local_agg.insert(init_require_output_names_from_local_agg.begin(), init_require_output_names_from_local_agg.end());

            NameSet global_agg_needs;
            for (const auto & aggregator : s0)
            {
                global_agg_needs.emplace(aggregator.column_name);
                for (const auto & argument_name : aggregator.argument_names)
                {
                    global_agg_needs.emplace(argument_name);
                }
            }

            // convert group by expr(xx) in global agg -> group by xx in local agg, xx must be saved in local agg.
            if (!proj_expr_to_origin_column.empty())
            {
                for (const auto & [expr, origin_column] : proj_expr_to_origin_column)
                {
                    if (require_output_names_from_local_agg.erase(expr))
                    {
                        require_output_names_from_local_agg.insert(origin_column);
                    }
                }
            }

            LOG_DEBUG(getLogger("test"), "before erase, g0={}, c1={}, require_output_names_from_local_agg={}, global_agg_needs={}",
                fmt::join(g0, ","), fmt::join(c1, ","), fmt::join(require_output_names_from_local_agg, ","), fmt::join(global_agg_needs, ","));
            std::erase_if(c1, [&](const String & v) { return !require_output_names_from_local_agg.contains(v); });
            if (!s0.empty())
                std::erase_if(c1, [&](const String & v) { return global_argument_name_to_local_from_projection.contains(v); });
            std::erase_if(c1, [&](const String & v) { return global_agg_needs.contains(v); });

            g0.insert(g0.end(), c1.begin(), c1.end());
            std::sort(g0.begin(), g0.end());
            g0.erase(std::unique(g0.begin(), g0.end()), g0.end());

            LOG_DEBUG(getLogger("test"), "collect new local group by target, join_id={}, index={}, g0={}, s0={}", join->getId(), index, fmt::join(g0, ","), formatS0(s0));
            result.emplace(join->getId(), LocalGroupByTarget{join, index, s0, g0, join_layer, !proj_expr_to_origin_column.empty()});

            return;
        }

        if (context.context->getSettingsRef().agg_push_down_every_join)
            has_visit_first_join = true;

        PlanNodePtr second_join = join->getChildren()[index];

        const auto & second_join_step = dynamic_cast<const JoinStep &>(*second_join->getStep());

        if (second_join_step.getFilter())
        {
            auto symbols = SymbolsExtractor::extract(second_join_step.getFilter());
            require_output_names_from_local_agg.insert(symbols.begin(), symbols.end());
        }
        require_output_names_from_local_agg.insert(second_join_step.getLeftKeys().begin(), second_join_step.getLeftKeys().end());
        require_output_names_from_local_agg.insert(second_join_step.getRightKeys().begin(), second_join_step.getRightKeys().end());

        auto second_names_from_left = second_join->getChildren()[0]->getCurrentDataStream().header.getNameSet();
        auto second_names_from_right = second_join->getChildren()[1]->getCurrentDataStream().header.getNameSet();

        // pattern1: push full projection + sub agg.
        if (!projection_require_symbols.empty())
        {
            auto old_result_size = result.size();
            if (SymbolUtils::containsAll(second_names_from_left, projection_require_symbols))
            {
                if (auto new_sg = updateAggS0AndG0(second_names_from_left, projection_gene_symbols, s0, g0))
                {
                    find_bottom_join(require_output_names_from_local_agg, second_join, 0, new_sg.descriptions, new_sg.keys, join_layer + 1, proj_expr_to_origin_column);
                }
            }
            if (old_result_size == result.size())
            {
                if (SymbolUtils::containsAll(second_names_from_right, projection_require_symbols))
                {
                    if (auto new_sg = updateAggS0AndG0(second_names_from_right, projection_gene_symbols, s0, g0))
                    {
                        find_bottom_join(require_output_names_from_local_agg, second_join, 1, new_sg.descriptions, new_sg.keys, join_layer + 1, proj_expr_to_origin_column);
                    }
                }
            }
        }
        else
        {
            // pattern2: only push sub agg.
            auto old_result_size = result.size();
            if (second_join->getChildren()[0]->getType()
                != IQueryPlanStep::Type::Aggregating) // avoid push agg through join which child is already an aggregation node.
            {
                if (auto new_sg = updateAggS0AndG0(second_names_from_left, {}, s0, g0))
                {
                    find_bottom_join(require_output_names_from_local_agg, second_join, 0, new_sg.descriptions, new_sg.keys, join_layer + 1, proj_expr_to_origin_column);
                }
            }
            if (old_result_size == result.size())
            {
                if (second_join->getChildren()[1]->getType() != IQueryPlanStep::Type::Aggregating)
                {
                    if (auto new_sg = updateAggS0AndG0(second_names_from_right, {}, s0, g0))
                    {
                        find_bottom_join(require_output_names_from_local_agg, second_join, 1, new_sg.descriptions, new_sg.keys, join_layer + 1, proj_expr_to_origin_column);
                    }
                }
            }
        }
    };

    find_bottom_join({}, parent_of_first_join, 0, init_s0, init_g0, 0, {});

    return result;
}

std::shared_ptr<AggregatingStep>
createLocalAggregate(const DataStream & input_stream, const AggregateDescriptions & s0, const Names & g0, const ContextPtr &)
{
    LOG_DEBUG(getLogger("test"), "create local_agg={}, keys={}", formatS0(s0), fmt::join(g0, ","));

    return std::make_shared<AggregatingStep>(
        input_stream, g0, NameSet{}, s0, GroupingSetsParamsList{}, true);
}

PlanNodePtr doInsertAggregation(
    const PlanNodePtr & aggregation,
    const AggregateDescriptions & s1,
    const Names & g1,
    bool push_projection,
    PlanNodeId bottom_join_id,
    int bottom_join_child_index,
    bool push_through_final_projection,
    const SymbolAllocatorPtr & symbol_allocator,
    RuleContext & rule_context)
{
    NameToNameMap global_argument_name_to_local;
    for (const auto & aggregator : s1)
    {
        for (const auto & argument_name : aggregator.argument_names)
        {
            if (!global_argument_name_to_local.contains(argument_name) && std::find(g1.begin(), g1.end(), argument_name) == g1.end())
            {
                String new_argument_name = symbol_allocator->newSymbol("inter#" + argument_name);
                global_argument_name_to_local.emplace(argument_name, new_argument_name);
                global_argument_name_to_local.emplace(aggregator.column_name, new_argument_name);
            }
        }
    }

    String names;
    for (const auto & [k, v] : global_argument_name_to_local)
        names += "k=" + k + ",v=" + v + "\n";
    LOG_DEBUG(getLogger("test"), "before doInsertAggregation, global_argument_name_to_local={}", names);

    auto symbol_mapper = SymbolMapper::simpleMapper(global_argument_name_to_local);

    bool has_visit_global_agg = false, has_visit_join = false;

    PlanNodePtr proj; // projection node which can be push through join.

    std::function<PlanNodePtr(const PlanNodePtr &)> update_plan_node_until_bottom_join
        = [&](const PlanNodePtr & current_node) -> PlanNodePtr {
        if (current_node->getType() == IQueryPlanStep::Type::Aggregating)
        {
            if (has_visit_global_agg)
                return current_node;
            has_visit_global_agg = true;

            const auto & agg_step = dynamic_cast<const AggregatingStep &>(*aggregation->getStep());

            PlanNodePtr child_node = update_plan_node_until_bottom_join(current_node->getChildren()[0]);

            auto new_global_agg_desc = agg_step.getAggregates();

            // mapping argument_names of global_aggregate.
            for (auto & agg_desc : new_global_agg_desc)
            {
                agg_desc.argument_names = symbol_mapper.map(agg_desc.argument_names);
                if (getClassOfAggFunc(agg_desc.function->getName()) == AggFuncClass::NEED_MERGE)
                {
                    AggregateFunctionProperties properties;
                    DataTypes arguments_types;
                    auto name_to_type = child_node->getOutputNamesToTypes();
                    for (const auto & argument_name : agg_desc.argument_names)
                        arguments_types.emplace_back(name_to_type.at(argument_name));
                    agg_desc.function = AggregateFunctionFactory::instance().get(getMergeName(agg_desc.function->getName()), arguments_types, agg_desc.parameters, properties);
                }
            }

            LOG_DEBUG(getLogger("test"), "create global_agg={}, keys={}", formatS0(new_global_agg_desc), fmt::join(agg_step.getKeys(), ","));

            auto new_global_agg_step = std::make_shared<AggregatingStep>(
                child_node->getCurrentDataStream(),
                agg_step.getKeys(),
                agg_step.getKeysNotHashed(),
                new_global_agg_desc,
                agg_step.getGroupingSetsParams(),
                agg_step.isFinal(),
                agg_step.getStagePolicy(),
                SortDescription{agg_step.getGroupBySortDescription()},
                agg_step.getGroupings(),
                agg_step.needOverflowRow(),
                agg_step.shouldProduceResultsInOrderOfBucketNumber(),
                agg_step.isNoShuffle(),
                agg_step.isStreamingForCache(),
                agg_step.getHints());

            return AggregatingNode::createPlanNode(aggregation->getId(), std::move(new_global_agg_step), {child_node});
        }
        else if (current_node->getType() == IQueryPlanStep::Type::Projection)
        {
            if (has_visit_join && !push_through_final_projection)
                return current_node;

            const auto & projection_step = dynamic_cast<const ProjectionStep &>(*current_node->getStep());

            if (current_node->getChildren()[0]->getType() != IQueryPlanStep::Type::Join)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "projection must be followed by join!");

            if (push_projection)
                proj = current_node;

            PlanNodePtr child_node = update_plan_node_until_bottom_join(current_node->getChildren()[0]);

            if (push_through_final_projection)
            {
                auto child_name_to_type = child_node->getCurrentDataStream().getNamesToTypes();

                Assignments new_assignments = projection_step.getAssignments();
                NameToType new_name_to_type = projection_step.getNameToType();

                NameSet new_name_from_local_agg;
                for (const auto & [k, v] : global_argument_name_to_local)
                {
                    // convert xx to inter#xx, because the local agg is push through current projection.
                    if (new_assignments.contains(k) && child_name_to_type.contains(v))
                    {
                        new_assignments.erase(k);
                        new_assignments.emplace(v, std::make_shared<ASTIdentifier>(v));
                        new_name_to_type.erase(k);
                        new_name_to_type.emplace(v, child_name_to_type.at(v));
                    }
                }

                auto new_projection_step = std::make_shared<ProjectionStep>(
                    child_node->getCurrentDataStream(),
                    new_assignments,
                    new_name_to_type,
                    projection_step.isFinalProject(),
                    projection_step.isIndexProject(),
                    projection_step.getHints());

                return ProjectionNode::createPlanNode(rule_context.context->nextNodeId(), std::move(new_projection_step), {child_node});
            }

            if (push_projection)
                return child_node;

            Assignments new_assignments;
            for (const auto & [name, ast] : projection_step.getAssignments())
                new_assignments.emplace(symbol_mapper.map(name), symbol_mapper.map(ast)); // TODO: only map assignment.second with multiIf?

            NameToType new_name_to_type;
            for (const auto & [name, type] : projection_step.getNameToType())
                new_name_to_type.emplace(symbol_mapper.map(name), type);

            auto new_projection_step = std::make_shared<ProjectionStep>(
                child_node->getCurrentDataStream(),
                new_assignments,
                new_name_to_type,
                projection_step.isFinalProject(),
                projection_step.isIndexProject(),
                projection_step.getHints());

            return ProjectionNode::createPlanNode(rule_context.context->nextNodeId(), std::move(new_projection_step), {child_node});
        }
        else if (current_node->getType() == IQueryPlanStep::Type::Join)
        {
            has_visit_join = true;
            const auto & join = current_node;
            const auto & join_step = dynamic_cast<const JoinStep &>(*join->getStep());

            PlanNodePtr left_child_node = join->getChildren()[0];
            PlanNodePtr right_child_node = join->getChildren()[1];
            if (join->getId() == bottom_join_id)
            {
                PlanNodePtr node_below_local_agg;
                if (push_projection)
                {
                    const auto & proj_step = static_cast<const ProjectionStep &>(*proj->getStep());
                    Assignments new_assignments;
                    NameToType new_name_to_type;
                    auto child_name_to_type = join->getChildren()[bottom_join_child_index]->getCurrentDataStream().getNamesToTypes();
                    for (const auto & assignment : proj_step.getAssignments())
                    {
                        if (Utils::isIdentity(assignment) && !child_name_to_type.contains(assignment.first))
                            continue;
                        new_assignments.emplace_back(assignment);
                        new_name_to_type.emplace(assignment.first, proj_step.getNameToType().at(assignment.first));
                    }
                    for (const auto & [name, type] : child_name_to_type)
                    {
                        if (!new_assignments.contains(name))
                        {
                            new_assignments.emplace(name, std::make_shared<ASTIdentifier>(name));
                            new_name_to_type.emplace(name, type);
                        }
                    }

                    auto new_proj_step = std::make_shared<ProjectionStep>(
                        join->getChildren()[bottom_join_child_index]->getCurrentDataStream(),
                        new_assignments,
                        new_name_to_type,
                        proj_step.isFinalProject(),
                        proj_step.isIndexProject(),
                        proj_step.getHints());
                    //
                    node_below_local_agg = ProjectionNode::createPlanNode(
                        rule_context.context->nextNodeId(), std::move(new_proj_step), {join->getChildren()[bottom_join_child_index]});
                }
                else
                {
                    node_below_local_agg = join->getChildren()[bottom_join_child_index];
                }

                // mapping column_name of local_aggregate.
                auto new_s1 = s1;
                for (auto & agg_desc : new_s1)
                {
                    agg_desc.column_name = symbol_mapper.map(agg_desc.column_name);

                    if (getClassOfAggFunc(agg_desc.function->getName()) == AggFuncClass::NEED_MERGE)
                    {
                        AggregateFunctionProperties properties;
                        DataTypes arguments_types;
                        auto name_to_type = node_below_local_agg->getOutputNamesToTypes();
                        for (const auto & argument_name : agg_desc.argument_names)
                            arguments_types.emplace_back(name_to_type.at(argument_name));
                        agg_desc.function = AggregateFunctionFactory::instance().get(getStateName(agg_desc.function->getName()), arguments_types, agg_desc.parameters, properties);
                    }
                }

                std::shared_ptr<AggregatingStep> local_agg_step
                    = createLocalAggregate(node_below_local_agg->getCurrentDataStream(), new_s1, {g1}, rule_context.context);

                if (bottom_join_child_index == 0)
                {
                    left_child_node = AggregatingNode::createPlanNode(
                        rule_context.context->nextNodeId(), std::move(local_agg_step), {node_below_local_agg});
                }
                else
                {
                    right_child_node = AggregatingNode::createPlanNode(
                        rule_context.context->nextNodeId(), std::move(local_agg_step), {node_below_local_agg});
                }
            }
            else
            {
                left_child_node = update_plan_node_until_bottom_join(join->getChildren()[0]);
                right_child_node = update_plan_node_until_bottom_join(join->getChildren()[1]);
            }

            ColumnsWithTypeAndName output_header;
            for (const auto & input_stream : {left_child_node->getCurrentDataStream(), right_child_node->getCurrentDataStream()})
            {
                for (const auto & header : input_stream.header.getColumnsWithTypeAndName())
                {
                    output_header.emplace_back(header);
                }
            }
            auto new_join_step = std::make_shared<JoinStep>(
                DataStreams{left_child_node->getCurrentDataStream(), right_child_node->getCurrentDataStream()},
                DataStream{output_header},
                join_step.getKind(),
                join_step.getStrictness(),
                join_step.getMaxStreams(),
                join_step.getKeepLeftReadInOrder(),
                join_step.getLeftKeys(),
                join_step.getRightKeys(),
                join_step.getKeyIdsNullSafe(),
                join_step.getFilter(),
                join_step.isHasUsing(),
                join_step.getRequireRightKeys(),
                join_step.getAsofInequality(),
                join_step.getDistributionType(),
                join_step.getJoinAlgorithm(),
                join_step.isMagic(),
                join_step.isOrdered(),
                join_step.isSimpleReordered(),
                join_step.getRuntimeFilterBuilders(),
                join_step.getHints());

            return JoinNode::createPlanNode(join->getId(), symbol_mapper.map(*new_join_step), {left_child_node, right_child_node});
        }
        else
        {
            return current_node;
        }
    };

    return update_plan_node_until_bottom_join(aggregation);
}

bool canAggPushDown(const LocalGroupByTarget & target, RuleContext & context)
{
    LOG_DEBUG(
        getLogger("test"),
        "judge local group by target, join_id={}, index={}, g0={}, s0={}, join_layer={}, push_through_final_projection={}",
        target.bottom_join->getId(),
        target.bottom_join_child_index,
        fmt::join(target.keys, ","),
        formatS0(target.aggs),
        target.join_layer,
        target.push_through_final_projection);

    const Settings & settings = context.context->getSettingsRef();
    String blocklist = settings.eager_agg_join_id_blocklist; // join_id
    Poco::StringTokenizer tokenizer(blocklist, ",", 0x11);
    if (tokenizer.has(std::to_string(target.bottom_join->getId())))
        return false;

    String whitelist = settings.eager_agg_join_id_whitelist;
    Poco::StringTokenizer tokenizer2(whitelist, ",", 0x11); // join_id-child_index
    if (tokenizer2.count())
    {
        return tokenizer2.has(std::to_string(target.bottom_join->getId()) + "-" + std::to_string(target.bottom_join_child_index));
    }


    const auto & bottom_node = target.bottom_join->getChildren()[target.bottom_join_child_index];
    auto bottom_stat = CardinalityEstimator::estimate(*bottom_node, context.cte_info, context.context);
    if (bottom_stat.value_or(nullptr))
    {
        const auto & child_stats = bottom_stat.value();
        double row_count = 1;
        bool all_unknown = true;

        std::vector<double> cndvs;
        for (const auto & key : target.keys)
        {
            if (child_stats->getSymbolStatistics().contains(key) && !child_stats->getSymbolStatistics(key)->isUnknown())
            {
                auto key_stats = child_stats->getSymbolStatistics(key)->copy();
                int null_rows = child_stats->getRowCount() == 0
                        || (static_cast<double>(key_stats->getNullsCount()) / child_stats->getRowCount() == 0.0)
                    ? 0
                    : 1;
                if (key_stats->getNdv() > 0)
                {
                    double cndv = static_cast<double>(key_stats->getNdv()) + null_rows;
                    cndvs.push_back(cndv);
                }

                all_unknown = false;
            }
        }
        if (all_unknown)
            return false;

        std::sort(cndvs.begin(), cndvs.end(), std::greater<double>());

        for (size_t i = 0; i < cndvs.size(); i++)
        {
            double cndv = cndvs[i];

            if (i != 0)
            {
                if (!target.keys.empty() && child_stats->getRowCount() > 1000000)
                {
                    if (row_count * cndv > child_stats->getRowCount() && cndv < cndvs[0] * 0.001)
                        continue;
                }
                row_count *= std::max(1.0, settings.multi_agg_keys_correlated_coefficient * cndv);
            }
            else
            {
                row_count *= cndv;
            }
        }

        row_count = std::min(row_count, static_cast<double>(child_stats->getRowCount()));

        if (settings.only_push_agg_with_functions && target.aggs.empty())
            return false;

        LOG_DEBUG(
            getLogger("test"),
            "Success pushdown Agg, agg_size={}, group_by_keys_size={}, new_row_count={}, old_row_count={}, ratio={}",
            target.aggs.size(),
            target.keys.size(),
            row_count,
            child_stats->getRowCount(),
            child_stats->getRowCount() / row_count);
        return child_stats->getRowCount() / row_count > settings.agg_push_down_threshold.value;
    }
    else if (settings.agg_push_down_threshold.value == 0)
        return true;
    return false;
}

TransformResult EagerAggregation::transformImpl(PlanNodePtr aggregation, const Captures &, RuleContext & rule_context)
{
    PlanNodePtr projection, join;
    PlanNodePtr parent_of_first_join;
    {
        PlanNodePtr node = aggregation;
        if (node->getChildren()[0]->getType() == IQueryPlanStep::Type::Projection)
        {
            projection = node->getChildren()[0];
            node = projection;
        }
        if (node->getChildren()[0]->getType() != IQueryPlanStep::Type::Join)
            return {};
        join = node->getChildren()[0];
        parent_of_first_join = node;
    }

    const auto & agg_step = dynamic_cast<const AggregatingStep &>(*aggregation->getStep());
    // const auto & join_step = dynamic_cast<const JoinStep &>(*join->getStep());

    auto names_from_left = join->getChildren()[0]->getCurrentDataStream().header.getNameSet();
    auto names_from_right = join->getChildren()[1]->getCurrentDataStream().header.getNameSet();

    AggregateDescriptions s1, s2;
    AggregateDescriptions composed_aggregates; // Can be further decomposed into s1 or s2.
    Names g1, g2;

    NameSet agg_step_keys_set(agg_step.getKeys().begin(), agg_step.getKeys().end()); // aggregate functions with agg_step_keys_set are no need to be push down.

    // Used to update the name of the path from local_aggregate to `global_aggregate(argument_names)`/`projection below global_aggregate`.
    NameToNameMap global_argument_name_to_local_only_projection_from_left;
    NameToNameMap global_argument_name_to_local_only_projection_from_right;

    const SymbolAllocatorPtr & symbol_allocator = rule_context.context->getSymbolAllocator();
    if (!decomposeAggJoin(
            agg_step.getAggregates(), agg_step_keys_set, names_from_left, names_from_right, composed_aggregates, s1, s2, g1, g2))
        return {};

    NameSet require_output_names_from_local_agg;
    {
        require_output_names_from_local_agg.insert(agg_step.getKeys().begin(), agg_step.getKeys().end());
        for (const auto & agg_desc : agg_step.getAggregates())
            require_output_names_from_local_agg.insert(agg_desc.argument_names.begin(), agg_desc.argument_names.end());
    }

    NameOrderedSet projection_require_symbols; // not empty means can be fully push down.
    NameSet projection_gene_symbols;
    if (projection)
    {
        const auto & projection_step = dynamic_cast<const ProjectionStep &>(*projection->getStep());

        if (!decomposeProjection(
                projection_step,
                composed_aggregates,
                agg_step_keys_set,
                names_from_left,
                names_from_right,
                global_argument_name_to_local_only_projection_from_left,
                global_argument_name_to_local_only_projection_from_right,
                s1,
                s2,
                projection_require_symbols,
                projection_gene_symbols,
                symbol_allocator))
            return {};

        if (projection_require_symbols.empty()) // no need push fully projection
        {
            for (const auto & assignment : projection_step.getAssignments())
            {
                auto symbols = SymbolsExtractor::extract(assignment.second);
                require_output_names_from_local_agg.insert(symbols.begin(), symbols.end());
            }
        }
    }

    PlanNodes results;

    LocalGroupByTargetMap target_map;

    if (!global_argument_name_to_local_only_projection_from_left.empty())
    {
        LocalGroupByTargetMap local_target_map = determineBottomJoin(
            parent_of_first_join,
            projection,
            s1,
            g1,
            projection_require_symbols,
            projection_gene_symbols,
            require_output_names_from_local_agg,
            global_argument_name_to_local_only_projection_from_left,
            rule_context);
        target_map.insert(local_target_map.begin(), local_target_map.end());
    }
    else if (!global_argument_name_to_local_only_projection_from_right.empty())
    {
        LocalGroupByTargetMap local_target_map = determineBottomJoin(
            parent_of_first_join,
            projection,
            s2,
            g2,
            projection_require_symbols,
            projection_gene_symbols,
            require_output_names_from_local_agg,
            global_argument_name_to_local_only_projection_from_right,
            rule_context);
        target_map.insert(local_target_map.begin(), local_target_map.end());
    }
    else
    {
        auto aggregates = agg_step.getAggregates();
        std::erase_if(aggregates, [&](const auto & aggregate) {
            for (const auto & name : aggregate.argument_names)
                if (agg_step_keys_set.contains(name))
                    return true;
            return false;
        });
        LocalGroupByTargetMap local_target_map = determineBottomJoin(
            parent_of_first_join,
            projection,
            aggregates,
            agg_step.getKeys(),
            projection_require_symbols,
            projection_gene_symbols,
            require_output_names_from_local_agg,
            {},
            rule_context);
        target_map.insert(local_target_map.begin(), local_target_map.end());
    }

    PlanNodePtr new_global_agg_node = aggregation;
    for (const auto & [target_id, target] : target_map)
    {
        if (!canAggPushDown(target, rule_context))
            continue;

        new_global_agg_node = doInsertAggregation(
            new_global_agg_node,
            target.aggs,
            target.keys,
            !projection_require_symbols.empty(),
            target_id,
            target.bottom_join_child_index,
            target.push_through_final_projection,
            symbol_allocator,
            rule_context);
        // GraphvizPrinter::printLogicalPlan(*new_global_agg_node, rule_context.context, fmt::format("target_id={}, index={}", target_id, target.bottom_join_child_index));
    }
    results.push_back(new_global_agg_node);


    return TransformResult{results};
}

const std::vector<RuleType> & EagerAggregation::blockRules() const
{
    static std::vector<RuleType> block{RuleType::EAGER_AGGREGATION};
    return block;
}

}
