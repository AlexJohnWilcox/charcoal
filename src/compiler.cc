#include "compiler.h"

#include "native.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coal {

namespace {

constexpr int kMaxReg = 255;     // r is u8
constexpr int kMaxConst = 65535; // k is u16
constexpr int kMaxCount = 255;   // n is u8

struct Compiler {
  CompileResult result;
  Module& m = result.module;
  std::string error;

  std::unordered_map<std::string, int> func_index;  // name -> funcs index

  // Per-function state, reset by begin_fn().
  std::vector<uint8_t>* code = nullptr;
  std::unordered_map<std::string, int> locals;  // name -> register
  int reg_top = 0;     // first free register
  int high_water = 0;  // max registers ever used -> num_regs

  // Loop context for break/continue: stacks of JUMP operand positions to
  // backpatch to the loop exit / continue target. One entry per enclosing loop.
  struct LoopCtx {
    std::vector<int> continue_patches;
    std::vector<int> break_patches;
  };
  std::vector<LoopCtx> loops;

  // Closure support. `enclosing_` holds the locals tables of the lexically
  // enclosing functions (for one-level upvalue capture: only .back() is used).
  // `upvals_`/`upval_src_` accumulate the CURRENT function's captured names and,
  // in parent-register terms, where each is copied from at OP_CLOSURE time.
  std::vector<const std::unordered_map<std::string, int>*> enclosing_;
  std::vector<std::string> upvals_;
  std::vector<int>         upval_src_;
  std::unordered_map<const Node*, int> lambda_func_;  // Lambda node -> funcs index

  int cur_line = 0;  // source line of the node currently being compiled

  void fail(const std::string& msg) {
    if (error.empty()) {
      err_line = cur_line;
      error = (cur_line > 0) ? std::to_string(cur_line) + ": " + msg : msg;
    }
  }
  int err_line = 0;
  bool failed() const { return !error.empty(); }

  void use(int upto) { if (upto > high_water) high_water = upto; }

  // --- constant interning ------------------------------------------------

  int intern(const Constant& c) {
    for (size_t i = 0; i < m.consts.size(); ++i) {
      const Constant& e = m.consts[i];
      if (e.tag != c.tag) continue;
      bool same = false;
      switch (c.tag) {
        case CTag::Int:    same = e.i == c.i; break;
        case CTag::Double: same = e.d == c.d; break;
        case CTag::Bool:   same = e.b == c.b; break;
        case CTag::Str:    same = e.s == c.s; break;
        case CTag::Nil:    same = true;       break;
      }
      if (same) return static_cast<int>(i);
    }
    m.consts.push_back(c);
    size_t idx = m.consts.size() - 1;
    if (idx > kMaxConst) { fail("too many constants"); return 0; }
    return static_cast<int>(idx);
  }

  int k_int(int64_t v)    { Constant c; c.tag = CTag::Int;    c.i = v; return intern(c); }
  int k_double(double v)  { Constant c; c.tag = CTag::Double; c.d = v; return intern(c); }
  int k_str(const std::string& s) { Constant c; c.tag = CTag::Str; c.s = s; return intern(c); }
  int k_bool(bool b)      { Constant c; c.tag = CTag::Bool;   c.b = b; return intern(c); }

  // --- emitters ----------------------------------------------------------

  void emit(uint8_t b) { code->push_back(b); }
  void emit_r(int r) {
    if (r < 0 || r > kMaxReg) { fail("register overflow"); return; }
    code->push_back(static_cast<uint8_t>(r));
  }
  void emit_k(int k) {  // u16 little-endian
    code->push_back(static_cast<uint8_t>(k & 0xff));
    code->push_back(static_cast<uint8_t>((k >> 8) & 0xff));
  }
  void emit_n(int n) {
    if (n < 0 || n > kMaxCount) { fail("too many elements"); return; }
    code->push_back(static_cast<uint8_t>(n));
  }

  int emit_offset() {  // two placeholder bytes, returns their position
    int pos = static_cast<int>(code->size());
    code->push_back(0);
    code->push_back(0);
    return pos;
  }
  void patch_to(int pos, int target) {
    int rel = target - (pos + 2);  // relative to the byte after the operand
    if (rel < -32768 || rel > 32767) { fail("jump out of range"); return; }
    int16_t r16 = static_cast<int16_t>(rel);
    (*code)[pos]     = static_cast<uint8_t>(r16 & 0xff);
    (*code)[pos + 1] = static_cast<uint8_t>((r16 >> 8) & 0xff);
  }
  void patch_jump(int pos) { patch_to(pos, static_cast<int>(code->size())); }

  // --- expression codegen ------------------------------------------------

  // Resolve `name` as an upvalue captured from the IMMEDIATE enclosing function.
  // Returns the upvalue index (adding it if new) or -1 if not a parent local.
  // One-level capture only: a reference needing a grandparent is not found here
  // and surfaces as "undefined variable" rather than being miscompiled.
  int find_or_add_upvalue(const std::string& name) {
    if (enclosing_.empty()) return -1;
    for (size_t i = 0; i < upvals_.size(); ++i)
      if (upvals_[i] == name) return static_cast<int>(i);
    const auto* parent = enclosing_.back();
    auto pit = parent->find(name);
    if (pit == parent->end()) return -1;
    int idx = static_cast<int>(upvals_.size());
    if (idx > kMaxCount) { fail("too many upvalues"); return -1; }
    upvals_.push_back(name);
    upval_src_.push_back(pit->second);  // parent register supplying the capture
    return idx;
  }

  // Compile a lambda body into its own (pre-indexed) function, then emit the
  // OP_CLOSURE that builds the closure value in `dst` within the parent. The
  // parent's per-function compiler state is saved across the nested compile and
  // restored before the OP_CLOSURE is emitted into the parent's code.
  void compile_lambda(const Node* lam, int dst) {
    auto it = lambda_func_.find(lam);
    if (it == lambda_func_.end()) { fail("internal: lambda not indexed"); return; }
    uint32_t fidx = static_cast<uint32_t>(it->second);
    if (fidx > 65535) { fail("too many functions"); return; }

    // Save the parent function's state.
    std::vector<uint8_t>* save_code = code;
    std::unordered_map<std::string, int> save_locals = std::move(locals);
    std::vector<LoopCtx> save_loops = std::move(loops);
    int save_reg = reg_top, save_hw = high_water, save_line = cur_line;
    std::vector<std::string> save_upvals = std::move(upvals_);
    std::vector<int> save_upsrc = std::move(upval_src_);

    // The lambda captures one level up: from `save_locals` (the parent).
    enclosing_.push_back(&save_locals);
    upvals_.clear();
    upval_src_.clear();

    std::vector<const Node*> body{lam->kids[0]};
    compile_body(m.funcs[fidx], body, lam->params, /*is_main=*/false);

    std::vector<int> captured = upval_src_;  // parent registers to capture, in order
    int n_up = static_cast<int>(captured.size());

    // Restore the parent function's state.
    enclosing_.pop_back();
    code = save_code;
    locals = std::move(save_locals);
    loops = std::move(save_loops);
    reg_top = save_reg;
    high_water = save_hw;
    cur_line = save_line;
    upvals_ = std::move(save_upvals);
    upval_src_ = std::move(save_upsrc);

    if (failed()) return;

    // Build the closure in the parent: OP_CLOSURE dst, fidx, n, [src regs...].
    emit(OP_CLOSURE);
    emit_r(dst);
    emit_k(static_cast<int>(fidx));  // u16 function index (into Module::funcs)
    emit_n(n_up);
    for (int s : captured) emit_r(s);
  }

  void compile_expr(const Node* n, int dst) {
    if (failed()) return;
    if (n->line > 0) cur_line = n->line;
    switch (n->kind) {
      case NodeKind::IntLit:
        emit(OP_LOAD_CONST); emit_r(dst); emit_k(k_int(n->ival));
        break;
      case NodeKind::FloatLit:
        emit(OP_LOAD_CONST); emit_r(dst); emit_k(k_double(n->dval));
        break;
      case NodeKind::StrLit:
        emit(OP_LOAD_CONST); emit_r(dst); emit_k(k_str(n->str));
        break;
      case NodeKind::BoolLit:
        emit(OP_LOAD_CONST); emit_r(dst); emit_k(k_bool(n->bval));
        break;
      case NodeKind::NilLit:
        emit(OP_LOAD_NIL); emit_r(dst);
        break;
      case NodeKind::Ident: {
        auto it = locals.find(n->str);
        if (it != locals.end()) {  // a local of this function
          if (dst != it->second) { emit(OP_MOVE); emit_r(dst); emit_r(it->second); }
          break;
        }
        int uidx = find_or_add_upvalue(n->str);  // captured from the parent?
        if (failed()) return;
        if (uidx >= 0) {
          emit(OP_GET_UPVAL); emit_r(dst); emit_n(uidx);
          break;
        }
        auto fit = func_index.find(n->str);  // a named function used as a value
        if (fit != func_index.end()) {
          if (fit->second > 65535) { fail("too many functions"); return; }
          emit(OP_CLOSURE); emit_r(dst); emit_k(fit->second); emit_n(0);  // 0-upvalue closure
          break;
        }
        if (find_native(n->str)) { fail("builtin '" + n->str + "' is not a value"); return; }
        fail("undefined variable: " + n->str);
        return;
      }
      case NodeKind::Lambda:
        compile_lambda(n, dst);
        break;
      case NodeKind::Binary: {
        int save = reg_top;
        int t1 = reg_top, t2 = reg_top + 1;
        reg_top += 2; use(reg_top);
        compile_expr(n->kids[0], t1);
        compile_expr(n->kids[1], t2);
        Op op;
        switch (n->op) {
          case '+': op = OP_ADD;  break;
          case '-': op = OP_SUB;  break;
          case '*': op = OP_MUL;  break;
          case '/': op = OP_DIV;  break;
          case '%': op = OP_MOD;  break;
          case 'E': op = OP_EQ;   break;
          case 'N': op = OP_NE;   break;
          case '<': op = OP_LT;   break;
          case 'l': op = OP_LE;   break;
          case '>': op = OP_GT;   break;
          case 'g': op = OP_GE;   break;
          case '&': op = OP_BAND; break;
          case '|': op = OP_BOR;  break;
          case '^': op = OP_BXOR; break;
          case 'L': op = OP_SHL;  break;
          case 'R': op = OP_SHR;  break;
          default:  fail("bad operator"); return;
        }
        emit(op); emit_r(dst); emit_r(t1); emit_r(t2);
        reg_top = save;
        break;
      }
      case NodeKind::Unary: {
        int save = reg_top;
        int t = reg_top;
        reg_top += 1; use(reg_top);
        compile_expr(n->kids[0], t);
        Op op;
        switch (n->op) {
          case '-': op = OP_NEG;  break;
          case '!': op = OP_NOT;  break;
          case '~': op = OP_BNOT; break;
          default:  fail("bad unary operator"); return;
        }
        emit(op); emit_r(dst); emit_r(t);
        reg_top = save;
        break;
      }
      case NodeKind::Logical: {
        // Short-circuit, yields a Bool in dst. Only JUMP_IF_FALSE is available,
        // so the two operators are built from that primitive.
        if (n->op == 'a') {
          // a && b : false if either operand is falsey, else true.
          compile_expr(n->kids[0], dst);
          emit(OP_JUMP_IF_FALSE); emit_r(dst);
          int jf1 = emit_offset();
          compile_expr(n->kids[1], dst);
          emit(OP_JUMP_IF_FALSE); emit_r(dst);
          int jf2 = emit_offset();
          emit(OP_LOAD_CONST); emit_r(dst); emit_k(k_bool(true));
          emit(OP_JUMP);
          int jend = emit_offset();
          patch_jump(jf1);   // a falsey
          patch_jump(jf2);   // b falsey
          emit(OP_LOAD_CONST); emit_r(dst); emit_k(k_bool(false));
          patch_jump(jend);
        } else {
          // a || b : true if either operand is truthy, else false.
          compile_expr(n->kids[0], dst);
          emit(OP_JUMP_IF_FALSE); emit_r(dst);
          int jcheckb = emit_offset();   // a falsey -> evaluate b
          emit(OP_JUMP);
          int jtrue_a = emit_offset();   // a truthy -> true
          patch_jump(jcheckb);
          compile_expr(n->kids[1], dst);
          emit(OP_JUMP_IF_FALSE); emit_r(dst);
          int jfalse = emit_offset();    // b falsey -> false
          patch_jump(jtrue_a);           // a-truthy and b-truthy meet here
          emit(OP_LOAD_CONST); emit_r(dst); emit_k(k_bool(true));
          emit(OP_JUMP);
          int jend = emit_offset();
          patch_jump(jfalse);
          emit(OP_LOAD_CONST); emit_r(dst); emit_k(k_bool(false));
          patch_jump(jend);
        }
        break;
      }
      case NodeKind::Index: {
        int save = reg_top;
        int c = reg_top, i = reg_top + 1;
        reg_top += 2; use(reg_top);
        compile_expr(n->kids[0], c);
        compile_expr(n->kids[1], i);
        emit(OP_ARRAY_GET); emit_r(dst); emit_r(c); emit_r(i);
        reg_top = save;
        break;
      }
      case NodeKind::Field: {
        int save = reg_top;
        int c = reg_top;
        reg_top += 1; use(reg_top);
        compile_expr(n->kids[0], c);
        emit(OP_GET_PROP); emit_r(dst); emit_r(c); emit_k(k_str(n->str));
        reg_top = save;
        break;
      }
      case NodeKind::ArrayLit: {
        int count = static_cast<int>(n->kids.size());
        emit(OP_NEW_ARRAY); emit_r(dst); emit_n(count);
        for (int idx = 0; idx < count; ++idx) {
          int save = reg_top;
          int idxReg = reg_top, valReg = reg_top + 1;
          reg_top += 2; use(reg_top);
          emit(OP_LOAD_CONST); emit_r(idxReg); emit_k(k_int(idx));
          compile_expr(n->kids[idx], valReg);
          emit(OP_ARRAY_SET); emit_r(dst); emit_r(idxReg); emit_r(valReg);
          reg_top = save;
        }
        break;
      }
      case NodeKind::MapLit: {
        emit(OP_NEW_OBJECT); emit_r(dst);
        for (size_t p = 0; p + 1 < n->kids.size(); p += 2) {
          const Node* key = n->kids[p];
          const Node* val = n->kids[p + 1];
          if (key->kind != NodeKind::StrLit) { fail("map key must be a string"); return; }
          int save = reg_top;
          int valReg = reg_top;
          reg_top += 1; use(reg_top);
          compile_expr(val, valReg);
          emit(OP_SET_PROP); emit_r(dst); emit_k(k_str(key->str)); emit_r(valReg);
          reg_top = save;
        }
        break;
      }
      case NodeKind::Call: {
        int count = static_cast<int>(n->kids.size());
        if (count > kMaxCount) { fail("too many arguments"); return; }

        // Resolution order: function VALUE (local/upvalue) -> push -> builtin ->
        // named function -> error. A local or captured callee is a closure value
        // we evaluate and invoke with OP_CALL_VALUE.
        bool callee_local = (locals.find(n->str) != locals.end());
        int callee_upval = -1;
        if (!callee_local) {
          callee_upval = find_or_add_upvalue(n->str);
          if (failed()) return;
        }
        if (callee_local || callee_upval >= 0) {
          int base = reg_top;
          reg_top = base + 1; use(reg_top);
          if (callee_local) {
            emit(OP_MOVE); emit_r(base); emit_r(locals[n->str]);
          } else {
            emit(OP_GET_UPVAL); emit_r(base); emit_n(callee_upval);
          }
          for (int a = 0; a < count; ++a) {  // args follow the callee at base+1..
            reg_top = base + 1 + a + 1; use(reg_top);
            compile_expr(n->kids[a], base + 1 + a);
          }
          use(base + 1 + count);
          emit(OP_CALL_VALUE); emit_r(base); emit_n(count);
          reg_top = base;
          if (dst != base) { emit(OP_MOVE); emit_r(dst); emit_r(base); }
          break;
        }

        // `push(array, value)` is a reserved builtin.
        if (n->str == "push") {
          if (n->kids.size() != 2) { fail("push expects (array, value)"); return; }
          int save = reg_top;
          int ra = reg_top, rv = reg_top + 1;
          reg_top += 2; use(reg_top);
          compile_expr(n->kids[0], ra);
          compile_expr(n->kids[1], rv);
          emit(OP_ARRAY_PUSH); emit_r(ra); emit_r(rv);
          reg_top = save;
          emit(OP_LOAD_NIL); emit_r(dst);  // push evaluates to nil
          break;
        }
        const NativeEntry* native = find_native(n->str);
        bool is_native = (native != nullptr);
        if (!is_native && func_index.find(n->str) == func_index.end()) {
          fail("call to unknown function: " + n->str);
          return;
        }
        if (is_native && (count < native->arity_min || count > native->arity_max)) {
          fail("wrong number of arguments to " + n->str);
          return;
        }

        // Identical codegen to OP_CALL: args into consecutive regs base..base+n,
        // result lands in base.
        int base = reg_top;
        for (int a = 0; a < count; ++a) {
          reg_top = base + a + 1; use(reg_top);
          compile_expr(n->kids[a], base + a);
        }
        use(base + 1);  // result lands in base, even with no args
        emit(is_native ? OP_CALL_NATIVE : OP_CALL);
        emit_r(base); emit_k(k_str(n->str)); emit_n(count);
        reg_top = base;
        if (dst != base) { emit(OP_MOVE); emit_r(dst); emit_r(base); }
        break;
      }
      default:
        fail("not an expression");
        break;
    }
  }

  // --- statement codegen -------------------------------------------------

  void compile_stmt(const Node* n) {
    if (failed()) return;
    if (n->line > 0) cur_line = n->line;
    switch (n->kind) {
      case NodeKind::Print: {
        int save = reg_top, t = reg_top;
        reg_top += 1; use(reg_top);
        compile_expr(n->kids[0], t);
        emit(OP_PRINT); emit_r(t);
        reg_top = save;
        break;
      }
      case NodeKind::ExprStmt: {
        int save = reg_top, t = reg_top;
        reg_top += 1; use(reg_top);
        compile_expr(n->kids[0], t);
        reg_top = save;
        break;
      }
      case NodeKind::Return: {
        int save = reg_top, t = reg_top;
        reg_top += 1; use(reg_top);
        if (n->kids.empty()) { emit(OP_LOAD_NIL); emit_r(t); }
        else                 compile_expr(n->kids[0], t);
        emit(OP_RET); emit_r(t);
        reg_top = save;
        break;
      }
      case NodeKind::Assign: {
        const Node* target = n->kids[0];
        const Node* value = n->kids[1];
        if (target->kind == NodeKind::Ident) {
          auto it = locals.find(target->str);
          if (it == locals.end()) { fail("undefined variable: " + target->str); return; }
          compile_expr(value, it->second);  // straight into the variable's register
        } else if (target->kind == NodeKind::Index) {
          int save = reg_top;
          int c = reg_top, i = reg_top + 1, v = reg_top + 2;
          reg_top += 3; use(reg_top);
          compile_expr(target->kids[0], c);
          compile_expr(target->kids[1], i);
          compile_expr(value, v);
          emit(OP_ARRAY_SET); emit_r(c); emit_r(i); emit_r(v);
          reg_top = save;
        } else if (target->kind == NodeKind::Field) {
          int save = reg_top;
          int c = reg_top, v = reg_top + 1;
          reg_top += 2; use(reg_top);
          compile_expr(target->kids[0], c);
          compile_expr(value, v);
          emit(OP_SET_PROP); emit_r(c); emit_k(k_str(target->str)); emit_r(v);
          reg_top = save;
        } else {
          fail("invalid assignment target");
        }
        break;
      }
      case NodeKind::Block:
        for (const Node* s : n->kids) { if (failed()) return; compile_stmt(s); }
        break;
      case NodeKind::If: {
        int save = reg_top, t = reg_top;
        reg_top += 1; use(reg_top);
        compile_expr(n->kids[0], t);
        emit(OP_JUMP_IF_FALSE); emit_r(t);
        int elseJump = emit_offset();
        reg_top = save;
        compile_stmt(n->kids[1]);
        if (n->kids.size() == 3) {
          emit(OP_JUMP);
          int endJump = emit_offset();
          patch_jump(elseJump);
          compile_stmt(n->kids[2]);
          patch_jump(endJump);
        } else {
          patch_jump(elseJump);
        }
        break;
      }
      case NodeKind::While: {
        int loop_start = static_cast<int>(code->size());
        loops.push_back({});
        int save = reg_top, t = reg_top;
        reg_top += 1; use(reg_top);
        compile_expr(n->kids[0], t);
        emit(OP_JUMP_IF_FALSE); emit_r(t);
        int exitJump = emit_offset();
        reg_top = save;
        compile_stmt(n->kids[1]);
        // continue re-tests the condition.
        for (int p : loops.back().continue_patches) patch_to(p, loop_start);
        emit(OP_JUMP);
        int backJump = emit_offset();
        patch_to(backJump, loop_start);
        int exit = static_cast<int>(code->size());
        patch_to(exitJump, exit);
        for (int p : loops.back().break_patches) patch_to(p, exit);
        loops.pop_back();
        break;
      }
      case NodeKind::For: {
        const Node* cond = n->kids[1];
        const Node* step = n->kids[2];
        compile_stmt(n->kids[0]);  // init (empty Block sentinel -> no-op)
        int loop_start = static_cast<int>(code->size());
        loops.push_back({});
        bool has_cond = !(cond->kind == NodeKind::Block && cond->kids.empty());
        int exitJump = -1;
        if (has_cond) {
          int save = reg_top, t = reg_top;
          reg_top += 1; use(reg_top);
          compile_expr(cond, t);
          emit(OP_JUMP_IF_FALSE); emit_r(t);
          exitJump = emit_offset();
          reg_top = save;
        }
        compile_stmt(n->kids[3]);  // body
        // continue jumps to the step (then the back-edge re-tests cond).
        int cont_target = static_cast<int>(code->size());
        for (int p : loops.back().continue_patches) patch_to(p, cont_target);
        compile_stmt(step);  // empty Block sentinel -> no-op
        emit(OP_JUMP);
        int backJump = emit_offset();
        patch_to(backJump, loop_start);
        int exit = static_cast<int>(code->size());
        if (exitJump != -1) patch_to(exitJump, exit);
        for (int p : loops.back().break_patches) patch_to(p, exit);
        loops.pop_back();
        break;
      }
      case NodeKind::Break: {
        if (loops.empty()) { fail("break outside loop"); return; }
        emit(OP_JUMP);
        loops.back().break_patches.push_back(emit_offset());
        break;
      }
      case NodeKind::Continue: {
        if (loops.empty()) { fail("continue outside loop"); return; }
        emit(OP_JUMP);
        loops.back().continue_patches.push_back(emit_offset());
        break;
      }
      default:
        fail("not a statement");
        break;
    }
  }

  // --- function setup ----------------------------------------------------

  // Named locals (params + bare-Ident assignment targets) get fixed low
  // registers so temporaries above reg_top never clobber a live variable.
  void collect_locals(const Node* n, std::unordered_set<std::string>& seen,
                      std::vector<std::string>& out) {
    // Don't descend into nested function bodies — their locals are their own.
    if (!n || n->kind == NodeKind::FnDecl || n->kind == NodeKind::Lambda) return;
    if (n->kind == NodeKind::Assign && !n->kids.empty() &&
        n->kids[0]->kind == NodeKind::Ident) {
      const std::string& nm = n->kids[0]->str;
      if (seen.insert(nm).second) out.push_back(nm);
    }
    for (const Node* k : n->kids) collect_locals(k, seen, out);
  }

  void compile_body(Function& f, const std::vector<const Node*>& stmts,
                    const std::vector<std::string>& params, bool is_main) {
    code = &f.code;
    locals.clear();
    loops.clear();
    upvals_.clear();
    upval_src_.clear();
    reg_top = 0;
    high_water = 0;

    int next = 0;
    for (const std::string& p : params) locals[p] = next++;

    std::unordered_set<std::string> seen(params.begin(), params.end());
    std::vector<std::string> localNames;
    for (const Node* s : stmts) collect_locals(s, seen, localNames);
    for (const std::string& nm : localNames) locals[nm] = next++;

    if (next > kMaxReg + 1) { fail("too many locals"); return; }
    reg_top = next;
    high_water = next;

    for (const Node* s : stmts) { if (failed()) return; compile_stmt(s); }
    if (failed()) return;

    // Safety terminator so execution can't fall off the end.
    if (is_main) {
      emit(OP_HALT);
    } else {
      int t = reg_top;
      use(t + 1);
      emit(OP_LOAD_NIL); emit_r(t);
      emit(OP_RET); emit_r(t);
    }

    if (high_water > kMaxReg) { fail("too many registers"); return; }
    f.num_regs = static_cast<uint8_t>(high_water);
  }

  // --- driver ------------------------------------------------------------

  // Assign a function slot to every Lambda node anywhere in the tree. Done in
  // pass 1 so `m.funcs` never reallocates during (re-entrant) pass-2 compilation,
  // which would invalidate the `code`/Function& references held mid-compile.
  void index_lambdas(const Node* n) {
    if (!n) return;
    if (n->kind == NodeKind::Lambda) {
      Function f;
      f.name_const = static_cast<uint32_t>(k_str("<lambda>"));
      f.num_params = static_cast<uint8_t>(n->params.size());
      lambda_func_[n] = static_cast<int>(m.funcs.size());
      m.funcs.push_back(f);
    }
    for (const Node* k : n->kids) index_lambdas(k);
  }

  void run(const Node* program) {
    // Pass 1: index every function so calls resolve forward references.
    m.entry_func = 0;
    Function main_fn;
    main_fn.name_const = static_cast<uint32_t>(k_str("main"));
    m.funcs.push_back(main_fn);

    for (const Node* s : program->kids) {
      if (s->kind != NodeKind::FnDecl) continue;
      if (func_index.count(s->str)) { fail("duplicate function: " + s->str); return; }
      if (s->params.size() > kMaxCount) { fail("too many parameters"); return; }
      Function f;
      f.name_const = static_cast<uint32_t>(k_str(s->str));
      f.num_params = static_cast<uint8_t>(s->params.size());
      func_index[s->str] = static_cast<int>(m.funcs.size());
      m.funcs.push_back(f);
    }

    // Pre-allocate function slots for every lambda (in main, in top-level fns,
    // and nested in other lambdas) before any body is compiled.
    index_lambdas(program);

    // Pass 2: compile bodies. main = top-level non-FnDecl statements.
    std::vector<const Node*> main_stmts;
    for (const Node* s : program->kids)
      if (s->kind != NodeKind::FnDecl) main_stmts.push_back(s);
    compile_body(m.funcs[0], main_stmts, {}, /*is_main=*/true);
    if (failed()) return;

    for (const Node* s : program->kids) {
      if (s->kind != NodeKind::FnDecl) continue;
      int idx = func_index[s->str];
      std::vector<const Node*> body{s->kids[0]};  // the Block
      compile_body(m.funcs[idx], body, s->params, /*is_main=*/false);
      if (failed()) return;
    }
  }
};

}  // namespace

CompileResult compile(const Node* program) {
  Compiler c;
  if (!program || program->kind != NodeKind::Program) {
    c.result.error = "no program to compile";
    c.result.ok = false;
    return c.result;
  }
  c.run(program);
  c.result.error = c.error;
  c.result.err_line = c.err_line;
  c.result.ok = c.error.empty();
  return c.result;
}

}  // namespace coal
