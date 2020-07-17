/*-------------------------------------------------------------------------
 * Copyright (C) 2019, 4paradigm
 * udf_registry.cc
 *
 * Author: chenjing
 * Date: 2019/11/26
 *--------------------------------------------------------------------------
 **/
#include "udf/udf_registry.h"
#include <memory>
#include <sstream>
#include "vm/transform.h"

namespace fesql {
namespace udf {

Status CompositeRegistry::Transform(UDFResolveContext* ctx,
                                    node::ExprNode** result) {
    if (sub_.size() == 1) {
        return sub_[0]->Transform(ctx, result);
    }
    std::string errs;
    std::vector<node::ExprNode*> candidates;
    for (auto registry : sub_) {
        node::ExprNode* cand = nullptr;
        auto status = registry->Transform(ctx, &cand);
        if (status.isOK()) {
            candidates.push_back(cand);
            break;
        } else {
            errs.append("\n --> ").append(status.msg);
        }
    }
    CHECK_TRUE(!candidates.empty(),
               "Fail to transform udf with underlying errors: ", errs);
    *result = candidates[0];
    return Status::OK();
}

Status ExprUDFRegistry::ResolveFunction(UDFResolveContext* ctx,
                                        node::FnDefNode** result) {
    // env check
    if (ctx->over() != nullptr) {
        CHECK_TRUE(allow_window_,
                   "Called in aggregate window is not supported: ", name());
    } else {
        CHECK_TRUE(allow_project_, "Called in project is not supported",
                   name());
    }

    // find generator with specified input argument types
    int variadic_pos = -1;
    std::shared_ptr<ExprUDFGenBase> gen_ptr;
    std::string signature;
    CHECK_STATUS(reg_table_.Find(ctx, &gen_ptr, &signature, &variadic_pos),
                 "Fail to resolve fn name \"", name(), "\"");

    DLOG(INFO) << "Resolve expression udf \"" << name() << "\" -> " << name()
              << "(" << signature << ")";

    // construct fn def node:
    // def fn(arg0, arg1, ...argN):
    //     return gen_impl(arg0, arg1, ...argN)
    auto nm = ctx->node_manager();
    auto func_header_param_list = nm->MakeFnListNode();
    std::vector<node::ExprNode*> func_params;
    for (size_t i = 0; i < ctx->arg_size(); ++i) {
        std::string arg_name = "arg_" + std::to_string(i);
        auto arg_type = ctx->arg(i)->GetOutputType();

        func_header_param_list->AddChild(
            nm->MakeFnParaNode(arg_name, arg_type));

        auto arg_expr = nm->MakeExprIdNode(arg_name);
        func_params.emplace_back(arg_expr);
        arg_expr->SetOutputType(arg_type);
    }
    auto ret_expr = gen_ptr->gen(ctx, func_params);
    CHECK_TRUE(ret_expr != nullptr && !ctx->HasError(),
               "Fail to create expr udf: ", ctx->GetError());

    vm::SchemaSourceList empty;
    vm::SchemasContext empty_schema(empty);
    vm::ResolveFnAndAttrs resolver(false, &empty_schema, nm, ctx->library());
    node::ExprNode* new_ret_expr = nullptr;
    auto status = resolver.Visit(ret_expr, &new_ret_expr);
    if (!status.isOK()) {
        LOG(WARNING) << status.msg;
    }

    auto ret_stmt = nm->MakeReturnStmtNode(new_ret_expr);
    auto body = nm->MakeFnListNode();
    body->AddChild(ret_stmt);
    auto header = nm->MakeFnHeaderNode(name(), func_header_param_list,
                                       new_ret_expr->GetOutputType());
    auto fn_def = nm->MakeFnDefNode(header, body);

    *result = reinterpret_cast<node::FnDefNode*>(
        nm->MakeUDFDefNode(reinterpret_cast<node::FnNodeFnDef*>(fn_def)));
    return Status::OK();
}

Status ExprUDFRegistry::Register(
    const std::vector<std::string>& args,
    std::shared_ptr<ExprUDFGenBase> gen_impl_func) {
    return reg_table_.Register(args, gen_impl_func);
}

Status LLVMUDFRegistry::ResolveFunction(UDFResolveContext* ctx,
                                        node::FnDefNode** result) {
    // env check
    if (ctx->over() != nullptr) {
        CHECK_TRUE(allow_window_,
                   "Called in aggregate window is not supported: ", name());
    } else {
        CHECK_TRUE(allow_project_, "Called in project is not supported",
                   name());
    }

    // find generator with specified input argument types
    int variadic_pos = -1;
    std::shared_ptr<LLVMUDFGenBase> gen_ptr;
    std::string signature;
    CHECK_STATUS(reg_table_.Find(ctx, &gen_ptr, &signature, &variadic_pos),
                 "Fail to resolve fn name \"", name(), "\"");

    DLOG(INFO) << "Resolve llvm codegen udf \"" << name() << "\" -> " << name()
              << "(" << signature << ")";

    std::vector<const node::TypeNode*> arg_types;
    for (size_t i = 0; i < ctx->arg_size(); ++i) {
        auto arg_type = ctx->arg(i)->GetOutputType();
        CHECK_TRUE(arg_type != nullptr, i,
                   "th argument node type is unknown: ", name());
        arg_types.push_back(arg_type);
    }
    auto return_type = gen_ptr->fixed_ret_type() == nullptr
                           ? gen_ptr->infer(ctx, arg_types)
                           : gen_ptr->fixed_ret_type();
    CHECK_TRUE(return_type != nullptr && !ctx->HasError(),
               "Infer node return type failed: ", ctx->GetError());

    auto udf_def = dynamic_cast<node::UDFByCodeGenDefNode*>(
        ctx->node_manager()->MakeUDFByCodeGenDefNode(arg_types, return_type));
    udf_def->SetGenImpl(gen_ptr);
    *result = udf_def;
    return Status::OK();
}

Status LLVMUDFRegistry::Register(
    const std::vector<std::string>& args,
    std::shared_ptr<LLVMUDFGenBase> gen_impl_func) {
    return reg_table_.Register(args, gen_impl_func);
}

Status ExternalFuncRegistry::ResolveFunction(UDFResolveContext* ctx,
                                             node::FnDefNode** result) {
    // env check
    if (ctx->over() != nullptr) {
        CHECK_TRUE(allow_window_,
                   "Called in aggregate window is not supported: ", name());
    } else {
        CHECK_TRUE(allow_project_, "Called in project is not supported",
                   name());
    }

    // find generator with specified input argument types
    int variadic_pos = -1;
    node::ExternalFnDefNode* external_def = nullptr;
    std::string signature;
    CHECK_STATUS(reg_table_.Find(ctx, &external_def, &signature, &variadic_pos),
                 "Fail to resolve fn name \"", name(), "\"");

    CHECK_TRUE(external_def->ret_type() != nullptr,
               "No return type specified for ", external_def->function_name());
    DLOG(INFO) << "Resolve udf \"" << name() << "\" -> "
              << external_def->function_name() << "(" << signature << ")";
    *result = external_def;
    return Status::OK();
}

Status ExternalFuncRegistry::Register(const std::vector<std::string>& args,
                                      node::ExternalFnDefNode* func) {
    return reg_table_.Register(args, func);
}

Status SimpleUDFRegistry::ResolveFunction(UDFResolveContext* ctx,
                                          node::FnDefNode** result) {
    int variadic_pos = -1;
    std::string signature;
    node::UDFDefNode* udf_def = nullptr;
    CHECK_STATUS(reg_table_.Find(ctx, &udf_def, &signature, &variadic_pos));
    *result = udf_def;
    return Status::OK();
}

Status SimpleUDFRegistry::Register(const std::vector<std::string>& args,
                                   node::UDFDefNode* udaf_def) {
    return reg_table_.Register(args, udaf_def);
}

Status SimpleUDAFRegistry::ResolveFunction(UDFResolveContext* ctx,
                                           node::FnDefNode** result) {
    CHECK_TRUE(ctx->arg_size() == 1,
               "Input arg_num of simple udaf should be 1: ", name());

    auto arg_type = ctx->arg(0)->GetOutputType();
    CHECK_TRUE(arg_type != nullptr && (arg_type->base_ == node::kIterator ||
                                       arg_type->base_ == node::kList),
               "Illegal input type for simple udaf: ",
               arg_type == nullptr ? "null" : arg_type->GetName());
    arg_type = arg_type->GetGenericType(0);

    auto iter = reg_table_.find(arg_type->GetName());
    CHECK_TRUE(iter != reg_table_.end(),
               "Fail to find registry for simple udaf ", name(),
               " of input element type ", arg_type->GetName());
    DLOG(INFO) << "Resolve simple udaf " << name() << "<" << arg_type->GetName()
              << ">";
    *result = iter->second;
    return Status::OK();
}

Status SimpleUDAFRegistry::Register(const std::string& input_arg,
                                    node::UDAFDefNode* udaf_def) {
    auto iter = reg_table_.find(input_arg);
    CHECK_TRUE(iter == reg_table_.end(), "Duplicate udaf register '", name(),
               "'' for element type ", input_arg);
    reg_table_.insert(iter, std::make_pair(input_arg, udaf_def));
    return Status::OK();
}

}  // namespace udf
}  // namespace fesql
