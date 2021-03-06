#include "analysis.h"
#include "llvm/Analysis/CFG.h"
#include <deque>
#include <unordered_set>

using namespace std;
using namespace llvm;

extern int GLOBAL_NUM_RAW_STORE;
extern int GLOBAL_NUM_RAW_CALL;
extern int GLOBAL_NUM_RAW_ARG;

namespace mh
{
    AnalysisContext::AnalysisContext(const SummaryEnvironment* env, const FunctionSummary* summary)
        : smt_solver_(summary->inputs.size()), ctrl_flow_info_(summary->func)
    {
        this->env_             = env;
        this->current_summary_ = summary;

        // add alias rejection
        const vector<const Value*>& inputs = summary->inputs;
        vector<int> ptr_nest_levels;
        for (const Value* arg : inputs)
        {
            ptr_nest_levels.push_back(GetPointerNestLevel(arg->getType()));
        }

        for (size_t i = 0; i < inputs.size(); ++i)
        {
            for (size_t j = 0; j < i; ++j)
            {
                const Value* arg_i = inputs[i];
                const Value* arg_j = inputs[j];
                const Type* type_i = arg_i->getType();
                const Type* type_j = arg_j->getType();

                if (!type_i->isPointerTy() || !type_j->isPointerTy())
                {
                    smt_solver_.RejectAlias(i, j);

#ifdef HEAP_ANALYSIS_DEBUG_MODE
                    // fmt::print("[analysis] rejecting alias({}, {}), reason: non-ptr type\n", i,
                    // j);
#endif
                }
                else if (ptr_nest_levels[i] != ptr_nest_levels[j])
                {
                    // TODO: exclude opaque pointer, i.e. void*
                    // TODO: add toggles for relaxed aliasing rules
                    // TODO: reject non-interfering alias, e.g. two pointers that are not written
                    smt_solver_.RejectAlias(i, j);

#ifdef HEAP_ANALYSIS_DEBUG_MODE
                    // fmt::print("[analysis] rejecting alias({}, {}), reason: different ptr
                    // level\n",
                    //            i, j);
#endif
                }
                else if (isa<GlobalVariable>(arg_i) && isa<GlobalVariable>(arg_j))
                {
                    smt_solver_.RejectAlias(i, j);

#ifdef HEAP_ANALYSIS_DEBUG_MODE
                    // fmt::print(
                    //     "[analysis] rejecting alias({}, {}), reason: different global
                    //     variable\n", i, j);
#endif
                }
                else if (type_i != type_j)
                {
                    // TODO: this is unsound!!!!!!
                    smt_solver_.RejectAlias(i, j);
#ifdef HEAP_ANALYSIS_DEBUG_MODE
// fmt::print("[analysis] rejecting alias({}, {}), reason:
//                     different type\n ", i,
//                     //            j);
#endif
                }
            }
        }

        // initialize entry store for analysis
        for (int i = 0; i < inputs.size(); ++i)
        {
            const Value* arg_i           = inputs[i];
            int ptr_level_i              = ptr_nest_levels[i];
            const AbstractLocation loc_i = AbstractLocation::FromRegister(arg_i);

            // add edges when there's strictly no aliasing, i.e. vi -> *vi -> **vi
            AbstractLocation loc         = loc_i;
            AbstractLocation loc_pointed = AbstractLocation::FromRuntimeMemory(arg_i, 0);
            entry_store_[loc]            = {{loc_pointed, smt_solver_.MakeAliasConstraint(i, i)}};

            for (int k = 0; k < ptr_level_i; ++k)
            {
                loc               = loc_pointed;
                loc_pointed       = AbstractLocation::FromRuntimeMemory(arg_i, k + 1);
                entry_store_[loc] = {{loc_pointed, Constraint{true}}};
            }

            // add edges under aliased conditions
            for (int j = 0; j < i; ++j)
            {
                if (smt_solver_.TestAlias(i, j))
                {
                    AbstractLocation loc_alias = AbstractLocation::FromRuntimeMemory(inputs[j], 0);
                    entry_store_[loc_i].insert(
                        pair{loc_alias, smt_solver_.MakeAliasConstraint(i, j)});
                }
            }
        }

        // move edges from register location to regfile from entry store
        for (auto it = entry_store_.begin(); it != entry_store_.end();)
        {
            if (it->first.Tag() == LocationTag::Register)
            {
                regfile_[it->first.Definition()] = move(it->second);

                it = entry_store_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    std::unique_ptr<AbstractExecution>
    AnalysisContext::InitializeExecution(const llvm::BasicBlock* bb)
    {
        AbstractStore bb_init_store;
        auto merge_store = [&](const AbstractStore& store) {
            if (bb_init_store.empty())
            {
                bb_init_store = store;
            }
            else
            {
                MergeAbstractStore(bb_init_store, store);
            }
        };

        // merge
        for (const BasicBlock* pred_bb : predecessors(bb))
        {
            bool loopback = ctrl_flow_info_.IsBackEdge(pred_bb, bb);
            if (auto it = exec_store_cache_.find(pred_bb); it != exec_store_cache_.end())
            {
                merge_store(it->second);
            }
            else if (!loopback)
            {
                // TODO: workaround: visit blocks in dominator tree traversal order!!!
                // throw "premature initialization of execution for this block";
            }
        }

        // TODO: is this condition sufficient for detecting the first basic block?
        if (bb_init_store.empty())
        {
            // then this is the first basic block of the function
            bb_init_store = this->entry_store_;
        }

        // NormalizeStore(smt_solver_, bb_init_store);
        return std::make_unique<AbstractExecution>(this, move(bb_init_store));
    }

    bool AnalysisContext::CommitExecution(const llvm::BasicBlock* bb,
                                          std::unique_ptr<AbstractExecution> exec)
    {
        // find iterator to the old store
        auto it = exec_store_cache_.find(bb);

        // first run, always update
        if (it == exec_store_cache_.end())
        {
            exec_store_cache_[bb] = move(exec->store_);
            return true;
        }

        // consequent run, update if execution state is changed
        if (exec->TestStoreUpdate(it->second))
        {
            it->second = move(exec->store_);
            return true;
        }

        // TODO: workaround, still update store as it's equivalent anyway
        it->second = move(exec->store_);

        // no update
        return false;
    }

    void AnalysisContext::BuildResultStore()
    {
        AbstractStore result = std::move(exec_store_cache_.at(&current_summary_->func->back()));
        for (auto& [reg, pt_map] : regfile_)
        {
            result[AbstractLocation::FromRegister(reg)] = std::move(pt_map);
        }

        NormalizeStore(smt_solver_, result);
        result_store_ = move(result);
    }

    void AnalysisContext::ExportRAWDependency()
    {
        // collect store and load instructions in the function
        vector<const StoreInst*> stores;
        vector<const LoadInst*> loads;

        for (const BasicBlock& bb : *current_summary_->func)
        {
            for (const Instruction& inst : bb)
            {
                if (auto store_inst = dyn_cast<StoreInst>(&inst))
                {
                    stores.push_back(store_inst);
                }
                else if (auto load_inst = dyn_cast<LoadInst>(&inst))
                {
                    loads.push_back(load_inst);
                }
            }
        }

        // compute pdge edges with constraints
        map<pair<const LoadInst*, const StoreInst*>, Constraint> pdg_edges;
        for (const LoadInst* load_inst : loads)
        {
            const PointToMap& load_ptr_pt_map =
                regfile_.at(TranslateAliasReg(load_inst->getPointerOperand()));

            for (const auto& [loc_load_ptr, c_load_ptr] : load_ptr_pt_map)
            {
                // lookup dependencies for the particular ptr location
                unordered_map<const StoreInst*, Constraint> dependencies;

                for (const StoreInst* store_inst : stores)
                {
                    if (ctrl_flow_info_.LookupExecAfterCondition(store_inst, load_inst) ==
                        ExecAfterCondition::Never)
                    {
                        // load instruction never executes after this store instruction
                        // violate control flow
                        continue;
                    }

                    const PointToMap& store_ptr_pt_map =
                        regfile_.at(TranslateAliasReg(store_inst->getPointerOperand()));

                    if (auto it_store_ptr = store_ptr_pt_map.find(loc_load_ptr);
                        it_store_ptr != store_ptr_pt_map.end())
                    {
                        const auto& [loc_store_ptr, c_store_ptr] = *it_store_ptr;

                        // pointer doesn't alias
                        if (loc_load_ptr != loc_store_ptr &&
                            !smt_solver_.TestSatisfiability(c_load_ptr && c_store_ptr))
                        {
                            continue;
                        }

                        // indicate if this store would be overwritten by a following store
                        bool store_overwritten = false;
                        for (auto it_dep_store = dependencies.begin();
                             it_dep_store != dependencies.end();)
                        {
                            const auto& [dep_store_inst, c_dep_store_ptr] = *it_dep_store;

                            if (ctrl_flow_info_.LookupExecAfterCondition(
                                    dep_store_inst, store_inst) == ExecAfterCondition::Must &&
                                smt_solver_.TestImplication(c_store_ptr, c_dep_store_ptr))
                            {
                                // this store instruction overwrite dep_store
                                it_dep_store = dependencies.erase(it_dep_store);
                                continue;
                            }

                            if (ctrl_flow_info_.LookupExecAfterCondition(
                                    store_inst, dep_store_inst) == ExecAfterCondition::Must &&
                                smt_solver_.TestImplication(c_dep_store_ptr, c_store_ptr))
                            {
                                // this store instruction is overwritten by dep_store
                                store_overwritten = true;
                                break;
                            }

                            ++it_dep_store;
                        }

                        if (!store_overwritten)
                        {
                            dependencies.insert_or_assign(store_inst, c_store_ptr);
                        }
                    }
                }

                for (const auto& [store_inst, c_store_ptr] : dependencies)
                {
                    Constraint& constraint = pdg_edges[pair{load_inst, store_inst}];

                    constraint = constraint || (c_load_ptr && c_store_ptr);
                }
            }
        }

        // test print
        if (kHeapAnalysisPresentationPrint)
        {
            fmt::print("digraph PDG {{\n");
        }
        else
        {
            fmt::print("[RAW deps]:\n");
        }
        for (auto& [loadstore, constraint] : pdg_edges)
        {
            constraint.Simplify();
            if (kHeapAnalysisPresentationPrint)
            {
                fmt::print("  \"{}\" -> \"{}\"[label=\"{}\"]\n",
                           *static_cast<const Value*>(loadstore.second),
                           *static_cast<const Value*>(loadstore.first), constraint);
            }
            else
            {
                fmt::print(" ({} -> {}) ? {}\n", *static_cast<const Value*>(loadstore.first),
                           *static_cast<const Value*>(loadstore.second), constraint);
            }
        }
        if (kHeapAnalysisPresentationPrint)
        {
            fmt::print("}}\n");
        }
    }

    void PrintStore(const AbstractStore& store, const vector<AbstractLocation>& root_locs,
                    const AbstractRegFile* regfile           = nullptr,
                    const vector<const llvm::Value*>* inputs = nullptr,
                    bool output_graphviz                     = kHeapAnalysisPresentationPrint)
    {
        unordered_set<AbstractLocation> known_locs;
        deque<AbstractLocation> important_locs;

        for (const auto& loc : root_locs)
        {
            known_locs.insert(loc);
            important_locs.push_back(loc);
        }

        fmt::print("[Abstract Store]\n");
        auto lookup_store = [&](AbstractLocation loc) -> const PointToMap& {
            if (loc.Tag() == LocationTag::Register && regfile != nullptr)
            {
                return regfile->at(loc.Definition());
            }
            else if (auto it = store.find(loc); it != store.end())
            {
                return it->second;
            }
            else
            {
                static const PointToMap empty_ptmap;
                return empty_ptmap;
            }
        };

        // print graphviz header
        if (output_graphviz)
        {
            fmt::print("digraph G {{\n");

            // legend for constraint terms
            if (inputs != nullptr)
            {
                for (int i = 0; i < inputs->size(); ++i)
                {
                    fmt::print("  \"x{}: {}\" [shape=box]\n", i,
                               AbstractLocation::FromRegister((*inputs)[i]));
                }
            }
        }

        // point-to graph
        while (!important_locs.empty())
        {
            auto loc = important_locs.front();
            important_locs.pop_front();

            const PointToMap& pt_map = lookup_store(loc);
            if (pt_map.empty())
            {
                continue;
            }

            if (output_graphviz)
            {
                for (const auto& [target_loc, constraint] : pt_map)
                {
                    fmt::print("  \"{}\" -> \"{}\" [label=\"{}\"];\n", loc, target_loc, constraint);

                    if (known_locs.find(target_loc) == known_locs.end())
                    {
                        known_locs.insert(target_loc);
                        important_locs.push_back(target_loc);
                    }
                }
            }
            else
            {
                fmt::print("| {}\n", loc);
                for (const auto& [target_loc, constraint] : pt_map)
                {
                    fmt::print("  -> {} ? {}\n", target_loc, constraint);

                    if (known_locs.find(target_loc) == known_locs.end())
                    {
                        known_locs.insert(target_loc);
                        important_locs.push_back(target_loc);
                    }
                }
            }
        }

        // print graphviz terminator
        if (output_graphviz)
        {
            fmt::print("}}\n");
        }
    }

    void AnalysisContext::DebugPrint(const BasicBlock* bb)
    {
        vector<AbstractLocation> root_locs;

        // to print input pointers
        for (const Value* input_reg : current_summary_->inputs)
        {
            root_locs.push_back(AbstractLocation::FromRegister(input_reg));
        }

        // to print registers in this block
        for (const Instruction& inst : *bb)
        {
            if (regfile_.find(&inst) != regfile_.end())
            {
                root_locs.push_back(AbstractLocation::FromRegister(&inst));
            }
        }

        const AbstractStore& store = bb != nullptr ? exec_store_cache_.at(bb) : entry_store_;
        PrintStore(store, root_locs, &regfile_, &current_summary_->inputs);
    }

    void DebugPrint(const AbstractStore& store)
    {
        vector<AbstractLocation> root_locs;

        for (const auto& [loc, pt_map] : store)
        {
            root_locs.push_back(loc);
        }

        PrintStore(store, root_locs, nullptr, nullptr);
    }

    void DebugPrint(const FunctionSummary& summary)
    {
        vector<AbstractLocation> root_locs;

        // to print input pointers
        for (const Value* input_reg : summary.inputs)
        {
            root_locs.push_back(AbstractLocation::FromRegister(input_reg));
        }

        // to print return value
        if (summary.return_inst != nullptr)
        {
            if (const Value* ret_val = summary.return_inst->getReturnValue(); ret_val != nullptr)
            {
                root_locs.push_back(AbstractLocation::FromRegister(ret_val));
            }
        }

        PrintStore(summary.store, root_locs, nullptr, &summary.inputs);
    }

    bool AnalysisContext::AnalyzeBlock_DataDep(const llvm::BasicBlock* bb)
    {
        int pred_index = 0;
        ConstrainedDataDependencyGraph graph;
        for (const BasicBlock* prev_bb : predecessors(bb))
        {
            if (pred_index == 0)
            {
                graph = data_dep_cache_[prev_bb];
            }
            else
            {
                graph.Merge(Solver(), data_dep_cache_[prev_bb]);
            }

            pred_index += 1;
        }

        // initialize if no predecessor, i.e. first basic block
        if (pred_index == 0)
        {
            for (int i = 0; i < current_summary_->inputs.size(); ++i)
            {
                const Value* arg_i = current_summary_->inputs[i];
                int ptr_level_i    = GetPointerNestLevel(arg_i->getType());

                for (int k = 0; k < ptr_level_i; ++k)
                {
                    graph[AbstractLocation::FromRuntimeMemory(arg_i, k)][arg_i] = Constraint{true};
                }
            }
        }

        for (const Instruction& inst : *bb)
        {
            if (isa<AllocaInst>(inst) || IsMallocCall(&inst))
            {
                auto loc_alloc          = AbstractLocation::FromAllocation(&inst);
                graph[loc_alloc][&inst] = Constraint{true};
            }
            else if (auto store_inst = dyn_cast<StoreInst>(&inst))
            {
                const PointToMap& ptr_locs = result_store_[AbstractLocation::FromRegister(
                    TranslateAliasReg(store_inst->getPointerOperand()))];

                for (const auto& [ptr, c_ptr] : ptr_locs)
                {
                    graph.OverwriteRelationEdge(ptr, store_inst, c_ptr);
                }
            }
            else if (auto load_inst = dyn_cast<LoadInst>(&inst))
            {
                // find data flow
                const PointToMap& ptr_locs = result_store_[AbstractLocation::FromRegister(
                    TranslateAliasReg(load_inst->getPointerOperand()))];

                for (const auto& [ptr, c_ptr] : ptr_locs)
                {
                    for (const auto [src_val, c_contrib] : graph[ptr])
                    {
                        Constraint c_dep = c_ptr && c_contrib;

                        if (Solver().TestSatisfiability(c_dep))
                        {
                            data_dep_result_[pair{load_inst, src_val}] = c_dep;
                        }
                    }
                }
            }
            else if (auto call_inst = dyn_cast<CallInst>(&inst))
            {
                if (auto it = update_hitory_.find(call_inst); it != update_hitory_.end())
                {
                    const PointToMap& ptr_locs = it->second;

                    for (const auto& [ptr, c_passin] : ptr_locs)
                    {
                        graph.OverwriteRelationEdge(ptr, call_inst, c_passin.Weaken());
                    }
                }
            }
        }

        auto& graph_cell = data_dep_cache_[bb];

        // TODO: workaround, verify soundness of such trick
        graph.UpdateCachedNumEdge();
        // bool updated = graph.CachedNumEdge() != graph_cell.CachedNumEdge();
        bool updated = graph.CachedNumEdge() != graph_cell.CachedNumEdge() ||
                       !graph.Equals(Solver(), graph_cell);

        graph_cell = move(graph);
        return updated;
    }

    void AnalyzeFunction_DataDep(AnalysisContext& ctx)
    {
        deque<const BasicBlock*> worklist;
        unordered_set<const BasicBlock*> workset;

        for (const BasicBlock& bb : *ctx.Func())
        {
            worklist.push_back(&bb);
            workset.insert(&bb);
        }

        while (!worklist.empty())
        {
            const BasicBlock* bb = worklist.front();
            worklist.pop_front();
            workset.erase(bb);

            if (ctx.AnalyzeBlock_DataDep(bb))
            {
                for (const BasicBlock* succ_bb : successors(bb))
                {
                    if (workset.find(succ_bb) == workset.end())
                    {
                        worklist.push_back(succ_bb);
                        workset.insert(succ_bb);
                    }
                }
            }
        }
    }

    bool AnalysisContext::AnalyzeBlock(const llvm::BasicBlock* bb)
    {
#ifdef HEAP_ANALYSIS_DEBUG_MODE
        // fmt::print("analyzing block {}...\n", bb->getName());
#endif

        auto exec = InitializeExecution(bb);

        for (const Instruction& inst : *bb)
        {
#ifdef HEAP_ANALYSIS_DEBUG_MODE
            // fmt::print("interpreting {}...\n", static_cast<const Value&>(inst));
#endif
            if (isa<BranchInst>(inst) || isa<SwitchInst>(inst))
            {
                // usu. last instruction in a block
                // do nothing as we initialize execution with predecessors
            }
            else if (isa<AllocaInst>(inst))
            {
                exec->DoAlloc(&inst, inst.getType()->isArrayTy());
            }
            else if (IsMallocCall(&inst))
            {
                exec->DoAlloc(&inst, true);
            }
            else if (isa<BitCastInst>(&inst))
            {
                // TODO:
                AssignAliasReg(&inst, inst.getOperand(0));
            }
            else if (isa<GetElementPtrInst>(&inst))
            {
                // TODO: mark summary location
                AssignAliasReg(&inst, inst.getOperand(0));
            }
            else if (auto store_inst = dyn_cast<StoreInst>(&inst))
            {
                exec->DoStore(store_inst->getValueOperand(), store_inst->getPointerOperand());
            }
            else if (auto load_inst = dyn_cast<LoadInst>(&inst))
            {
                exec->DoLoad(&inst, load_inst->getPointerOperand());
            }
            else if (auto call_inst = dyn_cast<CallInst>(&inst))
            {
                const Function* callee = call_inst->getCalledFunction();

                // TODO: workaround, why nullptr?
                if (callee != nullptr)
                {
                    // TODO: workaround: assume library function does not change pt-relation
                    if (callee->isDeclaration())
                    {
#ifdef HEAP_ANALYSIS_POINTS_TO_DETAIL
                        regfile_[call_inst] = {{AbstractLocation::FromProgramValue(callee),
                                                Constraint{true}.Weaken()}};
#endif
                    }
                    else
                    {
                        const FunctionSummary& callee_summary =
                            Environment()->LookupSummary(callee);

                        std::vector<const llvm::Value*> reg_inputs;
                        reg_inputs.reserve(callee_summary.inputs.size());
                        copy(call_inst->arg_begin(), call_inst->arg_end(),
                             back_inserter(reg_inputs));
                        copy(callee_summary.globals.begin(), callee_summary.globals.end(),
                             back_inserter(reg_inputs));

                        exec->DoInvoke(&inst, callee_summary, reg_inputs);
                    }
                }
            }
            else if (auto phi = dyn_cast<PHINode>(&inst))
            {
                exec->DoAssignPhi(&inst, phi->incoming_values());
            }
            else if (auto sel = dyn_cast<SelectInst>(&inst))
            {
                exec->DoAssignPhi(&inst, sel->getTrueValue(), sel->getFalseValue());
            }
            else
            {
#ifdef HEAP_ANALYSIS_POINTS_TO_DETAIL
                exec->DoAssign(&inst, AbstractLocation::FromProgramValue(&inst));
#endif
            }
        }

        return CommitExecution(bb, move(exec));
    }

    // analyze the function once, assuming summaries of all called functions ready
    void AnalyzeFunctionAux(SummaryEnvironment& env, FunctionSummary& summary,
                            bool dependencies_converged = false)
    {
        if (summary.converged)
        {
            return;
        }

#ifdef HEAP_ANALYSIS_DEBUG_MODE
        fmt::print("---------\n");
        fmt::print("processing function {}\n", summary.func->getName());

        auto t_start = chrono::high_resolution_clock::now();
#endif
        AnalysisContext ctx{&env, &summary};

        const Function* func = summary.func;
        deque<const BasicBlock*> worklist;
        unordered_set<const BasicBlock*> workset;

        for (const BasicBlock& bb : *func)
        {
            worklist.push_back(&bb);
            workset.insert(&bb);
        }

        while (!worklist.empty())
        {
            const BasicBlock* bb = worklist.front();
            worklist.pop_front();
            workset.erase(bb);

            if (ctx.AnalyzeBlock(bb))
            {
                for (const BasicBlock* succ_bb : successors(bb))
                {
                    if (workset.find(succ_bb) == workset.end())
                    {
                        worklist.push_back(succ_bb);
                        workset.insert(succ_bb);
                    }
                }
            }
        }

        // TODO: what solver to use?
        // TODO: verify reassignment is correct
        ctx.BuildResultStore();
        if (summary.store.empty() ||
            !EqualAbstractStore(ctx.Solver(), summary.store, ctx.ExportResultStore()))
        {
            if (dependencies_converged)
            {
                summary.converged = true;
            }
        }
        else
        {
            summary.converged = true;
        }

#ifdef HEAP_ANALYSIS_DEBUG_MODE
        if (summary.converged)
        {
            int num_raw_store = 0;
            int num_raw_call  = 0;
            int num_raw_arg   = 0;

            AnalyzeFunction_DataDep(ctx);
            // fmt::print("[Data Dependency]\n");
            // fmt::print("digraph DDG {{\n");
            // for (auto& [dep_pair, constraint] : ctx.data_dep_result_)
            // {
            //     constraint.Simplify();
            //     fmt::print("\"{}\" -> \"{}\" [label=\"{}\"]\n", *dep_pair.second,
            //                *static_cast<const Value*>(dep_pair.first), constraint);
            // }
            // fmt::print("}}\n");
            // mh::DebugPrint(ctx.ExportResultStore());

            for (auto& [dep_pair, constraint] : ctx.data_dep_result_)
            {
                if (isa<StoreInst>(dep_pair.second))
                {
                    num_raw_store += 1;
                }
                else if (isa<CallInst>(dep_pair.second))
                {
                    num_raw_call += 1;
                }
                else if (isa<Argument>(dep_pair.second) || isa<GlobalVariable>(dep_pair.second))
                {
                    num_raw_arg += 1;
                }
            }

            GLOBAL_NUM_RAW_STORE += num_raw_store;
            GLOBAL_NUM_RAW_CALL += num_raw_call;
            GLOBAL_NUM_RAW_ARG += num_raw_arg;

            using FpMilliseconds = std::chrono::duration<float, std::chrono::milliseconds::period>;
            auto t_stop          = chrono::high_resolution_clock::now();

            fmt::print("Run Time = {} ms\n", FpMilliseconds(t_stop - t_start).count());

            fmt::print("Num RAW (load-store) = {}\n", num_raw_store);
            fmt::print("Num RAW (load-call) = {}\n", num_raw_call);
            fmt::print("Num RAW (load-arg) = {}\n", num_raw_arg);
            fmt::print("Total RAW (load-store) = {}\n", GLOBAL_NUM_RAW_STORE);
            fmt::print("Total RAW (load-call) = {}\n", GLOBAL_NUM_RAW_CALL);
            fmt::print("Total RAW (load-arg) = {}\n", GLOBAL_NUM_RAW_ARG);
        }
#endif

        summary.store = move(ctx.ExportResultStore());
    }

    void AnalyzeFunctionRecursive(SummaryEnvironment& env, FunctionSummary& summary,
                                  unordered_set<const Function*>& analysis_history,
                                  bool expect_converge)
    {
        // function already in the call chain, omit analysis and return
        if (analysis_history.find(summary.func) != analysis_history.end())
        {
            return;
        }

        // collect recursive called functions and analyze non-recursive called functions
        vector<FunctionSummary*> recursive_summaries;
        for (const Function* called_func : summary.called_functions)
        {
            // TODO: workaround, why nullptr?
            if (called_func == nullptr)
            {
                continue;
            }

            FunctionSummary& called_summary = env.LookupSummary(called_func);
            if (called_summary.func->doesNotRecurse())
            {
                if (!called_summary.converged)
                {
                    AnalyzeFunctionRecursive(env, called_summary, analysis_history, true);
                }

                assert(called_summary.converged);
            }
            else
            {
                recursive_summaries.push_back(&called_summary);
            }
        }

        // add the current function to the call chain
        analysis_history.insert(summary.func);

        do
        {
            // analyze the called functions
            bool dep_converged = true;
            for (FunctionSummary* called_summmary : recursive_summaries)
            {
                if (!called_summmary->converged)
                {
                    AnalyzeFunctionRecursive(env, *called_summmary, analysis_history, false);
                }

                dep_converged = dep_converged && called_summmary->converged;
            }

            // analyze the current function
            AnalyzeFunctionAux(env, summary, dep_converged);
        } while (expect_converge && !summary.converged);

        // remove the current function from the call chain
        analysis_history.erase(summary.func);
    }

    void AnalyzeFunction(SummaryEnvironment& env, const Function* func)
    {
        FunctionSummary& summary = env.LookupSummary(func);
        if (summary.converged)
        {
            return;
        }

        unordered_set<const Function*> analysis_history;
        AnalyzeFunctionRecursive(env, summary, analysis_history, true);

        for (auto func : summary.called_functions)
        {
            env.NotifyUse(func);
        }
    }

} // namespace mh