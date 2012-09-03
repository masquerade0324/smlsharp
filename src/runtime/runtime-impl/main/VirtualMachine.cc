/**
 * Implementation of IML Virtual Machine.
 * @author YAMATODANI Kiyoshi
 * @version $Id: VirtualMachine.cc,v 1.72 2007/03/09 06:39:57 kiyoshiy Exp $
 */
#include <stdio.h>
#include <string>
#include <signal.h>

#include "Heap.hh"
#include "Primitives.hh"
#include "Constants.hh"
#include "PrimitiveSupport.hh"
#include "Instructions.hh"
#include "NoEnoughHeapException.hh"
#include "VirtualMachine.hh"
#include "IllegalArgumentException.hh"
#include "IllegalStateException.hh"
#include "FFIException.hh"
#include "InterruptedException.hh"
#include "Log.hh"
#include "Debug.hh"
#include "FFI.hh"

BEGIN_NAMESPACE(jp_ac_jaist_iml_runtime)

///////////////////////////////////////////////////////////////////////////////

const int PRIMITIVE_MAX_ARGUMENTS = 256;

const int CLOSURE_ENTRYPOINT_INDEX = 0;
const int CLOSURE_ENV_INDEX = 1;
const int CLOSURE_BITMAP = 2;
const int CLOSURE_FIELDS_COUNT = 2;

const int FINALIZABLE_BITMAP = 3;

const int INDEX_OF_NEST_POINTER = 0;

// Use non-NULL as dummy, to avoid assertion failure
UInt32Value * const RETURN_ADDRESS_OF_INITIAL_FRAME = (UInt32Value*)-1;
///////////////////////////////////////////////////////////////////////////////
// macro for heap access

#define HEAP_GETFIELD(block, index) \
(block)[(index)]

///////////////////////////////////////////////////////////////////////////////

Cell* VirtualMachine::ENV_ = 0;
UInt32Value* VirtualMachine::SP_ = 0;
UInt32Value* VirtualMachine::PC_ = 0;

UInt32Value* VirtualMachine::FrameStack::frameStack_ = 0;
UInt32Value* VirtualMachine::FrameStack::frameStackBottom_ = 0;

VirtualMachine* VirtualMachine::instance_ = 0;
Session* VirtualMachine::session_ = 0;

const char* VirtualMachine::name_ = 0;
int VirtualMachine::argumentsCount_ = 0;
const char** VirtualMachine::arguments_ = 0;

bool VirtualMachine::isPrimitiveExceptionRaised_ = false;
Cell VirtualMachine::primitiveException_;

UInt32Value* VirtualMachine::HandlerStack::stack_ = 0;
UInt32Value* VirtualMachine::HandlerStack::stackTop_ = 0;
UInt32Value* VirtualMachine::HandlerStack::currentTop_ = 0;

VirtualMachine::BlockPointerVector VirtualMachine::globalArrays_;

VirtualMachine::BlockPointerRefList VirtualMachine::temporaryPointers_;

bool VirtualMachine::interrupted_ = false;
void (*VirtualMachine::prevSIGINTHandler_)(int) = NULL;
jmp_buf VirtualMachine::onSIGFPE_jmp_buf;
void (*VirtualMachine::prevSIGFPEHandler_)(int) = NULL;
void (*VirtualMachine::prevSIGPIPEHandler_)(int) = NULL;

#ifdef IML_ENABLE_EXECUTION_MONITORING
VirtualMachine::MonitorList VirtualMachine::executionMonitors_;
#endif

///////////////////////////////////////////////////////////////////////////////

INLINE_FUN
static
UInt32Value leftShift(UInt32Value left, UInt32Value right){
    if(right < 32){
        return left << right;
    }
    else{return 0;}
}

INLINE_FUN
static
UInt32Value logicalRightShift(UInt32Value left, UInt32Value right)
{
    UInt32Value result = 0;
    /* Shift instruction of x86 architecture considers only lower 5bits of
     * shift count (= right).
     * It results incorrect value, if shift count is equal or mora then 32.
     * In those cases, 0 should be returned.
     */
    if(right < 32){
        /* It is undefined in C that >> operator is logical or arithmetic.
         * So, we first right-shift bits except the MSB.
         * Then, we obtain right-shift value of the MSB by left-shifting 1.
         */
        result = (0x7FFFFFFFUL & left) >> right;
        if(0x80000000UL & left){// MSB is 1
            result |= 0x1UL << (31 - right);// if right = 31, shift 0 bit.
        }
    }
    return result;
}

INLINE_FUN
static
UInt32Value arithmeticRightShift(UInt32Value left, UInt32Value right)
{
    if(right < 32){
        UInt32Value result = left >> right;
        if(0x80000000UL & left){// MSB is 1
            // set all bits higher than (31 - right)th bit.
            result |= ((UInt32Value)-1L) << (32 - right);
        }
        return result;
    }
    else{
        return ((SInt32Value)left) < 0 ? (UInt32Value)-1 : 0;
    }
}

VirtualMachine::VirtualMachine(const char* name,
                               const int argumentsCount,
                               const char** arguments,
                               const int stackSize)
{
    name_ = name;
    argumentsCount_ = argumentsCount;
    arguments_ = arguments;
    DBGWRAP(LOG.debug("VM: argc = %d", argumentsCount_);)

    FrameStack::initialize(stackSize);
    HandlerStack::initialize(stackSize);

    globalArrays_.clear();
    temporaryPointers_.clear();

#ifdef IML_ENABLE_EXECUTION_MONITORING
    executionMonitors_.clear();
#endif

    instance_ = this;
}

VirtualMachine::~VirtualMachine()
{
    FrameStack::finalize();
    HandlerStack::finalize();
}

///////////////////////////////////////////////////////////////////////////////

void
VirtualMachine::setSession(Session* session)
{
    session_ = session;
}

Session*
VirtualMachine::getSession()
{
    return session_;
}

void
VirtualMachine::
addExecutionMonitor(VirtualMachineExecutionMonitor* monitor)
{
#ifdef IML_ENABLE_EXECUTION_MONITORING
    executionMonitors_.push_front(monitor);
#endif
}

///////////////////////////////////////////////////////////////////////////////

/**
 * <code>INVOKE_ON_MONITORS</code>(<i>methodCall</i>) invokes <i>methodCall</i>
 * on each monitors in the <code>executionMonitors_</code>.
 *
 * <p>
 *  <i>methodCall</i> specifies the method and arguments to be passed.
 * </p>
 *
 */
#ifdef IML_ENABLE_EXECUTION_MONITORING
#define INVOKE_ON_MONITORS(methodCall) \
    { \
        for(MonitorList::iterator _i_ = executionMonitors_.begin() ; \
            _i_ != executionMonitors_.end() ; \
            _i_++){ \
            VirtualMachineExecutionMonitor* _monitor_ = *_i_; \
            _monitor_->methodCall; \
        } \
    }
#else
#define INVOKE_ON_MONITORS(method)
#endif

////////////////////////////////////////

#define SAVE_REGISTERS \
{ ENV_ = ENV; SP_ = SP; }

#define RESTORE_REGISTERS \
{ ENV = ENV_; /* restore of SP is not needed. */ }

#define ALLOCATE_RECORDBLOCK(ret, bitmap, number) \
{ \
    SAVE_REGISTERS; \
    Cell* _block_ = Heap::allocRecordBlock((bitmap), (number)); \
    RESTORE_REGISTERS; \
    ret = _block_; \
}

#define ALLOCATE_ATOMBLOCK(ret, number) \
{ \
    SAVE_REGISTERS; \
    Cell* _block_ = Heap::allocAtomBlock((number)); \
    RESTORE_REGISTERS; \
    ret = _block_; \
}

#define ALLOCATE_POINTERBLOCK(ret, number) \
{ \
    SAVE_REGISTERS; \
    Cell* _block_ = Heap::allocPointerBlock((number)); \
    RESTORE_REGISTERS; \
    ret = _block_; \
}

#define ALLOCATE_SINGLEATOMARRAY(ret, number) \
{ \
    SAVE_REGISTERS; \
    Cell* _block_ = Heap::allocSingleAtomArray((number)); \
    RESTORE_REGISTERS; \
    ret = _block_; \
}

#define ALLOCATE_DOUBLEATOMARRAY(ret, number) \
{ \
    SAVE_REGISTERS; \
    Cell* _block_ = Heap::allocDoubleAtomArray((number)); \
    RESTORE_REGISTERS; \
    ret = _block_; \
}

#define ALLOCATE_POINTERARRAY(ret, number) \
{ \
    SAVE_REGISTERS; \
    Cell* _block_ = Heap::allocPointerArray((number)); \
    RESTORE_REGISTERS; \
    ret = _block_; \
}

#define ALLOCATE_STRINGBLOCK(ret, buffer, length) \
{ \
    SAVE_REGISTERS; \
    Cell _block_ = \
    PrimitiveSupport::stringToCell((const char*)(buffer), (length)); \
    RESTORE_REGISTERS; \
    ret = _block_.blockRef; \
}

#define ALLOCATE_EMPTYBLOC(ret) \
{ \
    ret = Heap::allocEmptyBlock(); \
}

/**
 * fetches 4-byte word from an instruction stream and increment the pointer.
 *
 * @param pc head of an instruction stream
 * @return the 4-byte word which <code>pc</code> points to.
 */
INLINE_FUN
static
UInt32Value getWordAndInc(UInt32Value* &pc)
{
    // the following codes are equal to "return *(pc++);"
    UInt32Value value = *pc;
    pc += 1;
    return value;
}

///////////////////////////////////////////////////////////////////////////////

//  The below macro for register optimization is copied from 'interp.c' in
// the OCaml distribution.

/* Register optimization.
   Some compilers underestimate the use of the local variables representing
   the abstract machine registers, and don't put them in hardware registers,
   which slows down the interpreter considerably.
   For GCC, I have hand-assigned hardware registers for several architectures.
*/
/*
#if defined(__GNUC__) && !defined(IML_DEBUG)
#ifdef __mips__
#define PC_REG asm("$16")
#define SP_REG asm("$17")
#define AC_REG asm("$18")
#endif
#ifdef __sparc__
#define PC_REG asm("%l0")
#define SP_REG asm("%l1")
#define AC_REG asm("%l2")
#endif
#ifdef __alpha__
#ifdef __CRAY__
#define PC_REG asm("r9")
#define SP_REG asm("r10")
#define AC_REG asm("r11")
#define JUMPTBL_BASE_REG asm("r12")
#else
#define PC_REG asm("$9")
#define SP_REG asm("$10")
#define AC_REG asm("$11")
#define JUMPTBL_BASE_REG asm("$12")
#endif
#endif
#ifdef __i386__
//#define PC_REG asm("%esi")
//#define SP_REG asm("%edi")
#define PC_REG
#define SP_REG
#define AC_REG
#endif
#if defined(PPC) || defined(_POWER) || defined(_IBMR2)
#define PC_REG asm("26")
#define SP_REG asm("27")
#define AC_REG asm("28")
#endif
#ifdef __hppa__
#define PC_REG asm("%r18")
#define SP_REG asm("%r17")
#define AC_REG asm("%r16")
#endif
#ifdef __mc68000__
#define PC_REG asm("a5")
#define SP_REG asm("a4")
#define AC_REG asm("d7")
#endif
#ifdef __arm__
#define PC_REG asm("r9")
#define SP_REG asm("r8")
#define AC_REG asm("r7")
#endif
#ifdef __ia64__
#define PC_REG asm("36")
#define SP_REG asm("37")
#define AC_REG asm("38")
#define JUMPTBL_BASE_REG asm("39")
#endif
#ifdef __x86_64__
#define PC_REG asm("%r15")
#define SP_REG asm("%r14")
#define AC_REG asm("%r13")
#endif
#endif
*/
////////////////////////////////////////

INLINE_FUN
void
VirtualMachine::expandClosure(UInt32Value* SP,
                              UInt32Value closureIndex,
                              UInt32Value* &entryPoint,
                              Cell* &calleeENV)
{
    ASSERT(FrameStack::isPointerSlot(SP, closureIndex));
    Cell* closure = FRAME_ENTRY(SP, closureIndex).blockRef;
    ASSERT(Heap::isValidBlockPointer(closure));
    ASSERT(CLOSURE_FIELDS_COUNT == Heap::getPayloadSize(closure));
    ASSERT(CLOSURE_BITMAP == Heap::getBitmap(closure));

    entryPoint =
    (UInt32Value*)(HEAP_GETFIELD(closure, CLOSURE_ENTRYPOINT_INDEX).uint32);

    ASSERT(Heap::isPointerField(closure, CLOSURE_ENV_INDEX));
    calleeENV = (Cell*)(HEAP_GETFIELD(closure, CLOSURE_ENV_INDEX).uint32);
    ASSERT(Heap::isValidBlockPointer(calleeENV));
}

void
VirtualMachine::printStackTrace(UInt32Value *PC, UInt32Value* SP)
{
    char buffer[256];

    Executable* executable = FrameStack::getExecutableOfFrame(SP);
    UInt32Value offset = PC - (UInt32Value*)(executable->code_);
    IPToString(buffer, sizeof(buffer), executable, offset);
    PrimitiveSupport::writeToSTDOUT(strlen(buffer), buffer);
    PrimitiveSupport::writeToSTDOUT(strlen("\n"), "\n");

    UInt32Value *cursorSP = SP;
    UInt32Value* const stackBottom = FrameStack::getBottom();
    while((!interrupted_) && (stackBottom != cursorSP))
    {
        /*  get the instruction offset and the Executable of
         * the call instruction which created the current
         * frame. The offset of the call instruction is stored
         * in the current (= callee) frame. The Executable is
         * obtained in the upper (= caller) frame.
         */
        UInt32Value* returnAddress = FrameStack::getReturnAddress(cursorSP);
        if(RETURN_ADDRESS_OF_INITIAL_FRAME == returnAddress)
        {
            /* The initial frame does not have its return
             * address. */
            break;
        }

        cursorSP = FrameStack::getNextFrame(cursorSP);
        ASSERT(cursorSP);

        executable = FrameStack::getExecutableOfFrame(cursorSP);
        offset = returnAddress - executable->code_;

        IPToString(buffer, sizeof(buffer), executable, offset);
        PrimitiveSupport::writeToSTDOUT(strlen(buffer), buffer);
        PrimitiveSupport::writeToSTDOUT(strlen("\n"), "\n");
    }
    if(interrupted_){
        DBGWRAP(LOG.debug("printStackTrace is interrupted.");)
    }
}

/**
 * a variation of getFunInfo specialized for self recursive call.
 */
INLINE_FUN
UInt32Value*
VirtualMachine::getFunInfoForSelfRecursiveCall(UInt32Value *entryPoint,
                                               UInt32Value &frameSize,
                                               UInt32Value* &argDests,
                                               UInt32Value * &funInfoAddress)
{
    /* get function parameter out of FunEntry instruction */
    UInt32Value *PC = entryPoint;
    funInfoAddress = entryPoint;
    PC += 1;
    frameSize = getWordAndInc(PC);
    UInt32Value* startAddress = (UInt32Value*)getWordAndInc(PC);
    UInt32Value arity = getWordAndInc(PC);
//    ASSERT(1 == arity);// arity must be 1
    argDests = PC; // argDests does not include ENV.
    // bitmap informations are ignored.
    return startAddress;
}

/**
 * a variation of getFunInfo specialized for non-self recursive call.
 */
INLINE_FUN
UInt32Value*
VirtualMachine::getFunInfoForRecursiveCall(UInt32Value *entryPoint,
                                           UInt32Value &frameSize,
                                           UInt32Value &arity,
                                           UInt32Value* &argDests,
                                           UInt32Value &bitmapvalsFreesCount,
                                           UInt32Value &bitmapvalsFree,
                                           UInt32Value * &funInfoAddress)
{
    /* get function parameter out of FunEntry instruction */
    UInt32Value *PC = entryPoint;
    funInfoAddress = entryPoint;
    PC += 1;
    frameSize = getWordAndInc(PC);
    UInt32Value* startAddress = (UInt32Value*)getWordAndInc(PC);
    arity = getWordAndInc(PC); // arity does not include ENV.
    argDests = PC; // argDests does not include ENV.
    PC += arity;
    bitmapvalsFreesCount = getWordAndInc(PC);
    bitmapvalsFree = *PC;// only the first bitmapvalsFrees is used.
    // bitmapvalsArgs are ignored.
    return startAddress;
}

INLINE_FUN
UInt32Value*
VirtualMachine::getFunInfo(UInt32Value *entryPoint,
                           UInt32Value &frameSize,
                           UInt32Value &arity,
                           UInt32Value* &argDests,
                           UInt32Value &bitmapvalsFreesCount,
                           UInt32Value * &bitmapvalsFrees,
                           UInt32Value &bitmapvalsArgsCount,
                           UInt32Value * &bitmapvalsArgs,
                           UInt32Value * &funInfoAddress)
{
    /* get function parameter out of FunEntry instruction */
    UInt32Value *PC = entryPoint;
    funInfoAddress = entryPoint;
    PC += 1;
    frameSize = getWordAndInc(PC);
    UInt32Value* startAddress = (UInt32Value*)getWordAndInc(PC);
    arity = getWordAndInc(PC); // arity does not include ENV.
    argDests = PC; // argDests does not include ENV.
    PC += arity;
    bitmapvalsFreesCount = getWordAndInc(PC);
    bitmapvalsFrees = PC;
    PC += bitmapvalsFreesCount;
    bitmapvalsArgsCount = getWordAndInc(PC);
    bitmapvalsArgs = PC;
    return startAddress;
}

INLINE_FUN
Bitmap
VirtualMachine::composeBitmap(UInt32Value* SP,
                              UInt32Value* argIndexes,
                              Cell* calleeENV,
                              UInt32Value bitmapvalsFreesCount,
                              UInt32Value * bitmapvalsFrees,
                              UInt32Value bitmapvalsArgsCount,
                              UInt32Value * bitmapvalsArgs)
{
    /* compose bitmap value */
    Bitmap bitmap = (Bitmap)0;
    ASSERT((0 == bitmapvalsFreesCount) || (bitmapvalsFreesCount == 1));
    if(bitmapvalsFreesCount){
        bitmap = HEAP_GETFIELD(calleeENV, *bitmapvalsFrees).uint32;
    }
/*
    for(int index = 0; index < bitmapvalsFreesCount; index += 1){
        ASSERT(!Heap::isPointerField(calleeENV, *bitmapvalsFrees));
        bitmap << 1;
        bitmap |= HEAP_GETFIELD(calleeENV, *bitmapvalsFrees).uint32;
        bitmapvalsFrees += 1;
    }
*/
    /* the least significant bit of the frame bitmap is obtained from the bit
     * which is obtained from the first bitmap argument.
     */
    for(int index = bitmapvalsArgsCount - 1; 0 <= index ; index -= 1)
    {
        UInt32Value bitmapvalsArgIndex = bitmapvalsArgs[index];
        UInt32Value argIndex = argIndexes[bitmapvalsArgIndex];
        ASSERT(!FrameStack::isPointerSlot(SP, argIndex));

        Bitmap newBit = FRAME_ENTRY(SP, argIndex).uint32;
        // assert that the new bit is 1 bit width.
        ASSERT((0 == newBit) || (1 == newBit));
        // assert that the most significant bit of bitmap is zero.
        ASSERT(0 == (bitmap & (1 << (FrameStack::BITMAP_BIT_WIDTH - 1))));

        bitmap <<= 1;
        bitmap |= newBit;
    }
    return bitmap;
}

INLINE_FUN
UInt32Value*
VirtualMachine::getFunInfoAndBitmap(UInt32Value* SP,
                                    UInt32Value *entryPoint,
                                    Cell* calleeENV,
                                    UInt32Value* argIndexes,
                                    UInt32Value &frameSize,
                                    UInt32Value &arity,
                                    UInt32Value* &argDests,
                                    UInt32Value* &funInfoAddress,
                                    Bitmap &bitmap)
{
    UInt32Value bitmapvalsFreesCount;
    UInt32Value *bitmapvalsFrees;
    UInt32Value bitmapvalsArgsCount;
    UInt32Value *bitmapvalsArgs;

    UInt32Value* PC = getFunInfo(entryPoint,
                                 frameSize,
                                 arity,
                                 argDests,
                                 bitmapvalsFreesCount,
                                 bitmapvalsFrees,
                                 bitmapvalsArgsCount,
                                 bitmapvalsArgs,
                                 funInfoAddress);

    bitmap = composeBitmap(SP,
                           argIndexes,
                           calleeENV,
                           bitmapvalsFreesCount,
                           bitmapvalsFrees,
                           bitmapvalsArgsCount,
                           bitmapvalsArgs);

    return PC;
}

INLINE_FUN
UInt32Value*
VirtualMachine::fillFrameForTailCall_S(UInt32Value* funInfoAddress,
                                       UInt32Value frameSize,
                                       Bitmap bitmap,
                                       UInt32Value argIndex,
                                       UInt32Value argDest,
                                       UInt32Value* SP)
{
    Cell savedArg = FRAME_ENTRY(SP, argIndex);
    
    /* replace frame for tail call */
    UInt32Value* calleeSP =
    FrameStack::replaceFrame(SP, frameSize, bitmap, funInfoAddress);

    /* copy arguments from the caller to the callee */
    FRAME_ENTRY(calleeSP, argDest) = savedArg;
    ASSERT_VALID_FRAME_VAR(calleeSP, argDest);

    return calleeSP;
}

INLINE_FUN
UInt32Value*
VirtualMachine::fillFrameForNonTailCall_S(UInt32Value* funInfoAddress,
                                          UInt32Value frameSize,
                                          Bitmap bitmap,
                                          UInt32Value argIndex,
                                          UInt32Value argDest,
                                          UInt32Value* returnAddress,
                                          UInt32Value* SP)
{
    /* allocate new frame for non tail-call */
    UInt32Value* calleeSP = FrameStack::allocateFrame(SP,
                                                      frameSize,
                                                      bitmap,
                                                      funInfoAddress,
                                                      returnAddress);

    /* copy arguments from the caller to the callee */
    ASSERT_SAME_TYPE_SLOTS(calleeSP, argDest, SP, argIndex);
    FRAME_ENTRY(calleeSP, argDest) = FRAME_ENTRY(SP, argIndex);
    ASSERT_VALID_FRAME_VAR(calleeSP, argDest);

    return calleeSP;
}

INLINE_FUN
UInt32Value*
VirtualMachine::fillFrameForTailCall_D(UInt32Value* funInfoAddress,
                                       UInt32Value frameSize,
                                       Bitmap bitmap,
                                       UInt32Value argIndex,
                                       UInt32Value argDest,
                                       UInt32Value* SP)
{
    Real64Value savedArg;
    ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex));
    savedArg = *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, argIndex);
    
    /* replace frame for tail call */
    UInt32Value* calleeSP =
    FrameStack::replaceFrame(SP, frameSize, bitmap, funInfoAddress);

    /* copy arguments from the caller to the callee */
    ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(calleeSP, argDest));
    *(Real64Value*)FRAME_ENTRY_ADDRESS(calleeSP, argDest) = savedArg;
    ASSERT_VALID_FRAME_VAR(calleeSP, argDest);
    ASSERT_VALID_FRAME_VAR(calleeSP, argDest + 1);

    return calleeSP;
}

INLINE_FUN
UInt32Value*
VirtualMachine::fillFrameForNonTailCall_D(UInt32Value* funInfoAddress,
                                          UInt32Value frameSize,
                                          Bitmap bitmap,
                                          UInt32Value argIndex,
                                          UInt32Value argDest,
                                          UInt32Value* returnAddress,
                                          UInt32Value* SP)
{
    /* allocate new frame for non tail-call */
    UInt32Value* calleeSP = FrameStack::allocateFrame(SP,
                                                      frameSize,
                                                      bitmap,
                                                      funInfoAddress,
                                                      returnAddress);

    /* copy arguments from the caller to the callee */
    ASSERT_SAME_TYPE_SLOTS(calleeSP, argDest, SP, argIndex);
    ASSERT_SAME_TYPE_SLOTS(calleeSP, argDest + 1, SP, argIndex + 1);

    ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(calleeSP, argDest));
    ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex));
    *(Real64Value*)FRAME_ENTRY_ADDRESS(calleeSP, argDest) =
    *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, argIndex);
    ASSERT_VALID_FRAME_VAR(calleeSP, argDest);
    ASSERT_VALID_FRAME_VAR(calleeSP, argDest + 1);

    return calleeSP;
}

INLINE_FUN
UInt32Value*
VirtualMachine::fillFrameForTailCall_ML_S(UInt32Value* funInfoAddress,
                                          UInt32Value frameSize,
                                          UInt32Value arity,
                                          Bitmap bitmap,
                                          UInt32Value* argIndexes,
                                          UInt32Value* argDests,
                                          UInt32Value* SP)
{
    Cell savedArgs[arity];
    for(int index = 0; index < arity; index += 1){
        savedArgs[index] = FRAME_ENTRY(SP, *argIndexes);
        argIndexes += 1;
    }
    
    /* replace frame for tail call */
    UInt32Value* calleeSP =
    FrameStack::replaceFrame(SP, frameSize, bitmap, funInfoAddress);

    /* copy arguments from the caller to the callee */
    for(int index = 0; index < arity; index += 1){
        FRAME_ENTRY(calleeSP, *argDests) = savedArgs[index];
        ASSERT_VALID_FRAME_VAR(calleeSP, *argDests);
        argDests += 1;
    }
    return calleeSP;
}

INLINE_FUN
UInt32Value*
VirtualMachine::fillFrameForNonTailCall_ML_S(UInt32Value* funInfoAddress,
                                             UInt32Value frameSize,
                                             UInt32Value arity,
                                             Bitmap bitmap,
                                             UInt32Value* argIndexes,
                                             UInt32Value* argDests,
                                             UInt32Value* returnAddress,
                                             UInt32Value* SP)
{
    /* allocate new frame for non tail-call */
    UInt32Value* calleeSP = FrameStack::allocateFrame(SP,
                                                      frameSize,
                                                      bitmap,
                                                      funInfoAddress,
                                                      returnAddress);

    /* copy arguments from the caller to the callee */
    for(int index = 0; index < arity; index += 1){
        ASSERT_SAME_TYPE_SLOTS(calleeSP, *argDests, SP, *argIndexes);
        FRAME_ENTRY(calleeSP, *argDests) = FRAME_ENTRY(SP, *argIndexes);
        ASSERT_VALID_FRAME_VAR(calleeSP, *argDests);
        argIndexes += 1;
        argDests += 1;
    }
    return calleeSP;
}

INLINE_FUN
UInt32Value*
VirtualMachine::fillFrameForTailCall_ML_D(UInt32Value* funInfoAddress,
                                          UInt32Value frameSize,
                                          UInt32Value arity,
                                          Bitmap bitmap,
                                          UInt32Value* argIndexes,
                                          UInt32Value* argDests,
                                          UInt32Value* SP)
{
    Cell savedArgs[arity - 1];
    for(int index = 0; index < arity - 1; index += 1){
        savedArgs[index] = FRAME_ENTRY(SP, *argIndexes);
        argIndexes += 1;
    }
    ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, *argIndexes));
    Real64Value savedLastArg =
        *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, *argIndexes);
    
    /* replace frame for tail call */
    UInt32Value* calleeSP =
    FrameStack::replaceFrame(SP, frameSize, bitmap, funInfoAddress);

    /* copy arguments from the caller to the callee */
    for(int index = 0; index < arity - 1; index += 1){
        FRAME_ENTRY(calleeSP, *argDests) = savedArgs[index];
        ASSERT_VALID_FRAME_VAR(calleeSP, *argDests);
        argDests += 1;
    }

    ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(calleeSP, *argDests));
    *(Real64Value*)FRAME_ENTRY_ADDRESS(calleeSP, *argDests) = savedLastArg;
    ASSERT_VALID_FRAME_VAR(calleeSP, *argDests);
    ASSERT_VALID_FRAME_VAR(calleeSP, (*argDests) + 1);

    return calleeSP;
}

INLINE_FUN
UInt32Value*
VirtualMachine::fillFrameForNonTailCall_ML_D(UInt32Value* funInfoAddress,
                                             UInt32Value frameSize,
                                             UInt32Value arity,
                                             Bitmap bitmap,
                                             UInt32Value* argIndexes,
                                             UInt32Value* argDests,
                                             UInt32Value* returnAddress,
                                             UInt32Value* SP)
{
    /* allocate new frame for non tail-call */
    UInt32Value* calleeSP = FrameStack::allocateFrame(SP,
                                                      frameSize,
                                                      bitmap,
                                                      funInfoAddress,
                                                      returnAddress);

    /* copy arguments from the caller to the callee */
    for(int index = 0; index < arity - 1; index += 1){
        ASSERT_SAME_TYPE_SLOTS(calleeSP, *argDests, SP, *argIndexes);
        FRAME_ENTRY(calleeSP, *argDests) = FRAME_ENTRY(SP, *argIndexes);
        ASSERT_VALID_FRAME_VAR(calleeSP, *argDests);
        argIndexes += 1;
        argDests += 1;
    }

    ASSERT_SAME_TYPE_SLOTS(calleeSP, *argDests, SP, *argIndexes);
    ASSERT_SAME_TYPE_SLOTS(calleeSP, (*argDests) + 1, SP, (*argIndexes) + 1);
    ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(calleeSP, *argDests));
    ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, *argIndexes));

    *(Real64Value*)FRAME_ENTRY_ADDRESS(calleeSP, *argDests) = 
    *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, *argIndexes);

    ASSERT_VALID_FRAME_VAR(calleeSP, *argDests);
    ASSERT_VALID_FRAME_VAR(calleeSP, (*argDests) + 1);

    return calleeSP;
}

INLINE_FUN
UInt32Value*
VirtualMachine::fillFrameForTailCall_M(UInt32Value* funInfoAddress,
                                       UInt32Value frameSize,
                                       UInt32Value arity,
                                       Bitmap bitmap,
                                       UInt32Value* argIndexes,
                                       UInt32Value* argSizeIndexes,
                                       UInt32Value* argDests,
                                       UInt32Value* SP)
{
    Cell savedArgs[arity * 2];
    UInt32Value savedArgSizes[arity];
    for(int index = 0; index < arity; index += 1){
        UInt32Value argSize = FRAME_ENTRY(SP, argSizeIndexes[index]).uint32;
        ASSERT((1 == argSize) || (2 == argSize));
        for(int i = 0; i < argSize; i += 1){
            savedArgs[index * 2 + i] = FRAME_ENTRY(SP, (*argIndexes) + i);
        }
        savedArgSizes[index] = argSize;
        argIndexes += 1;
    }
    
    /* replace frame for tail call */
    UInt32Value* calleeSP =
    FrameStack::replaceFrame(SP, frameSize, bitmap, funInfoAddress);

    /* copy arguments from the caller to the callee */
    for(int index = 0; index < arity; index += 1){
        UInt32Value argSize = savedArgSizes[index];
        for(int i = 0; i < argSize; i += 1){
            FRAME_ENTRY(calleeSP, (*argDests) + i) = savedArgs[index * 2 + i];
            ASSERT_VALID_FRAME_VAR(calleeSP, (*argDests) + i);
        }
        argDests += 1;
    }

    return calleeSP;
}

INLINE_FUN
UInt32Value*
VirtualMachine::fillFrameForNonTailCall_M(UInt32Value* funInfoAddress,
                                          UInt32Value frameSize,
                                          UInt32Value arity,
                                          Bitmap bitmap,
                                          UInt32Value* argIndexes,
                                          UInt32Value* argSizeIndexes,
                                          UInt32Value* argDests,
                                          UInt32Value* returnAddress,
                                          UInt32Value* SP)
{
    /* allocate new frame for non tail-call */
    UInt32Value* calleeSP = FrameStack::allocateFrame(SP,
                                                      frameSize,
                                                      bitmap,
                                                      funInfoAddress,
                                                      returnAddress);

    /* copy arguments from the caller to the callee */
    for(int index = 0; index < arity; index += 1){
        ASSERT_SAME_TYPE_SLOTS(calleeSP, *argDests, SP, *argIndexes);
        UInt32Value argSize = FRAME_ENTRY(SP, argSizeIndexes[index]).uint32;
        ASSERT((1 == argSize) || (2 == argSize));
        for(int i = 0; i < argSize; i += 1){
            FRAME_ENTRY(calleeSP, (*argDests) + i) =
            FRAME_ENTRY(SP, (*argIndexes) + i);
            ASSERT_VALID_FRAME_VAR(calleeSP, (*argDests) + i);
        }
        argIndexes += 1;
        argDests += 1;
    }

    return calleeSP;
}

INLINE_FUN
void
VirtualMachine::callFunction_S(bool isTailCall,
                               UInt32Value* &PC,
                               UInt32Value* &SP,
                               Cell* &ENV,
                               UInt32Value *entryPoint,
                               Cell* calleeENV,
                               UInt32Value argIndex,
                               UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value arity;
    UInt32Value* argDests;
    UInt32Value *funInfoAddress;
    Bitmap bitmap;

    PC = getFunInfoAndBitmap(SP,
                             entryPoint,
                             calleeENV,
                             &argIndex,
                             frameSize,
                             arity,
                             argDests,
                             funInfoAddress,
                             bitmap);

    if(isTailCall){
        ASSERT(NULL == returnAddress);
        SP = fillFrameForTailCall_S(funInfoAddress,
                                    frameSize,
                                    bitmap,
                                    argIndex,
                                    *argDests,
                                    SP);
    }
    else{
        ASSERT(returnAddress);
        SP = fillFrameForNonTailCall_S(funInfoAddress,
                                       frameSize,
                                       bitmap,
                                       argIndex,
                                       *argDests,
                                       returnAddress,
                                       SP);
    }

    /* set registers */
    ENV = calleeENV;
}

INLINE_FUN
void
VirtualMachine::callFunction_D(bool isTailCall,
                               UInt32Value* &PC,
                               UInt32Value* &SP,
                               Cell* &ENV,
                               UInt32Value *entryPoint,
                               Cell* calleeENV,
                               UInt32Value argIndex,
                               UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value arity;
    UInt32Value* argDests;
    UInt32Value *funInfoAddress;
    Bitmap bitmap;

    PC = getFunInfoAndBitmap(SP,
                             entryPoint,
                             calleeENV,
                             &argIndex,
                             frameSize,
                             arity,
                             argDests,
                             funInfoAddress,
                             bitmap);

    if(isTailCall){
        ASSERT(NULL == returnAddress);
        SP = fillFrameForTailCall_D(funInfoAddress,
                                    frameSize,
                                    bitmap,
                                    argIndex,
                                    *argDests,
                                    SP);
    }
    else{
        ASSERT(returnAddress);
        SP = fillFrameForNonTailCall_D(funInfoAddress,
                                       frameSize,
                                       bitmap,
                                       argIndex,
                                       *argDests,
                                       returnAddress,
                                       SP);
    }

    /* set registers */
    ENV = calleeENV;
}

INLINE_FUN
void
VirtualMachine::callFunction_V(bool isTailCall,
                               UInt32Value* &PC,
                               UInt32Value* &SP,
                               Cell* &ENV,
                               UInt32Value *entryPoint,
                               Cell* calleeENV,
                               UInt32Value argIndex,
                               UInt32Value argSize,
                               UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value arity;
    UInt32Value* argDests;
    UInt32Value *funInfoAddress;
    Bitmap bitmap;

    PC = getFunInfoAndBitmap(SP,
                             entryPoint,
                             calleeENV,
                             &argIndex,
                             frameSize,
                             arity,
                             argDests,
                             funInfoAddress,
                             bitmap);

    if(isTailCall){
        ASSERT(NULL == returnAddress);
        if(1 == argSize){
            SP = fillFrameForTailCall_S(funInfoAddress,
                                        frameSize,
                                        bitmap,
                                        argIndex,
                                        *argDests,
                                        SP);
        }
        else{
            ASSERT(2 == argSize);
            SP = fillFrameForTailCall_D(funInfoAddress,
                                        frameSize,
                                        bitmap,
                                        argIndex,
                                        *argDests,
                                        SP);
        }
    }
    else{
        ASSERT(returnAddress);
        if(1 == argSize){
            SP = fillFrameForNonTailCall_S(funInfoAddress,
                                           frameSize,
                                           bitmap,
                                           argIndex,
                                           *argDests,
                                           returnAddress,
                                           SP);
        }
        else{
            ASSERT(2 == argSize);
            SP = fillFrameForNonTailCall_D(funInfoAddress,
                                           frameSize,
                                           bitmap,
                                           argIndex,
                                           *argDests,
                                           returnAddress,
                                           SP);
        }
    }

    /* set registers */
    ENV = calleeENV;
}

INLINE_FUN
void
VirtualMachine::callFunction_ML_S(bool isTailCall,
                                  UInt32Value* &PC,
                                  UInt32Value* &SP,
                                  Cell* &ENV,
                                  UInt32Value *entryPoint,
                                  Cell* calleeENV,
                                  UInt32Value* argIndexes,
                                  UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value arity;
    UInt32Value* argDests;
    UInt32Value *funInfoAddress;
    Bitmap bitmap;

    PC = getFunInfoAndBitmap(SP,
                             entryPoint,
                             calleeENV,
                             argIndexes,
                             frameSize,
                             arity,
                             argDests,
                             funInfoAddress,
                             bitmap);

    if(isTailCall){
        ASSERT(NULL == returnAddress);
        SP = fillFrameForTailCall_ML_S(funInfoAddress,
                                       frameSize,
                                       arity,
                                       bitmap,
                                       argIndexes,
                                       argDests,
                                       SP);
    }
    else{
        ASSERT(returnAddress);
        SP = fillFrameForNonTailCall_ML_S(funInfoAddress,
                                          frameSize,
                                          arity,
                                          bitmap,
                                          argIndexes,
                                          argDests,
                                          returnAddress,
                                          SP);
    }

    /* set registers */
    ENV = calleeENV;
}

INLINE_FUN
void
VirtualMachine::callFunction_ML_D(bool isTailCall,
                                  UInt32Value* &PC,
                                  UInt32Value* &SP,
                                  Cell* &ENV,
                                  UInt32Value *entryPoint,
                                  Cell* calleeENV,
                                  UInt32Value* argIndexes,
                                  UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value arity;
    UInt32Value* argDests;
    UInt32Value *funInfoAddress;
    Bitmap bitmap;

    PC = getFunInfoAndBitmap(SP,
                             entryPoint,
                             calleeENV,
                             argIndexes,
                             frameSize,
                             arity,
                             argDests,
                             funInfoAddress,
                             bitmap);

    if(isTailCall){
        ASSERT(NULL == returnAddress);
        SP = fillFrameForTailCall_ML_D(funInfoAddress,
                                       frameSize,
                                       arity,
                                       bitmap,
                                       argIndexes,
                                       argDests,
                                       SP);
    }
    else{
        ASSERT(returnAddress);
        SP = fillFrameForNonTailCall_ML_D(funInfoAddress,
                                          frameSize,
                                          arity,
                                          bitmap,
                                          argIndexes,
                                          argDests,
                                          returnAddress,
                                          SP);
    }

    /* set registers */
    ENV = calleeENV;
}

INLINE_FUN
void
VirtualMachine::callFunction_ML_V(bool isTailCall,
                                  UInt32Value* &PC,
                                  UInt32Value* &SP,
                                  Cell* &ENV,
                                  UInt32Value *entryPoint,
                                  Cell* calleeENV,
                                  UInt32Value* argIndexes,
                                  UInt32Value lastArgSize,
                                  UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value arity;
    UInt32Value* argDests;
    UInt32Value *funInfoAddress;
    Bitmap bitmap;

    PC = getFunInfoAndBitmap(SP,
                             entryPoint,
                             calleeENV,
                             argIndexes,
                             frameSize,
                             arity,
                             argDests,
                             funInfoAddress,
                             bitmap);

    if(isTailCall){
        ASSERT(NULL == returnAddress);
        if(1 == lastArgSize){
            SP = fillFrameForTailCall_ML_S(funInfoAddress,
                                           frameSize,
                                           arity,
                                           bitmap,
                                           argIndexes,
                                           argDests,
                                           SP);
        }
        else{
            ASSERT(2 == lastArgSize);
            SP = fillFrameForTailCall_ML_D(funInfoAddress,
                                           frameSize,
                                           arity,
                                           bitmap,
                                           argIndexes,
                                           argDests,
                                           SP);
        }
    }
    else{
        ASSERT(returnAddress);
        if(1 == lastArgSize){
            SP = fillFrameForNonTailCall_ML_S(funInfoAddress,
                                              frameSize,
                                              arity,
                                              bitmap,
                                              argIndexes,
                                              argDests,
                                              returnAddress,
                                              SP);
        }
        else{
            ASSERT(2 == lastArgSize);
            SP = fillFrameForNonTailCall_ML_D(funInfoAddress,
                                              frameSize,
                                              arity,
                                              bitmap,
                                              argIndexes,
                                              argDests,
                                              returnAddress,
                                              SP);
        }
    }

    /* set registers */
    ENV = calleeENV;
}

INLINE_FUN
void
VirtualMachine::callFunction_M(bool isTailCall,
                               UInt32Value* &PC,
                               UInt32Value* &SP,
                               Cell* &ENV,
                               UInt32Value *entryPoint,
                               Cell* calleeENV,
                               UInt32Value* argIndexes,
                               UInt32Value* argSizeIndexes,
                               UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value arity;
    UInt32Value* argDests;
    UInt32Value *funInfoAddress;
    Bitmap bitmap;

    PC = getFunInfoAndBitmap(SP,
                             entryPoint,
                             calleeENV,
                             argIndexes,
                             frameSize,
                             arity,
                             argDests,
                             funInfoAddress,
                             bitmap);

    if(isTailCall){
        ASSERT(NULL == returnAddress);
        SP = fillFrameForTailCall_M(funInfoAddress,
                                    frameSize,
                                    arity,
                                    bitmap,
                                    argIndexes,
                                    argSizeIndexes,
                                    argDests,
                                    SP);
    }
    else{
        ASSERT(returnAddress);
        SP = fillFrameForNonTailCall_M(funInfoAddress,
                                       frameSize,
                                       arity,
                                       bitmap,
                                       argIndexes,
                                       argSizeIndexes,
                                       argDests,
                                       returnAddress,
                                       SP);
    }

    /* set registers */
    ENV = calleeENV;
}

/**
 * Recursive function call can reuse the frame bitmap of the caller frame.
 */
INLINE_FUN
void
VirtualMachine::callRecursiveFunction_S(bool isTailCall,
                                        UInt32Value* &PC,
                                        UInt32Value* &SP,
                                        Cell* &ENV,
                                        UInt32Value *entryPoint,
                                        UInt32Value argIndex,
                                        UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value arity;
    UInt32Value* argDests;
    UInt32Value *funInfoAddress;

    UInt32Value bitmapvalsFreesCount;
    UInt32Value bitmapvalsFree;

    PC = getFunInfoForRecursiveCall(entryPoint,
                                    frameSize,
                                    arity,
                                    argDests,
                                    bitmapvalsFreesCount,
                                    bitmapvalsFree,
                                    funInfoAddress);

    Bitmap bitmap = 0;
    if(bitmapvalsFreesCount){
        bitmap = HEAP_GETFIELD(ENV, bitmapvalsFree).uint32;
    }

    if(isTailCall){
        ASSERT(NULL == returnAddress);
        SP = fillFrameForTailCall_S(funInfoAddress,
                                    frameSize,
                                    bitmap,
                                    argIndex,
                                    *argDests,
                                    SP);
    }
    else{
        ASSERT(returnAddress);
        SP = fillFrameForNonTailCall_S(funInfoAddress,
                                       frameSize,
                                       bitmap,
                                       argIndex,
                                       *argDests,
                                       returnAddress,
                                       SP);
    }
}

INLINE_FUN
void
VirtualMachine::callRecursiveFunction_D(bool isTailCall,
                                        UInt32Value* &PC,
                                        UInt32Value* &SP,
                                        Cell* &ENV,
                                        UInt32Value *entryPoint,
                                        UInt32Value argIndex,
                                        UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value arity;
    UInt32Value* argDests;
    UInt32Value *funInfoAddress;

    UInt32Value bitmapvalsFreesCount;
    UInt32Value bitmapvalsFree;

    PC = getFunInfoForRecursiveCall(entryPoint,
                                    frameSize,
                                    arity,
                                    argDests,
                                    bitmapvalsFreesCount,
                                    bitmapvalsFree,
                                    funInfoAddress);

    Bitmap bitmap = 0;
    if(bitmapvalsFreesCount){
        bitmap = HEAP_GETFIELD(ENV, bitmapvalsFree).uint32;
    }

    if(isTailCall){
        ASSERT(NULL == returnAddress);
        SP = fillFrameForTailCall_D(funInfoAddress,
                                    frameSize,
                                    bitmap,
                                    argIndex,
                                    *argDests,
                                    SP);
    }
    else{
        ASSERT(returnAddress);
        SP = fillFrameForNonTailCall_D(funInfoAddress,
                                       frameSize,
                                       bitmap,
                                       argIndex,
                                       *argDests,
                                       returnAddress,
                                       SP);
    }
}

INLINE_FUN
void
VirtualMachine::callRecursiveFunction_V(bool isTailCall,
                                        UInt32Value* &PC,
                                        UInt32Value* &SP,
                                        Cell* &ENV,
                                        UInt32Value *entryPoint,
                                        UInt32Value argIndex,
                                        UInt32Value lastArgSize,
                                        UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value arity;
    UInt32Value* argDests;
    UInt32Value *funInfoAddress;

    UInt32Value bitmapvalsFreesCount;
    UInt32Value bitmapvalsFree;

    PC = getFunInfoForRecursiveCall(entryPoint,
                                    frameSize,
                                    arity,
                                    argDests,
                                    bitmapvalsFreesCount,
                                    bitmapvalsFree,
                                    funInfoAddress);

    Bitmap bitmap = 0;
    if(bitmapvalsFreesCount){
        bitmap = HEAP_GETFIELD(ENV, bitmapvalsFree).uint32;
    }

    if(isTailCall){
        ASSERT(NULL == returnAddress);
        if(1 == lastArgSize){
            SP = fillFrameForTailCall_S(funInfoAddress,
                                        frameSize,
                                        bitmap,
                                        argIndex,
                                        *argDests,
                                        SP);
        }
        else{
            ASSERT(2 == lastArgSize);
            SP = fillFrameForTailCall_D(funInfoAddress,
                                        frameSize,
                                        bitmap,
                                        argIndex,
                                        *argDests,
                                        SP);
        }
    }
    else{
        ASSERT(returnAddress);
        if(1 == lastArgSize){
            SP = fillFrameForNonTailCall_S(funInfoAddress,
                                           frameSize,
                                           bitmap,
                                           argIndex,
                                           *argDests,
                                           returnAddress,
                                           SP);
        }
        else{
            ASSERT(2 == lastArgSize);
            SP = fillFrameForNonTailCall_D(funInfoAddress,
                                           frameSize,
                                           bitmap,
                                           argIndex,
                                           *argDests,
                                           returnAddress,
                                           SP);
        }
    }
}

INLINE_FUN
void
VirtualMachine::callRecursiveFunction_M(bool isTailCall,
                                        UInt32Value* &PC,
                                        UInt32Value* &SP,
                                        Cell* &ENV,
                                        UInt32Value *entryPoint,
                                        UInt32Value *argIndexes,
                                        UInt32Value *argSizeIndexes,
                                        UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value arity;
    UInt32Value* argDests;
    UInt32Value *funInfoAddress;

    UInt32Value bitmapvalsFreesCount;
    UInt32Value bitmapvalsFree;

    PC = getFunInfoForRecursiveCall(entryPoint,
                                    frameSize,
                                    arity,
                                    argDests,
                                    bitmapvalsFreesCount,
                                    bitmapvalsFree,
                                    funInfoAddress);

    Bitmap bitmap = 0;
    if(bitmapvalsFreesCount){
        bitmap = HEAP_GETFIELD(ENV, bitmapvalsFree).uint32;
    }

    if(isTailCall){
        ASSERT(NULL == returnAddress);
        SP = fillFrameForTailCall_M(funInfoAddress,
                                    frameSize,
                                    arity,
                                    bitmap,
                                    argIndexes,
                                    argSizeIndexes,
                                    argDests,
                                    SP);
    }
    else{
        ASSERT(returnAddress);
        SP = fillFrameForNonTailCall_M(funInfoAddress,
                                       frameSize,
                                       arity,
                                       bitmap,
                                       argIndexes,
                                       argSizeIndexes,
                                       argDests,
                                       returnAddress,
                                       SP);
    }
}

/**
 * Self recursive call can optimize frame allocation.
 * Tail call can reuse the caller frame.
 * Non-tail call has only to copy the caller frame.
 */
INLINE_FUN
void
VirtualMachine::callSelfRecursiveFunction_S(bool isTailCall,
                                            UInt32Value* &PC,
                                            UInt32Value* &SP,
                                            UInt32Value *entryPoint,
                                            UInt32Value argIndex,
                                            UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value *argDests;
    UInt32Value *funInfoAddress;

    PC = getFunInfoForSelfRecursiveCall(entryPoint,
                                        frameSize,
                                        argDests,
                                        funInfoAddress);

    UInt32Value* calleeSP;
    if(isTailCall){
        ASSERT(NULL == returnAddress);
        calleeSP = SP;
    }
    else{
        ASSERT(returnAddress);
        calleeSP = FrameStack::duplicateFrame(SP, frameSize, returnAddress);
    }

    ASSERT_SAME_TYPE_SLOTS(calleeSP, *argDests, SP, argIndex);
    FRAME_ENTRY(calleeSP, *argDests) = FRAME_ENTRY(SP, argIndex);

    SP = calleeSP;
}

INLINE_FUN
void
VirtualMachine::callSelfRecursiveFunction_D(bool isTailCall,
                                            UInt32Value* &PC,
                                            UInt32Value* &SP,
                                            UInt32Value *entryPoint,
                                            UInt32Value argIndex,
                                            UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value *argDests;
    UInt32Value *funInfoAddress;

    PC = getFunInfoForSelfRecursiveCall(entryPoint,
                                        frameSize,
                                        argDests,
                                        funInfoAddress);

    UInt32Value* calleeSP;
    if(isTailCall){
        ASSERT(NULL == returnAddress);
        calleeSP = SP;
        // assert that source and dest do not overlap partially.
        ASSERT(!((*argDests + 1 == argIndex) || (argIndex + 1 == *argDests)));
    }
    else{
        ASSERT(returnAddress);
        calleeSP = FrameStack::duplicateFrame(SP, frameSize, returnAddress);
    }

    ASSERT_SAME_TYPE_SLOTS(calleeSP, *argDests, SP, argIndex);
    ASSERT_SAME_TYPE_SLOTS(calleeSP, *argDests + 1, SP, argIndex + 1);
    ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(calleeSP, *argDests));
    ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex));

    *(Real64Value*)FRAME_ENTRY_ADDRESS(calleeSP, *argDests) = 
    *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, argIndex);

    SP = calleeSP;
}

INLINE_FUN
void
VirtualMachine::callSelfRecursiveFunction_V(bool isTailCall,
                                            UInt32Value* &PC,
                                            UInt32Value* &SP,
                                            UInt32Value *entryPoint,
                                            UInt32Value argIndex,
                                            UInt32Value lastArgSize,
                                            UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value *argDests;
    UInt32Value *funInfoAddress;

    PC = getFunInfoForSelfRecursiveCall(entryPoint,
                                        frameSize,
                                        argDests,
                                        funInfoAddress);

    UInt32Value* calleeSP;
    if(isTailCall){
        ASSERT(NULL == returnAddress);
        calleeSP = SP;
        // assert that source and dest do not overlap partially.
        ASSERT((1 == lastArgSize)
               || (!((*argDests + 1 == argIndex)
                     || (argIndex + 1 == *argDests))));
    }
    else{
        ASSERT(returnAddress);
        calleeSP = FrameStack::duplicateFrame(SP, frameSize, returnAddress);
    }

    switch(lastArgSize){
      case 1:
        ASSERT_SAME_TYPE_SLOTS(calleeSP, *argDests, SP, argIndex);
        FRAME_ENTRY(calleeSP, *argDests) = FRAME_ENTRY(SP, argIndex);
        break;
      case 2:
        ASSERT_SAME_TYPE_SLOTS(calleeSP, *argDests, SP, argIndex);
        ASSERT_SAME_TYPE_SLOTS(calleeSP, *argDests + 1, SP, argIndex + 1);
        ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(calleeSP, *argDests));
        ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex));

        *(Real64Value*)FRAME_ENTRY_ADDRESS(calleeSP, *argDests) = 
        *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, argIndex);

        break;
      default:
        DBGWRAP(LOG.error
                  ("callSelfRecursiveFunction_V::IllegalArgumentException");)
        throw IllegalArgumentException();// IllegalArgument???
    }

    SP = calleeSP;
}

INLINE_FUN
void
VirtualMachine::callSelfRecursiveFunction_M(bool isTailCall,
                                            UInt32Value* &PC,
                                            UInt32Value* &SP,
                                            UInt32Value *entryPoint,
                                            UInt32Value argsCount,
                                            UInt32Value *argIndexes,
                                            UInt32Value *argSizeIndexes,
                                            UInt32Value* returnAddress)
{
    UInt32Value frameSize;
    UInt32Value *argDests;
    UInt32Value *funInfoAddress;

    PC = getFunInfoForSelfRecursiveCall(entryPoint,
                                        frameSize,
                                        argDests,
                                        funInfoAddress);

    if(isTailCall){
        ASSERT(NULL == returnAddress);

        Cell savedArgs[argsCount * 2];
        UInt32Value argSizes[argsCount];

        for(int index = 0; index < argsCount; index += 1){
            UInt32Value argSize = FRAME_ENTRY(SP, *argSizeIndexes).uint32;
            for(int i = 0; i < argSize; i += 1){
                savedArgs[index * 2 + i] = FRAME_ENTRY(SP, (*argIndexes) + i);
            }
            argSizes[index] = argSize;
            argIndexes += 1;
            argSizeIndexes += 1;
        }

        for(int index = 0; index < argsCount; index += 1){
            UInt32Value argSize = argSizes[index];
            for(int i = 0; i < argSize; i += 1){
                FRAME_ENTRY(SP, (*argDests) + i) = savedArgs[index * 2 + i];
            }
            argDests += 1;
        }
    }
    else{
        UInt32Value* calleeSP;
        ASSERT(returnAddress);
        calleeSP = FrameStack::duplicateFrame(SP, frameSize, returnAddress);
        
        for(int index = 0; index < argsCount; index += 1){
            UInt32Value argSize = FRAME_ENTRY(SP, *argSizeIndexes).uint32;
            for(int i = 0; i < argSize; i += 1){
                ASSERT_SAME_TYPE_SLOTS
                (calleeSP, (*argDests) + i, SP, (*argIndexes) + i);
                FRAME_ENTRY(calleeSP, (*argDests) + i) =
                FRAME_ENTRY(SP, (*argIndexes) + i);
            }
            argIndexes += 1;
            argSizeIndexes += 1;
            argDests += 1;
        }

        SP = calleeSP;
    }
}

INLINE_FUN
void 
VirtualMachine::raiseException(UInt32Value* &SP,
                               UInt32Value* &PC,
                               Cell* &ENV,
                               Cell exceptionValue)
{
    DBGWRAP(LOG.debug("raiseException begin");)
    ASSERT(Heap::isValidBlockPointer(exceptionValue.blockRef));

    // find the handler to invoke
    UInt32Value* restoredSP;
    UInt32Value exceptionDestinationIndex;
    UInt32Value* handlerAddress;
    HandlerStack::pop(restoredSP, exceptionDestinationIndex, handlerAddress);
    FrameStack::popFramesUntil(SP, restoredSP);
    FrameStack::loadENV(SP, ENV);

    ASSERT(FrameStack::isPointerSlot(SP, exceptionDestinationIndex));
    FRAME_ENTRY(SP, exceptionDestinationIndex) = exceptionValue;
    PC = handlerAddress;
    DBGWRAP(LOG.debug("raiseException end");)
}

INLINE_FUN
void
VirtualMachine::LoadConstString(UInt32Value* ConstStringAddress,
                                UInt32Value* length,
                                UInt32Value** stringBuffer)
{
    ConstStringAddress += 1;
    *length = getWordAndInc(ConstStringAddress);
    *stringBuffer = ConstStringAddress;
}

INLINE_FUN
Real64Value
VirtualMachine::LoadConstReal64(UInt32Value* ConstRealAddress)
{
    Real64Value buffer;
    *(UInt32Value*)&buffer = *ConstRealAddress;
    *(((UInt32Value*)&buffer) + 1) = *(ConstRealAddress + 1);
    return buffer;
}

INLINE_FUN
Cell*
VirtualMachine::getNestedBlock(Cell* block, UInt32Value nestLevel)
{
    Cell* current = block;
    for(int index = nestLevel; 0 < index; index -= 1){
        ASSERT(Heap::isPointerField(current, INDEX_OF_NEST_POINTER));
        current = HEAP_GETFIELD(current, INDEX_OF_NEST_POINTER).blockRef;
        ASSERT(Heap::isValidBlockPointer(current));
    }
    return current;
}

void
VirtualMachine::execute(Executable* executable)
    throw(UserException,
          IMLRuntimeException,
          SystemError)
{
    // initialize machine registers
    PC_ = (UInt32Value*)(executable->code_);
    SP_ = FrameStack::getBottom();
    ENV_ = 0;

    HandlerStack::clear();

    interrupted_ = false;
    setSignalHandler();

    isPrimitiveExceptionRaised_ = false;
    temporaryPointers_.clear();

    // NOTE : the state of this machine might be modified by monitors.
    INVOKE_ON_MONITORS(beforeExecution(executable, PC_, ENV_, SP_));

    // Call the main function. This sets up initial frame.
    UInt32Value* entryPoint = (UInt32Value*)(executable->code_);
    // the main function never refers to arguments
    UInt32Value argIndexes[] = {0, 0};
    Cell* emptyENV = 0;
    callFunction_ML_S(false, // non tail-call
		      PC_,
		      SP_,
		      ENV_,
		      entryPoint,
		      emptyENV,
		      argIndexes,
		      RETURN_ADDRESS_OF_INITIAL_FRAME);

    executeLoop();
}

UInt32Value VirtualMachine::executeFunction(UncaughtExceptionHandleMode mode,
                                            UInt32Value *entryPoint,
                                            Cell *env,
                                            Cell *returnValue,
                                            bool returnBoxed,
                                            FFI::Arguments &args)
    throw(UserException,
          IMLRuntimeException,
          SystemError)
{
    /* Calculate layout of caller stack frame */
    UInt32Value arity = args.arity();
    // pointers
    UInt32Value exnIndex       = FRAME_FIRST_POINTER_SLOT;
    UInt32Value boxedReturn    = exnIndex       + 1;
    UInt32Value boxedArgs      = boxedReturn    + 1;
    // atoms
    UInt32Value argIndexes     = boxedArgs      + arity;
    UInt32Value argSizeIndexes = argIndexes     + arity;
    UInt32Value argSizes       = argSizeIndexes + arity;
    UInt32Value unboxedReturn  = argSizes       + arity + argSizes % 2;
    UInt32Value unboxedArgs    = unboxedReturn  + 2;
    UInt32Value frameSize      = unboxedArgs    + 2 * arity - 1;
    frameSize += frameSize % 2;

    UInt32Value returnEntry = returnBoxed ? boxedReturn : unboxedReturn;

    UInt32Value pointersCount  = argIndexes - exnIndex;
    UInt32Value atomsCount     = frameSize + 1 - argIndexes;

    /* Body of caller */
    UInt32Value insn[] = {
        5,   /* word length of instructions, excluding this header. */
        /* 1: */ LoadInt,                  // dummy
        /* 2: */ 0,                        // dummy
        /* 3: */ 0,                        // returnEntry
        /* 4: */ PopHandler,
        /* 5: */ Exit,
    };
    Executable executable(insn[0], insn);

    UInt32Value *returnAddress = &insn[3];
    UInt32Value *dummyStartAddress = &insn[1];
    UInt32Value *handlerAddress = &insn[5];
    *returnAddress = returnEntry;

    /* FunInfo of caller */
    UInt32Value funinfo[] = {
        (UInt32Value)&executable,        // executable (dummy)
        frameSize,                       // frame size
        (UInt32Value)&dummyStartAddress, // startAddress (dummy)
        0,                               // arity
        0,                               // bitmapvalsFreesCount
        0,                               // bitmapvalsArgsCount
        pointersCount,                   // pointersCount
        atomsCount,                      // atomsCount
        0,                               // recordGroupsCount
    };

    /* Allocate frame stack of caller */
    SP_ = FrameStack::allocateFrame(SP_,
                                    frameSize,   // frame size
                                    0x0,         // bitmap
				    funinfo,
				    PC_);

    /* Get informations of callee */
    UInt32Value closFrameSize;
    UInt32Value closArity, *closArgDests;
    UInt32Value closFreesCount, *closFrees;
    UInt32Value closArgsCount, *closArgs;
    UInt32Value *closFunInfoAddr;

    getFunInfo(entryPoint, closFrameSize, closArity, closArgDests,
               closFreesCount, closFrees, closArgsCount, closArgs,
               closFunInfoAddr);

    ASSERT(args.arity() == closArity);

    /* Setup arguments */
    for (UInt32Value i = 0; i < closArity; ++i, ++args) {
        Cell *value = args.value();
        UInt32Value size = args.size();
        bool boxed = args.boxed();
        UInt32Value &index = boxed ? boxedArgs : unboxedArgs;

        FRAME_ENTRY(SP_, argSizes + i).uint32 = size;
        FRAME_ENTRY(SP_, argSizeIndexes + i).uint32 = argSizes + i;

        FRAME_ENTRY(SP_, argIndexes + i).uint32 = index;
        FRAME_ENTRY(SP_, index++) = value[0];
        if (size > 1) FRAME_ENTRY(SP_, index++) = value[1];
    }

    /* PushHandler */
    HandlerStack::push(SP_, exnIndex, handlerAddress);

    /* Apply_M */
    FrameStack::storeENV(SP_, ENV_);
    callFunction_M(false,                      /* non tail call */
                   PC_,                        /* PC */
                   SP_,                        /* SP */
                   ENV_,                       /* ENV */
                   entryPoint,                 /* entryPoint */
                   env,                        /* calleeENV */
                   &FRAME_ENTRY(SP_, argIndexes).uint32,    /* argIndexes */
                   &FRAME_ENTRY(SP_, argSizeIndexes).uint32,/* argSizeIndexes */
                   returnAddress);             /* returnAddress */
    executeLoop();
    
    /* check whether exception was raised */
    if (FRAME_ENTRY(SP_, FRAME_FIRST_POINTER_SLOT + 0).blockRef != 0) {
        /* FIXME: an exception was raised */
        switch(mode){
          case Ignore:
            break;
          case Longjump:
            break;
        }
    }

    returnValue[0] = FRAME_ENTRY(SP_, returnEntry);
    returnValue[1] = FRAME_ENTRY(SP_, returnEntry + 1);

    FrameStack::popFrameAndReturn(SP_, PC_);
}

class SinglePointerArgument
    : public FFI::Arguments
{
  protected:
    void next() {}
    
  public:
    SinglePointerArgument(Cell *p) : FFI::Arguments(p) {}
    UInt32Value arity() { return 1; }
    UInt32Value size() { return 1; }
    bool boxed() { return true; }
};

/*
 * 'PA' means 
 *   P - the 1st argument is Pointer type.
 *   A - the return type is Atom type.
 */
UInt32Value VirtualMachine::executeClosure_PA(UncaughtExceptionHandleMode mode,
                                              Cell closure,
                                              Cell *arg)
    throw(UserException,
          IMLRuntimeException,
          SystemError)
{
    Cell ret[2];
    // Note : initialize args with a pointer to the argument.
    SinglePointerArgument args((Cell*)&arg);

    ASSERT(Heap::isValidBlockPointer(closure.blockRef));
    ASSERT(CLOSURE_FIELDS_COUNT == Heap::getPayloadSize(closure.blockRef));
    ASSERT(CLOSURE_BITMAP == Heap::getBitmap(closure.blockRef));
    UInt32Value *entryPoint = 
        (UInt32Value*)
        (HEAP_GETFIELD(closure.blockRef, CLOSURE_ENTRYPOINT_INDEX).uint32);
    Cell *env =
    (Cell*)(HEAP_GETFIELD(closure.blockRef, CLOSURE_ENV_INDEX).uint32);

    // false indicates return type is Atom.
    executeFunction(mode, entryPoint, env, ret, false, args);
    return ret[0].uint32;
}

void
VirtualMachine::executeLoop()
    throw(UserException,
          IMLRuntimeException,
          SystemError)
{
    register UInt32Value* PC;
    register UInt32Value* SP;
    Cell* ENV;
    UInt32Value* previousPC;

    PC = PC_;
    SP = SP_;
    ENV = ENV_;

    try{
	if(setjmp(onSIGFPE_jmp_buf))
	{
	    /* NOTE: All values of auto variables are undcidable here,
	     *       even if one of them doninates a hardware register. */
	    Cell exception =
	    PrimitiveSupport::constructExnSysErr(1, "arithmetic exception");
	    raiseException(SP, PC, ENV, exception);
	    /* fall through */
	}
	    
	// execution loop
        while(true)
        {
            if(interrupted_){ throw InterruptedException(); }

            INVOKE_ON_MONITORS(beforeInstruction(PC, ENV, SP));
#ifdef IML_ENABLE_EXECUTION_MONITORING
            previousPC = PC;
#endif
            instruction opcode = static_cast<instruction>(*PC);
            PC += 1;

            switch(opcode)
            {
              case LoadInt:
              case LoadWord:
              case LoadChar:
              case LoadFloat:
                {
                    UInt32Value constant = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, destination));
                    FRAME_ENTRY(SP, destination).uint32 = constant;
                    break;
                }
              case LoadString:
                {
                    UInt32Value *ConstStringAddress =
                    (UInt32Value*)getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    /* 
                     * string constant is stored in the operand of ConstString
                     * instruction.
                     * The string is stored in the first W fields.
                     * W is the number of words occupied by the string.
                     * In the last field, the length(bytes) of the string is
                     * stored
                     */
                    UInt32Value stringLength;
                    UInt32Value* stringBuffer;
                    LoadConstString
                    (ConstStringAddress, &stringLength, &stringBuffer);

                    Cell* block;
                    ALLOCATE_STRINGBLOCK(block, stringBuffer, stringLength);

                    ASSERT(FrameStack::isPointerSlot(SP, destination));
                    FRAME_ENTRY(SP, destination).blockRef = block;
                    break;
                }
              case LoadReal:
                {
                    UInt32Value *floatBuffer = PC;
                    PC += (sizeof(Real64Value) / sizeof(Cell));
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, destination));
                    ASSERT(!FrameStack::isPointerSlot(SP, destination + 1));
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, destination));

                    /* Because instruction operand is not aligned, 
                     * we must copy word by word. */
                    FRAME_ENTRY(SP, destination).uint32 = floatBuffer[0];
                    FRAME_ENTRY(SP, destination + 1).uint32 = floatBuffer[1];
                    break;
                }
              case LoadBoxedReal:
                {
                    UInt32Value *floatBuffer = PC;
                    PC += (sizeof(Real64Value) / sizeof(Cell));
                    UInt32Value destination = getWordAndInc(PC);

                    Cell* block;
                    ALLOCATE_ATOMBLOCK
                    (block, sizeof(Real64Value) / sizeof(Cell));
                    COPY_MEMORY(block, floatBuffer, sizeof(Real64Value));

                    ASSERT(FrameStack::isPointerSlot(SP, destination));
                    FRAME_ENTRY(SP, destination).blockRef = block;
                    break;
                }
              case LoadEmptyBlock:
                {
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(FrameStack::isPointerSlot(SP, destination));
                    Cell* block;
                    ALLOCATE_EMPTYBLOC(block);
                    FRAME_ENTRY(SP, destination).blockRef = block;
                    break;
                }
              case Access_S:
                {
                    UInt32Value variableIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT_SAME_TYPE_SLOTS(SP, destination, SP, variableIndex);
                    ASSERT_VALID_FRAME_VAR(SP, variableIndex);
                    FRAME_ENTRY(SP, destination) =
                    FRAME_ENTRY(SP, variableIndex);
                    break;
                }
              case Access_D:
                {
                    UInt32Value variableIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT_SAME_TYPE_SLOTS(SP, destination, SP, variableIndex);
                    ASSERT_SAME_TYPE_SLOTS
                        (SP, destination + 1, SP, variableIndex + 1);

                    ASSERT_VALID_FRAME_VAR(SP, variableIndex);
                    ASSERT_VALID_FRAME_VAR(SP, variableIndex + 1);
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, destination));
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, variableIndex));

                    *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, destination) =
                    *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, variableIndex);

                    break;
                }
              case Access_V:
                {
                    UInt32Value variableIndex = getWordAndInc(PC);
                    UInt32Value variableSizeIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);
                    
                    ASSERT(!FrameStack::isPointerSlot(SP, variableSizeIndex));
                    UInt32Value variableSize =
                    FRAME_ENTRY(SP, variableSizeIndex).uint32;

                    for(int index = 0; index < variableSize; index += 1){
                        ASSERT_SAME_TYPE_SLOTS(SP, destination + index,
                                               SP, variableIndex + index);
                        FRAME_ENTRY(SP, destination + index) =
                        FRAME_ENTRY(SP, variableIndex + index);
                    }
                    break;
                }
              case AccessEnv_S:
                {
                    UInt32Value variableIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, ENV, variableIndex);

                    FRAME_ENTRY(SP, destination) =
                    HEAP_GETFIELD(ENV, variableIndex);
                    break;
                }
              case AccessEnv_D:
                {
                    UInt32Value variableIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, ENV, variableIndex);
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination + 1, ENV, variableIndex + 1);
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, destination));

                    FRAME_ENTRY(SP, destination) =
                        HEAP_GETFIELD(ENV, variableIndex);
                    FRAME_ENTRY(SP, destination + 1) =
                        HEAP_GETFIELD(ENV, variableIndex + 1);

                    break;
                }
              case AccessEnv_V:
                {
                    UInt32Value variableIndex = getWordAndInc(PC);
                    UInt32Value variableSizeIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, variableSizeIndex));
                    UInt32Value variableSize =
                        FRAME_ENTRY(SP, variableSizeIndex).uint32;

                    for(int index = 0; index < variableSize; index += 1){
                        ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination + index, ENV, variableIndex + index);
                        FRAME_ENTRY(SP, destination + index) =
                        HEAP_GETFIELD(ENV, variableIndex + index);
                    }
                    break;
                }
              case AccessEnvIndirect_S:
                {
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!Heap::isPointerField(ENV, indirectOffset));
                    UInt32Value variableIndex = 
                        HEAP_GETFIELD(ENV, indirectOffset).uint32;

                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, ENV, variableIndex);

                    FRAME_ENTRY(SP, destination) =
                        HEAP_GETFIELD(ENV, variableIndex);
                    break;
                }
              case AccessEnvIndirect_D:
                {
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!Heap::isPointerField(ENV, indirectOffset));
                    UInt32Value variableIndex = 
                        HEAP_GETFIELD(ENV, indirectOffset).uint32;

                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, ENV, variableIndex);
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination + 1, ENV, variableIndex + 1);
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, destination));

                    FRAME_ENTRY(SP, destination) =
                        HEAP_GETFIELD(ENV, variableIndex);
                    FRAME_ENTRY(SP, destination + 1) =
                        HEAP_GETFIELD(ENV, variableIndex + 1);

                    break;
                }
              case AccessEnvIndirect_V:
                {
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value variableSizeIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, variableSizeIndex));
                    UInt32Value variableSize =
                        FRAME_ENTRY(SP, variableSizeIndex).uint32;

                    ASSERT(!Heap::isPointerField(ENV, indirectOffset));
                    UInt32Value variableIndex = 
                        HEAP_GETFIELD(ENV, indirectOffset).uint32;
                    for(int index = 0; index < variableSize; index += 1){
                        ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination + index, ENV, variableIndex + index);
                        FRAME_ENTRY(SP, destination + index) =
                            HEAP_GETFIELD(ENV, variableIndex + index);
                    }
                    break;
                }
              case AccessNestedEnv_S:
                {
                    UInt32Value nestLevel = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    Cell* block = getNestedBlock(ENV, nestLevel);

                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, block, variableIndex);

                    FRAME_ENTRY(SP, destination) =
                        HEAP_GETFIELD(block, variableIndex);
                    break;
                }
              case AccessNestedEnv_D:
                {
                    UInt32Value nestLevel = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    Cell* block = getNestedBlock(ENV, nestLevel);

                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, block, variableIndex);
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination + 1, block, variableIndex + 1);
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, destination));

                    FRAME_ENTRY(SP, destination) =
                        HEAP_GETFIELD(block, variableIndex);
                    FRAME_ENTRY(SP, destination + 1) =
                        HEAP_GETFIELD(block, variableIndex + 1);

                    break;
                }
              case AccessNestedEnv_V:
                {
                    UInt32Value nestLevel = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);
                    UInt32Value variableSizeIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    Cell* block = getNestedBlock(ENV, nestLevel);
                    ASSERT(!FrameStack::isPointerSlot(SP, variableSizeIndex));
                    UInt32Value variableSize =
                        FRAME_ENTRY(SP, variableSizeIndex).uint32;

                    for(int index = 0; index < variableSize; index += 1){
                        ASSERT_SAME_TYPE_SLOT_FIELD
                            (SP, destination + index,
                             block, variableIndex + index);

                        FRAME_ENTRY(SP, destination + index) =
                            HEAP_GETFIELD(block, variableIndex + index);
                    }
                    break;
                }
              case AccessNestedEnvIndirect_S:
                {
                    UInt32Value nestLevel = getWordAndInc(PC);
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    Cell* block = getNestedBlock(ENV, nestLevel);
                    ASSERT(!Heap::isPointerField(block, indirectOffset));
                    UInt32Value variableIndex = 
                        HEAP_GETFIELD(block, indirectOffset).uint32;

                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, block, variableIndex);

                    FRAME_ENTRY(SP, destination) =
                        HEAP_GETFIELD(block, variableIndex);
                    break;
                }
              case AccessNestedEnvIndirect_D:
                {
                    UInt32Value nestLevel = getWordAndInc(PC);
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    Cell* block = getNestedBlock(ENV, nestLevel);
                    ASSERT(!Heap::isPointerField(block, indirectOffset));
                    UInt32Value variableIndex = 
                        HEAP_GETFIELD(block, indirectOffset).uint32;

                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, block, variableIndex);
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination + 1, block, variableIndex + 1);
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, destination));

                    FRAME_ENTRY(SP, destination) =
                        HEAP_GETFIELD(block, variableIndex);
                    FRAME_ENTRY(SP, destination + 1) =
                        HEAP_GETFIELD(block, variableIndex + 1);

                    break;
                }
              case AccessNestedEnvIndirect_V:
                {
                    UInt32Value nestLevel = getWordAndInc(PC);
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value variableSizeIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    Cell* block = getNestedBlock(ENV, nestLevel);
                    ASSERT(!Heap::isPointerField(block, indirectOffset));
                    UInt32Value variableIndex = 
                        HEAP_GETFIELD(block, indirectOffset).uint32;

                    ASSERT(!FrameStack::isPointerSlot(SP, variableSizeIndex));
                    UInt32Value variableSize =
                        FRAME_ENTRY(SP, variableSizeIndex).uint32;

                    for(int index = 0; index < variableSize; index += 1){
                        ASSERT_SAME_TYPE_SLOT_FIELD
                            (SP, destination + index,
                             block, variableIndex + index);

                        FRAME_ENTRY(SP, destination + index) =
                            HEAP_GETFIELD(block, variableIndex + index);
                    }
                    break;
                }
              case GetField_S:
                {
                    UInt32Value fieldIndex = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT(fieldIndex < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, block, fieldIndex);

                    FRAME_ENTRY(SP, destination) =
                        HEAP_GETFIELD(block, fieldIndex);
                    break;
                }
              case GetField_D:
                {
                    UInt32Value fieldIndex = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT(fieldIndex + 1 < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, block, fieldIndex);
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination + 1, block, fieldIndex + 1);
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, destination));
                    ASSERT_REAL64_ALIGNED
                        (&HEAP_GETFIELD(block, fieldIndex));

                    *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, destination) =
                        *(Real64Value*)&HEAP_GETFIELD(block, fieldIndex);

                    break;
                }
              case GetField_V:
                {
                    UInt32Value fieldIndex = getWordAndInc(PC);
                    UInt32Value fieldSizeIndex = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, fieldSizeIndex));
                    UInt32Value fieldSize =
                        FRAME_ENTRY(SP, fieldSizeIndex).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT((fieldIndex + fieldSize - 1)
                           < Heap::getPayloadSize(block));
                    for(int index = 0; index < fieldSize; index += 1){
                        ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination + index, block, fieldIndex + index);

                        FRAME_ENTRY(SP, destination + index) =
                            HEAP_GETFIELD(block, fieldIndex + index);
                    }
                    break;
                }
              case GetFieldIndirect_S:
                {
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, indirectOffset));
                    UInt32Value fieldIndex =
                        FRAME_ENTRY(SP, indirectOffset).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT(fieldIndex < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, block, fieldIndex);

                    FRAME_ENTRY(SP, destination) =
                        HEAP_GETFIELD(block, fieldIndex);
                    break;
                }
              case GetFieldIndirect_D:
                {
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, indirectOffset));
                    UInt32Value fieldIndex =
                        FRAME_ENTRY(SP, indirectOffset).uint32;

                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT(fieldIndex + 1 < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, block, fieldIndex);
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination + 1, block, fieldIndex + 1);
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, destination));
                    ASSERT_REAL64_ALIGNED(&HEAP_GETFIELD(block, fieldIndex));

                    *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, destination) =
                        *(Real64Value*)&HEAP_GETFIELD(block, fieldIndex);

                    break;
                }
              case GetFieldIndirect_V:
                {
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value fieldSizeIndex = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, fieldSizeIndex));
                    UInt32Value fieldSize =
                        FRAME_ENTRY(SP, fieldSizeIndex).uint32;

                    ASSERT(!FrameStack::isPointerSlot(SP, indirectOffset));
                    UInt32Value fieldIndex =
                        FRAME_ENTRY(SP, indirectOffset).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT((fieldIndex + fieldSize - 1)
                           < Heap::getPayloadSize(block));

                    for(int index = 0; index < fieldSize; index += 1){
                        ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination + index, block, fieldIndex + index);

                        FRAME_ENTRY(SP, destination + index) =
                            HEAP_GETFIELD(block, fieldIndex + index);
                    }
                    break;
                }
              case GetNestedFieldIndirect_S:
                {
                    UInt32Value nestLevelOffset = getWordAndInc(PC);
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, nestLevelOffset));
                    UInt32Value nestLevel =
                        FRAME_ENTRY(SP, nestLevelOffset).uint32;

                    ASSERT(!FrameStack::isPointerSlot(SP, indirectOffset));
                    UInt32Value fieldIndex =
                        FRAME_ENTRY(SP, indirectOffset).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* root = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(root));

                    Cell* block = getNestedBlock(root, nestLevel);

                    ASSERT(fieldIndex < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, block, fieldIndex);
                    FRAME_ENTRY(SP, destination) =
                        HEAP_GETFIELD(block, fieldIndex);
                    break;
                }
              case GetNestedFieldIndirect_D:
                {
                    UInt32Value nestLevelOffset = getWordAndInc(PC);
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, nestLevelOffset));
                    UInt32Value nestLevel =
                        FRAME_ENTRY(SP, nestLevelOffset).uint32;

                    ASSERT(!FrameStack::isPointerSlot(SP, indirectOffset));
                    UInt32Value fieldIndex =
                        FRAME_ENTRY(SP, indirectOffset).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* root = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(root));

                    Cell* block = getNestedBlock(root, nestLevel);

                    ASSERT(fieldIndex + 1 < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, block, fieldIndex);
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination + 1, block, fieldIndex + 1);
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, destination));
                    ASSERT_REAL64_ALIGNED(&HEAP_GETFIELD(block, fieldIndex));

                    *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, destination) =
                        *(Real64Value*)&HEAP_GETFIELD(block, fieldIndex);

                    break;
                }
              case GetNestedFieldIndirect_V:
                {
                    UInt32Value nestLevelOffset = getWordAndInc(PC);
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value fieldSizeIndex = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, nestLevelOffset));
                    UInt32Value nestLevel =
                        FRAME_ENTRY(SP, nestLevelOffset).uint32;

                    ASSERT(!FrameStack::isPointerSlot(SP, indirectOffset));
                    UInt32Value fieldIndex =
                        FRAME_ENTRY(SP, indirectOffset).uint32;

                    ASSERT(!FrameStack::isPointerSlot(SP, fieldSizeIndex));
                    UInt32Value fieldSize =
                        FRAME_ENTRY(SP, fieldSizeIndex).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* root = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(root));

                    Cell* block = getNestedBlock(root, nestLevel);

                    ASSERT((fieldIndex + fieldSize - 1)
                           < Heap::getPayloadSize(block));

                    for(int index = 0; index < fieldSize; index += 1){
                        ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination + index, block, fieldIndex + index);

                        FRAME_ENTRY(SP, destination + index) =
                            HEAP_GETFIELD(block, fieldIndex + index);
                    }
                    break;
                }
              case SetField_S:
                {
                    UInt32Value fieldIndex = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT(fieldIndex < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex, block, fieldIndex);
                    Cell variableValue = FRAME_ENTRY(SP, variableIndex);
                    Heap::updateField(block, fieldIndex, variableValue);
                    break;
                }
              case SetField_D:
                {
                    UInt32Value fieldIndex = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT(fieldIndex + 1 < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex, block, fieldIndex);
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex + 1, block, fieldIndex + 1);
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, variableIndex));

                    Real64Value fieldValue =
                      *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, variableIndex);
                    Heap::updateField_D(block, fieldIndex, fieldValue);
                    break;
                }
              case SetField_V:
                {
                    UInt32Value fieldIndex = getWordAndInc(PC);
                    UInt32Value fieldSizeIndex = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, fieldSizeIndex));
                    UInt32Value fieldSize =
                        FRAME_ENTRY(SP, fieldSizeIndex).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT((fieldIndex + fieldSize - 1)
                           < Heap::getPayloadSize(block));

                    for(int index = 0; index < fieldSize; index += 1){
                        ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex + index, block, fieldIndex + index);

                        Cell variableValue =
                            FRAME_ENTRY(SP, variableIndex + index);
                        Heap::updateField
                            (block, fieldIndex + index, variableValue);
                    }
                    break;
                }
              case SetFieldIndirect_S:
                {
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, indirectOffset));
                    UInt32Value fieldIndex =
                        FRAME_ENTRY(SP, indirectOffset).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT(fieldIndex < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex, block, fieldIndex);
                    Cell variableValue = FRAME_ENTRY(SP, variableIndex);
                    Heap::updateField(block, fieldIndex, variableValue);
                    break;
                }
              case SetFieldIndirect_D:
                {
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, indirectOffset));
                    UInt32Value fieldIndex =
                        FRAME_ENTRY(SP, indirectOffset).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT(fieldIndex + 1 < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex, block, fieldIndex);
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex + 1, block, fieldIndex + 1);
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, variableIndex));

                    Real64Value fieldValue =
                        *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, variableIndex);
                    Heap::updateField_D(block, fieldIndex, fieldValue);

                    break;
                }
              case SetFieldIndirect_V:
                {
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value fieldSizeIndex = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, fieldSizeIndex));
                    UInt32Value fieldSize =
                        FRAME_ENTRY(SP, fieldSizeIndex).uint32;

                    ASSERT(!FrameStack::isPointerSlot(SP, indirectOffset));
                    UInt32Value fieldIndex =
                        FRAME_ENTRY(SP, indirectOffset).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT((fieldIndex + fieldSize - 1)
                           < Heap::getPayloadSize(block));

                    for(int index = 0; index < fieldSize; index += 1){
                        ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex + index, block, fieldIndex + index);
                        Cell variableValue =
                            FRAME_ENTRY(SP, variableIndex + index);
                        Heap::updateField
                            (block, fieldIndex + index, variableValue);
                    }
                    break;
                }
              case SetNestedFieldIndirect_S:
                {
                    UInt32Value nestLevelOffset = getWordAndInc(PC);
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, nestLevelOffset));
                    UInt32Value nestLevel =
                        FRAME_ENTRY(SP, nestLevelOffset).uint32;

                    ASSERT(!FrameStack::isPointerSlot(SP, indirectOffset));
                    UInt32Value fieldIndex =
                        FRAME_ENTRY(SP, indirectOffset).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* root = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(root));

                    Cell* block = getNestedBlock(root, nestLevel);
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT(fieldIndex < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex, block, fieldIndex);
                    Cell variableValue = FRAME_ENTRY(SP, variableIndex);
                    Heap::updateField(block, fieldIndex, variableValue);
                    break;
                }
              case SetNestedFieldIndirect_D:
                {
                    UInt32Value nestLevelOffset = getWordAndInc(PC);
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, nestLevelOffset));
                    UInt32Value nestLevel =
                        FRAME_ENTRY(SP, nestLevelOffset).uint32;

                    ASSERT(!FrameStack::isPointerSlot(SP, indirectOffset));
                    UInt32Value fieldIndex =
                        FRAME_ENTRY(SP, indirectOffset).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* root = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(root));

                    Cell* block = getNestedBlock(root, nestLevel);
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT(fieldIndex + 1 < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex, block, fieldIndex);
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex + 1, block, fieldIndex + 1);
                    ASSERT_REAL64_ALIGNED
                        (FRAME_ENTRY_ADDRESS(SP, variableIndex));

                    Real64Value fieldValue =
                        *(Real64Value*)FRAME_ENTRY_ADDRESS(SP, variableIndex);
                    Heap::updateField_D(block, fieldIndex, fieldValue);

                    break;
                }
              case SetNestedFieldIndirect_V:
                {
                    UInt32Value nestLevelOffset = getWordAndInc(PC);
                    UInt32Value indirectOffset = getWordAndInc(PC);
                    UInt32Value fieldSizeIndex = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, nestLevelOffset));
                    UInt32Value nestLevel =
                        FRAME_ENTRY(SP, nestLevelOffset).uint32;

                    ASSERT(!FrameStack::isPointerSlot(SP, indirectOffset));
                    UInt32Value fieldIndex =
                        FRAME_ENTRY(SP, indirectOffset).uint32;

                    ASSERT(!FrameStack::isPointerSlot(SP, fieldSizeIndex));
                    UInt32Value fieldSize =
                        FRAME_ENTRY(SP, fieldSizeIndex).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* root = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(root));

                    Cell* block = getNestedBlock(root, nestLevel);
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT((fieldIndex + fieldSize - 1)
                           < Heap::getPayloadSize(block));
                    for(int index = 0; index < fieldSize; index += 1){
                        ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex + index, block, fieldIndex + index);
                        Cell variableValue =
                            FRAME_ENTRY(SP, variableIndex + index);
                        Heap::updateField
                            (block, fieldIndex + index, variableValue);
                    }
                    break;
                }
              case CopyBlock:
                {
                    UInt32Value nestLevelOffset = getWordAndInc(PC);
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value destinationIndex = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, nestLevelOffset));
                    UInt32Value nestLevel =
                        FRAME_ENTRY(SP, nestLevelOffset).uint32;

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell *block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    ASSERT(FrameStack::isPointerSlot(SP, destinationIndex));
                    FRAME_ENTRY(SP, destinationIndex).blockRef = block;

                    for (int level = 0; level <= nestLevel; level += 1) {
                        Cell *origBlock, *destBlock;
                        origBlock = getNestedBlock(block, level);

                        Bitmap bitmap = Heap::getBitmap(origBlock);
                        int fieldsCount = Heap::getPayloadSize(origBlock);
                        ALLOCATE_RECORDBLOCK(destBlock, bitmap, fieldsCount);
                        ASSERT(Heap::isValidBlockPointer(destBlock));

                        /* get the block pointer again, because GC may occur
                         * in the above allocation.
                         */
                        block = FRAME_ENTRY(SP, destinationIndex).blockRef;
                        ASSERT(Heap::isValidBlockPointer(block));

                        if (level == 0) {
                            COPY_MEMORY
                                (destBlock, block, fieldsCount * sizeof(Cell));
                            FRAME_ENTRY(SP, destinationIndex).blockRef =
                                destBlock;
                            continue;
                        }

                        Cell *parent = getNestedBlock(block, level - 1);
                        ASSERT(Heap::isValidBlockPointer(parent));
                        origBlock = getNestedBlock(parent, 1);
                        ASSERT(Heap::isValidBlockPointer(origBlock));
                        COPY_MEMORY
                            (destBlock, origBlock, fieldsCount * sizeof(Cell));

                        Cell nest;
                        nest.blockRef = destBlock;
                        Heap::updateField(parent, INDEX_OF_NEST_POINTER, nest);
                    }
                    break;
                }

              case GetGlobal_S:
              case GetGlobal_D:
                {
                    UInt32Value arrayIndex = getWordAndInc(PC);
                    UInt32Value offset = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(arrayIndex < globalArrays_.size());
                    Cell* block = globalArrays_[arrayIndex];
                        
                    ASSERT(Heap::isValidBlockPointer(block));
/*
                    DBGWRAP(LOG.debug("GetGlobal: "
                                   "arrayIndex=%d, offset=%d, block=%x",
                                   arrayIndex, offset, block);)
*/
                    ASSERT(offset < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, destination, block, offset);
                    FRAME_ENTRY(SP, destination) =
                        HEAP_GETFIELD(block, offset);
                    if(GetGlobal_D == opcode){
                        FRAME_ENTRY(SP, destination + 1) =
                            HEAP_GETFIELD(block, offset + 1);
                    }
                    break;
                }
              case SetGlobal_S:
              case SetGlobal_D:
                {
                    UInt32Value arrayIndex = getWordAndInc(PC);
                    UInt32Value offset = getWordAndInc(PC);
                    UInt32Value variableIndex = getWordAndInc(PC);

                    ASSERT(arrayIndex < globalArrays_.size());
                    Cell* block = globalArrays_[arrayIndex];
                    ASSERT(Heap::isValidBlockPointer(block));
/*
                    DBGWRAP(LOG.debug("SetGlobal: "
                                   "arrayIndex=%d, offset=%d, block=%x",
                                   arrayIndex, offset, block);)
*/
                    ASSERT(offset < Heap::getPayloadSize(block));
                    ASSERT_SAME_TYPE_SLOT_FIELD
                        (SP, variableIndex, block, offset);
                    Cell value = FRAME_ENTRY(SP, variableIndex);
                    Heap::updateField(block, offset, value);
                    if(SetGlobal_D == opcode){
                        ASSERT(offset + 1 < Heap::getPayloadSize(block));
                        value = FRAME_ENTRY(SP, variableIndex + 1);
                        Heap::updateField(block, offset + 1, value);
                    }
                    break;
                }
              case InitGlobalArrayUnboxed:
              case InitGlobalArrayBoxed:
              case InitGlobalArrayDouble:
                {
                    UInt32Value arrayIndex = getWordAndInc(PC);
                    UInt32Value arraySize = getWordAndInc(PC);

                    Cell* block;
                    Cell initialValue;
                    switch(opcode){
                      case InitGlobalArrayUnboxed:
                        {
                            ALLOCATE_SINGLEATOMARRAY(block, arraySize);
                            initialValue.uint32 = 0;
                            break;
                        }
                      case InitGlobalArrayBoxed:
                        {
                            ALLOCATE_POINTERARRAY(block, arraySize);
                            initialValue.blockRef = block; // dummy 
                            break;
                        }
                      case InitGlobalArrayDouble:
                        {
                            ALLOCATE_DOUBLEATOMARRAY(block, arraySize);
                            initialValue.uint32 = 0;
                            break;
                        }
                    }

                    for(int index = 0; index < arraySize; index += 1){
                        Heap::initializeField
                            (block, index, initialValue);
                    }
/*                    
                    DBGWRAP(LOG.debug("InitGlobalArray: "
                                   "arrayIndex=%d, arraySize=%d, block=%x",
                                   arrayIndex, arraySize, block);)
*/
                    if(globalArrays_.size() <= arrayIndex){
                        globalArrays_.resize(arrayIndex + 1);
                    }
                    globalArrays_[arrayIndex] = block;
                    break;
                }
              case GetEnv:
                {
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(FrameStack::isPointerSlot(SP, destination));
                    FRAME_ENTRY(SP, destination).blockRef = ENV;
                    break;
                }
              case CallPrim:
                {
                    Cell* argRefsBuffer[PRIMITIVE_MAX_ARGUMENTS];

                    UInt32Value primitiveIndex = getWordAndInc(PC);
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value *argIndexes = PC;
                    PC += argsCount;
                    UInt32Value destination = getWordAndInc(PC);

#ifdef IML_DEBUG
                    if(NUMBER_OF_PRIMITIVES <= primitiveIndex){
                        DBGWRAP
                        (LOG.error("callPrim::IllegalArgumentException"));
                        throw IllegalArgumentException();// IllegalArgument???
                    }
#endif
                    Primitive primitive = primitives[primitiveIndex];

                    // copy argument values into the buffer array.
                    for(int index = 0; index < argsCount; index += 1){
                        argRefsBuffer[index] = &FRAME_ENTRY(SP, *argIndexes);
                        argIndexes += 1;
                    }
                    Cell *resultBuf = &FRAME_ENTRY(SP, destination);

                    // Because GC may be caused by allocation in primitive,
                    // save registers.
                    SAVE_REGISTERS;
                    primitive(argsCount, argRefsBuffer, resultBuf);
                    RESTORE_REGISTERS;

                    if(isPrimitiveExceptionRaised_){
                        raiseException(SP, PC, ENV, primitiveException_);
                        resetPrimitiveException();
                    }
                    break;
                }

              case Apply_S:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argIndex = getWordAndInc(PC);
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    /* jump to the function */
                    callFunction_S(false,
                                   PC,
                                   SP,
                                   ENV,
                                   entryPoint,
                                   restoredENV,
                                   argIndex,
                                   returnAddress);
                    break;
                }
              case Apply_ML_S:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    /* jump to the function */
                    callFunction_ML_S(false,
                                      PC,
                                      SP,
                                      ENV,
                                      entryPoint,
                                      restoredENV,
                                      argIndexes,
                                      returnAddress);
                    break;
                }
              case Apply_D:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argIndex = getWordAndInc(PC);
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    /* jump to the function */
                    callFunction_D(false,
                                   PC,
                                   SP,
                                   ENV,
                                   entryPoint,
                                   restoredENV,
                                   argIndex,
                                   returnAddress);
                    break;
                }
              case Apply_ML_D:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    /* jump to the function */
                    callFunction_ML_D(false,
                                      PC,
                                      SP,
                                      ENV,
                                      entryPoint,
                                      restoredENV,
                                      argIndexes,
                                      returnAddress);
                    break;
                }
              case Apply_V:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argIndex = getWordAndInc(PC);
                    UInt32Value argSizeIndex = getWordAndInc(PC);
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    UInt32Value argSize = FRAME_ENTRY(SP, argSizeIndex).uint32;

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    /* jump to the function */
                    callFunction_V(false,
                                   PC,
                                   SP,
                                   ENV,
                                   entryPoint,
                                   restoredENV,
                                   argIndex,
                                   argSize,
                                   returnAddress);
                    break;
                }
              case Apply_ML_V:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value lastArgSizeIndex = getWordAndInc(PC);
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    UInt32Value lastArgSize =
                    FRAME_ENTRY(SP, lastArgSizeIndex).uint32;

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    /* jump to the function */
                    callFunction_ML_V(false,
                                      PC,
                                      SP,
                                      ENV,
                                      entryPoint,
                                      restoredENV,
                                      argIndexes,
                                      lastArgSize,
                                      returnAddress);
                    break;
                }
              case Apply_M:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value* argSizeIndexes = PC;
                    PC += argsCount;
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    /* jump to the function */
                    callFunction_M(false,
                                   PC,
                                   SP,
                                   ENV,
                                   entryPoint,
                                   restoredENV,
                                   argIndexes,
                                   argSizeIndexes,
                                   returnAddress);
                    break;
                }
              case TailApply_S:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argIndex = getWordAndInc(PC);

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    callFunction_S(true,
                                   PC,
                                   SP,
                                   ENV,
                                   entryPoint,
                                   restoredENV,
                                   argIndex,
                                   NULL);
                    break;
                }
              case TailApply_ML_S:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    callFunction_ML_S(true,
                                      PC,
                                      SP,
                                      ENV,
                                      entryPoint,
                                      restoredENV,
                                      argIndexes,
                                      NULL);
                    break;
                }
              case TailApply_D:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argIndex = getWordAndInc(PC);

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    callFunction_D(true,
                                   PC,
                                   SP,
                                   ENV,
                                   entryPoint,
                                   restoredENV,
                                   argIndex,
                                   NULL);
                    break;
                }
              case TailApply_ML_D:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    callFunction_ML_D(true,
                                      PC,
                                      SP,
                                      ENV,
                                      entryPoint,
                                      restoredENV,
                                      argIndexes,
                                      NULL);
                    break;
                }
              case TailApply_V:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argIndex = getWordAndInc(PC);
                    UInt32Value argSizeIndex = getWordAndInc(PC);

                    UInt32Value argSize = FRAME_ENTRY(SP, argSizeIndex).uint32;

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    callFunction_V(true,
                                   PC,
                                   SP,
                                   ENV,
                                   entryPoint,
                                   restoredENV,
                                   argIndex,
                                   argSize,
                                   NULL);
                    break;
                }
              case TailApply_ML_V:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value lastArgSizeIndex = getWordAndInc(PC);

                    UInt32Value lastArgSize =
                    FRAME_ENTRY(SP, lastArgSizeIndex).uint32;

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    callFunction_ML_V(true,
                                      PC,
                                      SP,
                                      ENV,
                                      entryPoint,
                                      restoredENV,
                                      argIndexes,
                                      lastArgSize,
                                      NULL);
                    break;
                }
              case TailApply_M:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value* argSizeIndexes = PC;
                    PC += argsCount;

                    /* expand closure */
                    UInt32Value* entryPoint;
                    Cell* restoredENV;
                    expandClosure(SP, closureIndex, entryPoint, restoredENV);

                    callFunction_M(true,
                                   PC,
                                   SP,
                                   ENV,
                                   entryPoint,
                                   restoredENV,
                                   argIndexes,
                                   argSizeIndexes,
                                   NULL);
                    break;
                }
              case CallStatic_ML_S:
              case CallStatic_S:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value ENVIndex = getWordAndInc(PC);
                    UInt32Value argsCount;
                    switch(opcode){
                      case CallStatic_ML_S:
                        argsCount = getWordAndInc(PC); break;
                      case CallStatic_S: argsCount = 1; break;
                    }
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    // get the ENV block for callee.
                    ASSERT(FrameStack::isPointerSlot(SP, ENVIndex));
                    Cell* calleeENV = FRAME_ENTRY(SP, ENVIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(calleeENV));

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    callFunction_ML_S(false,
                                      PC,
                                      SP,
                                      ENV,
                                      entryPoint,
                                      calleeENV,
                                      argIndexes,
                                      returnAddress);
                    break;
                }
              case CallStatic_ML_D:
              case CallStatic_D:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value ENVIndex = getWordAndInc(PC);
                    UInt32Value argsCount;
                    switch(opcode){
                      case CallStatic_ML_D:
                        argsCount = getWordAndInc(PC); break;
                      case CallStatic_D: argsCount = 1; break;
                    }
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    // get the ENV block for callee.
                    ASSERT(FrameStack::isPointerSlot(SP, ENVIndex));
                    Cell* calleeENV = FRAME_ENTRY(SP, ENVIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(calleeENV));

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    callFunction_ML_D(false,
                                      PC,
                                      SP,
                                      ENV,
                                      entryPoint,
                                      calleeENV,
                                      argIndexes,
                                      returnAddress);
                    break;
                }
              case CallStatic_ML_V:
              case CallStatic_V:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value ENVIndex = getWordAndInc(PC);
                    UInt32Value argsCount;
                    switch(opcode){
                      case CallStatic_ML_V:
                        argsCount = getWordAndInc(PC); break;
                      case CallStatic_V: argsCount = 1; break;
                    }
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value lastArgSizeIndex = getWordAndInc(PC);
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    UInt32Value lastArgSize =
                    FRAME_ENTRY(SP, lastArgSizeIndex).uint32;

                    // get the ENV block for callee.
                    ASSERT(FrameStack::isPointerSlot(SP, ENVIndex));
                    Cell* calleeENV = FRAME_ENTRY(SP, ENVIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(calleeENV));

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    callFunction_ML_V(false,
                                      PC,
                                      SP,
                                      ENV,
                                      entryPoint,
                                      calleeENV,
                                      argIndexes,
                                      lastArgSize,
                                      returnAddress);
                    break;
                }
              case CallStatic_M:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value ENVIndex = getWordAndInc(PC);
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value* argSizeIndexes = PC;
                    PC += argsCount;
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    // get the ENV block for callee.
                    ASSERT(FrameStack::isPointerSlot(SP, ENVIndex));
                    Cell* calleeENV = FRAME_ENTRY(SP, ENVIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(calleeENV));

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    callFunction_M(false,
                                   PC,
                                   SP,
                                   ENV,
                                   entryPoint,
                                   calleeENV,
                                   argIndexes,
                                   argSizeIndexes,
                                   returnAddress);
                    break;
                }
              case TailCallStatic_ML_S:
              case TailCallStatic_S:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value ENVIndex = getWordAndInc(PC);
                    UInt32Value argsCount;
                    switch(opcode){
                      case TailCallStatic_ML_S:
                        argsCount = getWordAndInc(PC); break;
                      case TailCallStatic_S: argsCount = 1; break;
                    }
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;

                    // get the ENV block for callee.
                    ASSERT(FrameStack::isPointerSlot(SP, ENVIndex));
                    Cell* calleeENV = FRAME_ENTRY(SP, ENVIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(calleeENV));

                    callFunction_ML_S(true,
                                      PC,
                                      SP,
                                      ENV,
                                      entryPoint,
                                      calleeENV,
                                      argIndexes,
                                      NULL);
                    break;
                }
              case TailCallStatic_ML_D:
              case TailCallStatic_D:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value ENVIndex = getWordAndInc(PC);
                    UInt32Value argsCount;
                    switch(opcode){
                      case TailCallStatic_ML_D:
                        argsCount = getWordAndInc(PC); break;
                      case TailCallStatic_D: argsCount = 1; break;
                    }
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value lastArgSize;

                    // get the ENV block for callee.
                    ASSERT(FrameStack::isPointerSlot(SP, ENVIndex));
                    Cell* calleeENV = FRAME_ENTRY(SP, ENVIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(calleeENV));

                    callFunction_ML_D(true,
                                      PC,
                                      SP,
                                      ENV,
                                      entryPoint,
                                      calleeENV,
                                      argIndexes,
                                      NULL);
                    break;
                }
              case TailCallStatic_ML_V:
              case TailCallStatic_V:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value ENVIndex = getWordAndInc(PC);
                    UInt32Value argsCount;
                    switch(opcode){
                      case TailCallStatic_ML_V:
                        argsCount = getWordAndInc(PC); break;
                      case TailCallStatic_V: argsCount = 1; break;
                    }
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value lastArgSizeIndex = getWordAndInc(PC);

                    UInt32Value lastArgSize =
                    FRAME_ENTRY(SP, lastArgSizeIndex).uint32;

                    // get the ENV block for callee.
                    ASSERT(FrameStack::isPointerSlot(SP, ENVIndex));
                    Cell* calleeENV = FRAME_ENTRY(SP, ENVIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(calleeENV));

                    callFunction_ML_V(true,
                                      PC,
                                      SP,
                                      ENV,
                                      entryPoint,
                                      calleeENV,
                                      argIndexes,
                                      lastArgSize,
                                      NULL);
                    break;
                }
              case TailCallStatic_M:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value ENVIndex = getWordAndInc(PC);
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value* argSizeIndexes = PC;
                    PC += argsCount;

                    // get the ENV block for callee.
                    ASSERT(FrameStack::isPointerSlot(SP, ENVIndex));
                    Cell* calleeENV = FRAME_ENTRY(SP, ENVIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(calleeENV));

                    callFunction_M(true,
                                   PC,
                                   SP,
                                   ENV,
                                   entryPoint,
                                   calleeENV,
                                   argIndexes,
                                   argSizeIndexes,
                                   NULL);
                    break;
                }
              case RecursiveCallStatic_S:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argIndex = getWordAndInc(PC);
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    callRecursiveFunction_S(false,
                                            PC,
                                            SP,
                                            ENV,
                                            entryPoint,
                                            argIndex,
                                            returnAddress);
                    break;
                }
              case RecursiveCallStatic_D:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argIndex = getWordAndInc(PC);
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    callRecursiveFunction_D(false,
                                            PC,
                                            SP,
                                            ENV,
                                            entryPoint,
                                            argIndex,
                                            returnAddress);
                    break;
                }
              case RecursiveCallStatic_V:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argIndex = getWordAndInc(PC);
                    UInt32Value argSizeIndex = getWordAndInc(PC);
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    UInt32Value argSize = FRAME_ENTRY(SP, argSizeIndex).uint32;

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    callRecursiveFunction_V(false,
                                            PC,
                                            SP,
                                            ENV,
                                            entryPoint,
                                            argIndex,
                                            argSize,
                                            returnAddress);
                    break;
                }
              case RecursiveCallStatic_M:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value* argSizeIndexes = PC;
                    PC += argsCount;
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    callRecursiveFunction_M(false,
                                            PC,
                                            SP,
                                            ENV,
                                            entryPoint,
                                            argIndexes,
                                            argSizeIndexes,
                                            returnAddress);
                    break;
                }
              case RecursiveTailCallStatic_S:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argIndex = getWordAndInc(PC);

                    callRecursiveFunction_S(true,
                                            PC,
                                            SP,
                                            ENV,
                                            entryPoint,
                                            argIndex,
                                            NULL);
                    break;
                }
              case RecursiveTailCallStatic_D:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argIndex = getWordAndInc(PC);

                    callRecursiveFunction_D(true,
                                            PC,
                                            SP,
                                            ENV,
                                            entryPoint,
                                            argIndex,
                                            NULL);
                    break;
                }
              case RecursiveTailCallStatic_V:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argIndex = getWordAndInc(PC);
                    UInt32Value argSizeIndex = getWordAndInc(PC);

                    UInt32Value argSize = FRAME_ENTRY(SP, argSizeIndex).uint32;

                    callRecursiveFunction_V(true,
                                            PC,
                                            SP,
                                            ENV,
                                            entryPoint,
                                            argIndex,
                                            argSize,
                                            NULL);
                    break;
                }
              case RecursiveTailCallStatic_M:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value* argSizeIndexes = PC;
                    PC += argsCount;

                    callRecursiveFunction_M(true,
                                            PC,
                                            SP,
                                            ENV,
                                            entryPoint,
                                            argIndexes,
                                            argSizeIndexes,
                                            NULL);
                    break;
                }
              case SelfRecursiveCallStatic_S:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argIndex = getWordAndInc(PC);
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    callSelfRecursiveFunction_S(false,
                                                PC,
                                                SP,
                                                entryPoint,
                                                argIndex,
                                                returnAddress);
                    break;
                }
              case SelfRecursiveCallStatic_D:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argIndex = getWordAndInc(PC);
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    callSelfRecursiveFunction_D(false,
                                                PC,
                                                SP,
                                                entryPoint,
                                                argIndex,
                                                returnAddress);
                    break;
                }
              case SelfRecursiveCallStatic_V:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argIndex = getWordAndInc(PC);
                    UInt32Value argSizeIndex = getWordAndInc(PC);
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    UInt32Value argSize = FRAME_ENTRY(SP, argSizeIndex).uint32;

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    callSelfRecursiveFunction_V(false,
                                                PC,
                                                SP,
                                                entryPoint,
                                                argIndex,
                                                argSize,
                                                returnAddress);
                    break;
                }
              case SelfRecursiveCallStatic_M:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value* argSizeIndexes = PC;
                    PC += argsCount;
                    // now, PC points to destination operand.
                    UInt32Value* returnAddress = PC;

                    /* save ENV registers to the current frame. */
                    FrameStack::storeENV(SP, ENV);

                    callSelfRecursiveFunction_M(false,
                                                PC,
                                                SP,
                                                entryPoint,
                                                argsCount,
                                                argIndexes,
                                                argSizeIndexes,
                                                returnAddress);
                    break;
                }
              case SelfRecursiveTailCallStatic_S:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argIndex = getWordAndInc(PC);

                    callSelfRecursiveFunction_S(true,
                                                PC,
                                                SP,
                                                entryPoint,
                                                argIndex,
                                                NULL);
                    break;
                }
              case SelfRecursiveTailCallStatic_D:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argIndex = getWordAndInc(PC);

                    callSelfRecursiveFunction_D(true,
                                                PC,
                                                SP,
                                                entryPoint,
                                                argIndex,
                                                NULL);
                    break;
                }
              case SelfRecursiveTailCallStatic_V:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argIndex = getWordAndInc(PC);
                    UInt32Value argSizeIndex = getWordAndInc(PC);

                    UInt32Value argSize = FRAME_ENTRY(SP, argSizeIndex).uint32;

                    callSelfRecursiveFunction_V(true,
                                                PC,
                                                SP,
                                                entryPoint,
                                                argIndex,
                                                argSize,
                                                NULL);
                    break;
                }
              case SelfRecursiveTailCallStatic_M:
                {
                    UInt32Value* entryPoint =
                    (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value* argSizeIndexes = PC;
                    PC += argsCount;

                    callSelfRecursiveFunction_M(true,
                                                PC,
                                                SP,
                                                entryPoint,
                                                argsCount,
                                                argIndexes,
                                                argSizeIndexes,
                                                NULL);
                    break;
                }
              case MakeBlock:
                {
                    UInt32Value bitmapIndex = getWordAndInc(PC);
                    UInt32Value sizeIndex = getWordAndInc(PC);
                    UInt32Value fieldsCount = getWordAndInc(PC);
                    UInt32Value *fieldValueIndexes = PC;
                    PC += fieldsCount;
                    UInt32Value *fieldSizeIndexes = PC;
                    PC += fieldsCount;
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, bitmapIndex));
                    Bitmap bitmap =
                    (Bitmap)(FRAME_ENTRY(SP, bitmapIndex).uint32);

                    ASSERT(!FrameStack::isPointerSlot(SP, sizeIndex));
                    UInt32Value size = FRAME_ENTRY(SP, sizeIndex).uint32;

                    Cell* block;
                    ALLOCATE_RECORDBLOCK(block, bitmap, size);

                    UInt32Value fieldOffset = 0;
                    for(int fieldIndex = 0;
                        fieldIndex < fieldsCount;
                        fieldIndex += 1)
                    {
                        UInt32Value fieldValueIndex = *fieldValueIndexes;
                        UInt32Value fieldSizeIndex = *fieldSizeIndexes;
                        fieldValueIndexes += 1;
                        fieldSizeIndexes += 1;

                        ASSERT(!FrameStack::isPointerSlot(SP, fieldSizeIndex));
                        UInt32Value fieldSize =
                        FRAME_ENTRY(SP, fieldSizeIndex).uint32;
                        for(int index = 0; index < fieldSize; index += 1){
                            ASSERT_SAME_TYPE_SLOT_FIELD
                                (SP, fieldValueIndex + index,
                                 block, fieldOffset + index);
                            Cell fieldValue =
                            FRAME_ENTRY(SP, fieldValueIndex + index);
                            Heap::initializeField
                            (block, fieldOffset + index, fieldValue);
                        }
                        fieldOffset += fieldSize;
                    }
                    ASSERT(FrameStack::isPointerSlot(SP, destination));
                    FRAME_ENTRY(SP, destination).blockRef = block;
                    break;
                }
              case MakeBlockOfSingleValues:
                {
                    UInt32Value bitmapIndex = getWordAndInc(PC);
                    UInt32Value fieldsCount = getWordAndInc(PC);
                    UInt32Value *fieldValueIndexes = PC;
                    PC += fieldsCount;
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, bitmapIndex));
                    Bitmap bitmap =
                    (Bitmap)(FRAME_ENTRY(SP, bitmapIndex).uint32);

                    Cell* block;
                    ALLOCATE_RECORDBLOCK(block, bitmap, fieldsCount);
                    for(int index = 0; index < fieldsCount; index += 1){
                        UInt32Value fieldValueIndex = *fieldValueIndexes;
                        fieldValueIndexes += 1;

                        ASSERT_SAME_TYPE_SLOT_FIELD
                            (SP, fieldValueIndex, block, index);
                        Cell fieldValue = FRAME_ENTRY(SP, fieldValueIndex);
                        Heap::initializeField(block, index, fieldValue);
                    }
                    ASSERT(FrameStack::isPointerSlot(SP, destination));
                    FRAME_ENTRY(SP, destination).blockRef = block;
                    break;
                }
              case MakeArray_S:
              case MakeArray_D:
                {
                    UInt32Value bitmapIndex = getWordAndInc(PC);
                    UInt32Value sizeIndex = getWordAndInc(PC);
                    UInt32Value initialValueIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, bitmapIndex));
                    Bitmap bitmap =
                    (Bitmap)(FRAME_ENTRY(SP, bitmapIndex).uint32);

                    ASSERT(!FrameStack::isPointerSlot(SP, sizeIndex));
                    UInt32Value size = FRAME_ENTRY(SP, sizeIndex).uint32;

                    Cell* block;
                    switch(opcode){
                      case MakeArray_S:
                        {
                            if(0 == bitmap){
                                ALLOCATE_SINGLEATOMARRAY(block, size);
                            }
                            else{ALLOCATE_POINTERARRAY(block, size);}

                            Cell initialValue =
                            FRAME_ENTRY(SP, initialValueIndex);
#ifdef IML_DEBUG
                            if(bitmap){
                                ASSERT(FrameStack::isPointerSlot
                                       (SP, initialValueIndex));
                                ASSERT(Heap::isValidBlockPointer
                                       (initialValue.blockRef));
                            }
#endif
                            for(int index = 0; index < size; index += 1){
                                Heap::initializeField
                                (block, index, initialValue);
                            }
                            break;
                        }
                      case MakeArray_D:
                        {
                            ASSERT(0 == bitmap);
                            ALLOCATE_DOUBLEATOMARRAY(block, size);

                            Cell initialValue1 =
                            FRAME_ENTRY(SP, initialValueIndex);
                            Cell initialValue2 =
                            FRAME_ENTRY(SP, initialValueIndex + 1);
#ifdef IML_DEBUG
                            if(bitmap){// expected to never happen
                                ASSERT(FrameStack::isPointerSlot
                                       (SP, initialValueIndex));
                                ASSERT(FrameStack::isPointerSlot
                                       (SP, initialValueIndex + 1));
                                ASSERT(Heap::isValidBlockPointer
                                       (initialValue1.blockRef));
                                ASSERT(Heap::isValidBlockPointer
                                       (initialValue2.blockRef));
                            }
#endif
                            for(int index = 0; index < size; index += 2){
                                Heap::initializeField
                                (block, index, initialValue1);
                                Heap::initializeField
                                (block, index + 1, initialValue2);
                            }
                            break;
                        }
                    }

                    ASSERT(FrameStack::isPointerSlot(SP, destination));
                    FRAME_ENTRY(SP, destination).blockRef = block;
                    break;
                }
              case MakeArray_V:
                {
                    UInt32Value bitmapIndex = getWordAndInc(PC);
                    UInt32Value sizeIndex = getWordAndInc(PC);
                    UInt32Value initialValueIndex = getWordAndInc(PC);
                    UInt32Value initialValueSizeIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, bitmapIndex));
                    Bitmap bitmap =
                    (Bitmap)(FRAME_ENTRY(SP, bitmapIndex).uint32);

                    ASSERT(!FrameStack::isPointerSlot(SP, sizeIndex));
                    UInt32Value size = FRAME_ENTRY(SP, sizeIndex).uint32;

                    ASSERT(!FrameStack::isPointerSlot
                               (SP, initialValueSizeIndex));
                    UInt32Value initialValueSize =
                    FRAME_ENTRY(SP, initialValueSizeIndex).uint32;

                    Cell* block;
                    if(0 == bitmap){
                        switch(initialValueSize){
                          case 1:
                            ALLOCATE_SINGLEATOMARRAY(block, size);
                            break;
                          case 2:
                            ALLOCATE_DOUBLEATOMARRAY(block, size);
                            break;
                          default:
                            ASSERT(false);
                        }
                    }
                    else{ALLOCATE_POINTERARRAY(block, size);}

                    // get initial values
                    Cell initialValues[initialValueSize];
                    for(int index = 0; index < initialValueSize; index += 1){
                        initialValues[index] = 
                        FRAME_ENTRY(SP, initialValueIndex + index);
                    }
#ifdef IML_DEBUG
                    // assert runtime type
                    if(bitmap){
                        for(int index = 0;
                            index < initialValueSize;
                            index += 1)
                        {
                            ASSERT(FrameStack::isPointerSlot
                                       (SP, initialValueIndex + index));
                            ASSERT(Heap::isValidBlockPointer
                                       (initialValues[index].blockRef));
                        }
                    }
#endif
                    // copy initial values into the array.
                    for(int index = 0; index < size; index += initialValueSize)
                    {
                        for(int i = 0; i < initialValueSize; i += 1){
                            Heap::initializeField
                            (block, index + i, initialValues[i]);
                        }
                    }

                    ASSERT(FrameStack::isPointerSlot(SP, destination));
                    FRAME_ENTRY(SP, destination).blockRef = block;
                    break;
                }
              case MakeClosure:
                {
                    Cell entryPoint;
                    entryPoint.uint32 = getWordAndInc(PC);
                    UInt32Value ENVIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    Bitmap bitmap = CLOSURE_BITMAP;

                    Cell* block;
                    ALLOCATE_RECORDBLOCK
                    (block, bitmap, CLOSURE_FIELDS_COUNT);

                    Heap::initializeField(block,
                                         CLOSURE_ENTRYPOINT_INDEX,
                                         entryPoint);

                    ASSERT(FrameStack::isPointerSlot(SP, ENVIndex));
                    Cell ENVBlock = FRAME_ENTRY(SP, ENVIndex);
                    ASSERT(Heap::isValidBlockPointer(ENVBlock.blockRef));
                    Heap::initializeField(block,
                                         CLOSURE_ENV_INDEX,
                                         ENVBlock);

                    ASSERT(FrameStack::isPointerSlot(SP, destination));
                    FRAME_ENTRY(SP, destination).blockRef = block;
                    break;
                }
              case Raise:
                {
                    UInt32Value exceptionIndex = getWordAndInc(PC);

                    ASSERT(FrameStack::isPointerSlot(SP, exceptionIndex));
                    Cell exceptionValue = FRAME_ENTRY(SP, exceptionIndex);
                    raiseException(SP, PC, ENV, exceptionValue);
                    break;
                }
              case PushHandler:
                {
                    UInt32Value* handler = (UInt32Value*)(getWordAndInc(PC));
                    UInt32Value exceptionIndex = getWordAndInc(PC);

                    HandlerStack::push(SP, exceptionIndex, handler);
                    break;
                }
              case PopHandler:
                {

                    HandlerStack::remove();
                    break;
                }
	      case RegisterCallback:
		{
		    UInt32Value closureIndex = getWordAndInc(PC);
		    UInt32Value sizeTag = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);
                    UInt32Value *entryPoint;
                    Cell *env;
		    Cell returnValue;

                    expandClosure(SP, closureIndex, entryPoint, env);

		    returnValue.uint32 =
		    (UInt32Value)FFI::instance().callback(entryPoint, env,
                                                          sizeTag);

		    FRAME_ENTRY(SP, destination) = returnValue;
		    break;
		}
/* case branch consists of two words: Constant, offset to jump destination. */
#define WORDS_OF_CASE 2 
              case SwitchInt:
              case SwitchWord:
              case SwitchChar:
                {
                    UInt32Value targetIndex = getWordAndInc(PC);
                    UInt32Value casesCount = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, targetIndex));
                    UInt32Value targetValue =
                    FRAME_ENTRY(SP, targetIndex).uint32;
                    // binary search
                    UInt32Value* start = PC;
                    UInt32Value* end = PC + ((casesCount - 1) * WORDS_OF_CASE);
                    // default
                    PC = (UInt32Value*)(getQuadByte(end + WORDS_OF_CASE));
                    while(start <= end)
                    {
                        /* CAUTION: To calculate center, simply
                         *    start + (end - start) / 2
                         * is wrong.
                         * Division by WORDS_OF_CASE and multiplicaton by
                         * WORDS_OF_CASE are necessary to truncate address to
                         * WORDS_OF_CASE boundary.
                         */
                        UInt32Value* center =
                        start
                        + ((((end - start) / WORDS_OF_CASE) // number of elems
                            / 2)  // index of the center
                           * WORDS_OF_CASE); // number of words to the center
                        ASSERT(0 == ((center - start) % WORDS_OF_CASE));
                        /* NOTE: constant is regarded as unsigned, even for
                         *      SwitchInt. Compiler must take care of this.
                         */
                        UInt32Value constant = *center;
                        if (targetValue < constant){
                            end = center - WORDS_OF_CASE;
                        }
                        else if(constant < targetValue){
                            start = center + WORDS_OF_CASE;
                        }
                        else{
                            PC = (UInt32Value*)(getQuadByte(center + 1));
                            break;
                        }
                    }
/*
                    for(int index = 0; index < casesCount; index += 1){
                        UInt32Value constant = getWordAndInc(PC);
                        UInt32Value* destination =
                        (UInt32Value*)(getWordAndInc(PC));
                        if(constant == targetValue){
                            PC = destination;
                            goto SWITCH_TARGET_FOUND;
                        }
                    }
                    PC = (UInt32Value*)(getQuadByte(PC));
                  SWITCH_TARGET_FOUND:
*/
                    break;
                }
              case SwitchString:
                {
                    UInt32Value targetIndex = getWordAndInc(PC);
                    UInt32Value casesCount = getWordAndInc(PC);

                    ASSERT(FrameStack::isPointerSlot(SP, targetIndex));
                    Cell* targetBlock = FRAME_ENTRY(SP, targetIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(targetBlock));

                    int blockSize = Heap::getPayloadSize(targetBlock);
                    const UInt32Value targetLength =
                    HEAP_GETFIELD(targetBlock, blockSize - 1).uint32;
                    UInt32Value* targetBuffer =
                    (UInt32Value*)(&targetBlock[0]);

                    /* CAUTION : The binary search requires that list of
                     * constants is sorted in ascending order.
                     * This, then, requires that the compiler and the runtime
                     * agree about comparation method of string values.
                     * That is, String.compare and strcmp must return same
                     * order for all string pairs.
                     */
                    // binary search
                    UInt32Value* start = PC;
                    UInt32Value* end = PC + ((casesCount - 1) * WORDS_OF_CASE);
                    // default
                    PC = (UInt32Value*)(getQuadByte(end + WORDS_OF_CASE));
                    while(start <= end)
                    {
                        UInt32Value* center =
                        start
                        + ((((end - start) / WORDS_OF_CASE) // number of elems
                            / 2)  // index of the center
                           * WORDS_OF_CASE); // number of words to the center
                        ASSERT(0 == ((center - start) % WORDS_OF_CASE));

                        UInt32Value* constantAddress = (UInt32Value*)(*center);
                        UInt32Value constantLength;
                        UInt32Value* constantBuffer;
                        LoadConstString
                        (constantAddress, &constantLength, &constantBuffer);
                        int compare =
                        strcmp((char*)constantBuffer, (char*)targetBuffer);
                        if (0 < compare){// target < constant
                            end = center - WORDS_OF_CASE;
                        }
                        else if(compare < 0){// constant < target
                            start = center + WORDS_OF_CASE;
                        }
                        else{
                            PC = (UInt32Value*)(getQuadByte(center + 1));
                            break;
                        }
                    }
/*
                    for(int index = 0; index < casesCount; index += 1){
                        UInt32Value* constantAddress =
                        (UInt32Value*)getWordAndInc(PC);
                        UInt32Value* destination =
                        (UInt32Value*)(getWordAndInc(PC));
                        UInt32Value constantLength;
                        UInt32Value* constantBuffer;
                        LoadConstString
                        (constantAddress, &constantLength, &constantBuffer);

                        // compare string
                        if((targetLength == constantLength) &&
                           (0 ==
                            COMPARE_MEMORY
                            (targetBuffer, constantBuffer, constantLength)))
                        {
                            PC = destination;
                            goto SWITCH_STRING_TARGET_FOUND;
                        }
                    }
                    PC = (UInt32Value*)(getQuadByte(PC));
                  SWITCH_STRING_TARGET_FOUND:
*/
                    break;
                }
              case Jump:
                {
                    PC = (UInt32Value*)(getQuadByte(PC));
                    break;
                }
              case Exit:
                {
                    goto EXIT_LOOP;
                }
              case Return_S:
                {
                    UInt32Value variableIndex = getWordAndInc(PC);

                    UInt32Value* calleeSP = SP;
                    ASSERT_VALID_FRAME_VAR(SP, variableIndex);
                    Cell returnValue = FRAME_ENTRY(SP, variableIndex);
                    FrameStack::popFrameAndReturn(SP, PC);// SP,PC are updated.
                    FrameStack::loadENV(SP, ENV);

                    /* The destination operand of caller CallStatic/Apply
                       instruction is stored at the return address. */
                    UInt32Value destination = getWordAndInc(PC);
                    ASSERT_SAME_TYPE_SLOTS(calleeSP, variableIndex,
                                           SP, destination);
                    FRAME_ENTRY(SP, destination) = returnValue;
                    break;
                }
              case Return_D:
                {
                    UInt32Value variableIndex = getWordAndInc(PC);

                    UInt32Value *calleeSP = SP;
                    ASSERT_VALID_FRAME_VAR(SP, variableIndex);
                    ASSERT_VALID_FRAME_VAR(SP, variableIndex + 1);
                    Cell returnValue1 = FRAME_ENTRY(SP, variableIndex);
                    Cell returnValue2 = FRAME_ENTRY(SP, variableIndex + 1);
                    FrameStack::popFrameAndReturn(SP, PC);// SP,PC are updated.
                    FrameStack::loadENV(SP, ENV);

                    /* The destination operand of caller CallStatic/Apply
                       instruction is stored at the return address. */
                    UInt32Value destination = getWordAndInc(PC);
                    ASSERT_SAME_TYPE_SLOTS(calleeSP, variableIndex,
                                           SP, destination);
                    ASSERT_SAME_TYPE_SLOTS(calleeSP, variableIndex + 1,
                                           SP, destination + 1);

                    FRAME_ENTRY(SP, destination) = returnValue1;
                    FRAME_ENTRY(SP, destination + 1) = returnValue2;
                    break;
                }
              case Return_V:
                {
                    UInt32Value variableIndex = getWordAndInc(PC);
                    UInt32Value variableSizeIndex = getWordAndInc(PC);

                    ASSERT(!FrameStack::isPointerSlot(SP, variableSizeIndex));
                    UInt32Value variableSize =
                    FRAME_ENTRY(SP, variableSizeIndex).uint32;

                    Cell returnValues[variableSize];
                    for(int index = 0; index < variableSize; index += 1){
                        ASSERT_VALID_FRAME_VAR(SP, variableIndex + index);
                        returnValues[index] =
                        FRAME_ENTRY(SP, variableIndex + index);
                    }
                    UInt32Value *calleeSP = SP;
                    FrameStack::popFrameAndReturn(SP, PC);// SP,PC are updated.
                    FrameStack::loadENV(SP, ENV);

                    /* The destination operand of caller CallStatic/Apply
                       instruction is stored at the return address. */
                    UInt32Value destination = getWordAndInc(PC);
                    for(int index = 0; index < variableSize; index += 1){
                        ASSERT_SAME_TYPE_SLOTS(calleeSP, variableIndex + index,
                                               SP, destination + index);
                        FRAME_ENTRY(SP, destination + index) =
                        returnValues[index];
                    }
                    break;
                }
              case Nop:
                {
                    break;
                }
              case ForeignApply:
                {
                    UInt32Value closureIndex = getWordAndInc(PC);
                    UInt32Value argsCount = getWordAndInc(PC);
                    UInt32Value switchTag = getWordAndInc(PC);
                    UInt32Value convention = getWordAndInc(PC);
                    UInt32Value* argIndexes = PC;
                    PC += argsCount;
                    UInt32Value destination = getWordAndInc(PC);

                    void* function =
                      (void*)(FRAME_ENTRY(SP, closureIndex).uint32);
                    if(0 == function){
                        DBGWRAP(LOG.debug("null function pointer");)
                        throw FFIException("null function pointer");
                    }

		    PC_ = PC;
		    SP_ = SP;
		    ENV_ = ENV;

		    FFI::instance().call(&FRAME_ENTRY(SP, destination),
                                         function, SP,
                                         switchTag, convention,
                                         argIndexes, argsCount);

		    PC = PC_;
		    SP = SP_;
		    ENV = ENV_;

                    break;
                }

#define PRIMITIVE_I_I_op(op) \
            { \
                UInt32Value argIndex = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                FRAME_ENTRY(SP, destination).sint32 = \
                op (FRAME_ENTRY(SP, argIndex).sint32); \
                break; \
            } 
#define PRIMITIVE_W_W_op(op) \
            { \
                UInt32Value argIndex = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                FRAME_ENTRY(SP, destination).uint32 = \
                op (FRAME_ENTRY(SP, argIndex).uint32); \
                break; \
            } 
#ifdef FLOAT_UNBOXING
/*
#define PRIMITIVE_R_R_op(op) \
            { \
                UInt32Value argIndex = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex)); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, destination)); \
                Real64Value arg = \
                  PrimitiveSupport::WORDS_TO_REAL64 \
                                 (FRAME_ENTRY(SP, argIndex).uint32, \
                                  FRAME_ENTRY(SP, argIndex + 1).uint32); \
                Real64Value result = op (arg); \
                PrimitiveSupport::REAL64_TO_WORDS \
                               (result, \
                                &FRAME_ENTRY(SP, destination).uint32, \
                                &FRAME_ENTRY(SP, destination + 1).uint32); \
                break; \
            }
*/
#define PRIMITIVE_R_R_op(op) \
            { \
                UInt32Value argIndex = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex)); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, destination)); \
                Real64Value arg = \
                  *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, argIndex)); \
                Real64Value result = op (arg); \
                *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, destination)) = \
                  result; \
                break; \
            } 
#else // FLOAT_UNBOXING
#define PRIMITIVE_R_R_op(op) \
            { \
                UInt32Value argIndex = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                Real64Value arg = \
                  BLOCK_TO_REAL64(FRAME_ENTRY(SP, argIndex).blockRef); \
                Real64Value result = op (arg); \
                FRAME_ENTRY(SP, destination).blockRef = \
                  REAL64_TO_BLOCK(result); \
                break; \
            } 
#endif // FLOAT_UNBOXING
#define PRIMITIVE_II_I_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                FRAME_ENTRY(SP, destination).sint32 = \
                FRAME_ENTRY(SP, argIndex1).sint32 \
                op FRAME_ENTRY(SP, argIndex2).sint32; \
                break; \
            } 
#define PRIMITIVE_II_I_Const_1_op(op) \
            { \
                SInt32Value arg1 = (SInt32Value)getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                FRAME_ENTRY(SP, destination).sint32 = \
                arg1 op FRAME_ENTRY(SP, argIndex2).sint32; \
                break; \
            } 
#define PRIMITIVE_II_I_Const_2_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                SInt32Value arg2 = (SInt32Value)getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                FRAME_ENTRY(SP, destination).sint32 = \
                FRAME_ENTRY(SP, argIndex1).sint32 op arg2; \
                break; \
            } 
#define PRIMITIVE_WW_W_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                FRAME_ENTRY(SP, destination).uint32 = \
                FRAME_ENTRY(SP, argIndex1).uint32 \
                op FRAME_ENTRY(SP, argIndex2).uint32; \
                break; \
            } 
#define PRIMITIVE_WW_W_fun(f) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                FRAME_ENTRY(SP, destination).uint32 = \
                f(FRAME_ENTRY(SP, argIndex1).uint32, \
                  FRAME_ENTRY(SP, argIndex2).uint32); \
                break; \
            } 
#define PRIMITIVE_WW_W_Const_1_op(op) \
            { \
                UInt32Value arg1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                FRAME_ENTRY(SP, destination).uint32 = \
                arg1 op FRAME_ENTRY(SP, argIndex2).uint32; \
                break; \
            } 
#define PRIMITIVE_WW_W_Const_1_fun(f) \
            { \
                UInt32Value arg1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                FRAME_ENTRY(SP, destination).uint32 = \
                f(arg1, FRAME_ENTRY(SP, argIndex2).uint32); \
                break; \
            } 
#define PRIMITIVE_WW_W_Const_2_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value arg2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                FRAME_ENTRY(SP, destination).uint32 = \
                FRAME_ENTRY(SP, argIndex1).uint32 op arg2; \
                break; \
            } 
#define PRIMITIVE_WW_W_Const_2_fun(f) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value arg2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                FRAME_ENTRY(SP, destination).uint32 = \
                f(FRAME_ENTRY(SP, argIndex1).uint32, arg2); \
                break; \
            } 
#ifdef FLOAT_UNBOXING
#define PRIMITIVE_RR_R_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex1)); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex2)); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, destination)); \
                Real64Value arg1 = \
                  *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, argIndex1)); \
                Real64Value arg2 = \
                  *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, argIndex2)); \
                Real64Value result = arg1 op arg2; \
                *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, destination)) = \
                  result; \
                break; \
            } 
#define PRIMITIVE_RR_R_Const_1_op(op) \
            { \
                Real64Value arg1 = LoadConstReal64(PC); \
                PC += 2; \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex2)); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, destination)); \
                Real64Value arg2 = \
                  *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, argIndex2)); \
                Real64Value result = arg1 op arg2; \
                *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, destination)) = \
                  result; \
                break; \
            } 
#define PRIMITIVE_RR_R_Const_2_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                Real64Value arg2 = LoadConstReal64(PC); \
                PC += 2; \
                UInt32Value destination = getWordAndInc(PC); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex1)); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, destination)); \
                Real64Value arg1 = \
                  *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, argIndex1)); \
                Real64Value result = arg1 op arg2; \
                *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, destination)) = \
                  result; \
                break; \
            } 
#else
#define PRIMITIVE_RR_R_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                Real64Value arg1 = \
                  BLOCK_TO_REAL64(FRAME_ENTRY(SP, argIndex1).blockRef); \
                Real64Value arg2 = \
                  BLOCK_TO_REAL64(FRAME_ENTRY(SP, argIndex2).blockRef); \
                Real64Value result = arg1 op arg2; \
                FRAME_ENTRY(SP, destination).blockRef = \
                  REAL64_TO_BLOCK(result); \
                break; \
            } 
#endif // FLOAT_UNBOXING
#define PRIMITIVE_II_B_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                bool result = \
                     FRAME_ENTRY(SP, argIndex1).sint32 \
                     op FRAME_ENTRY(SP, argIndex2).sint32; \
                FRAME_ENTRY(SP, destination) = \
                  PrimitiveSupport::boolToCell(result); \
                break; \
            } 
#define PRIMITIVE_II_B_Const_1_op(op) \
            { \
                SInt32Value arg1 = (SInt32Value)getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                bool result = arg1 op FRAME_ENTRY(SP, argIndex2).sint32; \
                FRAME_ENTRY(SP, destination) = \
                  PrimitiveSupport::boolToCell(result); \
                break; \
            } 
#define PRIMITIVE_II_B_Const_2_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                SInt32Value arg2 = (SInt32Value)getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                bool result = FRAME_ENTRY(SP, argIndex1).sint32 op arg2 \
                FRAME_ENTRY(SP, destination) = \
                  PrimitiveSupport::boolToCell(result); \
                break; \
            } 
#define PRIMITIVE_WW_B_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                bool result = \
                     FRAME_ENTRY(SP, argIndex1).uint32 \
                     op FRAME_ENTRY(SP, argIndex2).uint32; \
                FRAME_ENTRY(SP, destination) = \
                  PrimitiveSupport::boolToCell(result); \
                break; \
            } 
#define PRIMITIVE_WW_B_Const_1_op(op) \
            { \
                UInt32Value arg1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                bool result = arg1 op FRAME_ENTRY(SP, argIndex2).uint32; \
                FRAME_ENTRY(SP, destination) = \
                  PrimitiveSupport::boolToCell(result); \
                break; \
            } 
#define PRIMITIVE_WW_B_Const_2_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value arg2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                bool result = FRAME_ENTRY(SP, argIndex1).uint32 op arg2 \
                FRAME_ENTRY(SP, destination) = \
                  PrimitiveSupport::boolToCell(result); \
                break; \
            } 
#ifdef FLOAT_UNBOXING
#define PRIMITIVE_RR_B_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex1)); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex2)); \
                Real64Value arg1 = \
                  *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, argIndex1)); \
                Real64Value arg2 = \
                  *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, argIndex2)); \
                bool result = arg1 op arg2; \
                FRAME_ENTRY(SP, destination) = \
                  PrimitiveSupport::boolToCell(result); \
                break; \
            } 
#define PRIMITIVE_RR_B_Const_1_op(op) \
            { \
                Real64Value arg1 = LoadConstReal64(PC); \
                PC += 2; \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex2)); \
                Real64Value arg2 = \
                  *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, argIndex2)); \
                bool result = arg1 op arg2; \
                FRAME_ENTRY(SP, destination) = \
                  PrimitiveSupport::boolToCell(result); \
                break; \
            } 
#define PRIMITIVE_RR_B_Const_2_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                Real64Value arg2 = LoadConstReal64(PC); \
                PC += 2; \
                UInt32Value destination = getWordAndInc(PC); \
                ASSERT_REAL64_ALIGNED(FRAME_ENTRY_ADDRESS(SP, argIndex1)); \
                Real64Value arg1 = \
                  *(Real64Value*)(FRAME_ENTRY_ADDRESS(SP, argIndex1)); \
                bool result = arg1 op arg2; \
                FRAME_ENTRY(SP, destination) = \
                  PrimitiveSupport::boolToCell(result); \
                break; \
            } 
#else // FLOAT_UNBOXING
#define PRIMITIVE_RR_B_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                Real64Value arg1 = \
                  BLOCK_TO_REAL64(FRAME_ENTRY(SP, argIndex1).blockRef); \
                Real64Value arg2 = \
                  BLOCK_TO_REAL64(FRAME_ENTRY(SP, argIndex2).blockRef); \
                bool result = arg1 op arg2; \
                FRAME_ENTRY(SP, destination) = \
                  PrimitiveSupport::boolToCell(result); \
                break; \
            } 
#endif // FLOAT_UNBOXING
#define PRIMITIVE_COMPARE_SS_B_op(op) \
            { \
                UInt32Value argIndex1 = getWordAndInc(PC); \
                UInt32Value argIndex2 = getWordAndInc(PC); \
                UInt32Value destination = getWordAndInc(PC); \
                const char* arg1 = \
                  PrimitiveSupport::cellToString(FRAME_ENTRY(SP, argIndex1)); \
                const char* arg2 = \
                  PrimitiveSupport::cellToString(FRAME_ENTRY(SP, argIndex2)); \
                bool result = strcmp(arg1, arg2) op; \
                FRAME_ENTRY(SP, destination) = \
                  PrimitiveSupport::boolToCell(result); \
                break; \
            } 

              case Equal:
                {
                    UInt32Value arg1Index = getWordAndInc(PC);
                    UInt32Value arg2Index = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT_SAME_TYPE_SLOTS(SP, arg1Index, SP, arg2Index);
                    Cell arg1Value = FRAME_ENTRY(SP, arg1Index);
                    Cell arg2Value = FRAME_ENTRY(SP, arg2Index);

                    bool isEqual;
                    if(arg1Value.uint32 == arg2Value.uint32){ isEqual = true; }
                    else if(FrameStack::isPointerSlot(SP, arg1Index)){
                        isEqual =
                        Heap::isSimilarBlockGraph
                        (arg1Value.blockRef, arg2Value.blockRef);
                    }
                    else{ isEqual = false; }

                    FRAME_ENTRY(SP, destination) =
                    PrimitiveSupport::boolToCell(isEqual);
                    break;
                }
              case AddInt: PRIMITIVE_II_I_op(+);
              case AddInt_Const_1: PRIMITIVE_II_I_Const_1_op(+);
              case AddInt_Const_2: PRIMITIVE_II_I_Const_2_op(+);
              case AddReal: PRIMITIVE_RR_R_op(+);
              case AddReal_Const_1: PRIMITIVE_RR_R_Const_1_op(+);
              case AddReal_Const_2: PRIMITIVE_RR_R_Const_2_op(+);
              case AddWord: PRIMITIVE_WW_W_op(+);
              case AddWord_Const_1: PRIMITIVE_WW_W_Const_1_op(+);
              case AddWord_Const_2: PRIMITIVE_WW_W_Const_2_op(+);
              case AddByte: PRIMITIVE_WW_W_op(+);
              case AddByte_Const_1: PRIMITIVE_WW_W_Const_1_op(+);
              case AddByte_Const_2: PRIMITIVE_WW_W_Const_2_op(+);
              case SubInt: PRIMITIVE_II_I_op(-);
              case SubInt_Const_1: PRIMITIVE_II_I_Const_1_op(-);
              case SubInt_Const_2: PRIMITIVE_II_I_Const_2_op(-);
              case SubReal: PRIMITIVE_RR_R_op(-);
              case SubReal_Const_1: PRIMITIVE_RR_R_Const_1_op(-);
              case SubReal_Const_2: PRIMITIVE_RR_R_Const_2_op(-);
              case SubWord: PRIMITIVE_WW_W_op(-);
              case SubWord_Const_1: PRIMITIVE_WW_W_Const_1_op(-);
              case SubWord_Const_2: PRIMITIVE_WW_W_Const_2_op(-);
              case SubByte: PRIMITIVE_WW_W_op(-);
              case SubByte_Const_1: PRIMITIVE_WW_W_Const_1_op(-);
              case SubByte_Const_2: PRIMITIVE_WW_W_Const_2_op(-);
              case MulInt: PRIMITIVE_II_I_op(*);
              case MulInt_Const_1: PRIMITIVE_II_I_Const_1_op(*);
              case MulInt_Const_2: PRIMITIVE_II_I_Const_2_op(*);
              case MulReal: PRIMITIVE_RR_R_op(*);
              case MulReal_Const_1: PRIMITIVE_RR_R_Const_1_op(*);
              case MulReal_Const_2: PRIMITIVE_RR_R_Const_2_op(*);
              case MulWord: PRIMITIVE_WW_W_op(*);
              case MulWord_Const_1: PRIMITIVE_WW_W_Const_1_op(*);
              case MulWord_Const_2: PRIMITIVE_WW_W_Const_2_op(*);
              case MulByte: PRIMITIVE_WW_W_op(*);
              case MulByte_Const_1: PRIMITIVE_WW_W_Const_1_op(*);
              case MulByte_Const_2: PRIMITIVE_WW_W_Const_2_op(*);
              case DivInt: PRIMITIVE_II_I_op(/);
              case DivInt_Const_1: PRIMITIVE_II_I_Const_1_op(/);
              case DivInt_Const_2: PRIMITIVE_II_I_Const_2_op(/);
              case DivWord: PRIMITIVE_WW_W_op(/);
              case DivWord_Const_1: PRIMITIVE_WW_W_Const_1_op(/);
              case DivWord_Const_2: PRIMITIVE_WW_W_Const_2_op(/);
              case DivByte: PRIMITIVE_WW_W_op(/);
              case DivByte_Const_1: PRIMITIVE_WW_W_Const_1_op(/);
              case DivByte_Const_2: PRIMITIVE_WW_W_Const_2_op(/);
              case DivReal: PRIMITIVE_RR_R_op(/);
              case DivReal_Const_1: PRIMITIVE_RR_R_Const_1_op(/);
              case DivReal_Const_2: PRIMITIVE_RR_R_Const_2_op(/);
              case ModInt: PRIMITIVE_II_I_op(%);
              case ModInt_Const_1: PRIMITIVE_II_I_Const_1_op(%);
              case ModInt_Const_2: PRIMITIVE_II_I_Const_2_op(%);
              case ModWord: PRIMITIVE_WW_W_op(%);
              case ModWord_Const_1: PRIMITIVE_WW_W_Const_1_op(%);
              case ModWord_Const_2: PRIMITIVE_WW_W_Const_2_op(%);
              case ModByte: PRIMITIVE_WW_W_op(%);
              case ModByte_Const_1: PRIMITIVE_WW_W_Const_1_op(%);
              case ModByte_Const_2: PRIMITIVE_WW_W_Const_2_op(%);
                /* ToDo : implement */
              case QuotInt: PRIMITIVE_II_I_op(/);
              case QuotInt_Const_1: PRIMITIVE_II_I_Const_1_op(/);
              case QuotInt_Const_2: PRIMITIVE_II_I_Const_2_op(/);
              case RemInt: PRIMITIVE_II_I_op(%);
              case RemInt_Const_1: PRIMITIVE_II_I_Const_1_op(%);
              case RemInt_Const_2: PRIMITIVE_II_I_Const_2_op(%);
              case NegInt: PRIMITIVE_I_I_op(-);
              case NegReal: PRIMITIVE_R_R_op(-);
              case AbsInt: PRIMITIVE_I_I_op(ABS_SINT32);
              case AbsReal: PRIMITIVE_R_R_op(ABS_REAL64);
              case LtInt: PRIMITIVE_II_B_op(<);
              case LtReal: PRIMITIVE_RR_B_op(<);
              case LtWord: PRIMITIVE_WW_B_op(<);
              case LtByte: PRIMITIVE_WW_B_op(<);
              case LtChar: PRIMITIVE_WW_B_op(<);
              case LtString: PRIMITIVE_COMPARE_SS_B_op(< 0);
              case GtInt: PRIMITIVE_II_B_op(>);
              case GtReal: PRIMITIVE_RR_B_op(>);
              case GtWord: PRIMITIVE_WW_B_op(>);
              case GtByte: PRIMITIVE_WW_B_op(>);
              case GtChar: PRIMITIVE_WW_B_op(>);
              case GtString: PRIMITIVE_COMPARE_SS_B_op(> 0);
              case LteqInt: PRIMITIVE_II_B_op(<=);
              case LteqReal: PRIMITIVE_RR_B_op(<=);
              case LteqWord: PRIMITIVE_WW_B_op(<=);
              case LteqByte: PRIMITIVE_WW_B_op(<=);
              case LteqChar: PRIMITIVE_WW_B_op(<=);
              case LteqString: PRIMITIVE_COMPARE_SS_B_op(<= 0);
              case GteqInt: PRIMITIVE_II_B_op(>=);
              case GteqReal: PRIMITIVE_RR_B_op(>=);
              case GteqWord: PRIMITIVE_WW_B_op(>=);
              case GteqByte: PRIMITIVE_WW_B_op(>=);
              case GteqChar: PRIMITIVE_WW_B_op(>=);
              case GteqString: PRIMITIVE_COMPARE_SS_B_op(>= 0);
              case Word_toIntX: PRIMITIVE_W_W_op(+);// argument unchanged
              case Word_fromInt: PRIMITIVE_W_W_op(+);// argument unchanged
              case Word_andb: PRIMITIVE_WW_W_op(&);
              case Word_andb_Const_1: PRIMITIVE_WW_W_Const_1_op(&);
              case Word_andb_Const_2: PRIMITIVE_WW_W_Const_2_op(&);
              case Word_orb: PRIMITIVE_WW_W_op(|);
              case Word_orb_Const_1: PRIMITIVE_WW_W_Const_1_op(|);
              case Word_orb_Const_2: PRIMITIVE_WW_W_Const_2_op(|);
              case Word_xorb: PRIMITIVE_WW_W_op(^);
              case Word_xorb_Const_1: PRIMITIVE_WW_W_Const_1_op(^);
              case Word_xorb_Const_2: PRIMITIVE_WW_W_Const_2_op(^);
              case Word_notb: PRIMITIVE_W_W_op(~);
              case Word_leftShift: PRIMITIVE_WW_W_fun(leftShift);
              case Word_leftShift_Const_1:
                PRIMITIVE_WW_W_Const_1_fun(leftShift);
              case Word_leftShift_Const_2:
                PRIMITIVE_WW_W_Const_2_fun(leftShift);
              case Word_logicalRightShift:
                PRIMITIVE_WW_W_fun(logicalRightShift);
              case Word_logicalRightShift_Const_1:
                PRIMITIVE_WW_W_Const_1_fun(logicalRightShift);
              case Word_logicalRightShift_Const_2:
                PRIMITIVE_WW_W_Const_2_fun(logicalRightShift);
              case Word_arithmeticRightShift:
                PRIMITIVE_WW_W_fun(arithmeticRightShift);
              case Word_arithmeticRightShift_Const_1:
                PRIMITIVE_WW_W_Const_1_fun(arithmeticRightShift);
              case Word_arithmeticRightShift_Const_2:
                PRIMITIVE_WW_W_Const_2_fun(arithmeticRightShift);

              case Array_length:
                {
                    UInt32Value blockIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    ASSERT(FrameStack::isPointerSlot(SP, blockIndex));
                    Cell* block = FRAME_ENTRY(SP, blockIndex).blockRef;
                    ASSERT(Heap::isValidBlockPointer(block));

                    int blockSize = Heap::getPayloadSize(block);
                    int length;
                    switch(Heap::getBlockType(block)){
                      case Heap::BLOCKTYPE_SINGLE_ATOM_ARRAY:
                      case Heap::BLOCKTYPE_POINTER_ARRAY:
                        length = blockSize;
                        break;
                      case Heap::BLOCKTYPE_DOUBLE_ATOM_ARRAY:
                        length = blockSize >> 1;
                        break;
                      default:
                        // empty array is a unit block.
                        ASSERT(0 == blockSize);
                        length = 0;
                        break;
                    }

                    FRAME_ENTRY(SP, destination).sint32 = length;
                    break;
                }
              case CurrentIP: 
                {
                    UInt32Value argIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    Executable* currentExecutable =
                    FrameStack::getExecutableOfFrame(SP);

                    /*  constructs a 2-tuple consisting of a pointer to the
                     * executable and the offset of the current instruction
                     * from the beginning of the code block.
                     */
                    Cell elements[2];
                    elements[0].uint32 = (UInt32Value)currentExecutable;
                    // adjust by 3 (= words of operands).
                    elements[1].uint32 =
                    PC - (UInt32Value*)(currentExecutable->code_) - 3;

                    SAVE_REGISTERS;
                    Cell tuple =
                    PrimitiveSupport::tupleElementsToCell(elements, 2);
                    RESTORE_REGISTERS;

                    FRAME_ENTRY(SP, destination) = tuple;
                    break;
                }

              case StackTrace:
                {
                    UInt32Value argIndex = getWordAndInc(PC);
                    UInt32Value destination = getWordAndInc(PC);

                    SAVE_REGISTERS;
                    Cell list = PrimitiveSupport::constructListNil();
                    RESTORE_REGISTERS;

                    UInt32Value *cursorSP = SP;
                    while(cursorSP)
                    {
                        /*  get the instruction offset and the Executable of
                         * the call instruction which created the current
                         * frame. The offset of the call instruction is stored
                         * in the current (= callee) frame. The Executable is
                         * obtained in the upper (= caller) frame.
                         */
                        UInt32Value* returnAddress =
                        FrameStack::getReturnAddress(cursorSP);
                        if(RETURN_ADDRESS_OF_INITIAL_FRAME == returnAddress)
                        {
                            /* The initial frame does not have its return
                             * address. */
                            break;
                        }

                        cursorSP = FrameStack::getNextFrame(cursorSP);
                        ASSERT(cursorSP);

                        Executable* callerExecutable =
                        FrameStack::getExecutableOfFrame(cursorSP);

                        Cell elements[2];
                        elements[0].uint32 = (UInt32Value)callerExecutable;
                        elements[1].uint32 =
                        returnAddress - callerExecutable->code_;

                        SAVE_REGISTERS;
                        Cell tuple =
                        PrimitiveSupport::tupleElementsToCell(elements, 2);
                        list =
                        PrimitiveSupport::constructListCons(&tuple,
                                                            &list,
                                                            true);
                        RESTORE_REGISTERS;
                    }

                    FRAME_ENTRY(SP, destination) = list;
                    break;
                }

              default:
                // ToDo : IllegalInstructionException ?
                DBGWRAP(LOG.error("invalid opcode::IllegalArgumentException"));
                throw IllegalArgumentException();
            }
            INVOKE_ON_MONITORS(afterInstruction(PC, previousPC, ENV, SP));
        }
      EXIT_LOOP:
        resetSignalHandler();
        INVOKE_ON_MONITORS(afterExecution(PC, ENV, SP));
    }
    catch(IMLException &exception)
    {
        DBGWRAP(LOG.debug("instruction = %s",
                          instructionToString
                          (static_cast<instruction>
                           (*previousPC)));)

        interrupted_ = false;

        const char* what = exception.what();
        PrimitiveSupport::writeToSTDOUT(strlen(what), what);
        PrimitiveSupport::writeToSTDOUT(strlen("\n"), "\n");

        printStackTrace(PC, SP);

        resetSignalHandler();
        INVOKE_ON_MONITORS(afterExecution(PC, ENV, SP));
        DBGWRAP(LOG.debug("VM.executeLoop: throw uncaught IMLException.");)

	PC_ = PC;
	SP_ = SP;
	ENV_ = ENV;
        throw;
    }
    catch(...) {
	PC_ = PC;
	SP_ = SP;
	ENV_ = ENV;
        throw;
    }
    PC_ = PC;
    SP_ = SP;
    ENV_ = ENV;
}

void
VirtualMachine::signalHandler(int signal)
{
    DBGWRAP(LOG.debug("SIGNAL caught: %d", signal));
    switch(signal){
      case SIGINT:
        interrupted_ = true;
        break;
      case SIGFPE:
        longjmp(onSIGFPE_jmp_buf, signal);
        break;// never reach here
#if defined(SIGPIPE)
      case SIGPIPE:// ignore
        break;
#endif
      default:
        ASSERT(false);
    }
}

void
VirtualMachine::setSignalHandler()
{
    /* Note : It is better to use sigaction, not signal.
     *        But MinGW does not provide sigaction.
     */
    prevSIGINTHandler_ = signal(SIGINT, &signalHandler);
    prevSIGFPEHandler_ = signal(SIGFPE, &signalHandler);
#if defined(SIGPIPE)
    prevSIGPIPEHandler_ = signal(SIGPIPE, &signalHandler);
#endif
}

void
VirtualMachine::resetSignalHandler()
{
    signal(SIGINT, prevSIGINTHandler_);
    signal(SIGFPE, prevSIGFPEHandler_);
#if defined(SIGPIPE)
    signal(SIGPIPE, prevSIGPIPEHandler_);
#endif
}

void
VirtualMachine::trace(RootTracer* tracer)
    throw(IMLRuntimeException)
{
    // registers
    if(ENV_){
        // At the beginning of the execution, ENV may be NULL.
        ENV_ = tracer->trace(ENV_);
    }

    // walk through stack frames
    FrameStack::trace(tracer, SP_);

    // global arrays
    // some elements in globalArrays might hold NULL pointer.
    for(BlockPointerVector::iterator i = globalArrays_.begin();
        i != globalArrays_.end();
        i++)
    {
        if(*i){ *i = tracer->trace(*i); }
    }

    for(BlockPointerRefList::iterator i = temporaryPointers_.begin();
        i != temporaryPointers_.end();
        i++)
    {
        **i = tracer->trace(**i);
    }

    if(isPrimitiveExceptionRaised_){
        primitiveException_.blockRef =
            tracer->trace(primitiveException_.blockRef);
    }

    FFI::instance().trace(tracer);
}

void
VirtualMachine::executeFinalizer(Cell* finalizable)
        throw(IMLRuntimeException)
{
    /* A finalizable has a type of
     * 'a ref * ('a ref -> unit) ref
     */
    ASSERT(2 == Heap::getPayloadSize(finalizable));
    ASSERT(FINALIZABLE_BITMAP == Heap::getBitmap(finalizable));
    Cell elements[2];
    PrimitiveSupport::blockToTupleElements(finalizable, elements, 2);

    Cell* arg = elements[0].blockRef;// 'a ref
    ASSERT(Heap::isValidBlockPointer(arg));

    Cell* closureRef = elements[1].blockRef;// 'a ref -> unit
    Cell closure = closureRef[0];
    ASSERT(Heap::isValidBlockPointer(closure.blockRef));
    ASSERT(CLOSURE_FIELDS_COUNT == Heap::getPayloadSize(closure.blockRef));
    ASSERT(CLOSURE_BITMAP == Heap::getBitmap(closure.blockRef));

    DBGWRAP(LOG.debug("begin finalizer."));
    /* save all registers. not necessary ? */
    Cell* originalENV = ENV_;
    UInt32Value* originalSP = SP_;
    UInt32Value* originalPC = PC_;
    /* execute finalizer */
    UInt32Value result = executeClosure_PA(Ignore, closure, arg);

    ENV_ = originalENV;
    SP_ = originalSP;
    PC_ = originalPC;
    DBGWRAP(LOG.debug("finalizer returned."));
}

///////////////////////////////////////////////////////////////////////////////

    DBGWRAP(LogAdaptor VirtualMachine::LOG =
            LogAdaptor("VirtualMachine"));

///////////////////////////////////////////////////////////////////////////////

END_NAMESPACE