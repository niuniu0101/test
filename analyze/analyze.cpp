#include "analyze.h"


/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query 
 */
std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse) {
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse)) {
        // 处理表名
        std::vector<std::string> &tables = x->tabs;
        query->tables = std::move(tables);
        /** TODO: 检查表是否存在 */
        for (auto &table_name: query->tables) {
            bool table_exists_flag = sm_manager_->db_.is_table(table_name);
            if (!table_exists_flag) {
                throw TableNotFoundError(table_name);
            }
        }

        // 处理target list，再target list中添加上表名，例如 a.id
        for (auto &item: x->select_list) {
            query->cols.emplace_back(TabCol{item->col->tab_name, item->col->col_name});
            query->agg_types.emplace_back(item->type);
            int last_agg_type = query->agg_types.back();
            if (last_agg_type != AGG_COL && item->alias.empty()) {
                std::string alias_str;
                std::string col_name = item->col->col_name;
                
                switch (last_agg_type) {
                    case AGG_COUNT: {
                        if (col_name.empty()) {
                            alias_str = "COUNT(*)";
                        } else {
                            alias_str = "COUNT(" + col_name + ")";
                        }
                        break;
                    }
                    case AGG_MAX: {
                        alias_str = "MAX(" + col_name + ")";
                        break;
                    }
                    case AGG_MIN: {
                        alias_str = "MIN(" + col_name + ")";
                        break;
                    }
                    case AGG_SUM: {
                        alias_str = "SUM(" + col_name + ")";
                        break;
                    }
                    case AGG_AVG: {
                        alias_str = "AVG(" + col_name + ")";
                        break;
                    }
                    default:
                        throw InternalError("Unexpected aggregate type!");
                }
                query->alias.emplace_back(alias_str);
            } else {
                query->alias.emplace_back(item->alias);
            }
        }

        std::vector<ColMeta> all_cols;
        // 得到扫描的表的所有列
        get_all_cols(query->tables, all_cols);

        if (query->cols.empty()) {
            // select all columns
            bool group_by_empty = x->group_bys.empty();
            bool having_empty = x->havings.empty();

            if (!group_by_empty || !having_empty) {
                throw InternalError("select * 不能包含聚合和分组子句！");
            }

            for (auto &col: all_cols) {
                query->cols.emplace_back(TabCol{col.tab_name, col.name});
            }
        } else {
            // 表名不存在，从列名推断表名；如果表名存在则对列做检查
            for (std::size_t i = 0; i < query->cols.size(); ++i) {
                // COUNT(*)
                bool is_count_all = (query->agg_types[i] == AGG_COUNT && query->cols[i].tab_name.empty() && query->cols[i].col_name.empty());
                if (is_count_all) {
                    continue;
                }
                // TODO 直接引用减少拷贝
                TabCol& current_col = query->cols[i];
                current_col = check_column(all_cols, current_col); // 列元数据校验
            }
        }

        // 处理 where 条件
        get_clause(x->conds, query->conds);

        // 处理 group by 条件
        for (auto &group_by: x->group_bys) {
            TabCol tc = TabCol{group_by->tab_name, group_by->col_name};
            query->group_bys.emplace_back(tc);
        }

        // 填充表名和列校验
        for (auto &tab_col: query->group_bys) {
            tab_col = check_column(all_cols, tab_col);
        }

        // 没有 group，不能出现 AGG_COL，必须都是聚合函数
        if (query->group_bys.empty()) {
            if (!x->havings.empty()) {
                throw InternalError("没有 GROUP BY 子句但是有 HAVING 子句！");
            }
            bool has_non_agg_col = false;
            bool has_agg_func = false;
            for (auto &agg_type: query->agg_types) {
                has_non_agg_col |= (agg_type == AGG_COL);
                has_agg_func |= (agg_type != AGG_COL);
                if (has_non_agg_col && has_agg_func) {
                    throw InternalError("没有 GROUP BY 子句且有聚合函数，但包含非聚合值！");
                }
            }
        } else {
            // SELECT 列表中不能出现没有在 GROUP BY 子句中的非聚集列
            // select id , score from grade group by course;
            for (std::size_t i = 0; i < query->cols.size(); ++i) {
                int current_agg_type = query->agg_types[i];
                if (current_agg_type == AGG_COL) {
                    auto &&pos = std::find_if(query->group_bys.begin(), query->group_bys.end(), [&](TabCol &tab_col) {
                        bool tab_match = tab_col.tab_name == query->cols[i].tab_name;
                        bool col_match = tab_col.col_name == query->cols[i].col_name;
                        return tab_match && col_match;
                    });
                    if (pos == query->group_bys.end()) {
                        throw InternalError("SELECT 列表中不能出现没有在 GROUP BY 子句中的非聚集列！");
                    }
                }
            }
        }

        // 处理 having 条件
        get_having_clause(x->havings, query->havings);

        // 处理 sortby 条件
        if (x->has_sort) {
            TabCol sort_col = TabCol{x->order->cols->tab_name, x->order->cols->col_name};
            query->sort_bys = check_column(all_cols, sort_col);
        }

        // 推断表名和检查左右类型是否匹配
        check_clause(query->tables, query->conds);
        check_clause(query->tables, query->havings);
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        /** TODO: */
        // 构造set_clauses
        for (auto &set: x->set_clauses) {
            // set语句只对某个表修改，不需要表名
            TabCol lhs = TabCol{"", set->col_name};
            Value rhs = convert_sv_value(set->val);
            bool is_incr = set->is_incr;
            query->set_clauses.emplace_back(SetClause{
                lhs, rhs, is_incr
            });
        }

        // 检查set左右值类型是否相同
        auto &tab_meta = sm_manager_->db_.get_table(x->tab_name);
        for (auto &set: query->set_clauses) {
            // 从表元信息中获取列元信息
            auto &&col_meta = tab_meta.get_col(set.lhs.col_name);
            int len = col_meta->len;
            
            // 兼容 int -> float
            if (col_meta->type == TYPE_FLOAT && set.rhs.type == TYPE_INT) {
                len = sizeof(float);
                set.rhs.set_float(static_cast<float>(set.rhs.int_val));
            } else if (col_meta->type != set.rhs.type) {
                std::string lhs_type_str = coltype2str(col_meta->type);
                std::string rhs_type_str = coltype2str(set.rhs.type);
                throw IncompatibleTypeError(lhs_type_str, rhs_type_str);
            }
            set.rhs.init_raw(len);
        }

        // 处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        // 处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        // 处理insert 的values值
        for (auto &sv_val: x->vals) {
            query->values.emplace_back(convert_sv_value(sv_val));
        }
    } else {
        // do nothing
    }
    query->parse = std::move(parse);
    return query;
}

TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string inferred_tab_name;
        for (auto &col: all_cols) {
            if (col.name == target.col_name) {
                if (!inferred_tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                inferred_tab_name = col.tab_name;
            }
        }
        if (inferred_tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = std::move(inferred_tab_name);
    } else {
        /** TODO: Make sure target column exists */
        bool found = false;
        for (auto &col: all_cols) {
            // select t.id from t,d where id = 1;
            bool tab_match = (col.tab_name == target.tab_name);
            bool col_match = (col.name == target.col_name);
            if (tab_match && col_match) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw ColumnNotFoundError(target.col_name);
        }
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name: tab_names) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr> > &sv_conds,
                         std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr: sv_conds) {
        Condition cond;
        cond.agg_type = AGG_COL;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        auto rhs_ptr = expr->rhs;
        
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(rhs_ptr)) {
            cond.is_rhs_val = true;
            cond.is_sub_query = false;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(rhs_ptr)) {
            cond.is_rhs_val = false;
            cond.is_sub_query = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        } else if (auto rhs_select = std::dynamic_pointer_cast<ast::SelectStmt>(rhs_ptr)) {
            // 子查询只能有一列
            // select * 需要进一步查表看看是否是单列
            // select * 和至少两个表，至少有两列，直接抛出异常
            // 这里应该只用保证单列就行，是否单行具体由算子检查
            bool select_list_is_empty = rhs_select->select_list.empty();
            if (select_list_is_empty) {
                throw InternalError("Operand should contain 1 column!");
                if (rhs_select->tabs.size() > 1) {
                    throw InternalError("Operand should contain 1 column!");
                }
                std::vector<ColMeta> all_cols;
                get_all_cols(rhs_select->tabs, all_cols);
                // select * 一个表但是包括多行
                if (all_cols.size() > 1) {
                    throw InternalError("Operand should contain 1 column!");
                }
                // 如果 select * 只有单列 col，则补全为 select col
                auto bound_expr = std::make_shared<ast::BoundExpr>(
                    std::make_shared<ast::Col>(all_cols[0].tab_name, all_cols[0].name), AGG_COL);
                rhs_select->select_list.emplace_back(std::move(bound_expr));
            }
            
            bool is_single_column_query = rhs_select->select_list.size() == 1;
            if (!is_single_column_query) {
                throw InternalError("Operand should contain 1 column!");
            }
            // 带有比较运算符的标量子查询右侧不一定是单一聚合函数，有可能是单列，只用保证返回单列单行就行
            // if (cond.op != OP_IN && rhs_select->select_list[0]->type == AGG_COL) {
            //     throw InternalError("Subquery returns more than 1 row!");
            // }
            cond.is_rhs_val = false;
            cond.is_sub_query = true;
            cond.sub_query = do_analyze(rhs_ptr);
        } else if (!expr->rhs_list.empty()) {
            bool is_in_op = cond.op == OP_IN;
            bool rhs_list_has_more_than_one = expr->rhs_list.size() > 1;
            bool rhs_list_is_empty = expr->rhs_list.empty();

            if (!is_in_op && (rhs_list_has_more_than_one || rhs_list_is_empty)) {
                throw InternalError("Operand should contain 1 column!");
            }
            
            cond.is_rhs_val = false;
            cond.is_sub_query = true;
            // > 只有一个 或者 in 有多个
            for (auto &value: expr->rhs_list) {
                cond.rhs_value_list.emplace_back(convert_sv_value(value));
            }
        }
        conds.emplace_back(cond);
    }
}

void Analyze::get_having_clause(const std::vector<std::shared_ptr<ast::HavingExpr> > &sv_conds,
                                std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr: sv_conds) {
        Condition cond;
        // having 语句左侧必须是聚合函数
        if (expr->lhs->type == AGG_COL) {
            throw InternalError("Having 语句左侧必须是聚合函数！");
        }
        cond.agg_type = expr->lhs->type;
        // 如果是 having count(*) > 1 这里为空
        cond.lhs_col = {.tab_name = expr->lhs->col->tab_name, .col_name = expr->lhs->col->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.is_sub_query = false;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            // 右边一定是数值
            throw InternalError("Having 语句右侧应该为数值！");
            // cond.is_rhs_val = false;
            // cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.emplace_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    // auto all_cols = get_all_cols(tab_names);
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    // Get raw values in where clause
    for (auto &cond: conds) {
        // count(*)
        // TODO having 支持子查询
        bool is_count_star = (cond.agg_type == AGG_COUNT && cond.lhs_col.tab_name.empty() && cond.lhs_col.col_name.empty());
        if (is_count_star) {
            if (cond.rhs_val.type == TYPE_INT) {
                cond.rhs_val.init_raw(sizeof(int));
            } else {
                std::string rhs_type_str = coltype2str(cond.rhs_val.type);
                throw IncompatibleTypeError("INT", rhs_type_str);
            }
            continue;
        }

        // Infer table name from column name
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        bool is_rhs_val_check = cond.is_rhs_val;
        bool is_sub_query_check = cond.is_sub_query;

        if (!is_rhs_val_check && !is_sub_query_check) {
            cond.rhs_col = check_column(all_cols, cond.rhs_col);
        }
        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        
        // 这里假设 where 语句左值必须为列值
        ColType lhs_type;
        int cond_agg_type = cond.agg_type;
        if (cond_agg_type == AGG_COUNT) {
            lhs_type = TYPE_INT;
        } else if (cond_agg_type == AGG_AVG) {
            lhs_type = TYPE_FLOAT;
        } else {
            lhs_type = lhs_col->type;
        }

        ColType rhs_type;
        if (cond.is_rhs_val) {
            // having count(course) > 1
            // having count(course) > 1.0
            // having count(course) > '1.0'
            // 左 char 右 int，左边在聚合算子内处理
            if (cond_agg_type == AGG_COUNT || cond_agg_type == AGG_AVG) {
                if (cond.rhs_val.type == TYPE_INT) {
                    cond.rhs_val.init_raw(sizeof(int));
                } else if (cond.rhs_val.type == TYPE_FLOAT) {
                    cond.rhs_val.init_raw(sizeof(float));
                } else if (cond.rhs_val.type == TYPE_STRING) {
                    cond.rhs_val.init_raw(lhs_col->len);
                } else {
                    throw InternalError("Unexpected data type!");
                }
            } else if (lhs_type == TYPE_FLOAT && cond.rhs_val.type == TYPE_INT) {
                float rhs_float_val = static_cast<float>(cond.rhs_val.int_val);
                cond.rhs_val.set_float(rhs_float_val);
                cond.rhs_val.init_raw(sizeof(float));
            } else {
                // 左边的类型可能和右边不同不能直接 col_len
                // 比如 where course > 0 having min(course) > 0.0
                if (cond.rhs_val.type == TYPE_INT) {
                    cond.rhs_val.init_raw(sizeof(int));
                } else if (cond.rhs_val.type == TYPE_FLOAT) {
                    cond.rhs_val.init_raw(sizeof(float));
                } else if (cond.rhs_val.type == TYPE_STRING) {
                    cond.rhs_val.init_raw(lhs_col->len);
                }
            }
            rhs_type = cond.rhs_val.type;
        } else if (cond.is_sub_query) {
            // 子查询是个值列表
            if (cond.sub_query == nullptr) {
                assert(!cond.rhs_value_list.empty());
                // 检查列表中值的类型是否都相同
                // int 可以转换为float
                for (auto &value: cond.rhs_value_list) {
                    if (lhs_type != value.type) {
                        if (lhs_type == TYPE_FLOAT && value.type == TYPE_INT) {
                            value.set_float(static_cast<float>(value.int_val));
                        } else {
                            std::string lhs_type_str = coltype2str(lhs_type);
                            std::string rhs_type_str = coltype2str(value.type);
                            throw IncompatibleTypeError(lhs_type_str, rhs_type_str);
                        }
                    }
                    value.init_raw(lhs_col->len);
                }
                continue;
            }
            // 子查询右边是唯一列
            auto &table_name = cond.sub_query->cols[0].tab_name;
            auto &col_name = cond.sub_query->cols[0].col_name;
            int sub_query_agg_type = cond.sub_query->agg_types[0];

            // where name = (select count(*) from t);
            if (sub_query_agg_type == AGG_COUNT) {
                rhs_type = (lhs_type == TYPE_FLOAT) ? TYPE_FLOAT : TYPE_INT;
            } else if (sub_query_agg_type == AGG_AVG) {
                // AVG 的右边类型固定为 float
                rhs_type = TYPE_FLOAT;
            } else {
                // where name = (select MAX(score) from grade);
                TabMeta &rhs_tab = sm_manager_->db_.get_table(table_name);
                auto rhs_col = rhs_tab.get_col(col_name);
                if (lhs_type == TYPE_FLOAT && rhs_col->type == TYPE_INT) {
                    rhs_type = TYPE_FLOAT;
                } else {
                    rhs_type = rhs_col->type;
                }
            }
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }

        if (lhs_type != rhs_type) {
            std::string lhs_type_str = coltype2str(lhs_type);
            std::string rhs_type_str = coltype2str(rhs_type);
            throw IncompatibleTypeError(lhs_type_str, rhs_type_str);
        }
    }
}

Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
        {ast::SV_OP_IN, OP_IN}
    };
    return m.at(op);
}