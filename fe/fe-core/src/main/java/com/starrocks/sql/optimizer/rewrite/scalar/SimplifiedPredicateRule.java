// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

package com.starrocks.sql.optimizer.rewrite.scalar;

import com.google.common.collect.Lists;
import com.google.common.collect.Sets;
import com.starrocks.analysis.Expr;
import com.starrocks.catalog.Function;
import com.starrocks.catalog.FunctionSet;
import com.starrocks.catalog.ScalarFunction;
import com.starrocks.catalog.Type;
import com.starrocks.sql.optimizer.operator.scalar.BinaryPredicateOperator;
import com.starrocks.sql.optimizer.operator.scalar.CallOperator;
import com.starrocks.sql.optimizer.operator.scalar.CaseWhenOperator;
import com.starrocks.sql.optimizer.operator.scalar.CompoundPredicateOperator;
import com.starrocks.sql.optimizer.operator.scalar.ConstantOperator;
import com.starrocks.sql.optimizer.operator.scalar.InPredicateOperator;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;
import com.starrocks.sql.optimizer.rewrite.EliminateNegationsRewriter;
import com.starrocks.sql.optimizer.rewrite.ScalarOperatorRewriteContext;

import java.util.Arrays;
import java.util.List;
import java.util.Set;
import java.util.stream.Collectors;

public class SimplifiedPredicateRule extends BottomUpScalarOperatorRewriteRule {
    private static final EliminateNegationsRewriter ELIMINATE_NEGATIONS_REWRITER = new EliminateNegationsRewriter();

    //
    // Simplified Case When Predicate
    // @Fixme: improve it:
    // 1. remove always false conditions
    // 2. return direct if first always true
    @Override
    public ScalarOperator visitCaseWhenOperator(CaseWhenOperator operator, ScalarOperatorRewriteContext context) {
        ScalarOperator result = simplifiedCaseWhenConstClause(operator);
        if (!(result instanceof CaseWhenOperator)) {
            return result;
        }
        return simplifiedCaseWhenToIfFunction((CaseWhenOperator) result);

    }

    // Trans one case when to if function, if function fast than case-when in BE
    // example:
    // case xx when 1 then 2 end => if (xx = 1, 2, NULL)
    ScalarOperator simplifiedCaseWhenToIfFunction(CaseWhenOperator operator) {
        if (operator.getWhenClauseSize() != 1) {
            return operator;
        }

        List<ScalarOperator> args = Lists.newArrayList();
        if (operator.hasCase()) {
            args.add(new BinaryPredicateOperator(BinaryPredicateOperator.BinaryType.EQ, operator.getCaseClause(),
                    operator.getWhenClause(0)));
        } else {
            args.add(operator.getWhenClause(0));
        }

        args.add(operator.getThenClause(0));

        if (operator.hasElse()) {
            args.add(operator.getElseClause());
        } else {
            args.add(ConstantOperator.createNull(operator.getType()));
        }

        Type[] argTypes = args.stream().map(ScalarOperator::getType).toArray(Type[]::new);
        Function fn = Expr.getBuiltinFunction(FunctionSet.IF, argTypes, Function.CompareMode.IS_IDENTICAL);

        if (fn == null) {
            return operator;
        }
        if (operator.getChildren().stream().anyMatch(s -> s.getType().isDecimalV3())) {
            Function decimalFn = ScalarFunction.createVectorizedBuiltin(fn.getId(), fn.getFunctionName().getFunction(),
                    Arrays.stream(argTypes).collect(Collectors.toList()), fn.hasVarArgs(), operator.getType());
            return new CallOperator("if", operator.getType(), args, decimalFn);
        }

        return new CallOperator("if", operator.getType(), args, fn);
    }

    ScalarOperator simplifiedCaseWhenConstClause(CaseWhenOperator operator) {
        // 0. if all result is same, direct return
        if (operator.hasElse()) {
            Set<ScalarOperator> result = Sets.newHashSet();
            for (int i = 0; i < operator.getWhenClauseSize(); i++) {
                result.add(operator.getThenClause(i));
            }
            result.add(operator.getElseClause());

            if (result.size() == 1) {
                return operator.getElseClause();
            }
        }

        // 1. check caseClause
        ConstantOperator caseOp;
        if (operator.hasCase()) {
            if (!operator.getCaseClause().isConstantRef()) {
                return operator;
            }

            caseOp = (ConstantOperator) operator.getCaseClause();
        } else {
            caseOp = ConstantOperator.createBoolean(true);
        }

        // 2. return if caseClause is NullLiteral
        if (caseOp.isNull()) {
            if (operator.hasElse()) {
                return operator.getElseClause();
            }

            return ConstantOperator.createNull(operator.getType());
        }

        // 3. caseClause is constant now, check constant whenClause
        for (int i = 0; i < operator.getWhenClauseSize(); i++) {
            if (!operator.getWhenClause(i).isConstantRef()) {
                // if when isn't constant, return direct
                return operator;
            }
        }

        // 4. caseClause is constant, all whenClause is constant
        for (int i = 0; i < operator.getWhenClauseSize(); i++) {
            ConstantOperator when = (ConstantOperator) operator.getWhenClause(i);

            if (when.isNull()) {
                continue;
            }

            if (0 == caseOp.compareTo(when)) {
                return operator.getThenClause(i);
            }
        }

        if (operator.hasElse()) {
            return operator.getElseClause();
        }

        return ConstantOperator.createNull(operator.getType());
    }

    //
    // Simplified Compound Predicate
    //
    // example:
    //        Compound(And)
    //        /      \
    //      true      false
    //
    // After rule:
    //         false
    //
    // example:
    //        Binary(or)
    //        /      \
    // A(Column)      true
    //
    // After rule:
    //          true
    //
    @Override
    public ScalarOperator visitCompoundPredicate(CompoundPredicateOperator predicate,
                                                 ScalarOperatorRewriteContext context) {
        // collect constant
        List<ConstantOperator> constantChildren = predicate.getChildren().stream().filter(ScalarOperator::isConstantRef)
                .map(d -> (ConstantOperator) d).collect(Collectors.toList());

        switch (predicate.getCompoundType()) {
            case AND: {
                if (constantChildren.stream().anyMatch(d -> (!d.isNull() && !d.getBoolean()))) {
                    // any false
                    return ConstantOperator.createBoolean(false);
                } else if (constantChildren.size() == 1) {
                    if (constantChildren.get(0).isNull()) {
                        // xxx and null
                        return predicate;
                    }

                    // (true and xxx)/(xxx and true)
                    ScalarOperator c0 = predicate.getChild(0);
                    if (c0.isConstantRef() && ((ConstantOperator) c0).getBoolean()) {
                        return predicate.getChild(1);
                    }

                    return predicate.getChild(0);
                } else if (constantChildren.size() == 2) {
                    if (constantChildren.stream().anyMatch(ConstantOperator::isNull)) {
                        // (true and null) or (null and null)
                        return ConstantOperator.createNull(Type.BOOLEAN);
                    }
                    // true and true
                    return ConstantOperator.createBoolean(true);
                }

                return predicate;
            }
            case OR: {
                if (constantChildren.stream().anyMatch(d -> (!d.isNull() && d.getBoolean()))) {
                    // any true
                    return ConstantOperator.createBoolean(true);
                } else if (constantChildren.size() == 1) {
                    if (constantChildren.get(0).isNull()) {
                        // xxx or null
                        return predicate;
                    }

                    // (false or xxx)/(xxx or false)
                    ScalarOperator c0 = predicate.getChild(0);
                    if (c0.isConstantRef() && !((ConstantOperator) c0).getBoolean()) {
                        return predicate.getChild(1);
                    }

                    return predicate.getChild(0);
                } else if (constantChildren.size() == 2) {
                    if (constantChildren.stream().anyMatch(ConstantOperator::isNull)) {
                        // (false or null) or (null or null)
                        return ConstantOperator.createNull(Type.BOOLEAN);
                    }
                    // false or false
                    return ConstantOperator.createBoolean(false);
                }

                return predicate;
            }
            case NOT: {
                ScalarOperator child = predicate.getChild(0);
                ScalarOperator result = child.accept(ELIMINATE_NEGATIONS_REWRITER, null);
                if (null == result) {
                    return predicate;
                }

                return result;
            }
        }

        return predicate;
    }

    //
    // example: a in ('xxxx')
    //
    // after: a = 'xxxx'
    //
    @Override
    public ScalarOperator visitInPredicate(InPredicateOperator predicate, ScalarOperatorRewriteContext context) {
        if (predicate.getChildren().size() != 2) {
            return predicate;
        }

        // like a in ("xxxx");
        if (predicate.isNotIn()) {
            return new BinaryPredicateOperator(BinaryPredicateOperator.BinaryType.NE, predicate.getChildren());
        } else {
            return new BinaryPredicateOperator(BinaryPredicateOperator.BinaryType.EQ, predicate.getChildren());
        }
    }

    //Simplify the comparison result of the same column
    //eg a >= a with not nullable transform to true constant;
    @Override
    public ScalarOperator visitBinaryPredicate(BinaryPredicateOperator predicate,
                                               ScalarOperatorRewriteContext context) {
        if (predicate.getChild(0).isVariable() && predicate.getChild(0).equals(predicate.getChild(1))) {
            if (predicate.getBinaryType().equals(BinaryPredicateOperator.BinaryType.EQ_FOR_NULL)) {
                return ConstantOperator.createBoolean(true);
            }
        }
        return predicate;
    }

    @Override
    public ScalarOperator visitCall(CallOperator call, ScalarOperatorRewriteContext context) {
        if (FunctionSet.IF.equalsIgnoreCase(call.getFnName())) {
            return ifCall(call);
        } else if (FunctionSet.IF_NULL.equalsIgnoreCase(call.getFnName())) {
            return ifNull(call);
        }

        return call;
    }

    private static ScalarOperator ifNull(CallOperator call) {
        if (!call.getChild(0).isConstantRef()) {
            return call;
        }

        return ((ConstantOperator) call.getChild(0)).isNull() ? call.getChild(1) : call.getChild(0);
    }

    private static ScalarOperator ifCall(CallOperator call) {
        if (!call.getChild(0).isConstantRef()) {
            return call;
        }

        return ((ConstantOperator) call.getChild(0)).getBoolean() ? call.getChild(1) : call.getChild(2);
    }
}
