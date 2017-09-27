# Tail Recursion Detection & Optimization
Let's detect and optimize tail recursion in function definitions.

This requires a new executable format (Instructions, instead of Operations).
My hope is to only implement a small subset of operations as Instructions,
just enough for simple tail recursion optimization,
and generically wrap most existing Operations as Instructions.

## Instructions (New Executable Format)
The body of a function is compiled into an IExpression, which is a new
executable representation of an expression. To execute a function call,
this code:
  return body_->eval(f);
is replaced by:
  Instruction* IP = &*body_;
  while (IP = IP->iexec(f)) // unique_ptr<Frame>& f, frame can be replaced
    continue;
  return f[0]; // slot 0 of a call frame is the return value

Actually, the above code is appropriate for Call_Expr::eval.
```
Instruction* ICall:iexec(unique_ptr<Frame>& f)
{
    auto fun = f[fun_->result_].to<Function>(...);
    Value arg = f[arg_->result_];
    unique_ptr<Frame> f2 = Frame::make(nslots_, ...);
}
```

IExpression:
* has Shared pointers to zero or more argument IExpressions. This DAG is
  constructed during analysis.
* has a slot_t result_, which is where it stashes the result value. This field
  is initialized during codegen.
* has an Instruction* next_instruction, the next instruction to execute.
  This is initialized during codegen.
An IExpression uses a CPS representation. It has slot_t result_, which is where
it stashes the result value. It has Shared pointers to zero or more
argument IExpressions. At run time, the arguments are executed first. The
IExpression simply reads the slots of each argument IExpression to get its
argument values. The IExpression graph is a DAG; the same iexpr object can be
referenced as an argument by multiple consumer iexprs.

An IAction doesn't have a result.

An IGenerator needs access to a value sink (List_Builder) where it can send
each value it generates. I guess it contains a Shared pointer to a Value_Sink
object, possibly containing a receive(value,frame) method, or possibly
containing a slot_t index of a List_Builder object stored in the Frame.

IExpression, IAction, IGenerator are subtypes of Instruction.
Probably, I'd like these to be separate classes, and to have a compile time
distinction between expressions, actions and generators. This moves certain
errors (eg action in an expression context) from run time to compile time.
And requires generic operations like Block to generate different code during
analysis depending on whether the result is a Block_Expression, a Block_Action
or a Block_Generator.

Instruction has an iexec method:
    Instruction* iexec(Frame&)
iexec(f) returns the next instruction to be executed.
Is this the CPS continuation?

During analysis, we construct an Instruction DAG, very similar to the current
Operation tree. Eg, IAdd contains pointers to its left and right arguments.
It's a DAG because local variables in a block are represented by IExpressions,
and multiple references to a variable in the code result in multiple
references to the variable object. Frame slots are not allocated during
analysis.
* An ILet contains a dictionary mapping names to IExpressions, and a body.
  The definientia are direct expressions, not IThunks.
* An IBlock contains a dictionary mapping names to IThunks, and a body.
  The IThunks prevent reference loops in the instruction DAG. Recursive
  references within an IThunk are analyzed as INonlocal references.
* De-thunking definitions is a valuable optimization for GL.

The code generation phase (codegen) arranges the instructions into a sequence
and allocates frame slots.

Tail recursion optimization: a tail call is implemented by replacing
the current frame with a new frame and jumping to the continuation instruction.
TODO: how exactly is this implemented?
* frame[0] is always the result of a function call. The frame can be replaced
  by another function's frame while interpreting a function body (see code
  above), and the result is still in the same known location. When the function
  call returns, frame[0] is usually moved to a slot in the caller's frame.
* Alternatively, the result of a function call is stored directly in a slot
  (specified at runtime) in the caller's frame.
* An instruction that performs a tail call must replace the current frame with
  a new frame. So the Instruction API must support this somehow.
  * Byte code interpreter that switches on the opcode. The current frame is one
    of the VM registers that an instruction can read and write.
  * `Instruction* iexec(unique_ptr<Frame>&)` allows any Instruction to replace
    the Frame. There is an ITailCall instruction which must be generated by
    the compiler for a call in a tail position. During analysis. There is a
    `tail_position` flag as part of the analyzer state.
  * Or we achieve the same thing at run-time.
    `Instruction* tail_exec(unique_ptr<Frame>&)` is called on the body of a
    function. IIf_Else::tail_exec() calls tail_exec() on its then or else
    clause. Ditto for IBlock. ICall::tail_exec() does a tail call.
  * Instructions are really a CPS system, and tail recursion optimization
    just falls out. Abstractly, at run time, an IExpression is passed a
    continuation function, which it tail-calls with the expression result.
    A function tail calls its body with its own continuation.
    But this must happen without growing the C stack. So there is a for loop.
    Tail-calling a continuation is actually done by:
    * putting the result (aka the continuation argument) into a compile-time
      specified frame slot of the "current frame" and jumping to another
      instruction.

How are Instructions generated?
* from Phrase::encode, as an alternative to analyze. Maybe analyze() eventually
  goes away, and Instruction is the new IR/Executable format?
* from Operation. Maybe Operation evolves into the IR, and Instruction becomes
  the new executable code.
* Maybe Instruction is a subclass of Meaning