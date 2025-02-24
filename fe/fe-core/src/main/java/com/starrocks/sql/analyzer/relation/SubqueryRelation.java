// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.
package com.starrocks.sql.analyzer.relation;

public class SubqueryRelation extends Relation {
    private final String name;
    private final QueryRelation query;

    public SubqueryRelation(String name, QueryRelation query) {
        this.name = name;
        this.query = query;
        // The order by is meaningless in subquery
        if (this.query instanceof SelectRelation && !((SelectRelation) this.query).hasLimit()) {
            SelectRelation qs = (SelectRelation) this.query;
            qs.clearOrder();
        }
    }

    public String getName() {
        return name;
    }

    public QueryRelation getQuery() {
        return query;
    }

    @Override
    public String toString() {
        return name == null ? "anonymous" : name;
    }

    public <R, C> R accept(RelationVisitor<R, C> visitor, C context) {
        return visitor.visitSubquery(this, context);
    }
}
