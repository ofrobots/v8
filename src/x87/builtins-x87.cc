// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if V8_TARGET_ARCH_X87

#include "src/code-factory.h"
#include "src/codegen.h"
#include "src/deoptimizer.h"
#include "src/full-codegen/full-codegen.h"
#include "src/x87/frames-x87.h"

namespace v8 {
namespace internal {


#define __ ACCESS_MASM(masm)


void Builtins::Generate_Adaptor(MacroAssembler* masm,
                                CFunctionId id,
                                BuiltinExtraArguments extra_args) {
  // ----------- S t a t e -------------
  //  -- eax                : number of arguments excluding receiver
  //  -- edi                : target
  //  -- edx                : new.target
  //  -- esp[0]             : return address
  //  -- esp[4]             : last argument
  //  -- ...
  //  -- esp[4 * argc]      : first argument
  //  -- esp[4 * (argc +1)] : receiver
  // -----------------------------------
  __ AssertFunction(edi);

  // Make sure we operate in the context of the called function (for example
  // ConstructStubs implemented in C++ will be run in the context of the caller
  // instead of the callee, due to the way that [[Construct]] is defined for
  // ordinary functions).
  __ mov(esi, FieldOperand(edi, JSFunction::kContextOffset));

  // Insert extra arguments.
  int num_extra_args = 0;
  if (extra_args != BuiltinExtraArguments::kNone) {
    __ PopReturnAddressTo(ecx);
    if (extra_args & BuiltinExtraArguments::kTarget) {
      ++num_extra_args;
      __ Push(edi);
    }
    if (extra_args & BuiltinExtraArguments::kNewTarget) {
      ++num_extra_args;
      __ Push(edx);
    }
    __ PushReturnAddressFrom(ecx);
  }

  // JumpToExternalReference expects eax to contain the number of arguments
  // including the receiver and the extra arguments.
  __ add(eax, Immediate(num_extra_args + 1));

  __ JumpToExternalReference(ExternalReference(id, masm->isolate()));
}


static void CallRuntimePassFunction(
    MacroAssembler* masm, Runtime::FunctionId function_id) {
  // ----------- S t a t e -------------
  //  -- edx : new target (preserved for callee)
  //  -- edi : target function (preserved for callee)
  // -----------------------------------

  FrameScope scope(masm, StackFrame::INTERNAL);
  // Push a copy of the target function and the new target.
  __ push(edi);
  __ push(edx);
  // Function is also the parameter to the runtime call.
  __ push(edi);

  __ CallRuntime(function_id, 1);
  // Restore target function and new target.
  __ pop(edx);
  __ pop(edi);
}


static void GenerateTailCallToSharedCode(MacroAssembler* masm) {
  __ mov(eax, FieldOperand(edi, JSFunction::kSharedFunctionInfoOffset));
  __ mov(eax, FieldOperand(eax, SharedFunctionInfo::kCodeOffset));
  __ lea(eax, FieldOperand(eax, Code::kHeaderSize));
  __ jmp(eax);
}


static void GenerateTailCallToReturnedCode(MacroAssembler* masm) {
  __ lea(eax, FieldOperand(eax, Code::kHeaderSize));
  __ jmp(eax);
}


void Builtins::Generate_InOptimizationQueue(MacroAssembler* masm) {
  // Checking whether the queued function is ready for install is optional,
  // since we come across interrupts and stack checks elsewhere.  However,
  // not checking may delay installing ready functions, and always checking
  // would be quite expensive.  A good compromise is to first check against
  // stack limit as a cue for an interrupt signal.
  Label ok;
  ExternalReference stack_limit =
      ExternalReference::address_of_stack_limit(masm->isolate());
  __ cmp(esp, Operand::StaticVariable(stack_limit));
  __ j(above_equal, &ok, Label::kNear);

  CallRuntimePassFunction(masm, Runtime::kTryInstallOptimizedCode);
  GenerateTailCallToReturnedCode(masm);

  __ bind(&ok);
  GenerateTailCallToSharedCode(masm);
}


static void Generate_JSConstructStubHelper(MacroAssembler* masm,
                                           bool is_api_function,
                                           bool create_implicit_receiver) {
  // ----------- S t a t e -------------
  //  -- eax: number of arguments
  //  -- edi: constructor function
  //  -- ebx: allocation site or undefined
  //  -- edx: new target
  // -----------------------------------

  // Enter a construct frame.
  {
    FrameScope scope(masm, StackFrame::CONSTRUCT);

    // Preserve the incoming parameters on the stack.
    __ AssertUndefinedOrAllocationSite(ebx);
    __ push(ebx);
    __ SmiTag(eax);
    __ push(eax);

    if (create_implicit_receiver) {
      __ push(edi);
      __ push(edx);

      // Try to allocate the object without transitioning into C code. If any of
      // the preconditions is not met, the code bails out to the runtime call.
      Label rt_call, allocated;
      if (FLAG_inline_new) {
        // Verify that the new target is a JSFunction.
        __ CmpObjectType(edx, JS_FUNCTION_TYPE, ebx);
        __ j(not_equal, &rt_call);

        // Load the initial map and verify that it is in fact a map.
        // edx: new target
        __ mov(eax,
               FieldOperand(edx, JSFunction::kPrototypeOrInitialMapOffset));
        // Will both indicate a NULL and a Smi
        __ JumpIfSmi(eax, &rt_call);
        // edi: constructor
        // eax: initial map (if proven valid below)
        __ CmpObjectType(eax, MAP_TYPE, ebx);
        __ j(not_equal, &rt_call);

        // Fall back to runtime if the expected base constructor and base
        // constructor differ.
        __ cmp(edi, FieldOperand(eax, Map::kConstructorOrBackPointerOffset));
        __ j(not_equal, &rt_call);

        // Check that the constructor is not constructing a JSFunction (see
        // comments in Runtime_NewObject in runtime.cc). In which case the
        // initial map's instance type would be JS_FUNCTION_TYPE.
        // edi: constructor
        // eax: initial map
        __ CmpInstanceType(eax, JS_FUNCTION_TYPE);
        __ j(equal, &rt_call);

        // Now allocate the JSObject on the heap.
        // edi: constructor
        // eax: initial map
        __ movzx_b(edi, FieldOperand(eax, Map::kInstanceSizeOffset));
        __ shl(edi, kPointerSizeLog2);

        __ Allocate(edi, ebx, edi, no_reg, &rt_call, NO_ALLOCATION_FLAGS);

        Factory* factory = masm->isolate()->factory();

        // Allocated the JSObject, now initialize the fields.
        // eax: initial map
        // ebx: JSObject (not HeapObject tagged - the actual address).
        // edi: start of next object
        __ mov(Operand(ebx, JSObject::kMapOffset), eax);
        __ mov(ecx, factory->empty_fixed_array());
        __ mov(Operand(ebx, JSObject::kPropertiesOffset), ecx);
        __ mov(Operand(ebx, JSObject::kElementsOffset), ecx);
        __ lea(ecx, Operand(ebx, JSObject::kHeaderSize));

        // Add the object tag to make the JSObject real, so that we can continue
        // and jump into the continuation code at any time from now on.
        __ or_(ebx, Immediate(kHeapObjectTag));

        // Fill all the in-object properties with the appropriate filler.
        // ebx: JSObject (tagged)
        // ecx: First in-object property of JSObject (not tagged)
        __ mov(edx, factory->undefined_value());

        if (!is_api_function) {
          Label no_inobject_slack_tracking;

          // The code below relies on these assumptions.
          STATIC_ASSERT(Map::kNoSlackTracking == 0);
          STATIC_ASSERT(Map::ConstructionCounter::kNext == 32);
          // Check if slack tracking is enabled.
          __ mov(esi, FieldOperand(eax, Map::kBitField3Offset));
          __ shr(esi, Map::ConstructionCounter::kShift);
          __ j(zero, &no_inobject_slack_tracking);  // Map::kNoSlackTracking
          __ push(esi);  // Save allocation count value.
          // Decrease generous allocation count.
          __ sub(FieldOperand(eax, Map::kBitField3Offset),
                 Immediate(1 << Map::ConstructionCounter::kShift));

          // Allocate object with a slack.
          __ movzx_b(esi, FieldOperand(eax, Map::kUnusedPropertyFieldsOffset));
          __ neg(esi);
          __ lea(esi, Operand(edi, esi, times_pointer_size, 0));
          // esi: offset of first field after pre-allocated fields
          if (FLAG_debug_code) {
            __ cmp(ecx, esi);
            __ Assert(less_equal,
                      kUnexpectedNumberOfPreAllocatedPropertyFields);
          }
          __ InitializeFieldsWithFiller(ecx, esi, edx);

          // To allow truncation fill the remaining fields with one pointer
          // filler map.
          __ mov(edx, factory->one_pointer_filler_map());
          __ InitializeFieldsWithFiller(ecx, edi, edx);

          __ pop(esi);  // Restore allocation count value before decreasing.
          __ cmp(esi, Map::kSlackTrackingCounterEnd);
          __ j(not_equal, &allocated);

          // Push the object to the stack, and then the initial map as
          // an argument to the runtime call.
          __ push(ebx);
          __ push(eax);  // initial map
          __ CallRuntime(Runtime::kFinalizeInstanceSize, 1);
          __ pop(ebx);

          // Continue with JSObject being successfully allocated
          // ebx: JSObject (tagged)
          __ jmp(&allocated);

          __ bind(&no_inobject_slack_tracking);
        }

        __ InitializeFieldsWithFiller(ecx, edi, edx);

        // Continue with JSObject being successfully allocated
        // ebx: JSObject (tagged)
        __ jmp(&allocated);
      }

      // Allocate the new receiver object using the runtime call.
      // edx: new target
      __ bind(&rt_call);
      int offset = kPointerSize;

      // Must restore esi (context) and edi (constructor) before calling
      // runtime.
      __ mov(esi, Operand(ebp, StandardFrameConstants::kContextOffset));
      __ mov(edi, Operand(esp, offset));
      __ push(edi);  // constructor function
      __ push(edx);  // new target
      __ CallRuntime(Runtime::kNewObject, 2);
      __ mov(ebx, eax);  // store result in ebx

      // New object allocated.
      // ebx: newly allocated object
      __ bind(&allocated);

      // Restore the parameters.
      __ pop(edx);  // new.target
      __ pop(edi);  // Constructor function.

      // Retrieve smi-tagged arguments count from the stack.
      __ mov(eax, Operand(esp, 0));
    }

    __ SmiUntag(eax);

    if (create_implicit_receiver) {
      // Push the allocated receiver to the stack. We need two copies
      // because we may have to return the original one and the calling
      // conventions dictate that the called function pops the receiver.
      __ push(ebx);
      __ push(ebx);
    } else {
      __ PushRoot(Heap::kTheHoleValueRootIndex);
    }

    // Set up pointer to last argument.
    __ lea(ebx, Operand(ebp, StandardFrameConstants::kCallerSPOffset));

    // Copy arguments and receiver to the expression stack.
    Label loop, entry;
    __ mov(ecx, eax);
    __ jmp(&entry);
    __ bind(&loop);
    __ push(Operand(ebx, ecx, times_4, 0));
    __ bind(&entry);
    __ dec(ecx);
    __ j(greater_equal, &loop);

    // Call the function.
    if (is_api_function) {
      __ mov(esi, FieldOperand(edi, JSFunction::kContextOffset));
      Handle<Code> code =
          masm->isolate()->builtins()->HandleApiCallConstruct();
      __ call(code, RelocInfo::CODE_TARGET);
    } else {
      ParameterCount actual(eax);
      __ InvokeFunction(edi, edx, actual, CALL_FUNCTION,
                        CheckDebugStepCallWrapper());
    }

    // Store offset of return address for deoptimizer.
    if (create_implicit_receiver && !is_api_function) {
      masm->isolate()->heap()->SetConstructStubDeoptPCOffset(masm->pc_offset());
    }

    // Restore context from the frame.
    __ mov(esi, Operand(ebp, StandardFrameConstants::kContextOffset));

    if (create_implicit_receiver) {
      // If the result is an object (in the ECMA sense), we should get rid
      // of the receiver and use the result; see ECMA-262 section 13.2.2-7
      // on page 74.
      Label use_receiver, exit;

      // If the result is a smi, it is *not* an object in the ECMA sense.
      __ JumpIfSmi(eax, &use_receiver);

      // If the type of the result (stored in its map) is less than
      // FIRST_JS_RECEIVER_TYPE, it is not an object in the ECMA sense.
      __ CmpObjectType(eax, FIRST_JS_RECEIVER_TYPE, ecx);
      __ j(above_equal, &exit);

      // Throw away the result of the constructor invocation and use the
      // on-stack receiver as the result.
      __ bind(&use_receiver);
      __ mov(eax, Operand(esp, 0));

      // Restore the arguments count and leave the construct frame. The
      // arguments count is stored below the receiver.
      __ bind(&exit);
      __ mov(ebx, Operand(esp, 1 * kPointerSize));
    } else {
      __ mov(ebx, Operand(esp, 0));
    }

    // Leave construct frame.
  }

  // Remove caller arguments from the stack and return.
  STATIC_ASSERT(kSmiTagSize == 1 && kSmiTag == 0);
  __ pop(ecx);
  __ lea(esp, Operand(esp, ebx, times_2, 1 * kPointerSize));  // 1 ~ receiver
  __ push(ecx);
  if (create_implicit_receiver) {
    __ IncrementCounter(masm->isolate()->counters()->constructed_objects(), 1);
  }
  __ ret(0);
}


void Builtins::Generate_JSConstructStubGeneric(MacroAssembler* masm) {
  Generate_JSConstructStubHelper(masm, false, true);
}


void Builtins::Generate_JSConstructStubApi(MacroAssembler* masm) {
  Generate_JSConstructStubHelper(masm, true, true);
}


void Builtins::Generate_JSBuiltinsConstructStub(MacroAssembler* masm) {
  Generate_JSConstructStubHelper(masm, false, false);
}


void Builtins::Generate_ConstructedNonConstructable(MacroAssembler* masm) {
  FrameScope scope(masm, StackFrame::INTERNAL);
  __ push(edi);
  __ CallRuntime(Runtime::kThrowConstructedNonConstructable, 1);
}


enum IsTagged { kEaxIsSmiTagged, kEaxIsUntaggedInt };


// Clobbers ecx, edx, edi; preserves all other registers.
static void Generate_CheckStackOverflow(MacroAssembler* masm,
                                        IsTagged eax_is_tagged) {
  // eax   : the number of items to be pushed to the stack
  //
  // Check the stack for overflow. We are not trying to catch
  // interruptions (e.g. debug break and preemption) here, so the "real stack
  // limit" is checked.
  Label okay;
  ExternalReference real_stack_limit =
      ExternalReference::address_of_real_stack_limit(masm->isolate());
  __ mov(edi, Operand::StaticVariable(real_stack_limit));
  // Make ecx the space we have left. The stack might already be overflowed
  // here which will cause ecx to become negative.
  __ mov(ecx, esp);
  __ sub(ecx, edi);
  // Make edx the space we need for the array when it is unrolled onto the
  // stack.
  __ mov(edx, eax);
  int smi_tag = eax_is_tagged == kEaxIsSmiTagged ? kSmiTagSize : 0;
  __ shl(edx, kPointerSizeLog2 - smi_tag);
  // Check if the arguments will overflow the stack.
  __ cmp(ecx, edx);
  __ j(greater, &okay);  // Signed comparison.

  // Out of stack space.
  __ CallRuntime(Runtime::kThrowStackOverflow, 0);

  __ bind(&okay);
}


static void Generate_JSEntryTrampolineHelper(MacroAssembler* masm,
                                             bool is_construct) {
  ProfileEntryHookStub::MaybeCallEntryHook(masm);

  // Clear the context before we push it when entering the internal frame.
  __ Move(esi, Immediate(0));

  {
    FrameScope scope(masm, StackFrame::INTERNAL);

    // Setup the context (we need to use the caller context from the isolate).
    ExternalReference context_address(Isolate::kContextAddress,
                                      masm->isolate());
    __ mov(esi, Operand::StaticVariable(context_address));

    // Load the previous frame pointer (ebx) to access C arguments
    __ mov(ebx, Operand(ebp, 0));

    // Push the function and the receiver onto the stack.
    __ push(Operand(ebx, EntryFrameConstants::kFunctionArgOffset));
    __ push(Operand(ebx, EntryFrameConstants::kReceiverArgOffset));

    // Load the number of arguments and setup pointer to the arguments.
    __ mov(eax, Operand(ebx, EntryFrameConstants::kArgcOffset));
    __ mov(ebx, Operand(ebx, EntryFrameConstants::kArgvOffset));

    // Check if we have enough stack space to push all arguments.
    // Expects argument count in eax. Clobbers ecx, edx, edi.
    Generate_CheckStackOverflow(masm, kEaxIsUntaggedInt);

    // Copy arguments to the stack in a loop.
    Label loop, entry;
    __ Move(ecx, Immediate(0));
    __ jmp(&entry, Label::kNear);
    __ bind(&loop);
    __ mov(edx, Operand(ebx, ecx, times_4, 0));  // push parameter from argv
    __ push(Operand(edx, 0));  // dereference handle
    __ inc(ecx);
    __ bind(&entry);
    __ cmp(ecx, eax);
    __ j(not_equal, &loop);

    // Load the previous frame pointer (ebx) to access C arguments
    __ mov(ebx, Operand(ebp, 0));

    // Get the new.target and function from the frame.
    __ mov(edx, Operand(ebx, EntryFrameConstants::kNewTargetArgOffset));
    __ mov(edi, Operand(ebx, EntryFrameConstants::kFunctionArgOffset));

    // Invoke the code.
    Handle<Code> builtin = is_construct
                               ? masm->isolate()->builtins()->Construct()
                               : masm->isolate()->builtins()->Call();
    __ Call(builtin, RelocInfo::CODE_TARGET);

    // Exit the internal frame. Notice that this also removes the empty.
    // context and the function left on the stack by the code
    // invocation.
  }
  __ ret(kPointerSize);  // Remove receiver.
}


void Builtins::Generate_JSEntryTrampoline(MacroAssembler* masm) {
  Generate_JSEntryTrampolineHelper(masm, false);
}


void Builtins::Generate_JSConstructEntryTrampoline(MacroAssembler* masm) {
  Generate_JSEntryTrampolineHelper(masm, true);
}


// Generate code for entering a JS function with the interpreter.
// On entry to the function the receiver and arguments have been pushed on the
// stack left to right.  The actual argument count matches the formal parameter
// count expected by the function.
//
// The live registers are:
//   o edi: the JS function object being called
//   o edx: the new target
//   o esi: our context
//   o ebp: the caller's frame pointer
//   o esp: stack pointer (pointing to return address)
//
// The function builds a JS frame.  Please see JavaScriptFrameConstants in
// frames-ia32.h for its layout.
// TODO(rmcilroy): We will need to include the current bytecode pointer in the
// frame.
void Builtins::Generate_InterpreterEntryTrampoline(MacroAssembler* masm) {
  // Open a frame scope to indicate that there is a frame on the stack.  The
  // MANUAL indicates that the scope shouldn't actually generate code to set up
  // the frame (that is done below).
  FrameScope frame_scope(masm, StackFrame::MANUAL);
  __ push(ebp);  // Caller's frame pointer.
  __ mov(ebp, esp);
  __ push(esi);  // Callee's context.
  __ push(edi);  // Callee's JS function.
  __ push(edx);  // Callee's new target.

  // Push zero for bytecode array offset.
  __ push(Immediate(0));

  // Get the bytecode array from the function object and load the pointer to the
  // first entry into edi (InterpreterBytecodeRegister).
  __ mov(eax, FieldOperand(edi, JSFunction::kSharedFunctionInfoOffset));
  __ mov(kInterpreterBytecodeArrayRegister,
         FieldOperand(eax, SharedFunctionInfo::kFunctionDataOffset));

  if (FLAG_debug_code) {
    // Check function data field is actually a BytecodeArray object.
    __ AssertNotSmi(kInterpreterBytecodeArrayRegister);
    __ CmpObjectType(kInterpreterBytecodeArrayRegister, BYTECODE_ARRAY_TYPE,
                     eax);
    __ Assert(equal, kFunctionDataShouldBeBytecodeArrayOnInterpreterEntry);
  }

  // Allocate the local and temporary register file on the stack.
  {
    // Load frame size from the BytecodeArray object.
    __ mov(ebx, FieldOperand(kInterpreterBytecodeArrayRegister,
                             BytecodeArray::kFrameSizeOffset));

    // Do a stack check to ensure we don't go over the limit.
    Label ok;
    __ mov(ecx, esp);
    __ sub(ecx, ebx);
    ExternalReference stack_limit =
        ExternalReference::address_of_real_stack_limit(masm->isolate());
    __ cmp(ecx, Operand::StaticVariable(stack_limit));
    __ j(above_equal, &ok);
    __ CallRuntime(Runtime::kThrowStackOverflow, 0);
    __ bind(&ok);

    // If ok, push undefined as the initial value for all register file entries.
    Label loop_header;
    Label loop_check;
    __ mov(eax, Immediate(masm->isolate()->factory()->undefined_value()));
    __ jmp(&loop_check);
    __ bind(&loop_header);
    // TODO(rmcilroy): Consider doing more than one push per loop iteration.
    __ push(eax);
    // Continue loop if not done.
    __ bind(&loop_check);
    __ sub(ebx, Immediate(kPointerSize));
    __ j(greater_equal, &loop_header);
  }

  // TODO(rmcilroy): List of things not currently dealt with here but done in
  // fullcodegen's prologue:
  //  - Support profiler (specifically profiling_counter).
  //  - Call ProfileEntryHookStub when isolate has a function_entry_hook.
  //  - Allow simulator stop operations if FLAG_stop_at is set.
  //  - Code aging of the BytecodeArray object.

  // Perform stack guard check.
  {
    Label ok;
    ExternalReference stack_limit =
        ExternalReference::address_of_stack_limit(masm->isolate());
    __ cmp(esp, Operand::StaticVariable(stack_limit));
    __ j(above_equal, &ok);
    __ push(kInterpreterBytecodeArrayRegister);
    __ CallRuntime(Runtime::kStackGuard, 0);
    __ pop(kInterpreterBytecodeArrayRegister);
    __ bind(&ok);
  }

  // Load accumulator, register file, bytecode offset, dispatch table into
  // registers.
  __ LoadRoot(kInterpreterAccumulatorRegister, Heap::kUndefinedValueRootIndex);
  __ mov(kInterpreterRegisterFileRegister, ebp);
  __ add(kInterpreterRegisterFileRegister,
         Immediate(InterpreterFrameConstants::kRegisterFilePointerFromFp));
  __ mov(kInterpreterBytecodeOffsetRegister,
         Immediate(BytecodeArray::kHeaderSize - kHeapObjectTag));
  // Since the dispatch table root might be set after builtins are generated,
  // load directly from the roots table.
  __ LoadRoot(ebx, Heap::kInterpreterTableRootIndex);
  __ add(ebx, Immediate(FixedArray::kHeaderSize - kHeapObjectTag));

  // Push dispatch table as a stack located parameter to the bytecode handler.
  DCHECK_EQ(-1, kInterpreterDispatchTableSpillSlot);
  __ push(ebx);

  // Dispatch to the first bytecode handler for the function.
  __ movzx_b(eax, Operand(kInterpreterBytecodeArrayRegister,
                          kInterpreterBytecodeOffsetRegister, times_1, 0));
  __ mov(ebx, Operand(ebx, eax, times_pointer_size, 0));
  // Restore undefined_value in accumulator (eax)
  // TODO(rmcilroy): Remove this once we move the dispatch table back into a
  // register.
  __ mov(eax, Immediate(masm->isolate()->factory()->undefined_value()));
  // TODO(rmcilroy): Make dispatch table point to code entrys to avoid untagging
  // and header removal.
  __ add(ebx, Immediate(Code::kHeaderSize - kHeapObjectTag));
  __ call(ebx);
  __ nop();  // Ensure that return address still counts as interpreter entry
             // trampoline.
}


void Builtins::Generate_InterpreterExitTrampoline(MacroAssembler* masm) {
  // TODO(rmcilroy): List of things not currently dealt with here but done in
  // fullcodegen's EmitReturnSequence.
  //  - Supporting FLAG_trace for Runtime::TraceExit.
  //  - Support profiler (specifically decrementing profiling_counter
  //    appropriately and calling out to HandleInterrupts if necessary).

  // The return value is in accumulator, which is already in rax.

  // Leave the frame (also dropping the register file).
  __ leave();

  // Drop receiver + arguments and return.
  __ mov(ebx, FieldOperand(kInterpreterBytecodeArrayRegister,
                           BytecodeArray::kParameterSizeOffset));
  __ pop(ecx);
  __ add(esp, ebx);
  __ push(ecx);
  __ ret(0);
}


static void Generate_InterpreterPushArgs(MacroAssembler* masm,
                                         Register array_limit) {
  // ----------- S t a t e -------------
  //  -- ebx : Pointer to the last argument in the args array.
  //  -- array_limit : Pointer to one before the first argument in the
  //                   args array.
  // -----------------------------------
  Label loop_header, loop_check;
  __ jmp(&loop_check);
  __ bind(&loop_header);
  __ Push(Operand(ebx, 0));
  __ sub(ebx, Immediate(kPointerSize));
  __ bind(&loop_check);
  __ cmp(ebx, array_limit);
  __ j(greater, &loop_header, Label::kNear);
}


// static
void Builtins::Generate_InterpreterPushArgsAndCall(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax : the number of arguments (not including the receiver)
  //  -- ebx : the address of the first argument to be pushed. Subsequent
  //           arguments should be consecutive above this, in the same order as
  //           they are to be pushed onto the stack.
  //  -- edi : the target to call (can be any Object).
  // -----------------------------------

  // Pop return address to allow tail-call after pushing arguments.
  __ Pop(edx);

  // Find the address of the last argument.
  __ mov(ecx, eax);
  __ add(ecx, Immediate(1));  // Add one for receiver.
  __ shl(ecx, kPointerSizeLog2);
  __ neg(ecx);
  __ add(ecx, ebx);

  Generate_InterpreterPushArgs(masm, ecx);

  // Call the target.
  __ Push(edx);  // Re-push return address.
  __ Jump(masm->isolate()->builtins()->Call(), RelocInfo::CODE_TARGET);
}


// static
void Builtins::Generate_InterpreterPushArgsAndConstruct(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax : the number of arguments (not including the receiver)
  //  -- edx : the new target
  //  -- edi : the constructor
  //  -- ebx : the address of the first argument to be pushed. Subsequent
  //           arguments should be consecutive above this, in the same order as
  //           they are to be pushed onto the stack.
  // -----------------------------------

  // Save number of arguments on the stack below where arguments are going
  // to be pushed.
  __ mov(ecx, eax);
  __ neg(ecx);
  __ mov(Operand(esp, ecx, times_pointer_size, -kPointerSize), eax);
  __ mov(eax, ecx);

  // Pop return address to allow tail-call after pushing arguments.
  __ Pop(ecx);

  // Find the address of the last argument.
  __ shl(eax, kPointerSizeLog2);
  __ add(eax, ebx);

  // Push padding for receiver.
  __ Push(Immediate(0));

  Generate_InterpreterPushArgs(masm, eax);

  // Restore number of arguments from slot on stack.
  __ mov(eax, Operand(esp, -kPointerSize));

  // Re-push return address.
  __ Push(ecx);

  // Call the constructor with unmodified eax, edi, ebi values.
  __ Jump(masm->isolate()->builtins()->Construct(), RelocInfo::CODE_TARGET);
}


static void Generate_InterpreterNotifyDeoptimizedHelper(
    MacroAssembler* masm, Deoptimizer::BailoutType type) {
  // Enter an internal frame.
  {
    FrameScope scope(masm, StackFrame::INTERNAL);
    __ Push(kInterpreterAccumulatorRegister);  // Save accumulator register.

    // Pass the deoptimization type to the runtime system.
    __ Push(Smi::FromInt(static_cast<int>(type)));

    __ CallRuntime(Runtime::kNotifyDeoptimized, 1);

    __ Pop(kInterpreterAccumulatorRegister);  // Restore accumulator register.
    // Tear down internal frame.
  }

  // Initialize register file register.
  __ mov(kInterpreterRegisterFileRegister, ebp);
  __ add(kInterpreterRegisterFileRegister,
         Immediate(InterpreterFrameConstants::kRegisterFilePointerFromFp));

  // Get the bytecode array pointer from the frame.
  __ mov(ebx, Operand(kInterpreterRegisterFileRegister,
                      InterpreterFrameConstants::kFunctionFromRegisterPointer));
  __ mov(ebx, FieldOperand(ebx, JSFunction::kSharedFunctionInfoOffset));
  __ mov(kInterpreterBytecodeArrayRegister,
         FieldOperand(ebx, SharedFunctionInfo::kFunctionDataOffset));

  if (FLAG_debug_code) {
    // Check function data field is actually a BytecodeArray object.
    __ AssertNotSmi(kInterpreterBytecodeArrayRegister);
    __ CmpObjectType(kInterpreterBytecodeArrayRegister, BYTECODE_ARRAY_TYPE,
                     ebx);
    __ Assert(equal, kFunctionDataShouldBeBytecodeArrayOnInterpreterEntry);
  }

  // Get the target bytecode offset from the frame.
  __ mov(
      kInterpreterBytecodeOffsetRegister,
      Operand(kInterpreterRegisterFileRegister,
              InterpreterFrameConstants::kBytecodeOffsetFromRegisterPointer));
  __ SmiUntag(kInterpreterBytecodeOffsetRegister);

  // Push dispatch table as a stack located parameter to the bytecode handler -
  // overwrite the state slot (we don't use these for interpreter deopts).
  __ LoadRoot(ebx, Heap::kInterpreterTableRootIndex);
  __ add(ebx, Immediate(FixedArray::kHeaderSize - kHeapObjectTag));
  DCHECK_EQ(-1, kInterpreterDispatchTableSpillSlot);
  __ mov(ebx, Operand(esp, -2 * kPointerSize));

  // Dispatch to the target bytecode.
  __ movzx_b(esi, Operand(kInterpreterBytecodeArrayRegister,
                          kInterpreterBytecodeOffsetRegister, times_1, 0));
  __ mov(ebx, Operand(ebx, esi, times_pointer_size, 0));

  // Get the context from the frame.
  // TODO(rmcilroy): Update interpreter frame to expect current context at the
  // context slot instead of the function context.
  __ mov(kContextRegister,
         Operand(kInterpreterRegisterFileRegister,
                 InterpreterFrameConstants::kContextFromRegisterPointer));

  // TODO(rmcilroy): Make dispatch table point to code entrys to avoid untagging
  // and header removal.
  __ add(ebx, Immediate(Code::kHeaderSize - kHeapObjectTag));
  __ jmp(ebx);
}


void Builtins::Generate_InterpreterNotifyDeoptimized(MacroAssembler* masm) {
  Generate_InterpreterNotifyDeoptimizedHelper(masm, Deoptimizer::EAGER);
}


void Builtins::Generate_InterpreterNotifySoftDeoptimized(MacroAssembler* masm) {
  Generate_InterpreterNotifyDeoptimizedHelper(masm, Deoptimizer::SOFT);
}


void Builtins::Generate_InterpreterNotifyLazyDeoptimized(MacroAssembler* masm) {
  Generate_InterpreterNotifyDeoptimizedHelper(masm, Deoptimizer::LAZY);
}


void Builtins::Generate_CompileLazy(MacroAssembler* masm) {
  CallRuntimePassFunction(masm, Runtime::kCompileLazy);
  GenerateTailCallToReturnedCode(masm);
}


void Builtins::Generate_CompileOptimized(MacroAssembler* masm) {
  CallRuntimePassFunction(masm, Runtime::kCompileOptimized_NotConcurrent);
  GenerateTailCallToReturnedCode(masm);
}


void Builtins::Generate_CompileOptimizedConcurrent(MacroAssembler* masm) {
  CallRuntimePassFunction(masm, Runtime::kCompileOptimized_Concurrent);
  GenerateTailCallToReturnedCode(masm);
}


static void GenerateMakeCodeYoungAgainCommon(MacroAssembler* masm) {
  // For now, we are relying on the fact that make_code_young doesn't do any
  // garbage collection which allows us to save/restore the registers without
  // worrying about which of them contain pointers. We also don't build an
  // internal frame to make the code faster, since we shouldn't have to do stack
  // crawls in MakeCodeYoung. This seems a bit fragile.

  // Re-execute the code that was patched back to the young age when
  // the stub returns.
  __ sub(Operand(esp, 0), Immediate(5));
  __ pushad();
  __ mov(eax, Operand(esp, 8 * kPointerSize));
  {
    FrameScope scope(masm, StackFrame::MANUAL);
    __ PrepareCallCFunction(2, ebx);
    __ mov(Operand(esp, 1 * kPointerSize),
           Immediate(ExternalReference::isolate_address(masm->isolate())));
    __ mov(Operand(esp, 0), eax);
    __ CallCFunction(
        ExternalReference::get_make_code_young_function(masm->isolate()), 2);
  }
  __ popad();
  __ ret(0);
}

#define DEFINE_CODE_AGE_BUILTIN_GENERATOR(C)                 \
void Builtins::Generate_Make##C##CodeYoungAgainEvenMarking(  \
    MacroAssembler* masm) {                                  \
  GenerateMakeCodeYoungAgainCommon(masm);                    \
}                                                            \
void Builtins::Generate_Make##C##CodeYoungAgainOddMarking(   \
    MacroAssembler* masm) {                                  \
  GenerateMakeCodeYoungAgainCommon(masm);                    \
}
CODE_AGE_LIST(DEFINE_CODE_AGE_BUILTIN_GENERATOR)
#undef DEFINE_CODE_AGE_BUILTIN_GENERATOR


void Builtins::Generate_MarkCodeAsExecutedOnce(MacroAssembler* masm) {
  // For now, as in GenerateMakeCodeYoungAgainCommon, we are relying on the fact
  // that make_code_young doesn't do any garbage collection which allows us to
  // save/restore the registers without worrying about which of them contain
  // pointers.
  __ pushad();
  __ mov(eax, Operand(esp, 8 * kPointerSize));
  __ sub(eax, Immediate(Assembler::kCallInstructionLength));
  {  // NOLINT
    FrameScope scope(masm, StackFrame::MANUAL);
    __ PrepareCallCFunction(2, ebx);
    __ mov(Operand(esp, 1 * kPointerSize),
           Immediate(ExternalReference::isolate_address(masm->isolate())));
    __ mov(Operand(esp, 0), eax);
    __ CallCFunction(
        ExternalReference::get_mark_code_as_executed_function(masm->isolate()),
        2);
  }
  __ popad();

  // Perform prologue operations usually performed by the young code stub.
  __ pop(eax);   // Pop return address into scratch register.
  __ push(ebp);  // Caller's frame pointer.
  __ mov(ebp, esp);
  __ push(esi);  // Callee's context.
  __ push(edi);  // Callee's JS Function.
  __ push(eax);  // Push return address after frame prologue.

  // Jump to point after the code-age stub.
  __ ret(0);
}


void Builtins::Generate_MarkCodeAsExecutedTwice(MacroAssembler* masm) {
  GenerateMakeCodeYoungAgainCommon(masm);
}


void Builtins::Generate_MarkCodeAsToBeExecutedOnce(MacroAssembler* masm) {
  Generate_MarkCodeAsExecutedOnce(masm);
}


static void Generate_NotifyStubFailureHelper(MacroAssembler* masm,
                                             SaveFPRegsMode save_doubles) {
  // Enter an internal frame.
  {
    FrameScope scope(masm, StackFrame::INTERNAL);

    // Preserve registers across notification, this is important for compiled
    // stubs that tail call the runtime on deopts passing their parameters in
    // registers.
    __ pushad();
    __ CallRuntime(Runtime::kNotifyStubFailure, 0, save_doubles);
    __ popad();
    // Tear down internal frame.
  }

  __ pop(MemOperand(esp, 0));  // Ignore state offset
  __ ret(0);  // Return to IC Miss stub, continuation still on stack.
}


void Builtins::Generate_NotifyStubFailure(MacroAssembler* masm) {
  Generate_NotifyStubFailureHelper(masm, kDontSaveFPRegs);
}


void Builtins::Generate_NotifyStubFailureSaveDoubles(MacroAssembler* masm) {
  Generate_NotifyStubFailureHelper(masm, kSaveFPRegs);
}


static void Generate_NotifyDeoptimizedHelper(MacroAssembler* masm,
                                             Deoptimizer::BailoutType type) {
  {
    FrameScope scope(masm, StackFrame::INTERNAL);

    // Pass deoptimization type to the runtime system.
    __ push(Immediate(Smi::FromInt(static_cast<int>(type))));
    __ CallRuntime(Runtime::kNotifyDeoptimized, 1);

    // Tear down internal frame.
  }

  // Get the full codegen state from the stack and untag it.
  __ mov(ecx, Operand(esp, 1 * kPointerSize));
  __ SmiUntag(ecx);

  // Switch on the state.
  Label not_no_registers, not_tos_eax;
  __ cmp(ecx, FullCodeGenerator::NO_REGISTERS);
  __ j(not_equal, &not_no_registers, Label::kNear);
  __ ret(1 * kPointerSize);  // Remove state.

  __ bind(&not_no_registers);
  __ mov(eax, Operand(esp, 2 * kPointerSize));
  __ cmp(ecx, FullCodeGenerator::TOS_REG);
  __ j(not_equal, &not_tos_eax, Label::kNear);
  __ ret(2 * kPointerSize);  // Remove state, eax.

  __ bind(&not_tos_eax);
  __ Abort(kNoCasesLeft);
}


void Builtins::Generate_NotifyDeoptimized(MacroAssembler* masm) {
  Generate_NotifyDeoptimizedHelper(masm, Deoptimizer::EAGER);
}


void Builtins::Generate_NotifySoftDeoptimized(MacroAssembler* masm) {
  Generate_NotifyDeoptimizedHelper(masm, Deoptimizer::SOFT);
}


void Builtins::Generate_NotifyLazyDeoptimized(MacroAssembler* masm) {
  Generate_NotifyDeoptimizedHelper(masm, Deoptimizer::LAZY);
}


// static
void Builtins::Generate_FunctionPrototypeApply(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax     : argc
  //  -- esp[0]  : return address
  //  -- esp[4]  : argArray
  //  -- esp[8]  : thisArg
  //  -- esp[12] : receiver
  // -----------------------------------

  // 1. Load receiver into edi, argArray into eax (if present), remove all
  // arguments from the stack (including the receiver), and push thisArg (if
  // present) instead.
  {
    Label no_arg_array, no_this_arg;
    __ LoadRoot(edx, Heap::kUndefinedValueRootIndex);
    __ mov(ebx, edx);
    __ mov(edi, Operand(esp, eax, times_pointer_size, kPointerSize));
    __ test(eax, eax);
    __ j(zero, &no_this_arg, Label::kNear);
    {
      __ mov(edx, Operand(esp, eax, times_pointer_size, 0));
      __ cmp(eax, Immediate(1));
      __ j(equal, &no_arg_array, Label::kNear);
      __ mov(ebx, Operand(esp, eax, times_pointer_size, -kPointerSize));
      __ bind(&no_arg_array);
    }
    __ bind(&no_this_arg);
    __ PopReturnAddressTo(ecx);
    __ lea(esp, Operand(esp, eax, times_pointer_size, kPointerSize));
    __ Push(edx);
    __ PushReturnAddressFrom(ecx);
    __ Move(eax, ebx);
  }

  // ----------- S t a t e -------------
  //  -- eax    : argArray
  //  -- edi    : receiver
  //  -- esp[0] : return address
  //  -- esp[4] : thisArg
  // -----------------------------------

  // 2. Make sure the receiver is actually callable.
  Label receiver_not_callable;
  __ JumpIfSmi(edi, &receiver_not_callable, Label::kNear);
  __ mov(ecx, FieldOperand(edi, HeapObject::kMapOffset));
  __ test_b(FieldOperand(ecx, Map::kBitFieldOffset), 1 << Map::kIsCallable);
  __ j(zero, &receiver_not_callable, Label::kNear);

  // 3. Tail call with no arguments if argArray is null or undefined.
  Label no_arguments;
  __ JumpIfRoot(eax, Heap::kNullValueRootIndex, &no_arguments, Label::kNear);
  __ JumpIfRoot(eax, Heap::kUndefinedValueRootIndex, &no_arguments,
                Label::kNear);

  // 4a. Apply the receiver to the given argArray (passing undefined for
  // new.target).
  __ LoadRoot(edx, Heap::kUndefinedValueRootIndex);
  __ Jump(masm->isolate()->builtins()->Apply(), RelocInfo::CODE_TARGET);

  // 4b. The argArray is either null or undefined, so we tail call without any
  // arguments to the receiver.
  __ bind(&no_arguments);
  {
    __ Set(eax, 0);
    __ Jump(masm->isolate()->builtins()->Call(), RelocInfo::CODE_TARGET);
  }

  // 4c. The receiver is not callable, throw an appropriate TypeError.
  __ bind(&receiver_not_callable);
  {
    __ mov(Operand(esp, kPointerSize), edi);
    __ TailCallRuntime(Runtime::kThrowApplyNonFunction, 1, 1);
  }
}


// static
void Builtins::Generate_FunctionPrototypeCall(MacroAssembler* masm) {
  // Stack Layout:
  // esp[0]           : Return address
  // esp[8]           : Argument n
  // esp[16]          : Argument n-1
  //  ...
  // esp[8 * n]       : Argument 1
  // esp[8 * (n + 1)] : Receiver (callable to call)
  //
  // eax contains the number of arguments, n, not counting the receiver.
  //
  // 1. Make sure we have at least one argument.
  {
    Label done;
    __ test(eax, eax);
    __ j(not_zero, &done, Label::kNear);
    __ PopReturnAddressTo(ebx);
    __ PushRoot(Heap::kUndefinedValueRootIndex);
    __ PushReturnAddressFrom(ebx);
    __ inc(eax);
    __ bind(&done);
  }

  // 2. Get the callable to call (passed as receiver) from the stack.
  __ mov(edi, Operand(esp, eax, times_pointer_size, kPointerSize));

  // 3. Shift arguments and return address one slot down on the stack
  //    (overwriting the original receiver).  Adjust argument count to make
  //    the original first argument the new receiver.
  {
    Label loop;
    __ mov(ecx, eax);
    __ bind(&loop);
    __ mov(ebx, Operand(esp, ecx, times_pointer_size, 0));
    __ mov(Operand(esp, ecx, times_pointer_size, kPointerSize), ebx);
    __ dec(ecx);
    __ j(not_sign, &loop);  // While non-negative (to copy return address).
    __ pop(ebx);            // Discard copy of return address.
    __ dec(eax);  // One fewer argument (first argument is new receiver).
  }

  // 4. Call the callable.
  __ Jump(masm->isolate()->builtins()->Call(), RelocInfo::CODE_TARGET);
}


void Builtins::Generate_ReflectApply(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax     : argc
  //  -- esp[0]  : return address
  //  -- esp[4]  : argumentsList
  //  -- esp[8]  : thisArgument
  //  -- esp[12] : target
  //  -- esp[16] : receiver
  // -----------------------------------

  // 1. Load target into edi (if present), argumentsList into eax (if present),
  // remove all arguments from the stack (including the receiver), and push
  // thisArgument (if present) instead.
  {
    Label done;
    __ LoadRoot(edi, Heap::kUndefinedValueRootIndex);
    __ mov(edx, edi);
    __ mov(ebx, edi);
    __ cmp(eax, Immediate(1));
    __ j(below, &done, Label::kNear);
    __ mov(edi, Operand(esp, eax, times_pointer_size, -0 * kPointerSize));
    __ j(equal, &done, Label::kNear);
    __ mov(edx, Operand(esp, eax, times_pointer_size, -1 * kPointerSize));
    __ cmp(eax, Immediate(3));
    __ j(below, &done, Label::kNear);
    __ mov(ebx, Operand(esp, eax, times_pointer_size, -2 * kPointerSize));
    __ bind(&done);
    __ PopReturnAddressTo(ecx);
    __ lea(esp, Operand(esp, eax, times_pointer_size, kPointerSize));
    __ Push(edx);
    __ PushReturnAddressFrom(ecx);
    __ Move(eax, ebx);
  }

  // ----------- S t a t e -------------
  //  -- eax    : argumentsList
  //  -- edi    : target
  //  -- esp[0] : return address
  //  -- esp[4] : thisArgument
  // -----------------------------------

  // 2. Make sure the target is actually callable.
  Label target_not_callable;
  __ JumpIfSmi(edi, &target_not_callable, Label::kNear);
  __ mov(ecx, FieldOperand(edi, HeapObject::kMapOffset));
  __ test_b(FieldOperand(ecx, Map::kBitFieldOffset), 1 << Map::kIsCallable);
  __ j(zero, &target_not_callable, Label::kNear);

  // 3a. Apply the target to the given argumentsList (passing undefined for
  // new.target).
  __ LoadRoot(edx, Heap::kUndefinedValueRootIndex);
  __ Jump(masm->isolate()->builtins()->Apply(), RelocInfo::CODE_TARGET);

  // 3b. The target is not callable, throw an appropriate TypeError.
  __ bind(&target_not_callable);
  {
    __ mov(Operand(esp, kPointerSize), edi);
    __ TailCallRuntime(Runtime::kThrowApplyNonFunction, 1, 1);
  }
}


void Builtins::Generate_ReflectConstruct(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax     : argc
  //  -- esp[0]  : return address
  //  -- esp[4]  : new.target (optional)
  //  -- esp[8]  : argumentsList
  //  -- esp[12] : target
  //  -- esp[16] : receiver
  // -----------------------------------

  // 1. Load target into edi (if present), argumentsList into eax (if present),
  // new.target into edx (if present, otherwise use target), remove all
  // arguments from the stack (including the receiver), and push thisArgument
  // (if present) instead.
  {
    Label done;
    __ LoadRoot(edi, Heap::kUndefinedValueRootIndex);
    __ mov(edx, edi);
    __ mov(ebx, edi);
    __ cmp(eax, Immediate(1));
    __ j(below, &done, Label::kNear);
    __ mov(edi, Operand(esp, eax, times_pointer_size, -0 * kPointerSize));
    __ mov(edx, edi);
    __ j(equal, &done, Label::kNear);
    __ mov(ebx, Operand(esp, eax, times_pointer_size, -1 * kPointerSize));
    __ cmp(eax, Immediate(3));
    __ j(below, &done, Label::kNear);
    __ mov(edx, Operand(esp, eax, times_pointer_size, -2 * kPointerSize));
    __ bind(&done);
    __ PopReturnAddressTo(ecx);
    __ lea(esp, Operand(esp, eax, times_pointer_size, kPointerSize));
    __ PushRoot(Heap::kUndefinedValueRootIndex);
    __ PushReturnAddressFrom(ecx);
    __ Move(eax, ebx);
  }

  // ----------- S t a t e -------------
  //  -- eax    : argumentsList
  //  -- edx    : new.target
  //  -- edi    : target
  //  -- esp[0] : return address
  //  -- esp[4] : receiver (undefined)
  // -----------------------------------

  // 2. Make sure the target is actually a constructor.
  Label target_not_constructor;
  __ JumpIfSmi(edi, &target_not_constructor, Label::kNear);
  __ mov(ecx, FieldOperand(edi, HeapObject::kMapOffset));
  __ test_b(FieldOperand(ecx, Map::kBitFieldOffset), 1 << Map::kIsConstructor);
  __ j(zero, &target_not_constructor, Label::kNear);

  // 3. Make sure the target is actually a constructor.
  Label new_target_not_constructor;
  __ JumpIfSmi(edx, &new_target_not_constructor, Label::kNear);
  __ mov(ecx, FieldOperand(edx, HeapObject::kMapOffset));
  __ test_b(FieldOperand(ecx, Map::kBitFieldOffset), 1 << Map::kIsConstructor);
  __ j(zero, &new_target_not_constructor, Label::kNear);

  // 4a. Construct the target with the given new.target and argumentsList.
  __ Jump(masm->isolate()->builtins()->Apply(), RelocInfo::CODE_TARGET);

  // 4b. The target is not a constructor, throw an appropriate TypeError.
  __ bind(&target_not_constructor);
  {
    __ mov(Operand(esp, kPointerSize), edi);
    __ TailCallRuntime(Runtime::kThrowCalledNonCallable, 1, 1);
  }

  // 4c. The new.target is not a constructor, throw an appropriate TypeError.
  __ bind(&new_target_not_constructor);
  {
    __ mov(Operand(esp, kPointerSize), edx);
    __ TailCallRuntime(Runtime::kThrowCalledNonCallable, 1, 1);
  }
}


void Builtins::Generate_InternalArrayCode(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax : argc
  //  -- esp[0] : return address
  //  -- esp[4] : last argument
  // -----------------------------------
  Label generic_array_code;

  // Get the InternalArray function.
  __ LoadGlobalFunction(Context::INTERNAL_ARRAY_FUNCTION_INDEX, edi);

  if (FLAG_debug_code) {
    // Initial map for the builtin InternalArray function should be a map.
    __ mov(ebx, FieldOperand(edi, JSFunction::kPrototypeOrInitialMapOffset));
    // Will both indicate a NULL and a Smi.
    __ test(ebx, Immediate(kSmiTagMask));
    __ Assert(not_zero, kUnexpectedInitialMapForInternalArrayFunction);
    __ CmpObjectType(ebx, MAP_TYPE, ecx);
    __ Assert(equal, kUnexpectedInitialMapForInternalArrayFunction);
  }

  // Run the native code for the InternalArray function called as a normal
  // function.
  // tail call a stub
  InternalArrayConstructorStub stub(masm->isolate());
  __ TailCallStub(&stub);
}


void Builtins::Generate_ArrayCode(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax : argc
  //  -- esp[0] : return address
  //  -- esp[4] : last argument
  // -----------------------------------
  Label generic_array_code;

  // Get the Array function.
  __ LoadGlobalFunction(Context::ARRAY_FUNCTION_INDEX, edi);
  __ mov(edx, edi);

  if (FLAG_debug_code) {
    // Initial map for the builtin Array function should be a map.
    __ mov(ebx, FieldOperand(edi, JSFunction::kPrototypeOrInitialMapOffset));
    // Will both indicate a NULL and a Smi.
    __ test(ebx, Immediate(kSmiTagMask));
    __ Assert(not_zero, kUnexpectedInitialMapForArrayFunction);
    __ CmpObjectType(ebx, MAP_TYPE, ecx);
    __ Assert(equal, kUnexpectedInitialMapForArrayFunction);
  }

  // Run the native code for the Array function called as a normal function.
  // tail call a stub
  __ mov(ebx, masm->isolate()->factory()->undefined_value());
  ArrayConstructorStub stub(masm->isolate());
  __ TailCallStub(&stub);
}


// static
void Builtins::Generate_StringConstructor(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax                 : number of arguments
  //  -- edi                 : constructor function
  //  -- esp[0]              : return address
  //  -- esp[(argc - n) * 4] : arg[n] (zero-based)
  //  -- esp[(argc + 1) * 4] : receiver
  // -----------------------------------

  // 1. Load the first argument into eax and get rid of the rest (including the
  // receiver).
  Label no_arguments;
  {
    __ test(eax, eax);
    __ j(zero, &no_arguments, Label::kNear);
    __ mov(ebx, Operand(esp, eax, times_pointer_size, 0));
    __ PopReturnAddressTo(ecx);
    __ lea(esp, Operand(esp, eax, times_pointer_size, kPointerSize));
    __ PushReturnAddressFrom(ecx);
    __ mov(eax, ebx);
  }

  // 2a. At least one argument, return eax if it's a string, otherwise
  // dispatch to appropriate conversion.
  Label to_string, symbol_descriptive_string;
  {
    __ JumpIfSmi(eax, &to_string, Label::kNear);
    STATIC_ASSERT(FIRST_NONSTRING_TYPE == SYMBOL_TYPE);
    __ CmpObjectType(eax, FIRST_NONSTRING_TYPE, edx);
    __ j(above, &to_string, Label::kNear);
    __ j(equal, &symbol_descriptive_string, Label::kNear);
    __ Ret();
  }

  // 2b. No arguments, return the empty string (and pop the receiver).
  __ bind(&no_arguments);
  {
    __ LoadRoot(eax, Heap::kempty_stringRootIndex);
    __ ret(1 * kPointerSize);
  }

  // 3a. Convert eax to a string.
  __ bind(&to_string);
  {
    ToStringStub stub(masm->isolate());
    __ TailCallStub(&stub);
  }

  // 3b. Convert symbol in eax to a string.
  __ bind(&symbol_descriptive_string);
  {
    __ PopReturnAddressTo(ecx);
    __ Push(eax);
    __ PushReturnAddressFrom(ecx);
    __ TailCallRuntime(Runtime::kSymbolDescriptiveString, 1, 1);
  }
}


// static
void Builtins::Generate_StringConstructor_ConstructStub(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax                 : number of arguments
  //  -- edi                 : constructor function
  //  -- edx                 : new target
  //  -- esp[0]              : return address
  //  -- esp[(argc - n) * 4] : arg[n] (zero-based)
  //  -- esp[(argc + 1) * 4] : receiver
  // -----------------------------------

  // 1. Load the first argument into ebx and get rid of the rest (including the
  // receiver).
  {
    Label no_arguments, done;
    __ test(eax, eax);
    __ j(zero, &no_arguments, Label::kNear);
    __ mov(ebx, Operand(esp, eax, times_pointer_size, 0));
    __ jmp(&done, Label::kNear);
    __ bind(&no_arguments);
    __ LoadRoot(ebx, Heap::kempty_stringRootIndex);
    __ bind(&done);
    __ PopReturnAddressTo(ecx);
    __ lea(esp, Operand(esp, eax, times_pointer_size, kPointerSize));
    __ PushReturnAddressFrom(ecx);
  }

  // 2. Make sure ebx is a string.
  {
    Label convert, done_convert;
    __ JumpIfSmi(ebx, &convert, Label::kNear);
    __ CmpObjectType(ebx, FIRST_NONSTRING_TYPE, ecx);
    __ j(below, &done_convert);
    __ bind(&convert);
    {
      FrameScope scope(masm, StackFrame::INTERNAL);
      ToStringStub stub(masm->isolate());
      __ Push(edi);
      __ Push(edx);
      __ Move(eax, ebx);
      __ CallStub(&stub);
      __ Move(ebx, eax);
      __ Pop(edx);
      __ Pop(edi);
    }
    __ bind(&done_convert);
  }

  // 3. Check if new target and constructor differ.
  Label new_object;
  __ cmp(edx, edi);
  __ j(not_equal, &new_object);

  // 4. Allocate a JSValue wrapper for the string.
  {
    // ----------- S t a t e -------------
    //  -- ebx : the first argument
    //  -- edi : constructor function
    //  -- edx : new target
    // -----------------------------------
    __ Allocate(JSValue::kSize, eax, ecx, no_reg, &new_object, TAG_OBJECT);

    // Initialize the JSValue in eax.
    __ LoadGlobalFunctionInitialMap(edi, ecx);
    __ mov(FieldOperand(eax, HeapObject::kMapOffset), ecx);
    __ mov(FieldOperand(eax, JSObject::kPropertiesOffset),
           masm->isolate()->factory()->empty_fixed_array());
    __ mov(FieldOperand(eax, JSObject::kElementsOffset),
           masm->isolate()->factory()->empty_fixed_array());
    __ mov(FieldOperand(eax, JSValue::kValueOffset), ebx);
    STATIC_ASSERT(JSValue::kSize == 4 * kPointerSize);
    __ Ret();
  }

  // 5. Fallback to the runtime to create new object.
  __ bind(&new_object);
  {
    FrameScope scope(masm, StackFrame::INTERNAL);
    __ Push(ebx);  // the first argument
    __ Push(edi);  // constructor function
    __ Push(edx);  // new target
    __ CallRuntime(Runtime::kNewObject, 2);
    __ Pop(FieldOperand(eax, JSValue::kValueOffset));
  }
  __ Ret();
}


static void ArgumentsAdaptorStackCheck(MacroAssembler* masm,
                                       Label* stack_overflow) {
  // ----------- S t a t e -------------
  //  -- eax : actual number of arguments
  //  -- ebx : expected number of arguments
  //  -- edx : new target (passed through to callee)
  // -----------------------------------
  // Check the stack for overflow. We are not trying to catch
  // interruptions (e.g. debug break and preemption) here, so the "real stack
  // limit" is checked.
  ExternalReference real_stack_limit =
      ExternalReference::address_of_real_stack_limit(masm->isolate());
  __ mov(edi, Operand::StaticVariable(real_stack_limit));
  // Make ecx the space we have left. The stack might already be overflowed
  // here which will cause ecx to become negative.
  __ mov(ecx, esp);
  __ sub(ecx, edi);
  // Make edi the space we need for the array when it is unrolled onto the
  // stack.
  __ mov(edi, ebx);
  __ shl(edi, kPointerSizeLog2);
  // Check if the arguments will overflow the stack.
  __ cmp(ecx, edi);
  __ j(less_equal, stack_overflow);  // Signed comparison.
}


static void EnterArgumentsAdaptorFrame(MacroAssembler* masm) {
  __ push(ebp);
  __ mov(ebp, esp);

  // Store the arguments adaptor context sentinel.
  __ push(Immediate(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));

  // Push the function on the stack.
  __ push(edi);

  // Preserve the number of arguments on the stack. Must preserve eax,
  // ebx and ecx because these registers are used when copying the
  // arguments and the receiver.
  STATIC_ASSERT(kSmiTagSize == 1);
  __ lea(edi, Operand(eax, eax, times_1, kSmiTag));
  __ push(edi);
}


static void LeaveArgumentsAdaptorFrame(MacroAssembler* masm) {
  // Retrieve the number of arguments from the stack.
  __ mov(ebx, Operand(ebp, ArgumentsAdaptorFrameConstants::kLengthOffset));

  // Leave the frame.
  __ leave();

  // Remove caller arguments from the stack.
  STATIC_ASSERT(kSmiTagSize == 1 && kSmiTag == 0);
  __ pop(ecx);
  __ lea(esp, Operand(esp, ebx, times_2, 1 * kPointerSize));  // 1 ~ receiver
  __ push(ecx);
}


// static
void Builtins::Generate_Apply(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax    : argumentsList
  //  -- edi    : target
  //  -- edx    : new.target (checked to be constructor or undefined)
  //  -- esp[0] : return address.
  //  -- esp[4] : thisArgument
  // -----------------------------------

  // Create the list of arguments from the array-like argumentsList.
  {
    Label create_arguments, create_array, create_runtime, done_create;
    __ JumpIfSmi(eax, &create_runtime);

    // Load the map of argumentsList into ecx.
    __ mov(ecx, FieldOperand(eax, HeapObject::kMapOffset));

    // Load native context into ebx.
    __ mov(ebx, NativeContextOperand());

    // Check if argumentsList is an (unmodified) arguments object.
    __ cmp(ecx, ContextOperand(ebx, Context::SLOPPY_ARGUMENTS_MAP_INDEX));
    __ j(equal, &create_arguments);
    __ cmp(ecx, ContextOperand(ebx, Context::STRICT_ARGUMENTS_MAP_INDEX));
    __ j(equal, &create_arguments);

    // Check if argumentsList is a fast JSArray.
    __ CmpInstanceType(ecx, JS_ARRAY_TYPE);
    __ j(equal, &create_array);

    // Ask the runtime to create the list (actually a FixedArray).
    __ bind(&create_runtime);
    {
      FrameScope scope(masm, StackFrame::INTERNAL);
      __ Push(edi);
      __ Push(edx);
      __ Push(eax);
      __ CallRuntime(Runtime::kCreateListFromArrayLike, 1);
      __ Pop(edx);
      __ Pop(edi);
      __ mov(ebx, FieldOperand(eax, FixedArray::kLengthOffset));
      __ SmiUntag(ebx);
    }
    __ jmp(&done_create);

    // Try to create the list from an arguments object.
    __ bind(&create_arguments);
    __ mov(ebx,
           FieldOperand(eax, JSObject::kHeaderSize +
                                 Heap::kArgumentsLengthIndex * kPointerSize));
    __ mov(ecx, FieldOperand(eax, JSObject::kElementsOffset));
    __ cmp(ebx, FieldOperand(ecx, FixedArray::kLengthOffset));
    __ j(not_equal, &create_runtime);
    __ SmiUntag(ebx);
    __ mov(eax, ecx);
    __ jmp(&done_create);

    // Try to create the list from a JSArray object.
    __ bind(&create_array);
    __ mov(ecx, FieldOperand(ecx, Map::kBitField2Offset));
    __ DecodeField<Map::ElementsKindBits>(ecx);
    STATIC_ASSERT(FAST_SMI_ELEMENTS == 0);
    STATIC_ASSERT(FAST_HOLEY_SMI_ELEMENTS == 1);
    STATIC_ASSERT(FAST_ELEMENTS == 2);
    __ cmp(ecx, Immediate(FAST_ELEMENTS));
    __ j(above, &create_runtime);
    __ cmp(ecx, Immediate(FAST_HOLEY_SMI_ELEMENTS));
    __ j(equal, &create_runtime);
    __ mov(ebx, FieldOperand(eax, JSArray::kLengthOffset));
    __ SmiUntag(ebx);
    __ mov(eax, FieldOperand(eax, JSArray::kElementsOffset));

    __ bind(&done_create);
  }

  // Check for stack overflow.
  {
    // Check the stack for overflow. We are not trying to catch interruptions
    // (i.e. debug break and preemption) here, so check the "real stack limit".
    Label done;
    ExternalReference real_stack_limit =
        ExternalReference::address_of_real_stack_limit(masm->isolate());
    __ mov(ecx, Operand::StaticVariable(real_stack_limit));
    // Make ecx the space we have left. The stack might already be overflowed
    // here which will cause ecx to become negative.
    __ neg(ecx);
    __ add(ecx, esp);
    __ sar(ecx, kPointerSizeLog2);
    // Check if the arguments will overflow the stack.
    __ cmp(ecx, ebx);
    __ j(greater, &done, Label::kNear);  // Signed comparison.
    __ TailCallRuntime(Runtime::kThrowStackOverflow, 1, 1);
    __ bind(&done);
  }

  // ----------- S t a t e -------------
  //  -- edi    : target
  //  -- eax    : args (a FixedArray built from argumentsList)
  //  -- ebx    : len (number of elements to push from args)
  //  -- edx    : new.target (checked to be constructor or undefined)
  //  -- esp[0] : return address.
  //  -- esp[4] : thisArgument
  // -----------------------------------

  // Push arguments onto the stack (thisArgument is already on the stack).
  {
    __ push(edx);
    __ fld_s(MemOperand(esp, 0));
    __ lea(esp, Operand(esp, kFloatSize));

    __ PopReturnAddressTo(edx);
    __ Move(ecx, Immediate(0));
    Label done, loop;
    __ bind(&loop);
    __ cmp(ecx, ebx);
    __ j(equal, &done, Label::kNear);
    __ Push(
        FieldOperand(eax, ecx, times_pointer_size, FixedArray::kHeaderSize));
    __ inc(ecx);
    __ jmp(&loop);
    __ bind(&done);
    __ PushReturnAddressFrom(edx);

    __ lea(esp, Operand(esp, -kFloatSize));
    __ fstp_s(MemOperand(esp, 0));
    __ pop(edx);

    __ Move(eax, ebx);
  }

  // Dispatch to Call or Construct depending on whether new.target is undefined.
  {
    __ CompareRoot(edx, Heap::kUndefinedValueRootIndex);
    __ j(equal, masm->isolate()->builtins()->Call(), RelocInfo::CODE_TARGET);
    __ Jump(masm->isolate()->builtins()->Construct(), RelocInfo::CODE_TARGET);
  }
}


// static
void Builtins::Generate_CallFunction(MacroAssembler* masm,
                                     ConvertReceiverMode mode) {
  // ----------- S t a t e -------------
  //  -- eax : the number of arguments (not including the receiver)
  //  -- edi : the function to call (checked to be a JSFunction)
  // -----------------------------------
  __ AssertFunction(edi);

  // See ES6 section 9.2.1 [[Call]] ( thisArgument, argumentsList)
  // Check that the function is not a "classConstructor".
  Label class_constructor;
  __ mov(edx, FieldOperand(edi, JSFunction::kSharedFunctionInfoOffset));
  __ test_b(FieldOperand(edx, SharedFunctionInfo::kFunctionKindByteOffset),
            SharedFunctionInfo::kClassConstructorBitsWithinByte);
  __ j(not_zero, &class_constructor);

  // Enter the context of the function; ToObject has to run in the function
  // context, and we also need to take the global proxy from the function
  // context in case of conversion.
  STATIC_ASSERT(SharedFunctionInfo::kNativeByteOffset ==
                SharedFunctionInfo::kStrictModeByteOffset);
  __ mov(esi, FieldOperand(edi, JSFunction::kContextOffset));
  // We need to convert the receiver for non-native sloppy mode functions.
  Label done_convert;
  __ test_b(FieldOperand(edx, SharedFunctionInfo::kNativeByteOffset),
            (1 << SharedFunctionInfo::kNativeBitWithinByte) |
                (1 << SharedFunctionInfo::kStrictModeBitWithinByte));
  __ j(not_zero, &done_convert);
  {
    // ----------- S t a t e -------------
    //  -- eax : the number of arguments (not including the receiver)
    //  -- edx : the shared function info.
    //  -- edi : the function to call (checked to be a JSFunction)
    //  -- esi : the function context.
    // -----------------------------------

    if (mode == ConvertReceiverMode::kNullOrUndefined) {
      // Patch receiver to global proxy.
      __ LoadGlobalProxy(ecx);
    } else {
      Label convert_to_object, convert_receiver;
      __ mov(ecx, Operand(esp, eax, times_pointer_size, kPointerSize));
      __ JumpIfSmi(ecx, &convert_to_object, Label::kNear);
      STATIC_ASSERT(LAST_JS_RECEIVER_TYPE == LAST_TYPE);
      __ CmpObjectType(ecx, FIRST_JS_RECEIVER_TYPE, ebx);
      __ j(above_equal, &done_convert);
      if (mode != ConvertReceiverMode::kNotNullOrUndefined) {
        Label convert_global_proxy;
        __ JumpIfRoot(ecx, Heap::kUndefinedValueRootIndex,
                      &convert_global_proxy, Label::kNear);
        __ JumpIfNotRoot(ecx, Heap::kNullValueRootIndex, &convert_to_object,
                         Label::kNear);
        __ bind(&convert_global_proxy);
        {
          // Patch receiver to global proxy.
          __ LoadGlobalProxy(ecx);
        }
        __ jmp(&convert_receiver);
      }
      __ bind(&convert_to_object);
      {
        // Convert receiver using ToObject.
        // TODO(bmeurer): Inline the allocation here to avoid building the frame
        // in the fast case? (fall back to AllocateInNewSpace?)
        FrameScope scope(masm, StackFrame::INTERNAL);
        __ SmiTag(eax);
        __ Push(eax);
        __ Push(edi);
        __ mov(eax, ecx);
        ToObjectStub stub(masm->isolate());
        __ CallStub(&stub);
        __ mov(ecx, eax);
        __ Pop(edi);
        __ Pop(eax);
        __ SmiUntag(eax);
      }
      __ mov(edx, FieldOperand(edi, JSFunction::kSharedFunctionInfoOffset));
      __ bind(&convert_receiver);
    }
    __ mov(Operand(esp, eax, times_pointer_size, kPointerSize), ecx);
  }
  __ bind(&done_convert);

  // ----------- S t a t e -------------
  //  -- eax : the number of arguments (not including the receiver)
  //  -- edx : the shared function info.
  //  -- edi : the function to call (checked to be a JSFunction)
  //  -- esi : the function context.
  // -----------------------------------

  __ mov(ebx,
         FieldOperand(edx, SharedFunctionInfo::kFormalParameterCountOffset));
  __ SmiUntag(ebx);
  ParameterCount actual(eax);
  ParameterCount expected(ebx);
  __ InvokeFunctionCode(edi, no_reg, expected, actual, JUMP_FUNCTION,
                        CheckDebugStepCallWrapper());
  // The function is a "classConstructor", need to raise an exception.
  __ bind(&class_constructor);
  {
    FrameScope frame(masm, StackFrame::INTERNAL);
    __ push(edi);
    __ CallRuntime(Runtime::kThrowConstructorNonCallableError, 1);
  }
}


namespace {

void Generate_PushBoundArguments(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax : the number of arguments (not including the receiver)
  //  -- edx : new.target (only in case of [[Construct]])
  //  -- edi : target (checked to be a JSBoundFunction)
  // -----------------------------------

  // Load [[BoundArguments]] into ecx and length of that into ebx.
  Label no_bound_arguments;
  __ mov(ecx, FieldOperand(edi, JSBoundFunction::kBoundArgumentsOffset));
  __ mov(ebx, FieldOperand(ecx, FixedArray::kLengthOffset));
  __ SmiUntag(ebx);
  __ test(ebx, ebx);
  __ j(zero, &no_bound_arguments);
  {
    // ----------- S t a t e -------------
    //  -- eax : the number of arguments (not including the receiver)
    //  -- edx : new.target (only in case of [[Construct]])
    //  -- edi : target (checked to be a JSBoundFunction)
    //  -- ecx : the [[BoundArguments]] (implemented as FixedArray)
    //  -- ebx : the number of [[BoundArguments]]
    // -----------------------------------

    // Reserve stack space for the [[BoundArguments]].
    {
      Label done;
      __ lea(ecx, Operand(ebx, times_pointer_size, 0));
      __ sub(esp, ecx);
      // Check the stack for overflow. We are not trying to catch interruptions
      // (i.e. debug break and preemption) here, so check the "real stack
      // limit".
      __ CompareRoot(esp, ecx, Heap::kRealStackLimitRootIndex);
      __ j(greater, &done, Label::kNear);  // Signed comparison.
      // Restore the stack pointer.
      __ lea(esp, Operand(esp, ebx, times_pointer_size, 0));
      {
        FrameScope scope(masm, StackFrame::MANUAL);
        __ EnterFrame(StackFrame::INTERNAL);
        __ CallRuntime(Runtime::kThrowStackOverflow, 0);
      }
      __ bind(&done);
    }

    // Adjust effective number of arguments to include return address.
    __ inc(eax);

    // Relocate arguments and return address down the stack.
    {
      Label loop;
      __ Set(ecx, 0);
      __ lea(ebx, Operand(esp, ebx, times_pointer_size, 0));
      __ bind(&loop);
      __ fld_s(Operand(ebx, ecx, times_pointer_size, 0));
      __ fstp_s(Operand(esp, ecx, times_pointer_size, 0));
      __ inc(ecx);
      __ cmp(ecx, eax);
      __ j(less, &loop);
    }

    // Copy [[BoundArguments]] to the stack (below the arguments).
    {
      Label loop;
      __ mov(ecx, FieldOperand(edi, JSBoundFunction::kBoundArgumentsOffset));
      __ mov(ebx, FieldOperand(ecx, FixedArray::kLengthOffset));
      __ SmiUntag(ebx);
      __ bind(&loop);
      __ dec(ebx);
      __ fld_s(
          FieldOperand(ecx, ebx, times_pointer_size, FixedArray::kHeaderSize));
      __ fstp_s(Operand(esp, eax, times_pointer_size, 0));
      __ lea(eax, Operand(eax, 1));
      __ j(greater, &loop);
    }

    // Adjust effective number of arguments (eax contains the number of
    // arguments from the call plus return address plus the number of
    // [[BoundArguments]]), so we need to subtract one for the return address.
    __ dec(eax);
  }
  __ bind(&no_bound_arguments);
}

}  // namespace


// static
void Builtins::Generate_CallBoundFunction(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax : the number of arguments (not including the receiver)
  //  -- edi : the function to call (checked to be a JSBoundFunction)
  // -----------------------------------
  __ AssertBoundFunction(edi);

  // Patch the receiver to [[BoundThis]].
  __ mov(ebx, FieldOperand(edi, JSBoundFunction::kBoundThisOffset));
  __ mov(Operand(esp, eax, times_pointer_size, kPointerSize), ebx);

  // Push the [[BoundArguments]] onto the stack.
  Generate_PushBoundArguments(masm);

  // Call the [[BoundTargetFunction]] via the Call builtin.
  __ mov(edi, FieldOperand(edi, JSBoundFunction::kBoundTargetFunctionOffset));
  __ mov(ecx, Operand::StaticVariable(ExternalReference(
                  Builtins::kCall_ReceiverIsAny, masm->isolate())));
  __ lea(ecx, FieldOperand(ecx, Code::kHeaderSize));
  __ jmp(ecx);
}


// static
void Builtins::Generate_Call(MacroAssembler* masm, ConvertReceiverMode mode) {
  // ----------- S t a t e -------------
  //  -- eax : the number of arguments (not including the receiver)
  //  -- edi : the target to call (can be any Object).
  // -----------------------------------

  Label non_callable, non_function, non_smi;
  __ JumpIfSmi(edi, &non_callable);
  __ bind(&non_smi);
  __ CmpObjectType(edi, JS_FUNCTION_TYPE, ecx);
  __ j(equal, masm->isolate()->builtins()->CallFunction(mode),
       RelocInfo::CODE_TARGET);
  __ CmpInstanceType(ecx, JS_BOUND_FUNCTION_TYPE);
  __ j(equal, masm->isolate()->builtins()->CallBoundFunction(),
       RelocInfo::CODE_TARGET);
  __ CmpInstanceType(ecx, JS_PROXY_TYPE);
  __ j(not_equal, &non_function);

  // 1. Runtime fallback for Proxy [[Call]].
  __ PopReturnAddressTo(ecx);
  __ Push(edi);
  __ PushReturnAddressFrom(ecx);
  // Increase the arguments size to include the pushed function and the
  // existing receiver on the stack.
  __ add(eax, Immediate(2));
  // Tail-call to the runtime.
  __ JumpToExternalReference(
      ExternalReference(Runtime::kJSProxyCall, masm->isolate()));

  // 2. Call to something else, which might have a [[Call]] internal method (if
  // not we raise an exception).
  __ bind(&non_function);
  // Check if target has a [[Call]] internal method.
  __ test_b(FieldOperand(ecx, Map::kBitFieldOffset), 1 << Map::kIsCallable);
  __ j(zero, &non_callable, Label::kNear);
  // Overwrite the original receiver with the (original) target.
  __ mov(Operand(esp, eax, times_pointer_size, kPointerSize), edi);
  // Let the "call_as_function_delegate" take care of the rest.
  __ LoadGlobalFunction(Context::CALL_AS_FUNCTION_DELEGATE_INDEX, edi);
  __ Jump(masm->isolate()->builtins()->CallFunction(
              ConvertReceiverMode::kNotNullOrUndefined),
          RelocInfo::CODE_TARGET);

  // 3. Call to something that is not callable.
  __ bind(&non_callable);
  {
    FrameScope scope(masm, StackFrame::INTERNAL);
    __ Push(edi);
    __ CallRuntime(Runtime::kThrowCalledNonCallable, 1);
  }
}


// static
void Builtins::Generate_ConstructFunction(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax : the number of arguments (not including the receiver)
  //  -- edx : the new target (checked to be a constructor)
  //  -- edi : the constructor to call (checked to be a JSFunction)
  // -----------------------------------
  __ AssertFunction(edi);

  // Calling convention for function specific ConstructStubs require
  // ebx to contain either an AllocationSite or undefined.
  __ LoadRoot(ebx, Heap::kUndefinedValueRootIndex);

  // Tail call to the function-specific construct stub (still in the caller
  // context at this point).
  __ mov(ecx, FieldOperand(edi, JSFunction::kSharedFunctionInfoOffset));
  __ mov(ecx, FieldOperand(ecx, SharedFunctionInfo::kConstructStubOffset));
  __ lea(ecx, FieldOperand(ecx, Code::kHeaderSize));
  __ jmp(ecx);
}


// static
void Builtins::Generate_ConstructBoundFunction(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax : the number of arguments (not including the receiver)
  //  -- edx : the new target (checked to be a constructor)
  //  -- edi : the constructor to call (checked to be a JSBoundFunction)
  // -----------------------------------
  __ AssertBoundFunction(edi);

  // Push the [[BoundArguments]] onto the stack.
  Generate_PushBoundArguments(masm);

  // Patch new.target to [[BoundTargetFunction]] if new.target equals target.
  {
    Label done;
    __ cmp(edi, edx);
    __ j(not_equal, &done, Label::kNear);
    __ mov(edx, FieldOperand(edi, JSBoundFunction::kBoundTargetFunctionOffset));
    __ bind(&done);
  }

  // Construct the [[BoundTargetFunction]] via the Construct builtin.
  __ mov(edi, FieldOperand(edi, JSBoundFunction::kBoundTargetFunctionOffset));
  __ mov(ecx, Operand::StaticVariable(
                  ExternalReference(Builtins::kConstruct, masm->isolate())));
  __ lea(ecx, FieldOperand(ecx, Code::kHeaderSize));
  __ jmp(ecx);
}


// static
void Builtins::Generate_ConstructProxy(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax : the number of arguments (not including the receiver)
  //  -- edi : the constructor to call (checked to be a JSProxy)
  //  -- edx : the new target (either the same as the constructor or
  //           the JSFunction on which new was invoked initially)
  // -----------------------------------

  // Call into the Runtime for Proxy [[Construct]].
  __ PopReturnAddressTo(ecx);
  __ Push(edi);
  __ Push(edx);
  __ PushReturnAddressFrom(ecx);
  // Include the pushed new_target, constructor and the receiver.
  __ add(eax, Immediate(3));
  // Tail-call to the runtime.
  __ JumpToExternalReference(
      ExternalReference(Runtime::kJSProxyConstruct, masm->isolate()));
}


// static
void Builtins::Generate_Construct(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax : the number of arguments (not including the receiver)
  //  -- edx : the new target (either the same as the constructor or
  //           the JSFunction on which new was invoked initially)
  //  -- edi : the constructor to call (can be any Object)
  // -----------------------------------

  // Check if target is a Smi.
  Label non_constructor;
  __ JumpIfSmi(edi, &non_constructor, Label::kNear);

  // Dispatch based on instance type.
  __ CmpObjectType(edi, JS_FUNCTION_TYPE, ecx);
  __ j(equal, masm->isolate()->builtins()->ConstructFunction(),
       RelocInfo::CODE_TARGET);

  // Check if target has a [[Construct]] internal method.
  __ test_b(FieldOperand(ecx, Map::kBitFieldOffset), 1 << Map::kIsConstructor);
  __ j(zero, &non_constructor, Label::kNear);

  // Only dispatch to bound functions after checking whether they are
  // constructors.
  __ CmpInstanceType(ecx, JS_BOUND_FUNCTION_TYPE);
  __ j(equal, masm->isolate()->builtins()->ConstructBoundFunction(),
       RelocInfo::CODE_TARGET);

  // Only dispatch to proxies after checking whether they are constructors.
  __ CmpInstanceType(ecx, JS_PROXY_TYPE);
  __ j(equal, masm->isolate()->builtins()->ConstructProxy(),
       RelocInfo::CODE_TARGET);

  // Called Construct on an exotic Object with a [[Construct]] internal method.
  {
    // Overwrite the original receiver with the (original) target.
    __ mov(Operand(esp, eax, times_pointer_size, kPointerSize), edi);
    // Let the "call_as_constructor_delegate" take care of the rest.
    __ LoadGlobalFunction(Context::CALL_AS_CONSTRUCTOR_DELEGATE_INDEX, edi);
    __ Jump(masm->isolate()->builtins()->CallFunction(),
            RelocInfo::CODE_TARGET);
  }

  // Called Construct on an Object that doesn't have a [[Construct]] internal
  // method.
  __ bind(&non_constructor);
  __ Jump(masm->isolate()->builtins()->ConstructedNonConstructable(),
          RelocInfo::CODE_TARGET);
}


void Builtins::Generate_ArgumentsAdaptorTrampoline(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax : actual number of arguments
  //  -- ebx : expected number of arguments
  //  -- edx : new target (passed through to callee)
  //  -- edi : function (passed through to callee)
  // -----------------------------------

  Label invoke, dont_adapt_arguments, stack_overflow;
  __ IncrementCounter(masm->isolate()->counters()->arguments_adaptors(), 1);

  Label enough, too_few;
  __ cmp(eax, ebx);
  __ j(less, &too_few);
  __ cmp(ebx, SharedFunctionInfo::kDontAdaptArgumentsSentinel);
  __ j(equal, &dont_adapt_arguments);

  {  // Enough parameters: Actual >= expected.
    __ bind(&enough);
    EnterArgumentsAdaptorFrame(masm);
    ArgumentsAdaptorStackCheck(masm, &stack_overflow);

    // Copy receiver and all expected arguments.
    const int offset = StandardFrameConstants::kCallerSPOffset;
    __ lea(edi, Operand(ebp, eax, times_4, offset));
    __ mov(eax, -1);  // account for receiver

    Label copy;
    __ bind(&copy);
    __ inc(eax);
    __ push(Operand(edi, 0));
    __ sub(edi, Immediate(kPointerSize));
    __ cmp(eax, ebx);
    __ j(less, &copy);
    // eax now contains the expected number of arguments.
    __ jmp(&invoke);
  }

  {  // Too few parameters: Actual < expected.
    __ bind(&too_few);

    // If the function is strong we need to throw an error.
    Label no_strong_error;
    __ mov(ecx, FieldOperand(edi, JSFunction::kSharedFunctionInfoOffset));
    __ test_b(FieldOperand(ecx, SharedFunctionInfo::kStrongModeByteOffset),
              1 << SharedFunctionInfo::kStrongModeBitWithinByte);
    __ j(equal, &no_strong_error, Label::kNear);

    // What we really care about is the required number of arguments.
    __ mov(ecx, FieldOperand(ecx, SharedFunctionInfo::kLengthOffset));
    __ SmiUntag(ecx);
    __ cmp(eax, ecx);
    __ j(greater_equal, &no_strong_error, Label::kNear);

    {
      FrameScope frame(masm, StackFrame::MANUAL);
      EnterArgumentsAdaptorFrame(masm);
      __ CallRuntime(Runtime::kThrowStrongModeTooFewArguments, 0);
    }

    __ bind(&no_strong_error);
    EnterArgumentsAdaptorFrame(masm);
    ArgumentsAdaptorStackCheck(masm, &stack_overflow);

    // Remember expected arguments in ecx.
    __ mov(ecx, ebx);

    // Copy receiver and all actual arguments.
    const int offset = StandardFrameConstants::kCallerSPOffset;
    __ lea(edi, Operand(ebp, eax, times_4, offset));
    // ebx = expected - actual.
    __ sub(ebx, eax);
    // eax = -actual - 1
    __ neg(eax);
    __ sub(eax, Immediate(1));

    Label copy;
    __ bind(&copy);
    __ inc(eax);
    __ push(Operand(edi, 0));
    __ sub(edi, Immediate(kPointerSize));
    __ test(eax, eax);
    __ j(not_zero, &copy);

    // Fill remaining expected arguments with undefined values.
    Label fill;
    __ bind(&fill);
    __ inc(eax);
    __ push(Immediate(masm->isolate()->factory()->undefined_value()));
    __ cmp(eax, ebx);
    __ j(less, &fill);

    // Restore expected arguments.
    __ mov(eax, ecx);
  }

  // Call the entry point.
  __ bind(&invoke);
  // Restore function pointer.
  __ mov(edi, Operand(ebp, JavaScriptFrameConstants::kFunctionOffset));
  // eax : expected number of arguments
  // edx : new target (passed through to callee)
  // edi : function (passed through to callee)
  __ mov(ecx, FieldOperand(edi, JSFunction::kCodeEntryOffset));
  __ call(ecx);

  // Store offset of return address for deoptimizer.
  masm->isolate()->heap()->SetArgumentsAdaptorDeoptPCOffset(masm->pc_offset());

  // Leave frame and return.
  LeaveArgumentsAdaptorFrame(masm);
  __ ret(0);

  // -------------------------------------------
  // Dont adapt arguments.
  // -------------------------------------------
  __ bind(&dont_adapt_arguments);
  __ mov(ecx, FieldOperand(edi, JSFunction::kCodeEntryOffset));
  __ jmp(ecx);

  __ bind(&stack_overflow);
  {
    FrameScope frame(masm, StackFrame::MANUAL);
    __ CallRuntime(Runtime::kThrowStackOverflow, 0);
    __ int3();
  }
}


static void CompatibleReceiverCheck(MacroAssembler* masm, Register receiver,
                                    Register function_template_info,
                                    Register scratch0, Register scratch1,
                                    Label* receiver_check_failed) {
  // If receiver is not an object, jump to receiver_check_failed.
  __ CmpObjectType(receiver, FIRST_JS_OBJECT_TYPE, scratch0);
  __ j(below, receiver_check_failed);

  // If there is no signature, return the holder.
  __ CompareRoot(FieldOperand(function_template_info,
                              FunctionTemplateInfo::kSignatureOffset),
                 Heap::kUndefinedValueRootIndex);
  Label receiver_check_passed;
  __ j(equal, &receiver_check_passed, Label::kNear);

  // Walk the prototype chain.
  Label prototype_loop_start;
  __ bind(&prototype_loop_start);

  // End if receiver is null or if it's a hidden prototype.
  __ CompareRoot(receiver, Heap::kNullValueRootIndex);
  __ j(equal, receiver_check_failed, Label::kNear);
  __ mov(scratch0, FieldOperand(receiver, HeapObject::kMapOffset));
  __ test(FieldOperand(scratch0, Map::kBitField3Offset),
          Immediate(Map::IsHiddenPrototype::kMask));
  __ j(not_zero, receiver_check_failed, Label::kNear);

  // Get the constructor, if any.
  __ GetMapConstructor(scratch0, scratch0, scratch1);
  __ CmpInstanceType(scratch1, JS_FUNCTION_TYPE);
  Label next_prototype;
  __ j(not_equal, &next_prototype, Label::kNear);

  // Get the constructor's signature.
  __ mov(scratch0,
         FieldOperand(scratch0, JSFunction::kSharedFunctionInfoOffset));
  __ mov(scratch0,
         FieldOperand(scratch0, SharedFunctionInfo::kFunctionDataOffset));

  // Loop through the chain of inheriting function templates.
  Label function_template_loop;
  __ bind(&function_template_loop);

  // If the signatures match, we have a compatible receiver.
  __ cmp(scratch0, FieldOperand(function_template_info,
                                FunctionTemplateInfo::kSignatureOffset));
  __ j(equal, &receiver_check_passed, Label::kNear);

  // If the current type is not a FunctionTemplateInfo, load the next prototype
  // in the chain.
  __ JumpIfSmi(scratch0, &next_prototype, Label::kNear);
  __ CmpObjectType(scratch0, FUNCTION_TEMPLATE_INFO_TYPE, scratch1);
  __ j(not_equal, &next_prototype, Label::kNear);

  // Otherwise load the parent function template and iterate.
  __ mov(scratch0,
         FieldOperand(scratch0, FunctionTemplateInfo::kParentTemplateOffset));
  __ jmp(&function_template_loop, Label::kNear);

  // Load the next prototype and iterate.
  __ bind(&next_prototype);
  __ mov(receiver, FieldOperand(receiver, HeapObject::kMapOffset));
  __ mov(receiver, FieldOperand(receiver, Map::kPrototypeOffset));
  __ jmp(&prototype_loop_start, Label::kNear);

  __ bind(&receiver_check_passed);
}


void Builtins::Generate_HandleFastApiCall(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- eax                : number of arguments (not including the receiver)
  //  -- edi                : callee
  //  -- esi                : context
  //  -- esp[0]             : return address
  //  -- esp[4]             : last argument
  //  -- ...
  //  -- esp[eax * 4]       : first argument
  //  -- esp[(eax + 1) * 4] : receiver
  // -----------------------------------

  // Load the receiver.
  Operand receiver_operand(esp, eax, times_pointer_size, kPCOnStackSize);
  __ mov(ecx, receiver_operand);

  // Update the receiver if this is a contextual call.
  Label set_global_proxy, valid_receiver;
  __ CompareRoot(ecx, Heap::kUndefinedValueRootIndex);
  __ j(equal, &set_global_proxy);
  __ bind(&valid_receiver);

  // Load the FunctionTemplateInfo.
  __ mov(ebx, FieldOperand(edi, JSFunction::kSharedFunctionInfoOffset));
  __ mov(ebx, FieldOperand(ebx, SharedFunctionInfo::kFunctionDataOffset));

  // Do the compatible receiver check.
  Label receiver_check_failed;
  __ Push(eax);
  CompatibleReceiverCheck(masm, ecx, ebx, edx, eax, &receiver_check_failed);
  __ Pop(eax);
  // Get the callback offset from the FunctionTemplateInfo, and jump to the
  // beginning of the code.
  __ mov(edx, FieldOperand(ebx, FunctionTemplateInfo::kCallCodeOffset));
  __ mov(edx, FieldOperand(edx, CallHandlerInfo::kFastHandlerOffset));
  __ add(edx, Immediate(Code::kHeaderSize - kHeapObjectTag));
  __ jmp(edx);

  __ bind(&set_global_proxy);
  __ mov(ecx, NativeContextOperand());
  __ mov(ecx, ContextOperand(ecx, Context::GLOBAL_PROXY_INDEX));
  __ mov(receiver_operand, ecx);
  __ jmp(&valid_receiver, Label::kNear);

  // Compatible receiver check failed: pop return address, arguments and
  // receiver and throw an Illegal Invocation exception.
  __ bind(&receiver_check_failed);
  __ Pop(eax);
  __ PopReturnAddressTo(ebx);
  __ lea(eax, Operand(eax, times_pointer_size, 1 * kPointerSize));
  __ add(esp, eax);
  __ PushReturnAddressFrom(ebx);
  {
    FrameScope scope(masm, StackFrame::INTERNAL);
    __ TailCallRuntime(Runtime::kThrowIllegalInvocation, 0, 1);
  }
}


void Builtins::Generate_OnStackReplacement(MacroAssembler* masm) {
  // Lookup the function in the JavaScript frame.
  __ mov(eax, Operand(ebp, JavaScriptFrameConstants::kFunctionOffset));
  {
    FrameScope scope(masm, StackFrame::INTERNAL);
    // Pass function as argument.
    __ push(eax);
    __ CallRuntime(Runtime::kCompileForOnStackReplacement, 1);
  }

  Label skip;
  // If the code object is null, just return to the unoptimized code.
  __ cmp(eax, Immediate(0));
  __ j(not_equal, &skip, Label::kNear);
  __ ret(0);

  __ bind(&skip);

  // Load deoptimization data from the code object.
  __ mov(ebx, Operand(eax, Code::kDeoptimizationDataOffset - kHeapObjectTag));

  // Load the OSR entrypoint offset from the deoptimization data.
  __ mov(ebx, Operand(ebx, FixedArray::OffsetOfElementAt(
      DeoptimizationInputData::kOsrPcOffsetIndex) - kHeapObjectTag));
  __ SmiUntag(ebx);

  // Compute the target address = code_obj + header_size + osr_offset
  __ lea(eax, Operand(eax, ebx, times_1, Code::kHeaderSize - kHeapObjectTag));

  // Overwrite the return address on the stack.
  __ mov(Operand(esp, 0), eax);

  // And "return" to the OSR entry point of the function.
  __ ret(0);
}


void Builtins::Generate_OsrAfterStackCheck(MacroAssembler* masm) {
  // We check the stack limit as indicator that recompilation might be done.
  Label ok;
  ExternalReference stack_limit =
      ExternalReference::address_of_stack_limit(masm->isolate());
  __ cmp(esp, Operand::StaticVariable(stack_limit));
  __ j(above_equal, &ok, Label::kNear);
  {
    FrameScope scope(masm, StackFrame::INTERNAL);
    __ CallRuntime(Runtime::kStackGuard, 0);
  }
  __ jmp(masm->isolate()->builtins()->OnStackReplacement(),
         RelocInfo::CODE_TARGET);

  __ bind(&ok);
  __ ret(0);
}

#undef __
}  // namespace internal
}  // namespace v8

#endif  // V8_TARGET_ARCH_X87
