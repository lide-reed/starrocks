// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

package com.starrocks.sql.optimizer.task;

import com.starrocks.sql.optimizer.ExpressionContext;
import com.starrocks.sql.optimizer.GroupExpression;
import com.starrocks.sql.optimizer.statistics.StatisticsCalculator;

/**
 * DeriveStatsTask derives any stats needed for costing a GroupExpression. This will
 * recursively derive stats and lazily collect stats for column needed.
 * <p>
 * This implementation refer to ORCA paper.
 */
public class DeriveStatsTask extends OptimizerTask implements Cloneable {
    private final GroupExpression groupExpression;

    public DeriveStatsTask(TaskContext context, GroupExpression expression) {
        super(context);
        this.groupExpression = expression;
    }

    // Shallow Clone here
    // We don't need to clone requiredColumns and groupExpression
    @Override
    public Object clone() {
        DeriveStatsTask task = null;
        try {
            task = (DeriveStatsTask) super.clone();
        } catch (CloneNotSupportedException ignored) {
        }
        return task;
    }

    @Override
    public String toString() {
        return "DeriveStatsTask for groupExpression " + groupExpression;
    }

    @Override
    public void execute() {
        if (groupExpression.isStatsDerived() || groupExpression.isUnused()) {
            return;
        }

        boolean needDerivedChildren = false;
        // If we haven't got enough stats to compute the current stats, derive them from the child first.
        // For CTE, we need derive left tree first, then derive right
        for (int i = groupExpression.arity() - 1; i >= 0; --i) {
            // TODO(kks): Currently we use the first child expression in the child
            // group to derive stats, in the future we may want to pick the one with
            // the highest confidence refer to ORCA paper
            GroupExpression childExpression = groupExpression.getInputs().get(i).
                    getFirstLogicalExpression();
            if (!childExpression.isStatsDerived()) {
                // The child group has not derived stats could happen when we do top-down
                // stats derivation for the first time or a new child group is just
                // generated by join order enumeration
                if (!needDerivedChildren) {
                    needDerivedChildren = true;
                    // Derive stats for root later
                    pushTask((DeriveStatsTask) clone());
                }
                pushTask(new DeriveStatsTask(context, childExpression));
            }
        }

        // We'll derive for the current group after deriving all stats columns of children
        if (needDerivedChildren) {
            return;
        }

        ExpressionContext expressionContext = new ExpressionContext(groupExpression);
        StatisticsCalculator statisticsCalculator = new StatisticsCalculator(expressionContext,
                context.getOptimizerContext().getColumnRefFactory(), context.getOptimizerContext());
        statisticsCalculator.estimatorStats();
        groupExpression.getGroup().setStatistics(expressionContext.getStatistics());

        groupExpression.setStatsDerived();
    }
}