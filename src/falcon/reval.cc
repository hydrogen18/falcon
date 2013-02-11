#include <Python.h>
#include <opcode.h>
#include <marshal.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "reval.h"
#include "rcompile.h"

static bool logging_enabled() {
  static bool _is_logging = getenv("EVAL_LOG") != NULL;
  return _is_logging;
}

#ifdef FALCON_DEBUG
#define EVAL_LOG(...) if (logging_enabled()) { fprintf(stderr, __VA_ARGS__); fputs("\n", stderr); }
#define CHECK_VALID(obj) { Reg_AssertGt(obj->ob_refcnt, 0); }
#else
#define EVAL_LOG(...)
#define CHECK_VALID(obj)
//#define CHECK_VALID(obj) Reg_AssertGt(obj->ob_refcnt, 0);
#endif

typedef PyObject* (*BinaryFunction)(PyObject*, PyObject*);
typedef PyObject* (*UnaryFunction)(PyObject*);

struct GilHelper {
  PyGILState_STATE state_;

  GilHelper() :
      state_(PyGILState_Ensure()) {
  }
  ~GilHelper() {
    PyGILState_Release(state_);
  }
};

RegisterFrame* Evaluator::frame_from_python(PyObject* obj, PyObject* args) {
  GilHelper h;

  if (args == NULL || !PyTuple_Check(args)) {
    throw RException(PyExc_TypeError, "Expected function argument tuple, got: %s", obj_to_str(PyObject_Type(args)));
  }

  RegisterCode* regcode = compiler_->compile(obj);
  if (!regcode) {
    // compilation failed, abort mission!
    return NULL;
  }

  return new RegisterFrame(regcode, obj, args, NULL);
}

RegisterFrame::RegisterFrame(RegisterCode* func, PyObject* obj, PyObject* args, PyObject* kw) {
  code = func;
  kw_ = kw;
  instructions = code->instructions.data();

  globals_ = PyFunction_GetGlobals(func->function);
  builtins_ = PyEval_GetBuiltins();

  call_args = NULL;
  locals_ = NULL;

  registers = new PyObject*[func->num_registers];

  // setup const and local register aliases.
  for (int i = 0; i < code->num_registers; ++i) {
    registers[i] = NULL;
  }

  int num_consts = PyTuple_Size(consts());

  for (int i = 0; i < num_consts; ++i) {
    registers[i] = PyTuple_GET_ITEM(consts(), i) ;
    Py_INCREF(registers[i]);
  }


  int needed_args = code->code()->co_argcount;
  int offset = num_consts;
  if (PyMethod_Check(obj)) {
    PyObject* klass = PyMethod_Class(obj);
    PyObject* self = PyMethod_Self(obj);

    Reg_Assert(self != NULL, "Method call without a bound self.");
    registers[offset++] = self;
    Py_INCREF(self);

    needed_args--;
  }

  PyObject* def_args = PyFunction_GetDefaults(code->function);
  int num_def_args = def_args == NULL ? 0 : PyTuple_Size(def_args);
  int num_args = PyTuple_Size(args);
  if (num_args + num_def_args < needed_args) {
    throw RException(PyExc_TypeError, "Wrong number of arguments for %s, expected %d, got %d.",
                     PyEval_GetFuncName(code->function), needed_args - num_def_args, num_args);
  }


  for (int i = 0; i < needed_args; ++i) {
    if (i < num_args) {
      registers[offset] = PyTuple_GET_ITEM(args, i) ;
    } else {
      registers[offset] = PyTuple_GET_ITEM(def_args, i - num_args);
    }
    Py_XINCREF(registers[offset]);
    ++offset;
  }
}

RegisterFrame::~RegisterFrame() {
  delete [] registers;
  Py_XDECREF(call_args);
}

Evaluator::Evaluator() {
  bzero(op_counts_, sizeof(op_counts_));
  bzero(op_times_, sizeof(op_times_));
  total_count_ = 0;
  last_clock_ = 0;
  compiler_ = new Compiler;
}

PyObject* Evaluator::eval_python(PyObject* func, PyObject* args) {
  RegisterFrame* frame = frame_from_python(func, args);
  if (!frame) {
    return NULL;
  }

  PyObject* result = eval(frame);
  delete frame;
  return result;
}

void Evaluator::dump_status() {
  Log_Info("Evaluator status:");
  Log_Info("%d operations executed.", total_count_);
  for (int i = 0; i < 256; ++i) {
    if (op_counts_[i] > 0) {
      Log_Info("%20s : %10d, %.3f", OpUtil::name(i), op_counts_[i], op_times_[i] / 1e9);
    }
  }
}

void Evaluator::collect_info(int opcode) {
  ++total_count_;
  //  ++op_counts_[opcode];
//    if (total_count_ % 113 == 0) {
  //    op_times_[opcode] += rdtsc() - last_clock_;
  //    last_clock_ = rdtsc();
  //  }
  if (total_count_ > 1e9) {
    dump_status();
    throw RException(PyExc_SystemError, "Execution entered infinite loop.");
  }
}

template<class OpType, class SubType>
struct Op {
};

template<class SubType>
struct Op<RegOp, SubType> {
  f_inline
  void eval(Evaluator* eval, RegisterFrame* state, const char** pc, PyObject** registers) {
    RegOp op = *((RegOp*) *pc);
    EVAL_LOG("%5d: %s", state->offset(*pc), op.str().c_str());
    *pc += sizeof(RegOp);
    ((SubType*) this)->_eval(eval, state, op, registers);
  }

  static SubType instance;
};

template<class SubType>
SubType Op<RegOp, SubType>::instance;

template<class SubType>
struct Op<VarRegOp, SubType> {
  f_inline
  void eval(Evaluator* eval, RegisterFrame* state, const char** pc, PyObject** registers) {
    VarRegOp *op = (VarRegOp*) (*pc);
    EVAL_LOG("%5d: %s", state->offset(*pc), op->str().c_str());
    *pc += RMachineOp::size(*op);
    ((SubType*) this)->_eval(eval, state, op, registers);
  }

  static SubType instance;
};
template<class SubType>
SubType Op<VarRegOp, SubType>::instance;

template<class SubType>
struct Op<BranchOp, SubType> {
  f_inline
  void eval(Evaluator* eval, RegisterFrame* state, const char** pc, PyObject** registers) {
    BranchOp op = *((BranchOp*) *pc);
    EVAL_LOG("%5d: %s", state->offset(*pc), op.str().c_str());
    ((SubType*) this)->_eval(eval, state, op, pc, registers);
  }

  static SubType instance;
};

template<class SubType>
SubType Op<BranchOp, SubType>::instance;

struct IntegerOps {
#define _OP(name, op)\
  static f_inline PyObject* name(PyObject* w, PyObject* v) {\
    if (!PyInt_CheckExact(v) || !PyInt_CheckExact(w)) {\
      return NULL;\
    }\
    register long a, b, i;\
    a = PyInt_AS_LONG(w);\
    b = PyInt_AS_LONG(v);\
    i = (long) ((unsigned long) a op b);\
    if ((i ^ a) < 0 && (i ^ b) < 0) {\
      return NULL;\
    }\
    return PyInt_FromLong(i);\
  }

  _OP(add, +)
  _OP(sub, -)
  _OP(mul, *)
  _OP(div, /)
  _OP(mod, %)

  static f_inline PyObject* compare(PyObject* w, PyObject* v, int arg) {
    if (!PyInt_CheckExact(v) || !PyInt_CheckExact(w)) {
      return NULL;
    }

    long a = PyInt_AS_LONG(w);
    long b = PyInt_AS_LONG(v);

    switch (arg) {
    case PyCmp_LT:
      return a < b ? Py_True : Py_False ;
    case PyCmp_LE:
      return a <= b ? Py_True : Py_False ;
    case PyCmp_EQ:
      return a == b ? Py_True : Py_False ;
    case PyCmp_NE:
      return a != b ? Py_True : Py_False ;
    case PyCmp_GT:
      return a > b ? Py_True : Py_False ;
    case PyCmp_GE:
      return a >= b ? Py_True : Py_False ;
    case PyCmp_IS:
      return v == w ? Py_True : Py_False ;
    case PyCmp_IS_NOT:
      return v != w ? Py_True : Py_False ;
    default:
      return NULL;
    }

    return NULL;
  }
};

struct FloatOps {
  static f_inline PyObject* compare(PyObject* w, PyObject* v, int arg) {
    if (!PyFloat_CheckExact(v) || !PyFloat_CheckExact(w)) {
      return NULL;
    }

    double a = PyFloat_AsDouble(w);
    double b = PyFloat_AsDouble(v);

    switch (arg) {
    case PyCmp_LT:
      return a < b ? Py_True : Py_False ;
    case PyCmp_LE:
      return a <= b ? Py_True : Py_False ;
    case PyCmp_EQ:
      return a == b ? Py_True : Py_False ;
    case PyCmp_NE:
      return a != b ? Py_True : Py_False ;
    case PyCmp_GT:
      return a > b ? Py_True : Py_False ;
    case PyCmp_GE:
      return a >= b ? Py_True : Py_False ;
    case PyCmp_IS:
      return v == w ? Py_True : Py_False ;
    case PyCmp_IS_NOT:
      return v != w ? Py_True : Py_False ;
    default:
      return NULL;
    }

    return NULL;
  }
};

template<int OpCode, BinaryFunction ObjF, BinaryFunction IntegerF>
struct BinaryOpWithSpecialization: public Op<RegOp, BinaryOpWithSpecialization<OpCode, ObjF, IntegerF> > {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* r1 = registers[op.reg_1];
    PyObject* r2 = registers[op.reg_2];
    PyObject* r3 = NULL;
    r3 = IntegerF(r1, r2);
    if (r3 == NULL) {
      r3 = ObjF(r1, r2);
    }
    Py_XDECREF(registers[op.reg_3]);
    registers[op.reg_3] = r3;
  }
};

template<int OpCode, BinaryFunction ObjF>
struct BinaryOp: public Op<RegOp, BinaryOp<OpCode, ObjF> > {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* r1 = registers[op.reg_1];
    PyObject* r2 = registers[op.reg_2];
    CHECK_VALID(r1);
    CHECK_VALID(r2);
    PyObject* r3 = ObjF(r1, r2);
    CHECK_VALID(r3);
    Py_XDECREF(registers[op.reg_3]);
    registers[op.reg_3] = r3;
  }
};

template<int OpCode, UnaryFunction ObjF>
struct UnaryOp: public Op<RegOp, UnaryOp<OpCode, ObjF> > {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* r1 = registers[op.reg_1];
    CHECK_VALID(r1);
    PyObject* r2 = ObjF(r1);
    CHECK_VALID(r2);
    Py_XDECREF(registers[op.reg_2]);
    registers[op.reg_2] = r2;
  }
};

struct UnaryNot: public Op<RegOp, UnaryNot> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* r1 = registers[op.reg_1];
    PyObject* res = PyObject_IsTrue(r1) ? Py_False : Py_True;
    Py_INCREF(res);
    Py_XDECREF(registers[op.reg_2]);
    registers[op.reg_2] = res;
  }
};

struct BinaryPower: public Op<RegOp, BinaryPower> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* r1 = registers[op.reg_1];
    CHECK_VALID(r1);
    PyObject* r2 = registers[op.reg_2];
    CHECK_VALID(r2);
    PyObject* r3 = PyNumber_Power(r1, r2, Py_None);
    CHECK_VALID(r3);
    Py_XDECREF(registers[op.reg_2]);
    registers[op.reg_3] = r3;
  }
};

struct BinarySubscr: public Op<RegOp, BinarySubscr> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* list = registers[op.reg_1];
    PyObject* key = registers[op.reg_2];
    CHECK_VALID(list);
    CHECK_VALID(key);
    PyObject* res = NULL;
    if (PyList_CheckExact(list) && PyInt_CheckExact(key)) {
      Py_ssize_t i = PyInt_AsSsize_t(key);
      if (i < 0) i += PyList_GET_SIZE(list);
      if (i >= 0 && i < PyList_GET_SIZE(list) ) {
        res = PyList_GET_ITEM(list, i) ;
        Py_INCREF(res);
      }
    }
    if (!res) {
      res = PyObject_GetItem(list, key);
    }
    if (!res) {
      throw RException();
    }

    CHECK_VALID(res);

    Py_XDECREF(registers[op.reg_3]);
    registers[op.reg_3] = res;
  }
};

struct InplacePower: public Op<RegOp, InplacePower> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* r1 = registers[op.reg_1];
    CHECK_VALID(r1);
    PyObject* r2 = registers[op.reg_2];
    CHECK_VALID(r2);
    PyObject* r3 = PyNumber_Power(r1, r2, Py_None);
    Py_XDECREF(registers[op.reg_2]);
    CHECK_VALID(r3);
    registers[op.reg_3] = r3;
  }
};

struct CompareOp: public Op<RegOp, CompareOp> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* r1 = registers[op.reg_1];
    CHECK_VALID(r1);
    PyObject* r2 = registers[op.reg_2];
    CHECK_VALID(r2);
    PyObject* r3 = IntegerOps::compare(r1, r2, op.arg);
    if (r3 == NULL) {
      r3 = FloatOps::compare(r1, r2, op.arg);
    }

    if (r3 != NULL) {
      Py_INCREF(r3);
    } else {
      r3 = PyObject_RichCompare(r1, r2, op.arg);
    }
    CHECK_VALID(r3);

    EVAL_LOG("Compare: %s, %s -> %s", obj_to_str(r1), obj_to_str(r2), obj_to_str(r3));
    Py_XDECREF(registers[op.reg_3]);
    registers[op.reg_3] = r3;
  }
};

struct IncRef: public Op<RegOp, IncRef> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    CHECK_VALID(registers[op.reg_1]);
    Py_INCREF(registers[op.reg_1]);
  }
};

struct DecRef: public Op<RegOp, DecRef> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    CHECK_VALID(registers[op.reg_1]);
    Py_DECREF(registers[op.reg_1]);
  }
};

struct LoadLocals: public Op<RegOp, LoadLocals> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    Py_XDECREF(registers[op.reg_1]);
    Py_INCREF(state->locals());
    registers[op.reg_1] = state->locals();
  }
};

struct LoadGlobal: public Op<RegOp, LoadGlobal> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* r1 = PyTuple_GET_ITEM(state->names(), op.arg) ;
    PyObject* r2 = PyDict_GetItem(state->globals(), r1);
    if (r2 == NULL) {
      r2 = PyDict_GetItem(state->builtins(), r1);
    }
    if (r2 == NULL) {
      throw RException(PyExc_NameError, "Global name %.200s not defined.", r1);
    }
    Py_INCREF(r2);
    Py_XDECREF(registers[op.reg_1]);
    CHECK_VALID(r2);
    registers[op.reg_1] = r2;
  }
};

struct LoadName: public Op<RegOp, LoadName> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* r1 = PyTuple_GET_ITEM(state->names(), op.arg) ;
    PyObject* r2 = PyDict_GetItem(state->locals(), r1);
    if (r2 == NULL) {
      r2 = PyDict_GetItem(state->globals(), r1);
    }
    if (r2 == NULL) {
      r2 = PyDict_GetItem(state->builtins(), r1);
    }
    if (r2 == NULL) {
      throw RException(PyExc_NameError, "Name %.200s not defined.", r1);
    }
    Py_INCREF(r2);
    Py_XDECREF(registers[op.reg_1]);
    CHECK_VALID(r2);
    registers[op.reg_1] = r2;
  }
};

struct LoadFast: public Op<RegOp, LoadFast> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    Py_INCREF(registers[op.reg_1]);
    Py_XDECREF(registers[op.reg_2]);
    CHECK_VALID(registers[op.reg_1]);
    registers[op.reg_2] = registers[op.reg_1];
  }
};

struct StoreFast: public Op<RegOp, StoreFast> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    Py_XDECREF(registers[op.reg_2]);
    Py_INCREF(registers[op.reg_1]);
    CHECK_VALID(registers[op.reg_1]);
    registers[op.reg_2] = registers[op.reg_1];
  }
};

struct StoreName: public Op<RegOp, StoreName> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* r1 = PyTuple_GET_ITEM(state->names(), op.arg) ;
    PyObject* r2 = registers[op.reg_1];
    CHECK_VALID(r1);
    CHECK_VALID(r2);
    PyObject_SetItem(state->locals(), r1, r2);
  }
};

struct StoreAttr: public Op<RegOp, StoreAttr> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* t = PyTuple_GET_ITEM(state->names(), op.arg) ;
    PyObject* key = registers[op.reg_1];
    PyObject* value = registers[op.reg_2];
    CHECK_VALID(t);
    CHECK_VALID(key);
    CHECK_VALID(value);
    PyObject_SetAttr(t, key, value);
  }
};

struct StoreSubscr: public Op<RegOp, StoreSubscr> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* key = registers[op.reg_1];
    PyObject* list = registers[op.reg_2];
    PyObject* value = registers[op.reg_3];
    CHECK_VALID(key);
    CHECK_VALID(list);
    CHECK_VALID(value);
    PyObject_SetItem(list, key, value);
  }
};

struct ConstIndex: public Op<RegOp, ConstIndex> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* list = registers[op.reg_1];
    uint8_t key = op.arg;
    Py_XDECREF(registers[op.reg_2]);
    PyObject* pykey = PyInt_FromLong(key);
    registers[op.reg_2] = PyObject_GetItem(list, pykey);
    Py_DECREF(pykey);
    Py_INCREF(registers[op.reg_2]);
    CHECK_VALID(registers[op.reg_2]);
  }
};

struct LoadAttr: public Op<RegOp, LoadAttr> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* obj = registers[op.reg_1];
    PyObject* name = PyTuple_GET_ITEM(state->names(), op.arg) ;
    Py_XDECREF(registers[op.reg_2]);
    registers[op.reg_2] = PyObject_GetAttr(obj, name);
    CHECK_VALID(registers[op.reg_2]);
//    Py_INCREF(registers[op.reg_2]);
  }
};

struct CallFunction: public Op<VarRegOp, CallFunction> {
  f_inline
  void _eval(Evaluator* eval, RegisterFrame* state, VarRegOp *op, PyObject** registers) {
    int na = op->arg & 0xff;
    int nk = (op->arg >> 8) & 0xff;
    int n = nk * 2 + na;
    int i;
    PyObject* fn = registers[op->regs[n]];

    assert(n + 2 == op->num_registers);

    if (state->call_args == NULL || PyTuple_GET_SIZE(state->call_args) != na) {
      Py_XDECREF(state->call_args);
      state->call_args = PyTuple_New(na);
    }

    PyObject* args = state->call_args;
    for (i = 0; i < na; ++i) {
      CHECK_VALID(registers[op->regs[i]]);
      PyTuple_SET_ITEM(args, i, registers[op->regs[i]]);
    }

    PyObject* kwdict = NULL;
    if (nk > 0) {
      kwdict = PyDict_New();
      for (i = na; i < nk * 2; i += 2) {
        CHECK_VALID(registers[op->regs[i]]);
        CHECK_VALID(registers[op->regs[i+i]]);
        PyDict_SetItem(kwdict, registers[op->regs[i]], registers[op->regs[i + 1]]);
      }
    }

    PyObject* res = NULL;
    if (PyCFunction_Check(fn)) {
      res = PyCFunction_Call(fn, args, kwdict);
    } else if (kwdict == NULL) {
      res = eval->eval_python(fn, args);
    }
    if (!res) {
      res = PyObject_Call(fn, args, kwdict);
    }

    if (res == NULL) {
      throw RException();
    }

    int dst = op->regs[n + 1];

    Py_XDECREF(registers[dst]);
    registers[dst] = res;
  }
};

struct GetIter: public Op<RegOp, GetIter> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* res = PyObject_GetIter(registers[op.reg_1]);
    Py_XDECREF(registers[op.reg_2]);
    registers[op.reg_2] = res;
  }
};

struct ForIter: public Op<BranchOp, ForIter> {
  f_inline
  void _eval(Evaluator* eval, RegisterFrame *state, BranchOp op, const char **pc, PyObject** registers) {
    CHECK_VALID(registers[op.reg_1]);
    PyObject* r1 = PyIter_Next(registers[op.reg_1]);
    if (r1) {
      Py_XDECREF(registers[op.reg_2]);
      registers[op.reg_2] = r1;
      *pc += sizeof(BranchOp);
    } else {
      *pc = state->instructions + op.label;
    }

  }
};

struct JumpIfFalseOrPop: public Op<BranchOp, JumpIfFalseOrPop> {
  f_inline
  void _eval(Evaluator* eval, RegisterFrame *state, BranchOp op, const char **pc, PyObject** registers) {
    PyObject *r1 = registers[op.reg_1];
    if (r1 == Py_False || (PyObject_IsTrue(r1) == 0)) {
//      EVAL_LOG("Jumping: %s -> %d", obj_to_str(r1), op.label);
      *pc = state->instructions + op.label;
    } else {
      *pc += sizeof(BranchOp);
    }

  }
};

struct JumpIfTrueOrPop: public Op<BranchOp, JumpIfTrueOrPop> {
  f_inline
  void _eval(Evaluator* eval, RegisterFrame *state, BranchOp op, const char **pc, PyObject** registers) {
    PyObject* r1 = registers[op.reg_1];
    if (r1 == Py_True || (PyObject_IsTrue(r1) == 1)) {
      *pc = state->instructions + op.label;
    } else {
      *pc += sizeof(BranchOp);
    }

  }
};

struct JumpAbsolute: public Op<BranchOp, JumpAbsolute> {
  f_inline
  void _eval(Evaluator* eval, RegisterFrame *state, BranchOp op, const char **pc, PyObject** registers) {
    EVAL_LOG("Jumping to: %d", op.label);
    *pc = state->instructions + op.label;
  }
};

struct ReturnValue: public Op<RegOp, ReturnValue> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* result = registers[op.reg_1];
    Py_INCREF(result);
    throw result;
  }
};

struct Nop: public Op<RegOp, Nop> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {

  }
};

struct BuildTuple: public Op<VarRegOp, BuildTuple> {
  f_inline
  void _eval(Evaluator* eval, RegisterFrame* state, VarRegOp *op, PyObject** registers) {
    int i;
    PyObject* t = PyTuple_New(op->arg);
    for (i = 0; i < op->arg; ++i) {
      PyTuple_SET_ITEM(t, i, registers[op->regs[i]]);
    }
    registers[op->regs[op->arg]] = t;

  }
};

struct BuildList: public Op<VarRegOp, BuildList> {
  f_inline
  void _eval(Evaluator* eval, RegisterFrame* state, VarRegOp *op, PyObject** registers) {
    int i;
    PyObject* t = PyList_New(op->arg);
    for (i = 0; i < op->arg; ++i) {
      PyList_SET_ITEM(t, i, registers[op->regs[i]]);
    }
    registers[op->regs[op->arg]] = t;
  }
};

struct PrintItem: public Op<RegOp, PrintItem> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* v = registers[op.reg_1];
    PyObject* w = op.reg_2 != kBadRegister ? registers[op.reg_2] : PySys_GetObject((char*) "stdout");

    int err = 0;
    if (w != NULL && PyFile_SoftSpace(w, 0)) {
      err = PyFile_WriteString(" ", w);
    }
    if (err == 0) {
      err = PyFile_WriteObject(v, w, Py_PRINT_RAW);
    }
    if (err == 0) {
      /* XXX move into writeobject() ? */
      if (PyString_Check(v)) {
        char *s = PyString_AS_STRING(v);
        Py_ssize_t len = PyString_GET_SIZE(v);
        if (len == 0 || !isspace(Py_CHARMASK(s[len-1]) ) || s[len - 1] == ' ') PyFile_SoftSpace(w, 1);
      }
#ifdef Py_USING_UNICODE
      else if (PyUnicode_Check(v)) {
        Py_UNICODE *s = PyUnicode_AS_UNICODE(v);
        Py_ssize_t len = PyUnicode_GET_SIZE(v);
        if (len == 0 || !Py_UNICODE_ISSPACE(s[len-1]) || s[len - 1] == ' ') PyFile_SoftSpace(w, 1);
      }
#endif
      else PyFile_SoftSpace(w, 1);
    }
  }
};

struct PrintNewline: public Op<RegOp, PrintNewline> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* w = op.reg_2 != kBadRegister ? registers[op.reg_2] : PySys_GetObject((char*) "stdout");
    int err = PyFile_WriteString("\n", w);
    if (err == 0) PyFile_SoftSpace(w, 0);
  }
};

struct ListAppend: public Op<RegOp, ListAppend> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyList_Append(registers[op.reg_1], registers[op.reg_2]);
  }
};

#define ISINDEX(x) ((x) == NULL || PyInt_Check(x) || PyLong_Check(x) || PyIndex_Check(x))
static PyObject * apply_slice(PyObject *u, PyObject *v, PyObject *w) {
  PyTypeObject *tp = u->ob_type;
  PySequenceMethods *sq = tp->tp_as_sequence;

  if (sq && sq->sq_slice && ISINDEX(v) && ISINDEX(w)) {
    Py_ssize_t ilow = 0, ihigh = PY_SSIZE_T_MAX;
    if (!_PyEval_SliceIndex(v, &ilow)) return NULL;
    if (!_PyEval_SliceIndex(w, &ihigh)) return NULL;
    return PySequence_GetSlice(u, ilow, ihigh);
  } else {
    PyObject *slice = PySlice_New(v, w, NULL);
    if (slice != NULL) {
      PyObject *res = PyObject_GetItem(u, slice);
      Py_DECREF(slice);
      return res;
    } else return NULL;
  }
}

struct Slice: public Op<RegOp, Slice> {
  f_inline
  void _eval(Evaluator *eval, RegisterFrame* state, RegOp op, PyObject** registers) {
    PyObject* list = registers[op.reg_1];
    PyObject* left = op.reg_2 != kBadRegister ? registers[op.reg_2] : NULL;
    PyObject* right = op.reg_3 != kBadRegister ? registers[op.reg_3] : NULL;
    registers[op.reg_4] = apply_slice(list, left, right);
  }
};

#define CONCAT(...) __VA_ARGS__

#define REGISTER_OP(opname)\
    static int _force_register_ ## opname = LabelRegistry::add_label(opname, &&op_ ## opname);

#define _DEFINE_OP(opname, impl)\
      /*collectInfo(opname);\*/\
      impl::instance.eval(this, frame, &pc, registers);\
      goto *labels[frame->next_code(pc)];

#define DEFINE_OP(opname, impl)\
    op_##opname:\
      _DEFINE_OP(opname, impl)

#define _BAD_OP(opname)\
        { EVAL_LOG("Not implemented: %s", #opname); throw RException(PyExc_SystemError, "Bad opcode %s", #opname); }

#define BAD_OP(opname)\
    op_##opname: _BAD_OP(opname)

#define FALLTHROUGH(opname) op_##opname:

#define INTEGER_OP(op)\
    a = PyInt_AS_LONG(v);\
    b = PyInt_AS_LONG(w);\
    i = (long)((unsigned long)a op b);\
    if ((i^a) < 0 && (i^b) < 0)\
        goto slow_path;\
    x = PyInt_FromLong(i);

#define BINARY_OP3(opname, objfn, intfn)\
    op_##opname: _DEFINE_OP(opname, BinaryOpWithSpecialization<CONCAT(opname, objfn, intfn)>)

#define BINARY_OP2(opname, objfn)\
    op_##opname: _DEFINE_OP(opname, BinaryOp<CONCAT(opname, objfn)>)

#define UNARY_OP2(opname, objfn)\
    op_##opname: _DEFINE_OP(opname, UnaryOp<CONCAT(opname, objfn)>)

PyObject * Evaluator::eval(RegisterFrame* frame) {
  Reg_Assert(frame != NULL, "NULL frame object.");
  Reg_Assert(PyTuple_Size(frame->code->code()->co_cellvars) == 0, "Cell vars (closures) not supported.");

  register PyObject** registers = frame->registers;
  const char* pc = frame->instructions;

  last_clock_ = rdtsc();

const void* const labels[] = {
  &&op_STOP_CODE,
  &&op_POP_TOP,
  &&op_ROT_TWO,
  &&op_ROT_THREE,
  &&op_DUP_TOP,
  &&op_ROT_FOUR,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_NOP,
  &&op_UNARY_POSITIVE,
  &&op_UNARY_NEGATIVE,
  &&op_UNARY_NOT,
  &&op_UNARY_CONVERT,
  &&op_BADCODE,
  &&op_UNARY_INVERT,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BINARY_POWER,
  &&op_BINARY_MULTIPLY,
  &&op_BINARY_DIVIDE,
  &&op_BINARY_MODULO,
  &&op_BINARY_ADD,
  &&op_BINARY_SUBTRACT,
  &&op_BINARY_SUBSCR,
  &&op_BINARY_FLOOR_DIVIDE,
  &&op_BINARY_TRUE_DIVIDE,
  &&op_INPLACE_FLOOR_DIVIDE,
  &&op_INPLACE_TRUE_DIVIDE,
  &&op_SLICE,
  &&op_SLICE,
  &&op_SLICE,
  &&op_SLICE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_STORE_SLICE,
  &&op_STORE_SLICE,
  &&op_STORE_SLICE,
  &&op_STORE_SLICE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_DELETE_SLICE,
  &&op_DELETE_SLICE,
  &&op_DELETE_SLICE,
  &&op_DELETE_SLICE,
  &&op_STORE_MAP,
  &&op_INPLACE_ADD,
  &&op_INPLACE_SUBTRACT,
  &&op_INPLACE_MULTIPLY,
  &&op_INPLACE_DIVIDE,
  &&op_INPLACE_MODULO,
  &&op_STORE_SUBSCR,
  &&op_DELETE_SUBSCR,
  &&op_BINARY_LSHIFT,
  &&op_BINARY_RSHIFT,
  &&op_BINARY_AND,
  &&op_BINARY_XOR,
  &&op_BINARY_OR,
  &&op_INPLACE_POWER,
  &&op_GET_ITER,
  &&op_BADCODE,
  &&op_PRINT_EXPR,
  &&op_PRINT_ITEM,
  &&op_PRINT_NEWLINE,
  &&op_PRINT_ITEM_TO,
  &&op_PRINT_NEWLINE_TO,
  &&op_INPLACE_LSHIFT,
  &&op_INPLACE_RSHIFT,
  &&op_INPLACE_AND,
  &&op_INPLACE_XOR,
  &&op_INPLACE_OR,
  &&op_BREAK_LOOP,
  &&op_WITH_CLEANUP,
  &&op_LOAD_LOCALS,
  &&op_RETURN_VALUE,
  &&op_IMPORT_STAR,
  &&op_EXEC_STMT,
  &&op_YIELD_VALUE,
  &&op_POP_BLOCK,
  &&op_END_FINALLY,
  &&op_BUILD_CLASS,
  &&op_STORE_NAME,
  &&op_DELETE_NAME,
  &&op_UNPACK_SEQUENCE,
  &&op_FOR_ITER,
  &&op_LIST_APPEND,
  &&op_STORE_ATTR,
  &&op_DELETE_ATTR,
  &&op_STORE_GLOBAL,
  &&op_DELETE_GLOBAL,
  &&op_DUP_TOPX,
  &&op_LOAD_CONST,
  &&op_LOAD_NAME,
  &&op_BUILD_TUPLE,
  &&op_BUILD_LIST,
  &&op_BUILD_SET,
  &&op_BUILD_MAP,
  &&op_LOAD_ATTR,
  &&op_COMPARE_OP,
  &&op_IMPORT_NAME,
  &&op_IMPORT_FROM,
  &&op_JUMP_FORWARD,
  &&op_JUMP_IF_FALSE_OR_POP,
  &&op_JUMP_IF_TRUE_OR_POP,
  &&op_JUMP_ABSOLUTE,
  &&op_POP_JUMP_IF_FALSE,
  &&op_POP_JUMP_IF_TRUE,
  &&op_LOAD_GLOBAL,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_CONTINUE_LOOP,
  &&op_SETUP_LOOP,
  &&op_SETUP_EXCEPT,
  &&op_SETUP_FINALLY,
  &&op_BADCODE,
  &&op_LOAD_FAST,
  &&op_STORE_FAST,
  &&op_DELETE_FAST,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_RAISE_VARARGS,
  &&op_CALL_FUNCTION,
  &&op_MAKE_FUNCTION,
  &&op_BUILD_SLICE,
  &&op_MAKE_CLOSURE,
  &&op_LOAD_CLOSURE,
  &&op_LOAD_DEREF,
  &&op_STORE_DEREF,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_CALL_FUNCTION_VAR,
  &&op_CALL_FUNCTION_KW,
  &&op_CALL_FUNCTION_VAR_KW,
  &&op_SETUP_WITH,
  &&op_BADCODE,
  &&op_EXTENDED_ARG,
  &&op_SET_ADD,
  &&op_MAP_ADD,
  &&op_INCREF,
  &&op_DECREF,
  &&op_CONST_INDEX
}
;

EVAL_LOG("New frame: %s", PyEval_GetFuncName(frame->code->function));
try {
goto *labels[frame->next_code(pc)];

        BINARY_OP3(BINARY_MULTIPLY, PyNumber_Multiply, IntegerOps::mul);
BINARY_OP3(BINARY_DIVIDE, PyNumber_Divide, IntegerOps::div);
BINARY_OP3(BINARY_ADD, PyNumber_Add, IntegerOps::add);
BINARY_OP3(BINARY_SUBTRACT, PyNumber_Subtract, IntegerOps::sub);
BINARY_OP3(BINARY_MODULO, PyNumber_Remainder, IntegerOps::mod);

BINARY_OP2(BINARY_OR, PyNumber_Or);
BINARY_OP2(BINARY_XOR, PyNumber_Xor);
BINARY_OP2(BINARY_AND, PyNumber_And);
BINARY_OP2(BINARY_RSHIFT, PyNumber_Rshift);
BINARY_OP2(BINARY_LSHIFT, PyNumber_Lshift);
BINARY_OP2(BINARY_TRUE_DIVIDE, PyNumber_TrueDivide);
BINARY_OP2(BINARY_FLOOR_DIVIDE, PyNumber_FloorDivide);
DEFINE_OP(BINARY_POWER, BinaryPower);
DEFINE_OP(BINARY_SUBSCR, BinarySubscr);

BINARY_OP3(INPLACE_MULTIPLY, PyNumber_InPlaceMultiply, IntegerOps::mul);
BINARY_OP3(INPLACE_DIVIDE, PyNumber_InPlaceDivide, IntegerOps::div);
BINARY_OP3(INPLACE_ADD, PyNumber_InPlaceAdd, IntegerOps::add);
BINARY_OP3(INPLACE_SUBTRACT, PyNumber_InPlaceSubtract, IntegerOps::sub);
BINARY_OP3(INPLACE_MODULO, PyNumber_InPlaceRemainder, IntegerOps::mod);

BINARY_OP2(INPLACE_OR, PyNumber_InPlaceOr);
BINARY_OP2(INPLACE_XOR, PyNumber_InPlaceXor);
BINARY_OP2(INPLACE_AND, PyNumber_InPlaceAnd);
BINARY_OP2(INPLACE_RSHIFT, PyNumber_InPlaceRshift);
BINARY_OP2(INPLACE_LSHIFT, PyNumber_InPlaceLshift);
BINARY_OP2(INPLACE_TRUE_DIVIDE, PyNumber_InPlaceTrueDivide);
BINARY_OP2(INPLACE_FLOOR_DIVIDE, PyNumber_InPlaceFloorDivide);
DEFINE_OP(INPLACE_POWER, InplacePower);

UNARY_OP2(UNARY_INVERT, PyNumber_Invert);
UNARY_OP2(UNARY_CONVERT, PyObject_Repr);
UNARY_OP2(UNARY_NEGATIVE, PyNumber_Negative);
UNARY_OP2(UNARY_POSITIVE, PyNumber_Positive);

DEFINE_OP(UNARY_NOT, UnaryNot);

DEFINE_OP(LOAD_FAST, LoadFast);
DEFINE_OP(LOAD_LOCALS, LoadLocals);
DEFINE_OP(LOAD_GLOBAL, LoadGlobal);
DEFINE_OP(LOAD_NAME, LoadName);
DEFINE_OP(LOAD_ATTR, LoadAttr);

DEFINE_OP(STORE_NAME, StoreName);
DEFINE_OP(STORE_ATTR, StoreAttr);
DEFINE_OP(STORE_SUBSCR, StoreSubscr);
DEFINE_OP(STORE_FAST, StoreFast);

DEFINE_OP(CONST_INDEX, ConstIndex);

DEFINE_OP(GET_ITER, GetIter);
DEFINE_OP(FOR_ITER, ForIter);
DEFINE_OP(RETURN_VALUE, ReturnValue);

DEFINE_OP(BUILD_TUPLE, BuildTuple);
DEFINE_OP(BUILD_LIST, BuildList);

DEFINE_OP(PRINT_NEWLINE, PrintNewline);
DEFINE_OP(PRINT_NEWLINE_TO, PrintNewline);
DEFINE_OP(PRINT_ITEM, PrintItem);
DEFINE_OP(PRINT_ITEM_TO, PrintItem);

FALLTHROUGH(CALL_FUNCTION);
FALLTHROUGH(CALL_FUNCTION_VAR);
FALLTHROUGH(CALL_FUNCTION_KW);
DEFINE_OP(CALL_FUNCTION_VAR_KW, CallFunction);

FALLTHROUGH(POP_JUMP_IF_FALSE);
DEFINE_OP(JUMP_IF_FALSE_OR_POP, JumpIfFalseOrPop);

FALLTHROUGH(POP_JUMP_IF_TRUE);
DEFINE_OP(JUMP_IF_TRUE_OR_POP, JumpIfTrueOrPop);

DEFINE_OP(JUMP_ABSOLUTE, JumpAbsolute);
DEFINE_OP(COMPARE_OP, CompareOp);
DEFINE_OP(INCREF, IncRef);
DEFINE_OP(DECREF, DecRef);

DEFINE_OP(LIST_APPEND, ListAppend);
DEFINE_OP(SLICE, Slice);

BAD_OP(SETUP_LOOP);
BAD_OP(POP_BLOCK);
BAD_OP(LOAD_CONST);
BAD_OP(JUMP_FORWARD);
BAD_OP(MAP_ADD);
BAD_OP(SET_ADD);
BAD_OP(EXTENDED_ARG);
BAD_OP(SETUP_WITH);
BAD_OP(STORE_DEREF);
BAD_OP(LOAD_DEREF);
BAD_OP(LOAD_CLOSURE);
BAD_OP(MAKE_CLOSURE);
BAD_OP(BUILD_SLICE);
BAD_OP(MAKE_FUNCTION);
BAD_OP(RAISE_VARARGS);
BAD_OP(DELETE_FAST);
BAD_OP(SETUP_FINALLY);
BAD_OP(SETUP_EXCEPT);
BAD_OP(CONTINUE_LOOP);
BAD_OP(IMPORT_FROM);
BAD_OP(IMPORT_NAME);
BAD_OP(BUILD_MAP);
BAD_OP(BUILD_SET);
BAD_OP(DUP_TOPX);
BAD_OP(DELETE_GLOBAL);
BAD_OP(STORE_GLOBAL);
BAD_OP(DELETE_ATTR);
BAD_OP(UNPACK_SEQUENCE);
BAD_OP(DELETE_NAME);
BAD_OP(BUILD_CLASS);
BAD_OP(END_FINALLY);
BAD_OP(YIELD_VALUE);
BAD_OP(EXEC_STMT);
BAD_OP(IMPORT_STAR);
BAD_OP(WITH_CLEANUP);
BAD_OP(BREAK_LOOP);
BAD_OP(PRINT_EXPR);
BAD_OP(DELETE_SUBSCR);
BAD_OP(STORE_MAP);
BAD_OP(DELETE_SLICE);
BAD_OP(STORE_SLICE);
BAD_OP(NOP);
BAD_OP(ROT_FOUR);
BAD_OP(DUP_TOP);
BAD_OP(ROT_THREE);
BAD_OP(ROT_TWO);
BAD_OP(POP_TOP);
BAD_OP(STOP_CODE);
op_BADCODE: {
      EVAL_LOG("Jump to invalid opcode!?");
throw RException(PyExc_SystemError, "Invalid jump.");
}
} catch (PyObject* result) {
  EVAL_LOG("Leaving frame: %s", PyEval_GetFuncName(frame->code->function));
return result;
} catch (RException &error) {
  EVAL_LOG("Leaving frame: %s", PyEval_GetFuncName(frame->code->function));
Reg_Assert(error.exception != NULL, "Error without exception set.");
error.set_python_err();
  return NULL;
}
Log_Fatal("Shouldn't reach here.");
return NULL;
}
