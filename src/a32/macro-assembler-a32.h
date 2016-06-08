// Copyright 2015, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may
//     be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef VIXL_A32_MACRO_ASSEMBLER_A32_H_
#define VIXL_A32_MACRO_ASSEMBLER_A32_H_

#include "utils-vixl.h"
#include "a32/instructions-a32.h"
#include "a32/assembler-a32.h"
#include "a32/operand-a32.h"

namespace vixl {
namespace aarch32 {

class JumpTableBase;

// LiteralPool class, defined as a container for literals
class LiteralPool {
 public:
  typedef std::list<RawLiteral*>::iterator RawLiteralListIterator;

 public:
  LiteralPool() : size_(0) {}
  ~LiteralPool() {
    VIXL_ASSERT(literals_.empty() && (size_ == 0));
    for (RawLiteralListIterator literal_it = keep_until_delete_.begin();
         literal_it != keep_until_delete_.end();
         literal_it++) {
      delete *literal_it;
    }
    keep_until_delete_.clear();
  }

  unsigned GetSize() const { return size_; }

  // Add a literal to the literal container.
  uint32_t AddLiteral(RawLiteral* literal) {
    uint32_t position = GetSize();
    literal->SetPositionInPool(position);
    literals_.push_back(literal);
    size_ += literal->GetAlignedSize();
    return position;
  }

  // First literal to be emitted.
  RawLiteralListIterator GetFirst() { return literals_.begin(); }

  // Mark the end of the literal container.
  RawLiteralListIterator GetEnd() { return literals_.end(); }

  // Remove all the literals from the container.
  // If the literal's memory management has been delegated to the container
  // it will be delete'd.
  void Clear() {
    for (RawLiteralListIterator literal_it = GetFirst(); literal_it != GetEnd();
         literal_it++) {
      RawLiteral* literal = *literal_it;
      switch (literal->GetDeletionPolicy()) {
        case RawLiteral::kDeletedOnPlacementByPool:
          delete literal;
          break;
        case RawLiteral::kDeletedOnPoolDestruction:
          keep_until_delete_.push_back(literal);
          break;
        case RawLiteral::kManuallyDeleted:
          break;
      }
    }
    literals_.clear();
    size_ = 0;
  }

 private:
  // Size (in bytes and including alignments) of the literal pool.
  unsigned size_;

  // Literal container.
  std::list<RawLiteral*> literals_;
  // Already bound Literal container the app requested this pool to keep.
  std::list<RawLiteral*> keep_until_delete_;
};

// TODO(all): Add MightSetFlags to determine best sequence to emit.
enum FlagsUpdate { SetFlags = 1, LeaveFlags = 0 };

// Macro assembler for aarch32 instruction set.
class MacroAssembler : public Assembler {
 public:
  enum EmitOption { kBranchRequired, kNoBranchRequired };

 private:
  class MacroAssemblerContext {
   public:
    MacroAssemblerContext() : count_(0) {}
    ~MacroAssemblerContext() {}
    unsigned GetRecursiveCount() const { return count_; }
    void Up() {
      count_++;
      VIXL_CHECK(count_ < kMaxRecursion);
    }
    void Down() {
      VIXL_ASSERT((count_ > 0) && (count_ < kMaxRecursion));
      count_--;
    }

   private:
    unsigned count_;
    static const uint32_t kMaxRecursion = 5;
  };

  class ContextScope {
   public:
    explicit ContextScope(MacroAssembler* const masm) : masm_(masm) {
      VIXL_ASSERT(masm_->AllowMacroInstructions());
      masm_->GetContext()->Up();
    }
    ~ContextScope() { masm_->GetContext()->Down(); }

   private:
    MacroAssembler* const masm_;
  };

  MacroAssemblerContext* GetContext() { return &context_; }

  class ITScope {
   public:
    ITScope(MacroAssembler* masm, Condition* cond, bool can_use_it = false)
        : masm_(masm), cond_(*cond), can_use_it_(can_use_it) {
      if (!cond_.Is(al) && masm->IsT32()) {
        if (can_use_it_) {
          // IT is not deprecated (that implies a 16 bit T32 instruction).
          // We generate an IT instruction and a conditional instruction.
          masm->it(cond_);
        } else {
          // The usage of IT is deprecated for the instruction.
          // We generate a conditional branch and an unconditional instruction.
          masm_->EnsureEmitFor(k16BitT32InstructionSizeInBytes +
                               kMaxT32MacroInstructionSizeInBytes);
          // Generate the branch.
          masm_->b(cond_.Negate(), Narrow, &label_);
          // Tell the macro-assembler to generate unconditional instructions.
          *cond = al;
        }
      }
#ifdef VIXL_DEBUG
      initial_cursor_offset_ = masm->GetCursorOffset();
#endif
    }
    ~ITScope() {
      if (!cond_.Is(al) && masm_->IsT32() && !can_use_it_) {
        VIXL_ASSERT(masm_->GetCursorOffset() - initial_cursor_offset_ <=
                    kMaxT32MacroInstructionSizeInBytes);
        masm_->bind(&label_);
      }
    }

   private:
    MacroAssembler* masm_;
    Condition cond_;
    Label label_;
    bool can_use_it_;
#ifdef VIXL_DEBUG
    uint32_t initial_cursor_offset_;
#endif
  };

  template <Assembler::InstructionCondDtDL asmfn>
  class EmitLiteralCondDtDL {
   public:
    EmitLiteralCondDtDL(Condition cond, DataType dt, DRegister rt)
        : cond_(cond), dt_(dt), rt_(rt) {}
    void emit(MacroAssembler* const masm, RawLiteral* const literal) {
      (masm->*asmfn)(cond_, dt_, rt_, literal);
    }

   private:
    Condition cond_;
    DataType dt_;
    DRegister rt_;
  };

  template <Assembler::InstructionCondDtSL asmfn>
  class EmitLiteralCondDtSL {
   public:
    EmitLiteralCondDtSL(Condition cond, DataType dt, SRegister rt)
        : cond_(cond), dt_(dt), rt_(rt) {}
    void emit(MacroAssembler* const masm, RawLiteral* const literal) {
      (masm->*asmfn)(cond_, dt_, rt_, literal);
    }

   private:
    Condition cond_;
    DataType dt_;
    SRegister rt_;
  };

  template <Assembler::InstructionCondRL asmfn>
  class EmitLiteralCondRL {
   public:
    EmitLiteralCondRL(Condition cond, Register rt) : cond_(cond), rt_(rt) {}
    void emit(MacroAssembler* const masm, RawLiteral* const literal) {
      (masm->*asmfn)(cond_, rt_, literal);
    }

   private:
    Condition cond_;
    Register rt_;
  };

  template <Assembler::InstructionCondRRL asmfn>
  class EmitLiteralCondRRL {
   public:
    EmitLiteralCondRRL(Condition cond, Register rt, Register rt2)
        : cond_(cond), rt_(rt), rt2_(rt2) {}
    void emit(MacroAssembler* const masm, RawLiteral* const literal) {
      (masm->*asmfn)(cond_, rt_, rt2_, literal);
    }

   private:
    Condition cond_;
    Register rt_, rt2_;
  };

  class LiteralPoolManager {
   public:
    explicit LiteralPoolManager(MacroAssembler* const masm) : masm_(masm) {
      ResetCheckpoint();
    }

    void ResetCheckpoint() { checkpoint_ = Label::kMaxOffset; }

    LiteralPool* GetLiteralPool() { return &literal_pool_; }
    Label::Offset GetCheckpoint() const {
      // Make room for a branch over the pools.
      return checkpoint_ - kMaxInstructionSizeInBytes;
    }
    size_t GetLiteralPoolSize() const { return literal_pool_.GetSize(); }

    // Checks if the insertion of the literal will put the forward reference
    // too far in the literal pool.
    bool IsInsertTooFar(RawLiteral* literal, uint32_t from) const {
      uint32_t checkpoint = from + literal->GetLastInsertForwardDistance();
      checkpoint =
          std::min(checkpoint, static_cast<uint32_t>(literal->GetCheckpoint()));
      bool too_far = AlignDown(checkpoint, 4) < from + literal_pool_.GetSize() +
                                                    kMaxInstructionSizeInBytes;
      return too_far;
    }

    // Set the different checkpoints where the literal pool has to be emited.
    void UpdateCheckpoint(RawLiteral* literal) {
      // The literal should have been placed somewhere in the literal pool
      VIXL_ASSERT(literal->GetPositionInPool() != Label::kMaxOffset);
      // TODO(all): Consider AddForwardRef as a  virtual so the checkpoint is
      //   updated when inserted. Or move checkpoint_ into Label,
      literal->UpdateCheckpoint();
      Label::Offset tmp =
          literal->GetAlignedCheckpoint(4) - literal->GetPositionInPool();
      if (checkpoint_ > tmp) {
        checkpoint_ = tmp;
        masm_->ComputeCheckpoint();
      }
    }

    // Inserts the literal in the pool, and update the different checkpoints.
    void AddLiteral(RawLiteral* literal) { literal_pool_.AddLiteral(literal); }

   private:
    MacroAssembler* const masm_;
    LiteralPool literal_pool_;

    // Max offset in the code buffer where the literal needs to be
    // emitted. A default value of Label::kMaxOffset means that the checkpoint
    // is invalid.
    Label::Offset checkpoint_;
  };

  class VeneerPoolManager {
   public:
    explicit VeneerPoolManager(MacroAssembler* masm)
        : masm_(masm), checkpoint_(Label::kMaxOffset) {}
    Label::Offset GetCheckpoint() const {
      // Make room for a branch over the pools.
      return checkpoint_ - kMaxInstructionSizeInBytes;
    }
    size_t GetMaxSize() const {
      return labels_.size() * kMaxInstructionSizeInBytes;
    }
    void AddLabel(Label* label) {
      if (!label->IsInVeneerPool()) {
        label->SetInVeneerPool();
        labels_.push_back(label);
      }
      Label::ForwardReference& back = label->GetBackForwardRef();
      back.SetIsBranch();
      label->UpdateCheckpoint();
      Label::Offset tmp = label->GetCheckpoint();
      if (checkpoint_ > tmp) {
        checkpoint_ = tmp;
        masm_->ComputeCheckpoint();
      }
    }
    void RemoveLabel(Label* label);
    void Emit(Label::Offset target);

   private:
    MacroAssembler* masm_;
    // List of all unbound labels which are used by a branch instruction.
    std::list<Label*> labels_;
    // Max offset in the code buffer where the veneer needs to be emitted.
    // A default value of Label::kMaxOffset means that the checkpoint is
    // invalid.
    Label::Offset checkpoint_;
  };

  void PerformEnsureEmit(Label::Offset target, uint32_t extra_size);

 protected:
  void HandleOutOfBoundsImmediate(Condition cond, Register tmp, uint32_t imm);

  // Generate the instruction and if it's not possible revert the whole thing.
  // emit the literal pool and regenerate the instruction.
  // Note: The instruction is generated via
  // void T::emit(MacroAssembler* const, RawLiteral* const)
  template <typename T>
  void GenerateInstruction(T instr_callback, RawLiteral* const literal) {
    ptrdiff_t cursor = GetBuffer().GetCursorOffset();
    uint32_t where = cursor + GetArchitectureStatePCOffset();
    // Emit the instruction, via the assembler
    instr_callback.emit(this, literal);
    if (IsInsertTooFar(literal, where)) {
      // The instruction's data is too far: revert the emission
      GetBuffer().Rewind(cursor);
      literal->InvalidateLastForwardReference(RawLiteral::kNoUpdateNecessary);
      EmitLiteralPool(kBranchRequired);
      instr_callback.emit(this, literal);
    }
    if (literal->GetPositionInPool() == Label::kMaxOffset) {
      literal_pool_manager_.AddLiteral(literal);
    }
    literal_pool_manager_.UpdateCheckpoint(literal);
  }

 public:
  MacroAssembler()
      : available_(r12),
        checkpoint_(Label::kMaxOffset),
        literal_pool_manager_(this),
        veneer_pool_manager_(this) {
#ifdef VIXL_DEBUG
    SetAllowMacroInstructions(true);
#endif
    ComputeCheckpoint();
  }
  explicit MacroAssembler(size_t size)
      : Assembler(size),
        available_(r12),
        checkpoint_(Label::kMaxOffset),
        literal_pool_manager_(this),
        veneer_pool_manager_(this) {
#ifdef VIXL_DEBUG
    SetAllowMacroInstructions(true);
#endif
    ComputeCheckpoint();
  }
  MacroAssembler(void* buffer, size_t size)
      : Assembler(buffer, size),
        available_(r12),
        checkpoint_(Label::kMaxOffset),
        literal_pool_manager_(this),
        veneer_pool_manager_(this) {
#ifdef VIXL_DEBUG
    SetAllowMacroInstructions(true);
#endif
    ComputeCheckpoint();
  }

#ifdef VIXL_DEBUG
  // Tell whether any of the macro instruction can be used. When false the
  // MacroAssembler will assert if a method which can emit a variable number
  // of instructions is called.
  void SetAllowMacroInstructions(bool value) {
    allow_macro_instructions_ = value;
  }
  bool AllowMacroInstructions() const { return allow_macro_instructions_; }
#endif

  void FinalizeCode() {
    EmitLiteralPool(kNoBranchRequired);
    Assembler::FinalizeCode();
  }

  RegisterList* GetScratchRegisterList() { return &available_; }
  VRegisterList* GetScratchVRegisterList() { return &available_vfp_; }

  // State and type helpers.
  bool IsModifiedImmediate(uint32_t imm) {
    return (IsT32() && ImmediateT32(imm).IsValid()) ||
           ImmediateA32(imm).IsValid();
  }

  void Bind(Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    bind(label);
    if (label->IsInVeneerPool()) veneer_pool_manager_.RemoveLabel(label);
  }

  void AddBranchLabel(Label* label) {
    if (label->IsBound()) return;
    veneer_pool_manager_.AddLabel(label);
  }

  void Place(RawLiteral* literal) {
    VIXL_ASSERT(allow_macro_instructions_);
    EnsureEmitFor(literal->GetSize());
    place(literal);
  }

  void ComputeCheckpoint();

  void EnsureEmitFor(uint32_t size = 0) {
    Label::Offset target = AlignUp(GetCursorOffset() + size, 4);
    if (target < checkpoint_) return;
    PerformEnsureEmit(target, size);
  }

  bool IsInsertTooFar(RawLiteral* literal, uint32_t where) {
    return literal_pool_manager_.IsInsertTooFar(literal, where);
  }

  // Emit the literal pool in the code buffer.
  // Every literal is placed on a 32bit boundary
  // All the literals in the pool will be removed from the pool and potentially
  // delete'd.
  void EmitLiteralPool(LiteralPool* const literal_pool, EmitOption option) {
    if (literal_pool->GetSize() > 0) {
#ifdef VIXL_DEBUG
      for (LiteralPool::RawLiteralListIterator literal_it =
               literal_pool->GetFirst();
           literal_it != literal_pool->GetEnd();
           literal_it++) {
        RawLiteral* literal = *literal_it;
        VIXL_ASSERT(GetCursorOffset() <
                    static_cast<uint32_t>(literal->GetCheckpoint()));
      }
#endif
      Label after_literal;
      if (option == kBranchRequired) {
        GetBuffer().EnsureSpaceFor(kMaxInstructionSizeInBytes);
        b(&after_literal);
      }
      GetBuffer().Align();
      GetBuffer().EnsureSpaceFor(literal_pool->GetSize());
      for (LiteralPool::RawLiteralListIterator it = literal_pool->GetFirst();
           it != literal_pool->GetEnd();
           it++) {
        place(*it);
      }
      if (option == kBranchRequired) bind(&after_literal);
      literal_pool->Clear();
    }
  }
  void EmitLiteralPool(EmitOption option = kBranchRequired) {
    EmitLiteralPool(literal_pool_manager_.GetLiteralPool(), option);
    literal_pool_manager_.ResetCheckpoint();
    ComputeCheckpoint();
  }

  unsigned GetLiteralPoolSize() const {
    return literal_pool_manager_.GetLiteralPoolSize();
  }

  // Add a Literal to the internal literal pool
  void AddLiteral(RawLiteral* literal) {
    return literal_pool_manager_.AddLiteral(literal);
  }

  // Generic Ldr(register, data)
  void Ldr(Condition cond, Register rt, uint32_t v) {
    RawLiteral* literal =
        new Literal<uint32_t>(v, RawLiteral::kDeletedOnPlacementByPool);
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    EmitLiteralCondRL<&Assembler::ldr> emit_helper(cond, rt);
    GenerateInstruction(emit_helper, literal);
  }
  // Ldr string pointer. The string is added to the literal pool and the
  // string's address in the literal pool is loaded in rt (adr instruction).
  void Ldr(Condition cond, Register rt, const char* str) {
    RawLiteral* literal =
        new Literal<const char*>(str, RawLiteral::kDeletedOnPlacementByPool);
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    EmitLiteralCondRL<&Assembler::adr> emit_helper(cond, rt);
    GenerateInstruction(emit_helper, literal);
  }
  template <typename T>
  void Ldr(Register rt, T v) {
    Ldr(al, rt, v);
  }

  // Generic Ldrd(rt, rt2, data)
  void Ldrd(Condition cond, Register rt, Register rt2, uint64_t v) {
    RawLiteral* literal =
        new Literal<uint64_t>(v, RawLiteral::kDeletedOnPlacementByPool);
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    EmitLiteralCondRRL<&Assembler::ldrd> emit_helper(cond, rt, rt2);
    GenerateInstruction(emit_helper, literal);
  }
  template <typename T>
  void Ldrd(Register rt, Register rt2, T v) {
    Ldrd(al, rt, rt2, v);
  }

  void Vldr(Condition cond, SRegister rt, float v) {
    RawLiteral* literal =
        new Literal<float>(v, RawLiteral::kDeletedOnPlacementByPool);
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    EmitLiteralCondDtSL<&Assembler::vldr> emit_helper(cond, Untyped32, rt);
    GenerateInstruction(emit_helper, literal);
  }
  void Vldr(SRegister rt, float v) { Vldr(al, rt, v); }

  void Vldr(Condition cond, DRegister rt, double v) {
    RawLiteral* literal =
        new Literal<double>(v, RawLiteral::kDeletedOnPlacementByPool);
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    EmitLiteralCondDtDL<&Assembler::vldr> emit_helper(cond, Untyped64, rt);
    GenerateInstruction(emit_helper, literal);
  }
  void Vldr(DRegister rt, double v) { Vldr(al, rt, v); }

  void Vmov(Condition cond, DRegister rt, double v) { Vmov(cond, F64, rt, v); }
  void Vmov(DRegister rt, double v) { Vmov(al, F64, rt, v); }
  void Vmov(Condition cond, SRegister rt, float v) { Vmov(cond, F32, rt, v); }
  void Vmov(SRegister rt, float v) { Vmov(al, F32, rt, v); }

  void Switch(Register reg, JumpTableBase* table);
  void Case(JumpTableBase* table, int case_index);
  void Break(JumpTableBase* table);
  void Default(JumpTableBase* table);
  void EndSwitch(JumpTableBase* table);

  // Claim memory on the stack
  // Note: Operations on SP are atomic, and thus require to be aligned
  // We must always keep the stack 32-bit aligned, and every acess must be
  // 32-bit aligned.
  // We could Align{Up.Down}(size, 4), but that's potentially problematic:
  //     Claim(3)
  //     Claim(1)
  //     Drop(4)
  // would seem correct, when in fact:
  //    Claim(3) -> sp = sp - 4
  //    Claim(1)  -> sp = sp - 4
  //    Drop(4)  -> sp = sp + 4
  //
  void Claim(int32_t size) {
    if (size == 0) return;
    // The stack must be kept 32bit aligned.
    VIXL_ASSERT((size > 0) && ((size % 4) == 0));
    Sub(sp, sp, size);
  }
  // Release memory on the stack
  void Drop(int32_t size) {
    if (size == 0) return;
    // The stack must be kept 32bit aligned.
    VIXL_ASSERT((size > 0) && ((size % 4) == 0));
    Add(sp, sp, size);
  }
  void Peek(Register dst, int32_t offset) {
    VIXL_ASSERT((offset >= 0) && ((offset % 4) == 0));
    Ldr(dst, MemOperand(sp, offset));
  }
  void Poke(Register src, int32_t offset) {
    VIXL_ASSERT((offset >= 0) && ((offset % 4) == 0));
    Str(src, MemOperand(sp, offset));
  }
  void Printf(const char* format,
              CPURegister reg1 = NoReg,
              CPURegister reg2 = NoReg,
              CPURegister reg3 = NoReg,
              CPURegister reg4 = NoReg);
  // Functions used by Printf for generation.
  void PushRegister(CPURegister reg);
#if !VIXL_GENERATE_SIMULATOR_INSTRUCTIONS_VALUE
  void PreparePrintfArgument(CPURegister reg,
                             int* core_count,
                             int* vfp_count,
                             uint32_t* printf_type);
#endif
  // Handlers for cases not handled by the assembler.
  virtual void Delegate(InstructionType type,
                        InstructionCondROp instruction,
                        Condition cond,
                        Register rn,
                        const Operand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeROp instruction,
                        Condition cond,
                        EncodingSize size,
                        Register rn,
                        const Operand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondRROp instruction,
                        Condition cond,
                        Register rd,
                        Register rn,
                        const Operand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeRROp instruction,
                        Condition cond,
                        EncodingSize size,
                        Register rd,
                        Register rn,
                        const Operand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionRL instruction,
                        Register rn,
                        Label* label);
  virtual void Delegate(InstructionType type,
                        InstructionCondDtSSop instruction,
                        Condition cond,
                        DataType dt,
                        SRegister rd,
                        const SOperand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDDop instruction,
                        Condition cond,
                        DataType dt,
                        DRegister rd,
                        const DOperand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQQop instruction,
                        Condition cond,
                        DataType dt,
                        QRegister rd,
                        const QOperand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondMop instruction,
                        Condition cond,
                        const MemOperand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondRMop instruction,
                        Condition cond,
                        Register rd,
                        const MemOperand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeRMop instruction,
                        Condition cond,
                        EncodingSize size,
                        Register rd,
                        const MemOperand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondRRMop instruction,
                        Condition cond,
                        Register rt,
                        Register rt2,
                        const MemOperand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondRRRMop instruction,
                        Condition cond,
                        Register rd,
                        Register rt,
                        Register rt2,
                        const MemOperand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondDtSMop instruction,
                        Condition cond,
                        DataType dt,
                        SRegister rd,
                        const MemOperand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDMop instruction,
                        Condition cond,
                        DataType dt,
                        DRegister rd,
                        const MemOperand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondDtNrlMop instruction,
                        Condition cond,
                        DataType dt,
                        const NeonRegisterList& reglist,
                        const MemOperand& operand);
  virtual void Delegate(InstructionType type,
                        InstructionCondMsrOp instruction,
                        Condition cond,
                        MaskedSpecialRegister spec_reg,
                        const Operand& operand);

  // Start of generated code.

  void Adc(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // ADC<c>{<q>} {<Rdn>,} <Rdn>, <Rm> ; T1
        operand.IsPlainRegister() && rn.IsLow() && rd.Is(rn) &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, can_use_it);
    adc(cond, rd, rn, operand);
  }
  void Adc(Register rd, Register rn, const Operand& operand) {
    Adc(al, rd, rn, operand);
  }

  void Adcs(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    adcs(cond, rd, rn, operand);
  }
  void Adcs(Register rd, Register rn, const Operand& operand) {
    Adcs(al, rd, rn, operand);
  }

  void Add(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // ADD<c>{<q>} <Rd>, <Rn>, #<imm3> ; T1
        (operand.IsImmediate() && (operand.GetImmediate() <= 7) && rn.IsLow() &&
         rd.IsLow()) ||
        // ADD<c>{<q>} {<Rdn>,} <Rdn>, #<imm8> ; T2
        (operand.IsImmediate() && (operand.GetImmediate() <= 255) &&
         rd.IsLow() && rn.Is(rd)) ||
        // ADD{<c>}{<q>} <Rd>, SP, #<imm8> ; T1
        (operand.IsImmediate() && (operand.GetImmediate() <= 508) &&
         ((operand.GetImmediate() & 0x3) == 0) && rd.IsLow() && rn.IsSP()) ||
        // ADD<c>{<q>} <Rd>, <Rn>, <Rm>
        (operand.IsPlainRegister() && rd.IsLow() && rn.IsLow() &&
         operand.GetBaseRegister().IsLow()) ||
        // ADD<c>{<q>} <Rdn>, <Rm> ; T2
        (operand.IsPlainRegister() && !rd.IsPC() && rn.Is(rd) &&
         !operand.GetBaseRegister().IsSP() &&
         !operand.GetBaseRegister().IsPC()) ||
        // ADD{<c>}{<q>} {<Rdm>,} SP, <Rdm> ; T1
        (operand.IsPlainRegister() && !rd.IsPC() && rn.IsSP() &&
         operand.GetBaseRegister().Is(rd));
    ITScope it_scope(this, &cond, can_use_it);
    add(cond, rd, rn, operand);
  }
  void Add(Register rd, Register rn, const Operand& operand) {
    Add(al, rd, rn, operand);
  }

  void Adds(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    adds(cond, rd, rn, operand);
  }
  void Adds(Register rd, Register rn, const Operand& operand) {
    Adds(al, rd, rn, operand);
  }

  void Addw(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    addw(cond, rd, rn, operand);
  }
  void Addw(Register rd, Register rn, const Operand& operand) {
    Addw(al, rd, rn, operand);
  }

  void Adr(Condition cond, Register rd, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    adr(cond, rd, label);
  }
  void Adr(Register rd, Label* label) { Adr(al, rd, label); }

  void And(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // AND<c>{<q>} {<Rdn>,} <Rdn>, <Rm> ; T1
        operand.IsPlainRegister() && rd.Is(rn) && rn.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, can_use_it);
    and_(cond, rd, rn, operand);
  }
  void And(Register rd, Register rn, const Operand& operand) {
    And(al, rd, rn, operand);
  }

  void Ands(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ands(cond, rd, rn, operand);
  }
  void Ands(Register rd, Register rn, const Operand& operand) {
    Ands(al, rd, rn, operand);
  }

  void Asr(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // ASR<c>{<q>} {<Rd>,} <Rm>, #<imm> ; T2
        (operand.IsImmediate() && (operand.GetImmediate() >= 1) &&
         (operand.GetImmediate() <= 32) && rd.IsLow() && rm.IsLow()) ||
        // ASR<c>{<q>} {<Rdm>,} <Rdm>, <Rs> ; T1
        (operand.IsPlainRegister() && rd.Is(rm) && rd.IsLow() &&
         operand.GetBaseRegister().IsLow());
    ITScope it_scope(this, &cond, can_use_it);
    asr(cond, rd, rm, operand);
  }
  void Asr(Register rd, Register rm, const Operand& operand) {
    Asr(al, rd, rm, operand);
  }

  void Asrs(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    asrs(cond, rd, rm, operand);
  }
  void Asrs(Register rd, Register rm, const Operand& operand) {
    Asrs(al, rd, rm, operand);
  }

  void B(Condition cond, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    b(cond, label);
    AddBranchLabel(label);
  }
  void B(Label* label) { B(al, label); }

  void Bfc(Condition cond, Register rd, uint32_t lsb, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    bfc(cond, rd, lsb, operand);
  }
  void Bfc(Register rd, uint32_t lsb, const Operand& operand) {
    Bfc(al, rd, lsb, operand);
  }

  void Bfi(Condition cond,
           Register rd,
           Register rn,
           uint32_t lsb,
           const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    bfi(cond, rd, rn, lsb, operand);
  }
  void Bfi(Register rd, Register rn, uint32_t lsb, const Operand& operand) {
    Bfi(al, rd, rn, lsb, operand);
  }

  void Bic(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // BIC<c>{<q>} {<Rdn>,} <Rdn>, <Rm> ; T1
        operand.IsPlainRegister() && rd.Is(rn) && rn.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, can_use_it);
    bic(cond, rd, rn, operand);
  }
  void Bic(Register rd, Register rn, const Operand& operand) {
    Bic(al, rd, rn, operand);
  }

  void Bics(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    bics(cond, rd, rn, operand);
  }
  void Bics(Register rd, Register rn, const Operand& operand) {
    Bics(al, rd, rn, operand);
  }

  void Bkpt(Condition cond, uint32_t imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    bkpt(cond, imm);
  }
  void Bkpt(uint32_t imm) { Bkpt(al, imm); }

  void Bl(Condition cond, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    bl(cond, label);
    AddBranchLabel(label);
  }
  void Bl(Label* label) { Bl(al, label); }

  void Blx(Condition cond, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    blx(cond, label);
    AddBranchLabel(label);
  }
  void Blx(Label* label) { Blx(al, label); }

  void Blx(Condition cond, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // BLX{<c>}{<q>} <Rm> ; T1
        !rm.IsPC();
    ITScope it_scope(this, &cond, can_use_it);
    blx(cond, rm);
  }
  void Blx(Register rm) { Blx(al, rm); }

  void Bx(Condition cond, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // BX{<c>}{<q>} <Rm> ; T1
        !rm.IsPC();
    ITScope it_scope(this, &cond, can_use_it);
    bx(cond, rm);
  }
  void Bx(Register rm) { Bx(al, rm); }

  void Bxj(Condition cond, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    bxj(cond, rm);
  }
  void Bxj(Register rm) { Bxj(al, rm); }

  void Cbnz(Register rn, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    cbnz(rn, label);
    AddBranchLabel(label);
  }

  void Cbz(Register rn, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    cbz(rn, label);
    AddBranchLabel(label);
  }

  void Clrex(Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    clrex(cond);
  }
  void Clrex() { Clrex(al); }

  void Clz(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    clz(cond, rd, rm);
  }
  void Clz(Register rd, Register rm) { Clz(al, rd, rm); }

  void Cmn(Condition cond, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // CMN{<c>}{<q>} <Rn>, <Rm> ; T1
        operand.IsPlainRegister() && rn.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, can_use_it);
    cmn(cond, rn, operand);
  }
  void Cmn(Register rn, const Operand& operand) { Cmn(al, rn, operand); }

  void Cmp(Condition cond, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // CMP{<c>}{<q>} <Rn>, #<imm8> ; T1
        (operand.IsImmediate() && (operand.GetImmediate() <= 255) &&
         rn.IsLow()) ||
        // CMP{<c>}{<q>} <Rn>, <Rm> ; T1 T2
        (operand.IsPlainRegister() && !rn.IsPC() &&
         !operand.GetBaseRegister().IsPC());
    ITScope it_scope(this, &cond, can_use_it);
    cmp(cond, rn, operand);
  }
  void Cmp(Register rn, const Operand& operand) { Cmp(al, rn, operand); }

  void Crc32b(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    crc32b(cond, rd, rn, rm);
  }
  void Crc32b(Register rd, Register rn, Register rm) { Crc32b(al, rd, rn, rm); }

  void Crc32cb(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    crc32cb(cond, rd, rn, rm);
  }
  void Crc32cb(Register rd, Register rn, Register rm) {
    Crc32cb(al, rd, rn, rm);
  }

  void Crc32ch(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    crc32ch(cond, rd, rn, rm);
  }
  void Crc32ch(Register rd, Register rn, Register rm) {
    Crc32ch(al, rd, rn, rm);
  }

  void Crc32cw(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    crc32cw(cond, rd, rn, rm);
  }
  void Crc32cw(Register rd, Register rn, Register rm) {
    Crc32cw(al, rd, rn, rm);
  }

  void Crc32h(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    crc32h(cond, rd, rn, rm);
  }
  void Crc32h(Register rd, Register rn, Register rm) { Crc32h(al, rd, rn, rm); }

  void Crc32w(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    crc32w(cond, rd, rn, rm);
  }
  void Crc32w(Register rd, Register rn, Register rm) { Crc32w(al, rd, rn, rm); }

  void Dmb(Condition cond, MemoryBarrier option) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    dmb(cond, option);
  }
  void Dmb(MemoryBarrier option) { Dmb(al, option); }

  void Dsb(Condition cond, MemoryBarrier option) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    dsb(cond, option);
  }
  void Dsb(MemoryBarrier option) { Dsb(al, option); }

  void Eor(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // EOR<c>{<q>} {<Rdn>,} <Rdn>, <Rm> ; T1
        operand.IsPlainRegister() && rd.Is(rn) && rn.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, can_use_it);
    eor(cond, rd, rn, operand);
  }
  void Eor(Register rd, Register rn, const Operand& operand) {
    Eor(al, rd, rn, operand);
  }

  void Eors(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    eors(cond, rd, rn, operand);
  }
  void Eors(Register rd, Register rn, const Operand& operand) {
    Eors(al, rd, rn, operand);
  }

  void Fldmdbx(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    fldmdbx(cond, rn, write_back, dreglist);
  }
  void Fldmdbx(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Fldmdbx(al, rn, write_back, dreglist);
  }

  void Fldmiax(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    fldmiax(cond, rn, write_back, dreglist);
  }
  void Fldmiax(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Fldmiax(al, rn, write_back, dreglist);
  }

  void Fstmdbx(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    fstmdbx(cond, rn, write_back, dreglist);
  }
  void Fstmdbx(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Fstmdbx(al, rn, write_back, dreglist);
  }

  void Fstmiax(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    fstmiax(cond, rn, write_back, dreglist);
  }
  void Fstmiax(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Fstmiax(al, rn, write_back, dreglist);
  }

  void Hlt(Condition cond, uint32_t imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    hlt(cond, imm);
  }
  void Hlt(uint32_t imm) { Hlt(al, imm); }

  void Hvc(Condition cond, uint32_t imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    hvc(cond, imm);
  }
  void Hvc(uint32_t imm) { Hvc(al, imm); }

  void Isb(Condition cond, MemoryBarrier option) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    isb(cond, option);
  }
  void Isb(MemoryBarrier option) { Isb(al, option); }

  void It(Condition cond, uint16_t mask) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    it(cond, mask);
  }

  void Lda(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    lda(cond, rt, operand);
  }
  void Lda(Register rt, const MemOperand& operand) { Lda(al, rt, operand); }

  void Ldab(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldab(cond, rt, operand);
  }
  void Ldab(Register rt, const MemOperand& operand) { Ldab(al, rt, operand); }

  void Ldaex(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldaex(cond, rt, operand);
  }
  void Ldaex(Register rt, const MemOperand& operand) { Ldaex(al, rt, operand); }

  void Ldaexb(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldaexb(cond, rt, operand);
  }
  void Ldaexb(Register rt, const MemOperand& operand) {
    Ldaexb(al, rt, operand);
  }

  void Ldaexd(Condition cond,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldaexd(cond, rt, rt2, operand);
  }
  void Ldaexd(Register rt, Register rt2, const MemOperand& operand) {
    Ldaexd(al, rt, rt2, operand);
  }

  void Ldaexh(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldaexh(cond, rt, operand);
  }
  void Ldaexh(Register rt, const MemOperand& operand) {
    Ldaexh(al, rt, operand);
  }

  void Ldah(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldah(cond, rt, operand);
  }
  void Ldah(Register rt, const MemOperand& operand) { Ldah(al, rt, operand); }

  void Ldm(Condition cond,
           Register rn,
           WriteBack write_back,
           RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldm(cond, rn, write_back, registers);
  }
  void Ldm(Register rn, WriteBack write_back, RegisterList registers) {
    Ldm(al, rn, write_back, registers);
  }

  void Ldmda(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldmda(cond, rn, write_back, registers);
  }
  void Ldmda(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmda(al, rn, write_back, registers);
  }

  void Ldmdb(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldmdb(cond, rn, write_back, registers);
  }
  void Ldmdb(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmdb(al, rn, write_back, registers);
  }

  void Ldmea(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldmea(cond, rn, write_back, registers);
  }
  void Ldmea(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmea(al, rn, write_back, registers);
  }

  void Ldmed(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldmed(cond, rn, write_back, registers);
  }
  void Ldmed(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmed(al, rn, write_back, registers);
  }

  void Ldmfa(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldmfa(cond, rn, write_back, registers);
  }
  void Ldmfa(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmfa(al, rn, write_back, registers);
  }

  void Ldmfd(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldmfd(cond, rn, write_back, registers);
  }
  void Ldmfd(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmfd(al, rn, write_back, registers);
  }

  void Ldmib(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldmib(cond, rn, write_back, registers);
  }
  void Ldmib(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmib(al, rn, write_back, registers);
  }

  void Ldr(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // LDR{<c>}{<q>} <Rt>, [<Rn> {, #{+}<imm>}] ; T1
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.IsOffsetImmediateWithinRange(0, 124, 4) &&
         (operand.GetAddrMode() == Offset)) ||
        // LDR{<c>}{<q>} <Rt>, [SP{, #{+}<imm>}] ; T2
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsSP() &&
         operand.IsOffsetImmediateWithinRange(0, 1020, 4) &&
         (operand.GetAddrMode() == Offset)) ||
        // LDR{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        (operand.IsPlainRegister() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
         (operand.GetAddrMode() == Offset));
    ITScope it_scope(this, &cond, can_use_it);
    ldr(cond, rt, operand);
  }
  void Ldr(Register rt, const MemOperand& operand) { Ldr(al, rt, operand); }

  void Ldr(Condition cond, Register rt, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldr(cond, rt, label);
  }
  void Ldr(Register rt, Label* label) { Ldr(al, rt, label); }

  void Ldrb(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // LDRB{<c>}{<q>} <Rt>, [<Rn> {, #{+}<imm>}] ; T1
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.IsOffsetImmediateWithinRange(0, 31) &&
         (operand.GetAddrMode() == Offset)) ||
        // LDRB{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        (operand.IsPlainRegister() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
         (operand.GetAddrMode() == Offset));
    ITScope it_scope(this, &cond, can_use_it);
    ldrb(cond, rt, operand);
  }
  void Ldrb(Register rt, const MemOperand& operand) { Ldrb(al, rt, operand); }

  void Ldrb(Condition cond, Register rt, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldrb(cond, rt, label);
  }
  void Ldrb(Register rt, Label* label) { Ldrb(al, rt, label); }

  void Ldrd(Condition cond,
            Register rt,
            Register rt2,
            const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldrd(cond, rt, rt2, operand);
  }
  void Ldrd(Register rt, Register rt2, const MemOperand& operand) {
    Ldrd(al, rt, rt2, operand);
  }

  void Ldrd(Condition cond, Register rt, Register rt2, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldrd(cond, rt, rt2, label);
  }
  void Ldrd(Register rt, Register rt2, Label* label) {
    Ldrd(al, rt, rt2, label);
  }

  void Ldrex(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldrex(cond, rt, operand);
  }
  void Ldrex(Register rt, const MemOperand& operand) { Ldrex(al, rt, operand); }

  void Ldrexb(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldrexb(cond, rt, operand);
  }
  void Ldrexb(Register rt, const MemOperand& operand) {
    Ldrexb(al, rt, operand);
  }

  void Ldrexd(Condition cond,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldrexd(cond, rt, rt2, operand);
  }
  void Ldrexd(Register rt, Register rt2, const MemOperand& operand) {
    Ldrexd(al, rt, rt2, operand);
  }

  void Ldrexh(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldrexh(cond, rt, operand);
  }
  void Ldrexh(Register rt, const MemOperand& operand) {
    Ldrexh(al, rt, operand);
  }

  void Ldrh(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // LDRH{<c>}{<q>} <Rt>, [<Rn> {, #{+}<imm>}] ; T1
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.IsOffsetImmediateWithinRange(0, 62, 2) &&
         (operand.GetAddrMode() == Offset)) ||
        // LDRH{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        (operand.IsPlainRegister() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
         (operand.GetAddrMode() == Offset));
    ITScope it_scope(this, &cond, can_use_it);
    ldrh(cond, rt, operand);
  }
  void Ldrh(Register rt, const MemOperand& operand) { Ldrh(al, rt, operand); }

  void Ldrh(Condition cond, Register rt, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldrh(cond, rt, label);
  }
  void Ldrh(Register rt, Label* label) { Ldrh(al, rt, label); }

  void Ldrsb(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // LDRSB{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        operand.IsPlainRegister() && rt.IsLow() &&
        operand.GetBaseRegister().IsLow() &&
        operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
        (operand.GetAddrMode() == Offset);
    ITScope it_scope(this, &cond, can_use_it);
    ldrsb(cond, rt, operand);
  }
  void Ldrsb(Register rt, const MemOperand& operand) { Ldrsb(al, rt, operand); }

  void Ldrsb(Condition cond, Register rt, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldrsb(cond, rt, label);
  }
  void Ldrsb(Register rt, Label* label) { Ldrsb(al, rt, label); }

  void Ldrsh(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // LDRSH{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        operand.IsPlainRegister() && rt.IsLow() &&
        operand.GetBaseRegister().IsLow() &&
        operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
        (operand.GetAddrMode() == Offset);
    ITScope it_scope(this, &cond, can_use_it);
    ldrsh(cond, rt, operand);
  }
  void Ldrsh(Register rt, const MemOperand& operand) { Ldrsh(al, rt, operand); }

  void Ldrsh(Condition cond, Register rt, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ldrsh(cond, rt, label);
  }
  void Ldrsh(Register rt, Label* label) { Ldrsh(al, rt, label); }

  void Lsl(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // LSL<c>{<q>} {<Rd>,} <Rm>, #<imm> ; T2
        (operand.IsImmediate() && (operand.GetImmediate() >= 1) &&
         (operand.GetImmediate() <= 31) && rd.IsLow() && rm.IsLow()) ||
        // LSL<c>{<q>} {<Rdm>,} <Rdm>, <Rs> ; T1
        (operand.IsPlainRegister() && rd.Is(rm) && rd.IsLow() &&
         operand.GetBaseRegister().IsLow());
    ITScope it_scope(this, &cond, can_use_it);
    lsl(cond, rd, rm, operand);
  }
  void Lsl(Register rd, Register rm, const Operand& operand) {
    Lsl(al, rd, rm, operand);
  }

  void Lsls(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    lsls(cond, rd, rm, operand);
  }
  void Lsls(Register rd, Register rm, const Operand& operand) {
    Lsls(al, rd, rm, operand);
  }

  void Lsr(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // LSR<c>{<q>} {<Rd>,} <Rm>, #<imm> ; T2
        (operand.IsImmediate() && (operand.GetImmediate() >= 1) &&
         (operand.GetImmediate() <= 32) && rd.IsLow() && rm.IsLow()) ||
        // LSR<c>{<q>} {<Rdm>,} <Rdm>, <Rs> ; T1
        (operand.IsPlainRegister() && rd.Is(rm) && rd.IsLow() &&
         operand.GetBaseRegister().IsLow());
    ITScope it_scope(this, &cond, can_use_it);
    lsr(cond, rd, rm, operand);
  }
  void Lsr(Register rd, Register rm, const Operand& operand) {
    Lsr(al, rd, rm, operand);
  }

  void Lsrs(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    lsrs(cond, rd, rm, operand);
  }
  void Lsrs(Register rd, Register rm, const Operand& operand) {
    Lsrs(al, rd, rm, operand);
  }

  void Mla(Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    mla(cond, rd, rn, rm, ra);
  }
  void Mla(Register rd, Register rn, Register rm, Register ra) {
    Mla(al, rd, rn, rm, ra);
  }

  void Mlas(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    mlas(cond, rd, rn, rm, ra);
  }
  void Mlas(Register rd, Register rn, Register rm, Register ra) {
    Mlas(al, rd, rn, rm, ra);
  }

  void Mls(Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    mls(cond, rd, rn, rm, ra);
  }
  void Mls(Register rd, Register rn, Register rm, Register ra) {
    Mls(al, rd, rn, rm, ra);
  }

  void Mov(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // MOV<c>{<q>} <Rd>, #<imm8> ; T1
        (operand.IsImmediate() && rd.IsLow() &&
         (operand.GetImmediate() <= 255)) ||
        // MOV{<c>}{<q>} <Rd>, <Rm> ; T1
        (operand.IsPlainRegister() && !rd.IsPC() &&
         !operand.GetBaseRegister().IsPC()) ||
        // MOV<c>{<q>} <Rd>, <Rm> {, <shift> #<amount>} ; T2
        (operand.IsImmediateShiftedRegister() && rd.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         (operand.GetShift().Is(LSL) || operand.GetShift().Is(LSR) ||
          operand.GetShift().Is(ASR))) ||
        // MOV<c>{<q>} <Rdm>, <Rdm>, LSL <Rs> ; T1
        // MOV<c>{<q>} <Rdm>, <Rdm>, LSR <Rs> ; T1
        // MOV<c>{<q>} <Rdm>, <Rdm>, ASR <Rs> ; T1
        // MOV<c>{<q>} <Rdm>, <Rdm>, ROR <Rs> ; T1
        (operand.IsRegisterShiftedRegister() &&
         rd.Is(operand.GetBaseRegister()) && rd.IsLow() &&
         (operand.GetShift().Is(LSL) || operand.GetShift().Is(LSR) ||
          operand.GetShift().Is(ASR) || operand.GetShift().Is(ROR)) &&
         operand.GetShiftRegister().IsLow());
    ITScope it_scope(this, &cond, can_use_it);
    mov(cond, rd, operand);
  }
  void Mov(Register rd, const Operand& operand) { Mov(al, rd, operand); }

  void Movs(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    movs(cond, rd, operand);
  }
  void Movs(Register rd, const Operand& operand) { Movs(al, rd, operand); }

  void Movt(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    movt(cond, rd, operand);
  }
  void Movt(Register rd, const Operand& operand) { Movt(al, rd, operand); }

  void Movw(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    movw(cond, rd, operand);
  }
  void Movw(Register rd, const Operand& operand) { Movw(al, rd, operand); }

  void Mrs(Condition cond, Register rd, SpecialRegister spec_reg) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    mrs(cond, rd, spec_reg);
  }
  void Mrs(Register rd, SpecialRegister spec_reg) { Mrs(al, rd, spec_reg); }

  void Msr(Condition cond,
           MaskedSpecialRegister spec_reg,
           const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    msr(cond, spec_reg, operand);
  }
  void Msr(MaskedSpecialRegister spec_reg, const Operand& operand) {
    Msr(al, spec_reg, operand);
  }

  void Mul(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // MUL<c>{<q>} <Rdm>, <Rn>{, <Rdm>} ; T1
        rd.Is(rm) && rn.IsLow() && rm.IsLow();
    ITScope it_scope(this, &cond, can_use_it);
    mul(cond, rd, rn, rm);
  }
  void Mul(Register rd, Register rn, Register rm) { Mul(al, rd, rn, rm); }

  void Muls(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    muls(cond, rd, rn, rm);
  }
  void Muls(Register rd, Register rn, Register rm) { Muls(al, rd, rn, rm); }

  void Mvn(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // MVN<c>{<q>} <Rd>, <Rm> ; T1
        operand.IsPlainRegister() && rd.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, can_use_it);
    mvn(cond, rd, operand);
  }
  void Mvn(Register rd, const Operand& operand) { Mvn(al, rd, operand); }

  void Mvns(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    mvns(cond, rd, operand);
  }
  void Mvns(Register rd, const Operand& operand) { Mvns(al, rd, operand); }

  void Nop(Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    nop(cond);
  }
  void Nop() { Nop(al); }

  void Orn(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    orn(cond, rd, rn, operand);
  }
  void Orn(Register rd, Register rn, const Operand& operand) {
    Orn(al, rd, rn, operand);
  }

  void Orns(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    orns(cond, rd, rn, operand);
  }
  void Orns(Register rd, Register rn, const Operand& operand) {
    Orns(al, rd, rn, operand);
  }

  void Orr(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // ORR<c>{<q>} {<Rdn>,} <Rdn>, <Rm> ; T1
        operand.IsPlainRegister() && rd.Is(rn) && rn.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, can_use_it);
    orr(cond, rd, rn, operand);
  }
  void Orr(Register rd, Register rn, const Operand& operand) {
    Orr(al, rd, rn, operand);
  }

  void Orrs(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    orrs(cond, rd, rn, operand);
  }
  void Orrs(Register rd, Register rn, const Operand& operand) {
    Orrs(al, rd, rn, operand);
  }

  void Pkhbt(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    pkhbt(cond, rd, rn, operand);
  }
  void Pkhbt(Register rd, Register rn, const Operand& operand) {
    Pkhbt(al, rd, rn, operand);
  }

  void Pkhtb(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    pkhtb(cond, rd, rn, operand);
  }
  void Pkhtb(Register rd, Register rn, const Operand& operand) {
    Pkhtb(al, rd, rn, operand);
  }

  void Pld(Condition cond, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    pld(cond, label);
  }
  void Pld(Label* label) { Pld(al, label); }

  void Pld(Condition cond, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    pld(cond, operand);
  }
  void Pld(const MemOperand& operand) { Pld(al, operand); }

  void Pldw(Condition cond, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    pldw(cond, operand);
  }
  void Pldw(const MemOperand& operand) { Pldw(al, operand); }

  void Pli(Condition cond, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    pli(cond, operand);
  }
  void Pli(const MemOperand& operand) { Pli(al, operand); }

  void Pli(Condition cond, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    pli(cond, label);
  }
  void Pli(Label* label) { Pli(al, label); }

  void Pop(Condition cond, RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    pop(cond, registers);
  }
  void Pop(RegisterList registers) { Pop(al, registers); }

  void Pop(Condition cond, Register rt) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    pop(cond, rt);
  }
  void Pop(Register rt) { Pop(al, rt); }

  void Push(Condition cond, RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    push(cond, registers);
  }
  void Push(RegisterList registers) { Push(al, registers); }

  void Push(Condition cond, Register rt) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    push(cond, rt);
  }
  void Push(Register rt) { Push(al, rt); }

  void Qadd(Condition cond, Register rd, Register rm, Register rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    qadd(cond, rd, rm, rn);
  }
  void Qadd(Register rd, Register rm, Register rn) { Qadd(al, rd, rm, rn); }

  void Qadd16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    qadd16(cond, rd, rn, rm);
  }
  void Qadd16(Register rd, Register rn, Register rm) { Qadd16(al, rd, rn, rm); }

  void Qadd8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    qadd8(cond, rd, rn, rm);
  }
  void Qadd8(Register rd, Register rn, Register rm) { Qadd8(al, rd, rn, rm); }

  void Qasx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    qasx(cond, rd, rn, rm);
  }
  void Qasx(Register rd, Register rn, Register rm) { Qasx(al, rd, rn, rm); }

  void Qdadd(Condition cond, Register rd, Register rm, Register rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    qdadd(cond, rd, rm, rn);
  }
  void Qdadd(Register rd, Register rm, Register rn) { Qdadd(al, rd, rm, rn); }

  void Qdsub(Condition cond, Register rd, Register rm, Register rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    qdsub(cond, rd, rm, rn);
  }
  void Qdsub(Register rd, Register rm, Register rn) { Qdsub(al, rd, rm, rn); }

  void Qsax(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    qsax(cond, rd, rn, rm);
  }
  void Qsax(Register rd, Register rn, Register rm) { Qsax(al, rd, rn, rm); }

  void Qsub(Condition cond, Register rd, Register rm, Register rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    qsub(cond, rd, rm, rn);
  }
  void Qsub(Register rd, Register rm, Register rn) { Qsub(al, rd, rm, rn); }

  void Qsub16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    qsub16(cond, rd, rn, rm);
  }
  void Qsub16(Register rd, Register rn, Register rm) { Qsub16(al, rd, rn, rm); }

  void Qsub8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    qsub8(cond, rd, rn, rm);
  }
  void Qsub8(Register rd, Register rn, Register rm) { Qsub8(al, rd, rn, rm); }

  void Rbit(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    rbit(cond, rd, rm);
  }
  void Rbit(Register rd, Register rm) { Rbit(al, rd, rm); }

  void Rev(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    rev(cond, rd, rm);
  }
  void Rev(Register rd, Register rm) { Rev(al, rd, rm); }

  void Rev16(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    rev16(cond, rd, rm);
  }
  void Rev16(Register rd, Register rm) { Rev16(al, rd, rm); }

  void Revsh(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    revsh(cond, rd, rm);
  }
  void Revsh(Register rd, Register rm) { Revsh(al, rd, rm); }

  void Ror(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // ROR<c>{<q>} {<Rd>,} <Rm>, #<imm> ; T2
        (operand.IsImmediate() && (operand.GetImmediate() >= 1) &&
         (operand.GetImmediate() <= 31) && rd.IsLow() && rm.IsLow()) ||
        // ROR<c>{<q>} {<Rdm>,} <Rdm>, <Rs> ; T1
        (operand.IsPlainRegister() && rd.Is(rm) && rd.IsLow() &&
         operand.GetBaseRegister().IsLow());
    ITScope it_scope(this, &cond, can_use_it);
    ror(cond, rd, rm, operand);
  }
  void Ror(Register rd, Register rm, const Operand& operand) {
    Ror(al, rd, rm, operand);
  }

  void Rors(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    rors(cond, rd, rm, operand);
  }
  void Rors(Register rd, Register rm, const Operand& operand) {
    Rors(al, rd, rm, operand);
  }

  void Rrx(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    rrx(cond, rd, rm);
  }
  void Rrx(Register rd, Register rm) { Rrx(al, rd, rm); }

  void Rrxs(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    rrxs(cond, rd, rm);
  }
  void Rrxs(Register rd, Register rm) { Rrxs(al, rd, rm); }

  void Rsb(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // RSB<c>{<q>} {<Rd>, }<Rn>, #0 ; T1
        operand.IsImmediate() && rd.IsLow() && rn.IsLow() &&
        (operand.GetImmediate() == 0);
    ITScope it_scope(this, &cond, can_use_it);
    rsb(cond, rd, rn, operand);
  }
  void Rsb(Register rd, Register rn, const Operand& operand) {
    Rsb(al, rd, rn, operand);
  }

  void Rsbs(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    rsbs(cond, rd, rn, operand);
  }
  void Rsbs(Register rd, Register rn, const Operand& operand) {
    Rsbs(al, rd, rn, operand);
  }

  void Rsc(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    rsc(cond, rd, rn, operand);
  }
  void Rsc(Register rd, Register rn, const Operand& operand) {
    Rsc(al, rd, rn, operand);
  }

  void Rscs(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    rscs(cond, rd, rn, operand);
  }
  void Rscs(Register rd, Register rn, const Operand& operand) {
    Rscs(al, rd, rn, operand);
  }

  void Sadd16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sadd16(cond, rd, rn, rm);
  }
  void Sadd16(Register rd, Register rn, Register rm) { Sadd16(al, rd, rn, rm); }

  void Sadd8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sadd8(cond, rd, rn, rm);
  }
  void Sadd8(Register rd, Register rn, Register rm) { Sadd8(al, rd, rn, rm); }

  void Sasx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sasx(cond, rd, rn, rm);
  }
  void Sasx(Register rd, Register rn, Register rm) { Sasx(al, rd, rn, rm); }

  void Sbc(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // SBC<c>{<q>} {<Rdn>,} <Rdn>, <Rm> ; T1
        operand.IsPlainRegister() && rn.IsLow() && rd.Is(rn) &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, can_use_it);
    sbc(cond, rd, rn, operand);
  }
  void Sbc(Register rd, Register rn, const Operand& operand) {
    Sbc(al, rd, rn, operand);
  }

  void Sbcs(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sbcs(cond, rd, rn, operand);
  }
  void Sbcs(Register rd, Register rn, const Operand& operand) {
    Sbcs(al, rd, rn, operand);
  }

  void Sbfx(Condition cond,
            Register rd,
            Register rn,
            uint32_t lsb,
            const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sbfx(cond, rd, rn, lsb, operand);
  }
  void Sbfx(Register rd, Register rn, uint32_t lsb, const Operand& operand) {
    Sbfx(al, rd, rn, lsb, operand);
  }

  void Sdiv(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sdiv(cond, rd, rn, rm);
  }
  void Sdiv(Register rd, Register rn, Register rm) { Sdiv(al, rd, rn, rm); }

  void Sel(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sel(cond, rd, rn, rm);
  }
  void Sel(Register rd, Register rn, Register rm) { Sel(al, rd, rn, rm); }

  void Shadd16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    shadd16(cond, rd, rn, rm);
  }
  void Shadd16(Register rd, Register rn, Register rm) {
    Shadd16(al, rd, rn, rm);
  }

  void Shadd8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    shadd8(cond, rd, rn, rm);
  }
  void Shadd8(Register rd, Register rn, Register rm) { Shadd8(al, rd, rn, rm); }

  void Shasx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    shasx(cond, rd, rn, rm);
  }
  void Shasx(Register rd, Register rn, Register rm) { Shasx(al, rd, rn, rm); }

  void Shsax(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    shsax(cond, rd, rn, rm);
  }
  void Shsax(Register rd, Register rn, Register rm) { Shsax(al, rd, rn, rm); }

  void Shsub16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    shsub16(cond, rd, rn, rm);
  }
  void Shsub16(Register rd, Register rn, Register rm) {
    Shsub16(al, rd, rn, rm);
  }

  void Shsub8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    shsub8(cond, rd, rn, rm);
  }
  void Shsub8(Register rd, Register rn, Register rm) { Shsub8(al, rd, rn, rm); }

  void Smlabb(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlabb(cond, rd, rn, rm, ra);
  }
  void Smlabb(Register rd, Register rn, Register rm, Register ra) {
    Smlabb(al, rd, rn, rm, ra);
  }

  void Smlabt(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlabt(cond, rd, rn, rm, ra);
  }
  void Smlabt(Register rd, Register rn, Register rm, Register ra) {
    Smlabt(al, rd, rn, rm, ra);
  }

  void Smlad(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlad(cond, rd, rn, rm, ra);
  }
  void Smlad(Register rd, Register rn, Register rm, Register ra) {
    Smlad(al, rd, rn, rm, ra);
  }

  void Smladx(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smladx(cond, rd, rn, rm, ra);
  }
  void Smladx(Register rd, Register rn, Register rm, Register ra) {
    Smladx(al, rd, rn, rm, ra);
  }

  void Smlal(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlal(cond, rdlo, rdhi, rn, rm);
  }
  void Smlal(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlal(al, rdlo, rdhi, rn, rm);
  }

  void Smlalbb(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlalbb(cond, rdlo, rdhi, rn, rm);
  }
  void Smlalbb(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlalbb(al, rdlo, rdhi, rn, rm);
  }

  void Smlalbt(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlalbt(cond, rdlo, rdhi, rn, rm);
  }
  void Smlalbt(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlalbt(al, rdlo, rdhi, rn, rm);
  }

  void Smlald(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlald(cond, rdlo, rdhi, rn, rm);
  }
  void Smlald(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlald(al, rdlo, rdhi, rn, rm);
  }

  void Smlaldx(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlaldx(cond, rdlo, rdhi, rn, rm);
  }
  void Smlaldx(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlaldx(al, rdlo, rdhi, rn, rm);
  }

  void Smlals(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlals(cond, rdlo, rdhi, rn, rm);
  }
  void Smlals(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlals(al, rdlo, rdhi, rn, rm);
  }

  void Smlaltb(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlaltb(cond, rdlo, rdhi, rn, rm);
  }
  void Smlaltb(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlaltb(al, rdlo, rdhi, rn, rm);
  }

  void Smlaltt(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlaltt(cond, rdlo, rdhi, rn, rm);
  }
  void Smlaltt(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlaltt(al, rdlo, rdhi, rn, rm);
  }

  void Smlatb(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlatb(cond, rd, rn, rm, ra);
  }
  void Smlatb(Register rd, Register rn, Register rm, Register ra) {
    Smlatb(al, rd, rn, rm, ra);
  }

  void Smlatt(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlatt(cond, rd, rn, rm, ra);
  }
  void Smlatt(Register rd, Register rn, Register rm, Register ra) {
    Smlatt(al, rd, rn, rm, ra);
  }

  void Smlawb(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlawb(cond, rd, rn, rm, ra);
  }
  void Smlawb(Register rd, Register rn, Register rm, Register ra) {
    Smlawb(al, rd, rn, rm, ra);
  }

  void Smlawt(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlawt(cond, rd, rn, rm, ra);
  }
  void Smlawt(Register rd, Register rn, Register rm, Register ra) {
    Smlawt(al, rd, rn, rm, ra);
  }

  void Smlsd(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlsd(cond, rd, rn, rm, ra);
  }
  void Smlsd(Register rd, Register rn, Register rm, Register ra) {
    Smlsd(al, rd, rn, rm, ra);
  }

  void Smlsdx(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlsdx(cond, rd, rn, rm, ra);
  }
  void Smlsdx(Register rd, Register rn, Register rm, Register ra) {
    Smlsdx(al, rd, rn, rm, ra);
  }

  void Smlsld(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlsld(cond, rdlo, rdhi, rn, rm);
  }
  void Smlsld(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlsld(al, rdlo, rdhi, rn, rm);
  }

  void Smlsldx(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smlsldx(cond, rdlo, rdhi, rn, rm);
  }
  void Smlsldx(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlsldx(al, rdlo, rdhi, rn, rm);
  }

  void Smmla(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smmla(cond, rd, rn, rm, ra);
  }
  void Smmla(Register rd, Register rn, Register rm, Register ra) {
    Smmla(al, rd, rn, rm, ra);
  }

  void Smmlar(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smmlar(cond, rd, rn, rm, ra);
  }
  void Smmlar(Register rd, Register rn, Register rm, Register ra) {
    Smmlar(al, rd, rn, rm, ra);
  }

  void Smmls(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smmls(cond, rd, rn, rm, ra);
  }
  void Smmls(Register rd, Register rn, Register rm, Register ra) {
    Smmls(al, rd, rn, rm, ra);
  }

  void Smmlsr(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smmlsr(cond, rd, rn, rm, ra);
  }
  void Smmlsr(Register rd, Register rn, Register rm, Register ra) {
    Smmlsr(al, rd, rn, rm, ra);
  }

  void Smmul(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smmul(cond, rd, rn, rm);
  }
  void Smmul(Register rd, Register rn, Register rm) { Smmul(al, rd, rn, rm); }

  void Smmulr(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smmulr(cond, rd, rn, rm);
  }
  void Smmulr(Register rd, Register rn, Register rm) { Smmulr(al, rd, rn, rm); }

  void Smuad(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smuad(cond, rd, rn, rm);
  }
  void Smuad(Register rd, Register rn, Register rm) { Smuad(al, rd, rn, rm); }

  void Smuadx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smuadx(cond, rd, rn, rm);
  }
  void Smuadx(Register rd, Register rn, Register rm) { Smuadx(al, rd, rn, rm); }

  void Smulbb(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smulbb(cond, rd, rn, rm);
  }
  void Smulbb(Register rd, Register rn, Register rm) { Smulbb(al, rd, rn, rm); }

  void Smulbt(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smulbt(cond, rd, rn, rm);
  }
  void Smulbt(Register rd, Register rn, Register rm) { Smulbt(al, rd, rn, rm); }

  void Smull(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smull(cond, rdlo, rdhi, rn, rm);
  }
  void Smull(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smull(al, rdlo, rdhi, rn, rm);
  }

  void Smulls(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smulls(cond, rdlo, rdhi, rn, rm);
  }
  void Smulls(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smulls(al, rdlo, rdhi, rn, rm);
  }

  void Smultb(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smultb(cond, rd, rn, rm);
  }
  void Smultb(Register rd, Register rn, Register rm) { Smultb(al, rd, rn, rm); }

  void Smultt(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smultt(cond, rd, rn, rm);
  }
  void Smultt(Register rd, Register rn, Register rm) { Smultt(al, rd, rn, rm); }

  void Smulwb(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smulwb(cond, rd, rn, rm);
  }
  void Smulwb(Register rd, Register rn, Register rm) { Smulwb(al, rd, rn, rm); }

  void Smulwt(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smulwt(cond, rd, rn, rm);
  }
  void Smulwt(Register rd, Register rn, Register rm) { Smulwt(al, rd, rn, rm); }

  void Smusd(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smusd(cond, rd, rn, rm);
  }
  void Smusd(Register rd, Register rn, Register rm) { Smusd(al, rd, rn, rm); }

  void Smusdx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    smusdx(cond, rd, rn, rm);
  }
  void Smusdx(Register rd, Register rn, Register rm) { Smusdx(al, rd, rn, rm); }

  void Ssat(Condition cond, Register rd, uint32_t imm, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ssat(cond, rd, imm, operand);
  }
  void Ssat(Register rd, uint32_t imm, const Operand& operand) {
    Ssat(al, rd, imm, operand);
  }

  void Ssat16(Condition cond, Register rd, uint32_t imm, Register rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ssat16(cond, rd, imm, rn);
  }
  void Ssat16(Register rd, uint32_t imm, Register rn) {
    Ssat16(al, rd, imm, rn);
  }

  void Ssax(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ssax(cond, rd, rn, rm);
  }
  void Ssax(Register rd, Register rn, Register rm) { Ssax(al, rd, rn, rm); }

  void Ssub16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ssub16(cond, rd, rn, rm);
  }
  void Ssub16(Register rd, Register rn, Register rm) { Ssub16(al, rd, rn, rm); }

  void Ssub8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ssub8(cond, rd, rn, rm);
  }
  void Ssub8(Register rd, Register rn, Register rm) { Ssub8(al, rd, rn, rm); }

  void Stl(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stl(cond, rt, operand);
  }
  void Stl(Register rt, const MemOperand& operand) { Stl(al, rt, operand); }

  void Stlb(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stlb(cond, rt, operand);
  }
  void Stlb(Register rt, const MemOperand& operand) { Stlb(al, rt, operand); }

  void Stlex(Condition cond,
             Register rd,
             Register rt,
             const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stlex(cond, rd, rt, operand);
  }
  void Stlex(Register rd, Register rt, const MemOperand& operand) {
    Stlex(al, rd, rt, operand);
  }

  void Stlexb(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stlexb(cond, rd, rt, operand);
  }
  void Stlexb(Register rd, Register rt, const MemOperand& operand) {
    Stlexb(al, rd, rt, operand);
  }

  void Stlexd(Condition cond,
              Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stlexd(cond, rd, rt, rt2, operand);
  }
  void Stlexd(Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    Stlexd(al, rd, rt, rt2, operand);
  }

  void Stlexh(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stlexh(cond, rd, rt, operand);
  }
  void Stlexh(Register rd, Register rt, const MemOperand& operand) {
    Stlexh(al, rd, rt, operand);
  }

  void Stlh(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stlh(cond, rt, operand);
  }
  void Stlh(Register rt, const MemOperand& operand) { Stlh(al, rt, operand); }

  void Stm(Condition cond,
           Register rn,
           WriteBack write_back,
           RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stm(cond, rn, write_back, registers);
  }
  void Stm(Register rn, WriteBack write_back, RegisterList registers) {
    Stm(al, rn, write_back, registers);
  }

  void Stmda(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stmda(cond, rn, write_back, registers);
  }
  void Stmda(Register rn, WriteBack write_back, RegisterList registers) {
    Stmda(al, rn, write_back, registers);
  }

  void Stmdb(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stmdb(cond, rn, write_back, registers);
  }
  void Stmdb(Register rn, WriteBack write_back, RegisterList registers) {
    Stmdb(al, rn, write_back, registers);
  }

  void Stmea(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stmea(cond, rn, write_back, registers);
  }
  void Stmea(Register rn, WriteBack write_back, RegisterList registers) {
    Stmea(al, rn, write_back, registers);
  }

  void Stmed(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stmed(cond, rn, write_back, registers);
  }
  void Stmed(Register rn, WriteBack write_back, RegisterList registers) {
    Stmed(al, rn, write_back, registers);
  }

  void Stmfa(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stmfa(cond, rn, write_back, registers);
  }
  void Stmfa(Register rn, WriteBack write_back, RegisterList registers) {
    Stmfa(al, rn, write_back, registers);
  }

  void Stmfd(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stmfd(cond, rn, write_back, registers);
  }
  void Stmfd(Register rn, WriteBack write_back, RegisterList registers) {
    Stmfd(al, rn, write_back, registers);
  }

  void Stmib(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    stmib(cond, rn, write_back, registers);
  }
  void Stmib(Register rn, WriteBack write_back, RegisterList registers) {
    Stmib(al, rn, write_back, registers);
  }

  void Str(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // STR{<c>}{<q>} <Rt>, [<Rn> {, #{+}<imm>}] ; T1
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.IsOffsetImmediateWithinRange(0, 124, 4) &&
         (operand.GetAddrMode() == Offset)) ||
        // STR{<c>}{<q>} <Rt>, [SP{, #{+}<imm>}] ; T2
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsSP() &&
         operand.IsOffsetImmediateWithinRange(0, 1020, 4) &&
         (operand.GetAddrMode() == Offset)) ||
        // STR{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        (operand.IsPlainRegister() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
         (operand.GetAddrMode() == Offset));
    ITScope it_scope(this, &cond, can_use_it);
    str(cond, rt, operand);
  }
  void Str(Register rt, const MemOperand& operand) { Str(al, rt, operand); }

  void Strb(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // STRB{<c>}{<q>} <Rt>, [<Rn> {, #{+}<imm>}] ; T1
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.IsOffsetImmediateWithinRange(0, 31) &&
         (operand.GetAddrMode() == Offset)) ||
        // STRB{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        (operand.IsPlainRegister() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
         (operand.GetAddrMode() == Offset));
    ITScope it_scope(this, &cond, can_use_it);
    strb(cond, rt, operand);
  }
  void Strb(Register rt, const MemOperand& operand) { Strb(al, rt, operand); }

  void Strd(Condition cond,
            Register rt,
            Register rt2,
            const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    strd(cond, rt, rt2, operand);
  }
  void Strd(Register rt, Register rt2, const MemOperand& operand) {
    Strd(al, rt, rt2, operand);
  }

  void Strex(Condition cond,
             Register rd,
             Register rt,
             const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    strex(cond, rd, rt, operand);
  }
  void Strex(Register rd, Register rt, const MemOperand& operand) {
    Strex(al, rd, rt, operand);
  }

  void Strexb(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    strexb(cond, rd, rt, operand);
  }
  void Strexb(Register rd, Register rt, const MemOperand& operand) {
    Strexb(al, rd, rt, operand);
  }

  void Strexd(Condition cond,
              Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    strexd(cond, rd, rt, rt2, operand);
  }
  void Strexd(Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    Strexd(al, rd, rt, rt2, operand);
  }

  void Strexh(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    strexh(cond, rd, rt, operand);
  }
  void Strexh(Register rd, Register rt, const MemOperand& operand) {
    Strexh(al, rd, rt, operand);
  }

  void Strh(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // STRH{<c>}{<q>} <Rt>, [<Rn> {, #{+}<imm>}] ; T1
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.IsOffsetImmediateWithinRange(0, 62, 2) &&
         (operand.GetAddrMode() == Offset)) ||
        // STRH{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        (operand.IsPlainRegister() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
         (operand.GetAddrMode() == Offset));
    ITScope it_scope(this, &cond, can_use_it);
    strh(cond, rt, operand);
  }
  void Strh(Register rt, const MemOperand& operand) { Strh(al, rt, operand); }

  void Sub(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // SUB<c>{<q>} <Rd>, <Rn>, #<imm3> ; T1
        (operand.IsImmediate() && (operand.GetImmediate() <= 7) && rn.IsLow() &&
         rd.IsLow()) ||
        // SUB<c>{<q>} {<Rdn>,} <Rdn>, #<imm8> ; T2
        (operand.IsImmediate() && (operand.GetImmediate() <= 255) &&
         rd.IsLow() && rn.Is(rd)) ||
        // SUB<c>{<q>} <Rd>, <Rn>, <Rm>
        (operand.IsPlainRegister() && rd.IsLow() && rn.IsLow() &&
         operand.GetBaseRegister().IsLow());
    ITScope it_scope(this, &cond, can_use_it);
    sub(cond, rd, rn, operand);
  }
  void Sub(Register rd, Register rn, const Operand& operand) {
    Sub(al, rd, rn, operand);
  }

  void Subs(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    subs(cond, rd, rn, operand);
  }
  void Subs(Register rd, Register rn, const Operand& operand) {
    Subs(al, rd, rn, operand);
  }

  void Subw(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    subw(cond, rd, rn, operand);
  }
  void Subw(Register rd, Register rn, const Operand& operand) {
    Subw(al, rd, rn, operand);
  }

  void Svc(Condition cond, uint32_t imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    svc(cond, imm);
  }
  void Svc(uint32_t imm) { Svc(al, imm); }

  void Sxtab(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sxtab(cond, rd, rn, operand);
  }
  void Sxtab(Register rd, Register rn, const Operand& operand) {
    Sxtab(al, rd, rn, operand);
  }

  void Sxtab16(Condition cond,
               Register rd,
               Register rn,
               const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sxtab16(cond, rd, rn, operand);
  }
  void Sxtab16(Register rd, Register rn, const Operand& operand) {
    Sxtab16(al, rd, rn, operand);
  }

  void Sxtah(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sxtah(cond, rd, rn, operand);
  }
  void Sxtah(Register rd, Register rn, const Operand& operand) {
    Sxtah(al, rd, rn, operand);
  }

  void Sxtb(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sxtb(cond, rd, operand);
  }
  void Sxtb(Register rd, const Operand& operand) { Sxtb(al, rd, operand); }

  void Sxtb16(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sxtb16(cond, rd, operand);
  }
  void Sxtb16(Register rd, const Operand& operand) { Sxtb16(al, rd, operand); }

  void Sxth(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    sxth(cond, rd, operand);
  }
  void Sxth(Register rd, const Operand& operand) { Sxth(al, rd, operand); }

  void Tbb(Condition cond, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    tbb(cond, rn, rm);
  }
  void Tbb(Register rn, Register rm) { Tbb(al, rn, rm); }

  void Tbh(Condition cond, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    tbh(cond, rn, rm);
  }
  void Tbh(Register rn, Register rm) { Tbh(al, rn, rm); }

  void Teq(Condition cond, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    teq(cond, rn, operand);
  }
  void Teq(Register rn, const Operand& operand) { Teq(al, rn, operand); }

  void Tst(Condition cond, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    bool can_use_it =
        // TST{<c>}{<q>} <Rn>, <Rm> ; T1
        operand.IsPlainRegister() && rn.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, can_use_it);
    tst(cond, rn, operand);
  }
  void Tst(Register rn, const Operand& operand) { Tst(al, rn, operand); }

  void Uadd16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uadd16(cond, rd, rn, rm);
  }
  void Uadd16(Register rd, Register rn, Register rm) { Uadd16(al, rd, rn, rm); }

  void Uadd8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uadd8(cond, rd, rn, rm);
  }
  void Uadd8(Register rd, Register rn, Register rm) { Uadd8(al, rd, rn, rm); }

  void Uasx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uasx(cond, rd, rn, rm);
  }
  void Uasx(Register rd, Register rn, Register rm) { Uasx(al, rd, rn, rm); }

  void Ubfx(Condition cond,
            Register rd,
            Register rn,
            uint32_t lsb,
            const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    ubfx(cond, rd, rn, lsb, operand);
  }
  void Ubfx(Register rd, Register rn, uint32_t lsb, const Operand& operand) {
    Ubfx(al, rd, rn, lsb, operand);
  }

  void Udf(Condition cond, uint32_t imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    udf(cond, imm);
  }
  void Udf(uint32_t imm) { Udf(al, imm); }

  void Udiv(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    udiv(cond, rd, rn, rm);
  }
  void Udiv(Register rd, Register rn, Register rm) { Udiv(al, rd, rn, rm); }

  void Uhadd16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uhadd16(cond, rd, rn, rm);
  }
  void Uhadd16(Register rd, Register rn, Register rm) {
    Uhadd16(al, rd, rn, rm);
  }

  void Uhadd8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uhadd8(cond, rd, rn, rm);
  }
  void Uhadd8(Register rd, Register rn, Register rm) { Uhadd8(al, rd, rn, rm); }

  void Uhasx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uhasx(cond, rd, rn, rm);
  }
  void Uhasx(Register rd, Register rn, Register rm) { Uhasx(al, rd, rn, rm); }

  void Uhsax(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uhsax(cond, rd, rn, rm);
  }
  void Uhsax(Register rd, Register rn, Register rm) { Uhsax(al, rd, rn, rm); }

  void Uhsub16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uhsub16(cond, rd, rn, rm);
  }
  void Uhsub16(Register rd, Register rn, Register rm) {
    Uhsub16(al, rd, rn, rm);
  }

  void Uhsub8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uhsub8(cond, rd, rn, rm);
  }
  void Uhsub8(Register rd, Register rn, Register rm) { Uhsub8(al, rd, rn, rm); }

  void Umaal(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    umaal(cond, rdlo, rdhi, rn, rm);
  }
  void Umaal(Register rdlo, Register rdhi, Register rn, Register rm) {
    Umaal(al, rdlo, rdhi, rn, rm);
  }

  void Umlal(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    umlal(cond, rdlo, rdhi, rn, rm);
  }
  void Umlal(Register rdlo, Register rdhi, Register rn, Register rm) {
    Umlal(al, rdlo, rdhi, rn, rm);
  }

  void Umlals(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    umlals(cond, rdlo, rdhi, rn, rm);
  }
  void Umlals(Register rdlo, Register rdhi, Register rn, Register rm) {
    Umlals(al, rdlo, rdhi, rn, rm);
  }

  void Umull(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    umull(cond, rdlo, rdhi, rn, rm);
  }
  void Umull(Register rdlo, Register rdhi, Register rn, Register rm) {
    Umull(al, rdlo, rdhi, rn, rm);
  }

  void Umulls(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    umulls(cond, rdlo, rdhi, rn, rm);
  }
  void Umulls(Register rdlo, Register rdhi, Register rn, Register rm) {
    Umulls(al, rdlo, rdhi, rn, rm);
  }

  void Uqadd16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uqadd16(cond, rd, rn, rm);
  }
  void Uqadd16(Register rd, Register rn, Register rm) {
    Uqadd16(al, rd, rn, rm);
  }

  void Uqadd8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uqadd8(cond, rd, rn, rm);
  }
  void Uqadd8(Register rd, Register rn, Register rm) { Uqadd8(al, rd, rn, rm); }

  void Uqasx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uqasx(cond, rd, rn, rm);
  }
  void Uqasx(Register rd, Register rn, Register rm) { Uqasx(al, rd, rn, rm); }

  void Uqsax(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uqsax(cond, rd, rn, rm);
  }
  void Uqsax(Register rd, Register rn, Register rm) { Uqsax(al, rd, rn, rm); }

  void Uqsub16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uqsub16(cond, rd, rn, rm);
  }
  void Uqsub16(Register rd, Register rn, Register rm) {
    Uqsub16(al, rd, rn, rm);
  }

  void Uqsub8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uqsub8(cond, rd, rn, rm);
  }
  void Uqsub8(Register rd, Register rn, Register rm) { Uqsub8(al, rd, rn, rm); }

  void Usad8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    usad8(cond, rd, rn, rm);
  }
  void Usad8(Register rd, Register rn, Register rm) { Usad8(al, rd, rn, rm); }

  void Usada8(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    usada8(cond, rd, rn, rm, ra);
  }
  void Usada8(Register rd, Register rn, Register rm, Register ra) {
    Usada8(al, rd, rn, rm, ra);
  }

  void Usat(Condition cond, Register rd, uint32_t imm, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    usat(cond, rd, imm, operand);
  }
  void Usat(Register rd, uint32_t imm, const Operand& operand) {
    Usat(al, rd, imm, operand);
  }

  void Usat16(Condition cond, Register rd, uint32_t imm, Register rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    usat16(cond, rd, imm, rn);
  }
  void Usat16(Register rd, uint32_t imm, Register rn) {
    Usat16(al, rd, imm, rn);
  }

  void Usax(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    usax(cond, rd, rn, rm);
  }
  void Usax(Register rd, Register rn, Register rm) { Usax(al, rd, rn, rm); }

  void Usub16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    usub16(cond, rd, rn, rm);
  }
  void Usub16(Register rd, Register rn, Register rm) { Usub16(al, rd, rn, rm); }

  void Usub8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    usub8(cond, rd, rn, rm);
  }
  void Usub8(Register rd, Register rn, Register rm) { Usub8(al, rd, rn, rm); }

  void Uxtab(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uxtab(cond, rd, rn, operand);
  }
  void Uxtab(Register rd, Register rn, const Operand& operand) {
    Uxtab(al, rd, rn, operand);
  }

  void Uxtab16(Condition cond,
               Register rd,
               Register rn,
               const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uxtab16(cond, rd, rn, operand);
  }
  void Uxtab16(Register rd, Register rn, const Operand& operand) {
    Uxtab16(al, rd, rn, operand);
  }

  void Uxtah(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uxtah(cond, rd, rn, operand);
  }
  void Uxtah(Register rd, Register rn, const Operand& operand) {
    Uxtah(al, rd, rn, operand);
  }

  void Uxtb(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uxtb(cond, rd, operand);
  }
  void Uxtb(Register rd, const Operand& operand) { Uxtb(al, rd, operand); }

  void Uxtb16(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uxtb16(cond, rd, operand);
  }
  void Uxtb16(Register rd, const Operand& operand) { Uxtb16(al, rd, operand); }

  void Uxth(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    uxth(cond, rd, operand);
  }
  void Uxth(Register rd, const Operand& operand) { Uxth(al, rd, operand); }

  void Vaba(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vaba(cond, dt, rd, rn, rm);
  }
  void Vaba(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vaba(al, dt, rd, rn, rm);
  }

  void Vaba(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vaba(cond, dt, rd, rn, rm);
  }
  void Vaba(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vaba(al, dt, rd, rn, rm);
  }

  void Vabal(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vabal(cond, dt, rd, rn, rm);
  }
  void Vabal(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vabal(al, dt, rd, rn, rm);
  }

  void Vabd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vabd(cond, dt, rd, rn, rm);
  }
  void Vabd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vabd(al, dt, rd, rn, rm);
  }

  void Vabd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vabd(cond, dt, rd, rn, rm);
  }
  void Vabd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vabd(al, dt, rd, rn, rm);
  }

  void Vabdl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vabdl(cond, dt, rd, rn, rm);
  }
  void Vabdl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vabdl(al, dt, rd, rn, rm);
  }

  void Vabs(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vabs(cond, dt, rd, rm);
  }
  void Vabs(DataType dt, DRegister rd, DRegister rm) { Vabs(al, dt, rd, rm); }

  void Vabs(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vabs(cond, dt, rd, rm);
  }
  void Vabs(DataType dt, QRegister rd, QRegister rm) { Vabs(al, dt, rd, rm); }

  void Vabs(Condition cond, DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vabs(cond, dt, rd, rm);
  }
  void Vabs(DataType dt, SRegister rd, SRegister rm) { Vabs(al, dt, rd, rm); }

  void Vacge(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vacge(cond, dt, rd, rn, rm);
  }
  void Vacge(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vacge(al, dt, rd, rn, rm);
  }

  void Vacge(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vacge(cond, dt, rd, rn, rm);
  }
  void Vacge(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vacge(al, dt, rd, rn, rm);
  }

  void Vacgt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vacgt(cond, dt, rd, rn, rm);
  }
  void Vacgt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vacgt(al, dt, rd, rn, rm);
  }

  void Vacgt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vacgt(cond, dt, rd, rn, rm);
  }
  void Vacgt(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vacgt(al, dt, rd, rn, rm);
  }

  void Vacle(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vacle(cond, dt, rd, rn, rm);
  }
  void Vacle(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vacle(al, dt, rd, rn, rm);
  }

  void Vacle(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vacle(cond, dt, rd, rn, rm);
  }
  void Vacle(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vacle(al, dt, rd, rn, rm);
  }

  void Vaclt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vaclt(cond, dt, rd, rn, rm);
  }
  void Vaclt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vaclt(al, dt, rd, rn, rm);
  }

  void Vaclt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vaclt(cond, dt, rd, rn, rm);
  }
  void Vaclt(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vaclt(al, dt, rd, rn, rm);
  }

  void Vadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vadd(cond, dt, rd, rn, rm);
  }
  void Vadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vadd(al, dt, rd, rn, rm);
  }

  void Vadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vadd(cond, dt, rd, rn, rm);
  }
  void Vadd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vadd(al, dt, rd, rn, rm);
  }

  void Vadd(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vadd(cond, dt, rd, rn, rm);
  }
  void Vadd(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vadd(al, dt, rd, rn, rm);
  }

  void Vaddhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vaddhn(cond, dt, rd, rn, rm);
  }
  void Vaddhn(DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    Vaddhn(al, dt, rd, rn, rm);
  }

  void Vaddl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vaddl(cond, dt, rd, rn, rm);
  }
  void Vaddl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vaddl(al, dt, rd, rn, rm);
  }

  void Vaddw(
      Condition cond, DataType dt, QRegister rd, QRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vaddw(cond, dt, rd, rn, rm);
  }
  void Vaddw(DataType dt, QRegister rd, QRegister rn, DRegister rm) {
    Vaddw(al, dt, rd, rn, rm);
  }

  void Vand(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vand(cond, dt, rd, rn, operand);
  }
  void Vand(DataType dt, DRegister rd, DRegister rn, const DOperand& operand) {
    Vand(al, dt, rd, rn, operand);
  }

  void Vand(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vand(cond, dt, rd, rn, operand);
  }
  void Vand(DataType dt, QRegister rd, QRegister rn, const QOperand& operand) {
    Vand(al, dt, rd, rn, operand);
  }

  void Vbic(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vbic(cond, dt, rd, rn, operand);
  }
  void Vbic(DataType dt, DRegister rd, DRegister rn, const DOperand& operand) {
    Vbic(al, dt, rd, rn, operand);
  }

  void Vbic(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vbic(cond, dt, rd, rn, operand);
  }
  void Vbic(DataType dt, QRegister rd, QRegister rn, const QOperand& operand) {
    Vbic(al, dt, rd, rn, operand);
  }

  void Vbif(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vbif(cond, dt, rd, rn, rm);
  }
  void Vbif(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vbif(al, dt, rd, rn, rm);
  }
  void Vbif(Condition cond, DRegister rd, DRegister rn, DRegister rm) {
    Vbif(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Vbif(DRegister rd, DRegister rn, DRegister rm) {
    Vbif(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vbif(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vbif(cond, dt, rd, rn, rm);
  }
  void Vbif(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vbif(al, dt, rd, rn, rm);
  }
  void Vbif(Condition cond, QRegister rd, QRegister rn, QRegister rm) {
    Vbif(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Vbif(QRegister rd, QRegister rn, QRegister rm) {
    Vbif(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vbit(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vbit(cond, dt, rd, rn, rm);
  }
  void Vbit(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vbit(al, dt, rd, rn, rm);
  }
  void Vbit(Condition cond, DRegister rd, DRegister rn, DRegister rm) {
    Vbit(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Vbit(DRegister rd, DRegister rn, DRegister rm) {
    Vbit(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vbit(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vbit(cond, dt, rd, rn, rm);
  }
  void Vbit(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vbit(al, dt, rd, rn, rm);
  }
  void Vbit(Condition cond, QRegister rd, QRegister rn, QRegister rm) {
    Vbit(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Vbit(QRegister rd, QRegister rn, QRegister rm) {
    Vbit(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vbsl(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vbsl(cond, dt, rd, rn, rm);
  }
  void Vbsl(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vbsl(al, dt, rd, rn, rm);
  }
  void Vbsl(Condition cond, DRegister rd, DRegister rn, DRegister rm) {
    Vbsl(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Vbsl(DRegister rd, DRegister rn, DRegister rm) {
    Vbsl(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vbsl(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vbsl(cond, dt, rd, rn, rm);
  }
  void Vbsl(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vbsl(al, dt, rd, rn, rm);
  }
  void Vbsl(Condition cond, QRegister rd, QRegister rn, QRegister rm) {
    Vbsl(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Vbsl(QRegister rd, QRegister rn, QRegister rm) {
    Vbsl(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vceq(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vceq(cond, dt, rd, rm, operand);
  }
  void Vceq(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vceq(al, dt, rd, rm, operand);
  }

  void Vceq(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vceq(cond, dt, rd, rm, operand);
  }
  void Vceq(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vceq(al, dt, rd, rm, operand);
  }

  void Vceq(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vceq(cond, dt, rd, rn, rm);
  }
  void Vceq(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vceq(al, dt, rd, rn, rm);
  }

  void Vceq(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vceq(cond, dt, rd, rn, rm);
  }
  void Vceq(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vceq(al, dt, rd, rn, rm);
  }

  void Vcge(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcge(cond, dt, rd, rm, operand);
  }
  void Vcge(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vcge(al, dt, rd, rm, operand);
  }

  void Vcge(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcge(cond, dt, rd, rm, operand);
  }
  void Vcge(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vcge(al, dt, rd, rm, operand);
  }

  void Vcge(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcge(cond, dt, rd, rn, rm);
  }
  void Vcge(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vcge(al, dt, rd, rn, rm);
  }

  void Vcge(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcge(cond, dt, rd, rn, rm);
  }
  void Vcge(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vcge(al, dt, rd, rn, rm);
  }

  void Vcgt(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcgt(cond, dt, rd, rm, operand);
  }
  void Vcgt(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vcgt(al, dt, rd, rm, operand);
  }

  void Vcgt(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcgt(cond, dt, rd, rm, operand);
  }
  void Vcgt(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vcgt(al, dt, rd, rm, operand);
  }

  void Vcgt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcgt(cond, dt, rd, rn, rm);
  }
  void Vcgt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vcgt(al, dt, rd, rn, rm);
  }

  void Vcgt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcgt(cond, dt, rd, rn, rm);
  }
  void Vcgt(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vcgt(al, dt, rd, rn, rm);
  }

  void Vcle(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcle(cond, dt, rd, rm, operand);
  }
  void Vcle(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vcle(al, dt, rd, rm, operand);
  }

  void Vcle(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcle(cond, dt, rd, rm, operand);
  }
  void Vcle(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vcle(al, dt, rd, rm, operand);
  }

  void Vcle(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcle(cond, dt, rd, rn, rm);
  }
  void Vcle(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vcle(al, dt, rd, rn, rm);
  }

  void Vcle(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcle(cond, dt, rd, rn, rm);
  }
  void Vcle(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vcle(al, dt, rd, rn, rm);
  }

  void Vcls(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcls(cond, dt, rd, rm);
  }
  void Vcls(DataType dt, DRegister rd, DRegister rm) { Vcls(al, dt, rd, rm); }

  void Vcls(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcls(cond, dt, rd, rm);
  }
  void Vcls(DataType dt, QRegister rd, QRegister rm) { Vcls(al, dt, rd, rm); }

  void Vclt(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vclt(cond, dt, rd, rm, operand);
  }
  void Vclt(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vclt(al, dt, rd, rm, operand);
  }

  void Vclt(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vclt(cond, dt, rd, rm, operand);
  }
  void Vclt(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vclt(al, dt, rd, rm, operand);
  }

  void Vclt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vclt(cond, dt, rd, rn, rm);
  }
  void Vclt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vclt(al, dt, rd, rn, rm);
  }

  void Vclt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vclt(cond, dt, rd, rn, rm);
  }
  void Vclt(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vclt(al, dt, rd, rn, rm);
  }

  void Vclz(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vclz(cond, dt, rd, rm);
  }
  void Vclz(DataType dt, DRegister rd, DRegister rm) { Vclz(al, dt, rd, rm); }

  void Vclz(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vclz(cond, dt, rd, rm);
  }
  void Vclz(DataType dt, QRegister rd, QRegister rm) { Vclz(al, dt, rd, rm); }

  void Vcmp(Condition cond, DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcmp(cond, dt, rd, rm);
  }
  void Vcmp(DataType dt, SRegister rd, SRegister rm) { Vcmp(al, dt, rd, rm); }

  void Vcmp(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcmp(cond, dt, rd, rm);
  }
  void Vcmp(DataType dt, DRegister rd, DRegister rm) { Vcmp(al, dt, rd, rm); }

  void Vcmp(Condition cond, DataType dt, SRegister rd, double imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcmp(cond, dt, rd, imm);
  }
  void Vcmp(DataType dt, SRegister rd, double imm) { Vcmp(al, dt, rd, imm); }

  void Vcmp(Condition cond, DataType dt, DRegister rd, double imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcmp(cond, dt, rd, imm);
  }
  void Vcmp(DataType dt, DRegister rd, double imm) { Vcmp(al, dt, rd, imm); }

  void Vcmpe(Condition cond, DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcmpe(cond, dt, rd, rm);
  }
  void Vcmpe(DataType dt, SRegister rd, SRegister rm) { Vcmpe(al, dt, rd, rm); }

  void Vcmpe(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcmpe(cond, dt, rd, rm);
  }
  void Vcmpe(DataType dt, DRegister rd, DRegister rm) { Vcmpe(al, dt, rd, rm); }

  void Vcmpe(Condition cond, DataType dt, SRegister rd, double imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcmpe(cond, dt, rd, imm);
  }
  void Vcmpe(DataType dt, SRegister rd, double imm) { Vcmpe(al, dt, rd, imm); }

  void Vcmpe(Condition cond, DataType dt, DRegister rd, double imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcmpe(cond, dt, rd, imm);
  }
  void Vcmpe(DataType dt, DRegister rd, double imm) { Vcmpe(al, dt, rd, imm); }

  void Vcnt(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcnt(cond, dt, rd, rm);
  }
  void Vcnt(DataType dt, DRegister rd, DRegister rm) { Vcnt(al, dt, rd, rm); }

  void Vcnt(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcnt(cond, dt, rd, rm);
  }
  void Vcnt(DataType dt, QRegister rd, QRegister rm) { Vcnt(al, dt, rd, rm); }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvt(Condition cond,
            DataType dt1,
            DataType dt2,
            DRegister rd,
            DRegister rm,
            int32_t fbits) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvt(cond, dt1, dt2, rd, rm, fbits);
  }
  void Vcvt(
      DataType dt1, DataType dt2, DRegister rd, DRegister rm, int32_t fbits) {
    Vcvt(al, dt1, dt2, rd, rm, fbits);
  }

  void Vcvt(Condition cond,
            DataType dt1,
            DataType dt2,
            QRegister rd,
            QRegister rm,
            int32_t fbits) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvt(cond, dt1, dt2, rd, rm, fbits);
  }
  void Vcvt(
      DataType dt1, DataType dt2, QRegister rd, QRegister rm, int32_t fbits) {
    Vcvt(al, dt1, dt2, rd, rm, fbits);
  }

  void Vcvt(Condition cond,
            DataType dt1,
            DataType dt2,
            SRegister rd,
            SRegister rm,
            int32_t fbits) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvt(cond, dt1, dt2, rd, rm, fbits);
  }
  void Vcvt(
      DataType dt1, DataType dt2, SRegister rd, SRegister rm, int32_t fbits) {
    Vcvt(al, dt1, dt2, rd, rm, fbits);
  }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, DRegister rd, QRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, QRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, QRegister rd, DRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvta(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvta(dt1, dt2, rd, rm);
  }

  void Vcvta(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvta(dt1, dt2, rd, rm);
  }

  void Vcvta(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvta(dt1, dt2, rd, rm);
  }

  void Vcvta(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvta(dt1, dt2, rd, rm);
  }

  void Vcvtb(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvtb(cond, dt1, dt2, rd, rm);
  }
  void Vcvtb(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    Vcvtb(al, dt1, dt2, rd, rm);
  }

  void Vcvtb(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvtb(cond, dt1, dt2, rd, rm);
  }
  void Vcvtb(DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    Vcvtb(al, dt1, dt2, rd, rm);
  }

  void Vcvtb(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvtb(cond, dt1, dt2, rd, rm);
  }
  void Vcvtb(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    Vcvtb(al, dt1, dt2, rd, rm);
  }

  void Vcvtm(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvtm(dt1, dt2, rd, rm);
  }

  void Vcvtm(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvtm(dt1, dt2, rd, rm);
  }

  void Vcvtm(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvtm(dt1, dt2, rd, rm);
  }

  void Vcvtm(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvtm(dt1, dt2, rd, rm);
  }

  void Vcvtn(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvtn(dt1, dt2, rd, rm);
  }

  void Vcvtn(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvtn(dt1, dt2, rd, rm);
  }

  void Vcvtn(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvtn(dt1, dt2, rd, rm);
  }

  void Vcvtn(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvtn(dt1, dt2, rd, rm);
  }

  void Vcvtp(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvtp(dt1, dt2, rd, rm);
  }

  void Vcvtp(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvtp(dt1, dt2, rd, rm);
  }

  void Vcvtp(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvtp(dt1, dt2, rd, rm);
  }

  void Vcvtp(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vcvtp(dt1, dt2, rd, rm);
  }

  void Vcvtr(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvtr(cond, dt1, dt2, rd, rm);
  }
  void Vcvtr(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    Vcvtr(al, dt1, dt2, rd, rm);
  }

  void Vcvtr(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvtr(cond, dt1, dt2, rd, rm);
  }
  void Vcvtr(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    Vcvtr(al, dt1, dt2, rd, rm);
  }

  void Vcvtt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvtt(cond, dt1, dt2, rd, rm);
  }
  void Vcvtt(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    Vcvtt(al, dt1, dt2, rd, rm);
  }

  void Vcvtt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvtt(cond, dt1, dt2, rd, rm);
  }
  void Vcvtt(DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    Vcvtt(al, dt1, dt2, rd, rm);
  }

  void Vcvtt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vcvtt(cond, dt1, dt2, rd, rm);
  }
  void Vcvtt(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    Vcvtt(al, dt1, dt2, rd, rm);
  }

  void Vdiv(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vdiv(cond, dt, rd, rn, rm);
  }
  void Vdiv(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vdiv(al, dt, rd, rn, rm);
  }

  void Vdiv(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vdiv(cond, dt, rd, rn, rm);
  }
  void Vdiv(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vdiv(al, dt, rd, rn, rm);
  }

  void Vdup(Condition cond, DataType dt, QRegister rd, Register rt) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vdup(cond, dt, rd, rt);
  }
  void Vdup(DataType dt, QRegister rd, Register rt) { Vdup(al, dt, rd, rt); }

  void Vdup(Condition cond, DataType dt, DRegister rd, Register rt) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vdup(cond, dt, rd, rt);
  }
  void Vdup(DataType dt, DRegister rd, Register rt) { Vdup(al, dt, rd, rt); }

  void Vdup(Condition cond, DataType dt, DRegister rd, DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vdup(cond, dt, rd, rm);
  }
  void Vdup(DataType dt, DRegister rd, DRegisterLane rm) {
    Vdup(al, dt, rd, rm);
  }

  void Vdup(Condition cond, DataType dt, QRegister rd, DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vdup(cond, dt, rd, rm);
  }
  void Vdup(DataType dt, QRegister rd, DRegisterLane rm) {
    Vdup(al, dt, rd, rm);
  }

  void Veor(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    veor(cond, dt, rd, rn, rm);
  }
  void Veor(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Veor(al, dt, rd, rn, rm);
  }
  void Veor(Condition cond, DRegister rd, DRegister rn, DRegister rm) {
    Veor(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Veor(DRegister rd, DRegister rn, DRegister rm) {
    Veor(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Veor(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    veor(cond, dt, rd, rn, rm);
  }
  void Veor(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Veor(al, dt, rd, rn, rm);
  }
  void Veor(Condition cond, QRegister rd, QRegister rn, QRegister rm) {
    Veor(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Veor(QRegister rd, QRegister rn, QRegister rm) {
    Veor(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vext(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vext(cond, dt, rd, rn, rm, operand);
  }
  void Vext(DataType dt,
            DRegister rd,
            DRegister rn,
            DRegister rm,
            const DOperand& operand) {
    Vext(al, dt, rd, rn, rm, operand);
  }

  void Vext(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vext(cond, dt, rd, rn, rm, operand);
  }
  void Vext(DataType dt,
            QRegister rd,
            QRegister rn,
            QRegister rm,
            const QOperand& operand) {
    Vext(al, dt, rd, rn, rm, operand);
  }

  void Vfma(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vfma(cond, dt, rd, rn, rm);
  }
  void Vfma(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vfma(al, dt, rd, rn, rm);
  }

  void Vfma(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vfma(cond, dt, rd, rn, rm);
  }
  void Vfma(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vfma(al, dt, rd, rn, rm);
  }

  void Vfma(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vfma(cond, dt, rd, rn, rm);
  }
  void Vfma(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vfma(al, dt, rd, rn, rm);
  }

  void Vfms(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vfms(cond, dt, rd, rn, rm);
  }
  void Vfms(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vfms(al, dt, rd, rn, rm);
  }

  void Vfms(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vfms(cond, dt, rd, rn, rm);
  }
  void Vfms(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vfms(al, dt, rd, rn, rm);
  }

  void Vfms(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vfms(cond, dt, rd, rn, rm);
  }
  void Vfms(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vfms(al, dt, rd, rn, rm);
  }

  void Vfnma(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vfnma(cond, dt, rd, rn, rm);
  }
  void Vfnma(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vfnma(al, dt, rd, rn, rm);
  }

  void Vfnma(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vfnma(cond, dt, rd, rn, rm);
  }
  void Vfnma(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vfnma(al, dt, rd, rn, rm);
  }

  void Vfnms(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vfnms(cond, dt, rd, rn, rm);
  }
  void Vfnms(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vfnms(al, dt, rd, rn, rm);
  }

  void Vfnms(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vfnms(cond, dt, rd, rn, rm);
  }
  void Vfnms(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vfnms(al, dt, rd, rn, rm);
  }

  void Vhadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vhadd(cond, dt, rd, rn, rm);
  }
  void Vhadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vhadd(al, dt, rd, rn, rm);
  }

  void Vhadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vhadd(cond, dt, rd, rn, rm);
  }
  void Vhadd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vhadd(al, dt, rd, rn, rm);
  }

  void Vhsub(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vhsub(cond, dt, rd, rn, rm);
  }
  void Vhsub(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vhsub(al, dt, rd, rn, rm);
  }

  void Vhsub(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vhsub(cond, dt, rd, rn, rm);
  }
  void Vhsub(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vhsub(al, dt, rd, rn, rm);
  }

  void Vld1(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vld1(cond, dt, nreglist, operand);
  }
  void Vld1(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vld1(al, dt, nreglist, operand);
  }

  void Vld2(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vld2(cond, dt, nreglist, operand);
  }
  void Vld2(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vld2(al, dt, nreglist, operand);
  }

  void Vld3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vld3(cond, dt, nreglist, operand);
  }
  void Vld3(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vld3(al, dt, nreglist, operand);
  }

  void Vld3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vld3(cond, dt, nreglist, operand);
  }
  void Vld3(DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand) {
    Vld3(al, dt, nreglist, operand);
  }

  void Vld4(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vld4(cond, dt, nreglist, operand);
  }
  void Vld4(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vld4(al, dt, nreglist, operand);
  }

  void Vldm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vldm(cond, dt, rn, write_back, dreglist);
  }
  void Vldm(DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    Vldm(al, dt, rn, write_back, dreglist);
  }
  void Vldm(Condition cond,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    Vldm(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void Vldm(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Vldm(al, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void Vldm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vldm(cond, dt, rn, write_back, sreglist);
  }
  void Vldm(DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    Vldm(al, dt, rn, write_back, sreglist);
  }
  void Vldm(Condition cond,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    Vldm(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void Vldm(Register rn, WriteBack write_back, SRegisterList sreglist) {
    Vldm(al, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void Vldmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vldmdb(cond, dt, rn, write_back, dreglist);
  }
  void Vldmdb(DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vldmdb(al, dt, rn, write_back, dreglist);
  }
  void Vldmdb(Condition cond,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vldmdb(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void Vldmdb(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Vldmdb(al, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void Vldmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vldmdb(cond, dt, rn, write_back, sreglist);
  }
  void Vldmdb(DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vldmdb(al, dt, rn, write_back, sreglist);
  }
  void Vldmdb(Condition cond,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vldmdb(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void Vldmdb(Register rn, WriteBack write_back, SRegisterList sreglist) {
    Vldmdb(al, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void Vldmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vldmia(cond, dt, rn, write_back, dreglist);
  }
  void Vldmia(DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vldmia(al, dt, rn, write_back, dreglist);
  }
  void Vldmia(Condition cond,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vldmia(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void Vldmia(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Vldmia(al, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void Vldmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vldmia(cond, dt, rn, write_back, sreglist);
  }
  void Vldmia(DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vldmia(al, dt, rn, write_back, sreglist);
  }
  void Vldmia(Condition cond,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vldmia(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void Vldmia(Register rn, WriteBack write_back, SRegisterList sreglist) {
    Vldmia(al, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void Vldr(Condition cond, DataType dt, DRegister rd, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vldr(cond, dt, rd, label);
  }
  void Vldr(DataType dt, DRegister rd, Label* label) {
    Vldr(al, dt, rd, label);
  }
  void Vldr(Condition cond, DRegister rd, Label* label) {
    Vldr(cond, Untyped64, rd, label);
  }
  void Vldr(DRegister rd, Label* label) { Vldr(al, Untyped64, rd, label); }

  void Vldr(Condition cond,
            DataType dt,
            DRegister rd,
            const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vldr(cond, dt, rd, operand);
  }
  void Vldr(DataType dt, DRegister rd, const MemOperand& operand) {
    Vldr(al, dt, rd, operand);
  }
  void Vldr(Condition cond, DRegister rd, const MemOperand& operand) {
    Vldr(cond, Untyped64, rd, operand);
  }
  void Vldr(DRegister rd, const MemOperand& operand) {
    Vldr(al, Untyped64, rd, operand);
  }

  void Vldr(Condition cond, DataType dt, SRegister rd, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vldr(cond, dt, rd, label);
  }
  void Vldr(DataType dt, SRegister rd, Label* label) {
    Vldr(al, dt, rd, label);
  }
  void Vldr(Condition cond, SRegister rd, Label* label) {
    Vldr(cond, Untyped32, rd, label);
  }
  void Vldr(SRegister rd, Label* label) { Vldr(al, Untyped32, rd, label); }

  void Vldr(Condition cond,
            DataType dt,
            SRegister rd,
            const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vldr(cond, dt, rd, operand);
  }
  void Vldr(DataType dt, SRegister rd, const MemOperand& operand) {
    Vldr(al, dt, rd, operand);
  }
  void Vldr(Condition cond, SRegister rd, const MemOperand& operand) {
    Vldr(cond, Untyped32, rd, operand);
  }
  void Vldr(SRegister rd, const MemOperand& operand) {
    Vldr(al, Untyped32, rd, operand);
  }

  void Vmax(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmax(cond, dt, rd, rn, rm);
  }
  void Vmax(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vmax(al, dt, rd, rn, rm);
  }

  void Vmax(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmax(cond, dt, rd, rn, rm);
  }
  void Vmax(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vmax(al, dt, rd, rn, rm);
  }

  void Vmaxnm(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vmaxnm(dt, rd, rn, rm);
  }

  void Vmaxnm(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vmaxnm(dt, rd, rn, rm);
  }

  void Vmaxnm(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vmaxnm(dt, rd, rn, rm);
  }

  void Vmin(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmin(cond, dt, rd, rn, rm);
  }
  void Vmin(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vmin(al, dt, rd, rn, rm);
  }

  void Vmin(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmin(cond, dt, rd, rn, rm);
  }
  void Vmin(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vmin(al, dt, rd, rn, rm);
  }

  void Vminnm(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vminnm(dt, rd, rn, rm);
  }

  void Vminnm(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vminnm(dt, rd, rn, rm);
  }

  void Vminnm(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vminnm(dt, rd, rn, rm);
  }

  void Vmla(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmla(cond, dt, rd, rn, rm);
  }
  void Vmla(DataType dt, DRegister rd, DRegister rn, DRegisterLane rm) {
    Vmla(al, dt, rd, rn, rm);
  }

  void Vmla(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmla(cond, dt, rd, rn, rm);
  }
  void Vmla(DataType dt, QRegister rd, QRegister rn, DRegisterLane rm) {
    Vmla(al, dt, rd, rn, rm);
  }

  void Vmla(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmla(cond, dt, rd, rn, rm);
  }
  void Vmla(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vmla(al, dt, rd, rn, rm);
  }

  void Vmla(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmla(cond, dt, rd, rn, rm);
  }
  void Vmla(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vmla(al, dt, rd, rn, rm);
  }

  void Vmla(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmla(cond, dt, rd, rn, rm);
  }
  void Vmla(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vmla(al, dt, rd, rn, rm);
  }

  void Vmlal(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rn,
             DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmlal(cond, dt, rd, rn, rm);
  }
  void Vmlal(DataType dt, QRegister rd, DRegister rn, DRegisterLane rm) {
    Vmlal(al, dt, rd, rn, rm);
  }

  void Vmlal(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmlal(cond, dt, rd, rn, rm);
  }
  void Vmlal(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vmlal(al, dt, rd, rn, rm);
  }

  void Vmls(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmls(cond, dt, rd, rn, rm);
  }
  void Vmls(DataType dt, DRegister rd, DRegister rn, DRegisterLane rm) {
    Vmls(al, dt, rd, rn, rm);
  }

  void Vmls(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmls(cond, dt, rd, rn, rm);
  }
  void Vmls(DataType dt, QRegister rd, QRegister rn, DRegisterLane rm) {
    Vmls(al, dt, rd, rn, rm);
  }

  void Vmls(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmls(cond, dt, rd, rn, rm);
  }
  void Vmls(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vmls(al, dt, rd, rn, rm);
  }

  void Vmls(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmls(cond, dt, rd, rn, rm);
  }
  void Vmls(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vmls(al, dt, rd, rn, rm);
  }

  void Vmls(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmls(cond, dt, rd, rn, rm);
  }
  void Vmls(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vmls(al, dt, rd, rn, rm);
  }

  void Vmlsl(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rn,
             DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmlsl(cond, dt, rd, rn, rm);
  }
  void Vmlsl(DataType dt, QRegister rd, DRegister rn, DRegisterLane rm) {
    Vmlsl(al, dt, rd, rn, rm);
  }

  void Vmlsl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmlsl(cond, dt, rd, rn, rm);
  }
  void Vmlsl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vmlsl(al, dt, rd, rn, rm);
  }

  void Vmov(Condition cond, Register rt, SRegister rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmov(cond, rt, rn);
  }
  void Vmov(Register rt, SRegister rn) { Vmov(al, rt, rn); }

  void Vmov(Condition cond, SRegister rn, Register rt) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmov(cond, rn, rt);
  }
  void Vmov(SRegister rn, Register rt) { Vmov(al, rn, rt); }

  void Vmov(Condition cond, Register rt, Register rt2, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmov(cond, rt, rt2, rm);
  }
  void Vmov(Register rt, Register rt2, DRegister rm) { Vmov(al, rt, rt2, rm); }

  void Vmov(Condition cond, DRegister rm, Register rt, Register rt2) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmov(cond, rm, rt, rt2);
  }
  void Vmov(DRegister rm, Register rt, Register rt2) { Vmov(al, rm, rt, rt2); }

  void Vmov(
      Condition cond, Register rt, Register rt2, SRegister rm, SRegister rm1) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmov(cond, rt, rt2, rm, rm1);
  }
  void Vmov(Register rt, Register rt2, SRegister rm, SRegister rm1) {
    Vmov(al, rt, rt2, rm, rm1);
  }

  void Vmov(
      Condition cond, SRegister rm, SRegister rm1, Register rt, Register rt2) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmov(cond, rm, rm1, rt, rt2);
  }
  void Vmov(SRegister rm, SRegister rm1, Register rt, Register rt2) {
    Vmov(al, rm, rm1, rt, rt2);
  }

  void Vmov(Condition cond, DataType dt, DRegisterLane rd, Register rt) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmov(cond, dt, rd, rt);
  }
  void Vmov(DataType dt, DRegisterLane rd, Register rt) {
    Vmov(al, dt, rd, rt);
  }
  void Vmov(Condition cond, DRegisterLane rd, Register rt) {
    Vmov(cond, kDataTypeValueNone, rd, rt);
  }
  void Vmov(DRegisterLane rd, Register rt) {
    Vmov(al, kDataTypeValueNone, rd, rt);
  }

  void Vmov(Condition cond,
            DataType dt,
            DRegister rd,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmov(cond, dt, rd, operand);
  }
  void Vmov(DataType dt, DRegister rd, const DOperand& operand) {
    Vmov(al, dt, rd, operand);
  }

  void Vmov(Condition cond,
            DataType dt,
            QRegister rd,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmov(cond, dt, rd, operand);
  }
  void Vmov(DataType dt, QRegister rd, const QOperand& operand) {
    Vmov(al, dt, rd, operand);
  }

  void Vmov(Condition cond,
            DataType dt,
            SRegister rd,
            const SOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmov(cond, dt, rd, operand);
  }
  void Vmov(DataType dt, SRegister rd, const SOperand& operand) {
    Vmov(al, dt, rd, operand);
  }

  void Vmov(Condition cond, DataType dt, Register rt, DRegisterLane rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmov(cond, dt, rt, rn);
  }
  void Vmov(DataType dt, Register rt, DRegisterLane rn) {
    Vmov(al, dt, rt, rn);
  }
  void Vmov(Condition cond, Register rt, DRegisterLane rn) {
    Vmov(cond, kDataTypeValueNone, rt, rn);
  }
  void Vmov(Register rt, DRegisterLane rn) {
    Vmov(al, kDataTypeValueNone, rt, rn);
  }

  void Vmovl(Condition cond, DataType dt, QRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmovl(cond, dt, rd, rm);
  }
  void Vmovl(DataType dt, QRegister rd, DRegister rm) { Vmovl(al, dt, rd, rm); }

  void Vmovn(Condition cond, DataType dt, DRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmovn(cond, dt, rd, rm);
  }
  void Vmovn(DataType dt, DRegister rd, QRegister rm) { Vmovn(al, dt, rd, rm); }

  void Vmrs(Condition cond,
            RegisterOrAPSR_nzcv rt,
            SpecialFPRegister spec_reg) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmrs(cond, rt, spec_reg);
  }
  void Vmrs(RegisterOrAPSR_nzcv rt, SpecialFPRegister spec_reg) {
    Vmrs(al, rt, spec_reg);
  }

  void Vmsr(Condition cond, SpecialFPRegister spec_reg, Register rt) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmsr(cond, spec_reg, rt);
  }
  void Vmsr(SpecialFPRegister spec_reg, Register rt) { Vmsr(al, spec_reg, rt); }

  void Vmul(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegister dm,
            unsigned index) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmul(cond, dt, rd, rn, dm, index);
  }
  void Vmul(
      DataType dt, DRegister rd, DRegister rn, DRegister dm, unsigned index) {
    Vmul(al, dt, rd, rn, dm, index);
  }

  void Vmul(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            DRegister dm,
            unsigned index) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmul(cond, dt, rd, rn, dm, index);
  }
  void Vmul(
      DataType dt, QRegister rd, QRegister rn, DRegister dm, unsigned index) {
    Vmul(al, dt, rd, rn, dm, index);
  }

  void Vmul(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmul(cond, dt, rd, rn, rm);
  }
  void Vmul(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vmul(al, dt, rd, rn, rm);
  }

  void Vmul(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmul(cond, dt, rd, rn, rm);
  }
  void Vmul(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vmul(al, dt, rd, rn, rm);
  }

  void Vmul(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmul(cond, dt, rd, rn, rm);
  }
  void Vmul(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vmul(al, dt, rd, rn, rm);
  }

  void Vmull(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rn,
             DRegister dm,
             unsigned index) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmull(cond, dt, rd, rn, dm, index);
  }
  void Vmull(
      DataType dt, QRegister rd, DRegister rn, DRegister dm, unsigned index) {
    Vmull(al, dt, rd, rn, dm, index);
  }

  void Vmull(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmull(cond, dt, rd, rn, rm);
  }
  void Vmull(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vmull(al, dt, rd, rn, rm);
  }

  void Vmvn(Condition cond,
            DataType dt,
            DRegister rd,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmvn(cond, dt, rd, operand);
  }
  void Vmvn(DataType dt, DRegister rd, const DOperand& operand) {
    Vmvn(al, dt, rd, operand);
  }

  void Vmvn(Condition cond,
            DataType dt,
            QRegister rd,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vmvn(cond, dt, rd, operand);
  }
  void Vmvn(DataType dt, QRegister rd, const QOperand& operand) {
    Vmvn(al, dt, rd, operand);
  }

  void Vneg(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vneg(cond, dt, rd, rm);
  }
  void Vneg(DataType dt, DRegister rd, DRegister rm) { Vneg(al, dt, rd, rm); }

  void Vneg(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vneg(cond, dt, rd, rm);
  }
  void Vneg(DataType dt, QRegister rd, QRegister rm) { Vneg(al, dt, rd, rm); }

  void Vneg(Condition cond, DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vneg(cond, dt, rd, rm);
  }
  void Vneg(DataType dt, SRegister rd, SRegister rm) { Vneg(al, dt, rd, rm); }

  void Vnmla(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vnmla(cond, dt, rd, rn, rm);
  }
  void Vnmla(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vnmla(al, dt, rd, rn, rm);
  }

  void Vnmla(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vnmla(cond, dt, rd, rn, rm);
  }
  void Vnmla(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vnmla(al, dt, rd, rn, rm);
  }

  void Vnmls(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vnmls(cond, dt, rd, rn, rm);
  }
  void Vnmls(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vnmls(al, dt, rd, rn, rm);
  }

  void Vnmls(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vnmls(cond, dt, rd, rn, rm);
  }
  void Vnmls(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vnmls(al, dt, rd, rn, rm);
  }

  void Vnmul(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vnmul(cond, dt, rd, rn, rm);
  }
  void Vnmul(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vnmul(al, dt, rd, rn, rm);
  }

  void Vnmul(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vnmul(cond, dt, rd, rn, rm);
  }
  void Vnmul(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vnmul(al, dt, rd, rn, rm);
  }

  void Vorn(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vorn(cond, dt, rd, rn, operand);
  }
  void Vorn(DataType dt, DRegister rd, DRegister rn, const DOperand& operand) {
    Vorn(al, dt, rd, rn, operand);
  }

  void Vorn(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vorn(cond, dt, rd, rn, operand);
  }
  void Vorn(DataType dt, QRegister rd, QRegister rn, const QOperand& operand) {
    Vorn(al, dt, rd, rn, operand);
  }

  void Vorr(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vorr(cond, dt, rd, rn, operand);
  }
  void Vorr(DataType dt, DRegister rd, DRegister rn, const DOperand& operand) {
    Vorr(al, dt, rd, rn, operand);
  }
  void Vorr(Condition cond,
            DRegister rd,
            DRegister rn,
            const DOperand& operand) {
    Vorr(cond, kDataTypeValueNone, rd, rn, operand);
  }
  void Vorr(DRegister rd, DRegister rn, const DOperand& operand) {
    Vorr(al, kDataTypeValueNone, rd, rn, operand);
  }

  void Vorr(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vorr(cond, dt, rd, rn, operand);
  }
  void Vorr(DataType dt, QRegister rd, QRegister rn, const QOperand& operand) {
    Vorr(al, dt, rd, rn, operand);
  }
  void Vorr(Condition cond,
            QRegister rd,
            QRegister rn,
            const QOperand& operand) {
    Vorr(cond, kDataTypeValueNone, rd, rn, operand);
  }
  void Vorr(QRegister rd, QRegister rn, const QOperand& operand) {
    Vorr(al, kDataTypeValueNone, rd, rn, operand);
  }

  void Vpadal(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vpadal(cond, dt, rd, rm);
  }
  void Vpadal(DataType dt, DRegister rd, DRegister rm) {
    Vpadal(al, dt, rd, rm);
  }

  void Vpadal(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vpadal(cond, dt, rd, rm);
  }
  void Vpadal(DataType dt, QRegister rd, QRegister rm) {
    Vpadal(al, dt, rd, rm);
  }

  void Vpadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vpadd(cond, dt, rd, rn, rm);
  }
  void Vpadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vpadd(al, dt, rd, rn, rm);
  }

  void Vpaddl(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vpaddl(cond, dt, rd, rm);
  }
  void Vpaddl(DataType dt, DRegister rd, DRegister rm) {
    Vpaddl(al, dt, rd, rm);
  }

  void Vpaddl(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vpaddl(cond, dt, rd, rm);
  }
  void Vpaddl(DataType dt, QRegister rd, QRegister rm) {
    Vpaddl(al, dt, rd, rm);
  }

  void Vpmax(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vpmax(cond, dt, rd, rn, rm);
  }
  void Vpmax(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vpmax(al, dt, rd, rn, rm);
  }

  void Vpmin(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vpmin(cond, dt, rd, rn, rm);
  }
  void Vpmin(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vpmin(al, dt, rd, rn, rm);
  }

  void Vpop(Condition cond, DataType dt, DRegisterList dreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vpop(cond, dt, dreglist);
  }
  void Vpop(DataType dt, DRegisterList dreglist) { Vpop(al, dt, dreglist); }
  void Vpop(Condition cond, DRegisterList dreglist) {
    Vpop(cond, kDataTypeValueNone, dreglist);
  }
  void Vpop(DRegisterList dreglist) { Vpop(al, kDataTypeValueNone, dreglist); }

  void Vpop(Condition cond, DataType dt, SRegisterList sreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vpop(cond, dt, sreglist);
  }
  void Vpop(DataType dt, SRegisterList sreglist) { Vpop(al, dt, sreglist); }
  void Vpop(Condition cond, SRegisterList sreglist) {
    Vpop(cond, kDataTypeValueNone, sreglist);
  }
  void Vpop(SRegisterList sreglist) { Vpop(al, kDataTypeValueNone, sreglist); }

  void Vpush(Condition cond, DataType dt, DRegisterList dreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vpush(cond, dt, dreglist);
  }
  void Vpush(DataType dt, DRegisterList dreglist) { Vpush(al, dt, dreglist); }
  void Vpush(Condition cond, DRegisterList dreglist) {
    Vpush(cond, kDataTypeValueNone, dreglist);
  }
  void Vpush(DRegisterList dreglist) {
    Vpush(al, kDataTypeValueNone, dreglist);
  }

  void Vpush(Condition cond, DataType dt, SRegisterList sreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vpush(cond, dt, sreglist);
  }
  void Vpush(DataType dt, SRegisterList sreglist) { Vpush(al, dt, sreglist); }
  void Vpush(Condition cond, SRegisterList sreglist) {
    Vpush(cond, kDataTypeValueNone, sreglist);
  }
  void Vpush(SRegisterList sreglist) {
    Vpush(al, kDataTypeValueNone, sreglist);
  }

  void Vqabs(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqabs(cond, dt, rd, rm);
  }
  void Vqabs(DataType dt, DRegister rd, DRegister rm) { Vqabs(al, dt, rd, rm); }

  void Vqabs(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqabs(cond, dt, rd, rm);
  }
  void Vqabs(DataType dt, QRegister rd, QRegister rm) { Vqabs(al, dt, rd, rm); }

  void Vqadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqadd(cond, dt, rd, rn, rm);
  }
  void Vqadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vqadd(al, dt, rd, rn, rm);
  }

  void Vqadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqadd(cond, dt, rd, rn, rm);
  }
  void Vqadd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vqadd(al, dt, rd, rn, rm);
  }

  void Vqdmlal(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqdmlal(cond, dt, rd, rn, rm);
  }
  void Vqdmlal(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vqdmlal(al, dt, rd, rn, rm);
  }

  void Vqdmlal(Condition cond,
               DataType dt,
               QRegister rd,
               DRegister rn,
               DRegister dm,
               unsigned index) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqdmlal(cond, dt, rd, rn, dm, index);
  }
  void Vqdmlal(
      DataType dt, QRegister rd, DRegister rn, DRegister dm, unsigned index) {
    Vqdmlal(al, dt, rd, rn, dm, index);
  }

  void Vqdmlsl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqdmlsl(cond, dt, rd, rn, rm);
  }
  void Vqdmlsl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vqdmlsl(al, dt, rd, rn, rm);
  }

  void Vqdmlsl(Condition cond,
               DataType dt,
               QRegister rd,
               DRegister rn,
               DRegister dm,
               unsigned index) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqdmlsl(cond, dt, rd, rn, dm, index);
  }
  void Vqdmlsl(
      DataType dt, QRegister rd, DRegister rn, DRegister dm, unsigned index) {
    Vqdmlsl(al, dt, rd, rn, dm, index);
  }

  void Vqdmulh(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqdmulh(cond, dt, rd, rn, rm);
  }
  void Vqdmulh(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vqdmulh(al, dt, rd, rn, rm);
  }

  void Vqdmulh(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqdmulh(cond, dt, rd, rn, rm);
  }
  void Vqdmulh(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vqdmulh(al, dt, rd, rn, rm);
  }

  void Vqdmulh(Condition cond,
               DataType dt,
               DRegister rd,
               DRegister rn,
               DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqdmulh(cond, dt, rd, rn, rm);
  }
  void Vqdmulh(DataType dt, DRegister rd, DRegister rn, DRegisterLane rm) {
    Vqdmulh(al, dt, rd, rn, rm);
  }

  void Vqdmulh(Condition cond,
               DataType dt,
               QRegister rd,
               QRegister rn,
               DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqdmulh(cond, dt, rd, rn, rm);
  }
  void Vqdmulh(DataType dt, QRegister rd, QRegister rn, DRegisterLane rm) {
    Vqdmulh(al, dt, rd, rn, rm);
  }

  void Vqdmull(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqdmull(cond, dt, rd, rn, rm);
  }
  void Vqdmull(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vqdmull(al, dt, rd, rn, rm);
  }

  void Vqdmull(Condition cond,
               DataType dt,
               QRegister rd,
               DRegister rn,
               DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqdmull(cond, dt, rd, rn, rm);
  }
  void Vqdmull(DataType dt, QRegister rd, DRegister rn, DRegisterLane rm) {
    Vqdmull(al, dt, rd, rn, rm);
  }

  void Vqmovn(Condition cond, DataType dt, DRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqmovn(cond, dt, rd, rm);
  }
  void Vqmovn(DataType dt, DRegister rd, QRegister rm) {
    Vqmovn(al, dt, rd, rm);
  }

  void Vqmovun(Condition cond, DataType dt, DRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqmovun(cond, dt, rd, rm);
  }
  void Vqmovun(DataType dt, DRegister rd, QRegister rm) {
    Vqmovun(al, dt, rd, rm);
  }

  void Vqneg(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqneg(cond, dt, rd, rm);
  }
  void Vqneg(DataType dt, DRegister rd, DRegister rm) { Vqneg(al, dt, rd, rm); }

  void Vqneg(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqneg(cond, dt, rd, rm);
  }
  void Vqneg(DataType dt, QRegister rd, QRegister rm) { Vqneg(al, dt, rd, rm); }

  void Vqrdmulh(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqrdmulh(cond, dt, rd, rn, rm);
  }
  void Vqrdmulh(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vqrdmulh(al, dt, rd, rn, rm);
  }

  void Vqrdmulh(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqrdmulh(cond, dt, rd, rn, rm);
  }
  void Vqrdmulh(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vqrdmulh(al, dt, rd, rn, rm);
  }

  void Vqrdmulh(Condition cond,
                DataType dt,
                DRegister rd,
                DRegister rn,
                DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqrdmulh(cond, dt, rd, rn, rm);
  }
  void Vqrdmulh(DataType dt, DRegister rd, DRegister rn, DRegisterLane rm) {
    Vqrdmulh(al, dt, rd, rn, rm);
  }

  void Vqrdmulh(Condition cond,
                DataType dt,
                QRegister rd,
                QRegister rn,
                DRegisterLane rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqrdmulh(cond, dt, rd, rn, rm);
  }
  void Vqrdmulh(DataType dt, QRegister rd, QRegister rn, DRegisterLane rm) {
    Vqrdmulh(al, dt, rd, rn, rm);
  }

  void Vqrshl(
      Condition cond, DataType dt, DRegister rd, DRegister rm, DRegister rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqrshl(cond, dt, rd, rm, rn);
  }
  void Vqrshl(DataType dt, DRegister rd, DRegister rm, DRegister rn) {
    Vqrshl(al, dt, rd, rm, rn);
  }

  void Vqrshl(
      Condition cond, DataType dt, QRegister rd, QRegister rm, QRegister rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqrshl(cond, dt, rd, rm, rn);
  }
  void Vqrshl(DataType dt, QRegister rd, QRegister rm, QRegister rn) {
    Vqrshl(al, dt, rd, rm, rn);
  }

  void Vqrshrn(Condition cond,
               DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqrshrn(cond, dt, rd, rm, operand);
  }
  void Vqrshrn(DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand) {
    Vqrshrn(al, dt, rd, rm, operand);
  }

  void Vqrshrun(Condition cond,
                DataType dt,
                DRegister rd,
                QRegister rm,
                const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqrshrun(cond, dt, rd, rm, operand);
  }
  void Vqrshrun(DataType dt,
                DRegister rd,
                QRegister rm,
                const QOperand& operand) {
    Vqrshrun(al, dt, rd, rm, operand);
  }

  void Vqshl(Condition cond,
             DataType dt,
             DRegister rd,
             DRegister rm,
             const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqshl(cond, dt, rd, rm, operand);
  }
  void Vqshl(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vqshl(al, dt, rd, rm, operand);
  }

  void Vqshl(Condition cond,
             DataType dt,
             QRegister rd,
             QRegister rm,
             const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqshl(cond, dt, rd, rm, operand);
  }
  void Vqshl(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vqshl(al, dt, rd, rm, operand);
  }

  void Vqshlu(Condition cond,
              DataType dt,
              DRegister rd,
              DRegister rm,
              const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqshlu(cond, dt, rd, rm, operand);
  }
  void Vqshlu(DataType dt,
              DRegister rd,
              DRegister rm,
              const DOperand& operand) {
    Vqshlu(al, dt, rd, rm, operand);
  }

  void Vqshlu(Condition cond,
              DataType dt,
              QRegister rd,
              QRegister rm,
              const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqshlu(cond, dt, rd, rm, operand);
  }
  void Vqshlu(DataType dt,
              QRegister rd,
              QRegister rm,
              const QOperand& operand) {
    Vqshlu(al, dt, rd, rm, operand);
  }

  void Vqshrn(Condition cond,
              DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqshrn(cond, dt, rd, rm, operand);
  }
  void Vqshrn(DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand) {
    Vqshrn(al, dt, rd, rm, operand);
  }

  void Vqshrun(Condition cond,
               DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqshrun(cond, dt, rd, rm, operand);
  }
  void Vqshrun(DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand) {
    Vqshrun(al, dt, rd, rm, operand);
  }

  void Vqsub(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqsub(cond, dt, rd, rn, rm);
  }
  void Vqsub(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vqsub(al, dt, rd, rn, rm);
  }

  void Vqsub(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vqsub(cond, dt, rd, rn, rm);
  }
  void Vqsub(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vqsub(al, dt, rd, rn, rm);
  }

  void Vraddhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vraddhn(cond, dt, rd, rn, rm);
  }
  void Vraddhn(DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    Vraddhn(al, dt, rd, rn, rm);
  }

  void Vrecpe(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrecpe(cond, dt, rd, rm);
  }
  void Vrecpe(DataType dt, DRegister rd, DRegister rm) {
    Vrecpe(al, dt, rd, rm);
  }

  void Vrecpe(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrecpe(cond, dt, rd, rm);
  }
  void Vrecpe(DataType dt, QRegister rd, QRegister rm) {
    Vrecpe(al, dt, rd, rm);
  }

  void Vrecps(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrecps(cond, dt, rd, rn, rm);
  }
  void Vrecps(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vrecps(al, dt, rd, rn, rm);
  }

  void Vrecps(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrecps(cond, dt, rd, rn, rm);
  }
  void Vrecps(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vrecps(al, dt, rd, rn, rm);
  }

  void Vrev16(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrev16(cond, dt, rd, rm);
  }
  void Vrev16(DataType dt, DRegister rd, DRegister rm) {
    Vrev16(al, dt, rd, rm);
  }

  void Vrev16(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrev16(cond, dt, rd, rm);
  }
  void Vrev16(DataType dt, QRegister rd, QRegister rm) {
    Vrev16(al, dt, rd, rm);
  }

  void Vrev32(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrev32(cond, dt, rd, rm);
  }
  void Vrev32(DataType dt, DRegister rd, DRegister rm) {
    Vrev32(al, dt, rd, rm);
  }

  void Vrev32(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrev32(cond, dt, rd, rm);
  }
  void Vrev32(DataType dt, QRegister rd, QRegister rm) {
    Vrev32(al, dt, rd, rm);
  }

  void Vrev64(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrev64(cond, dt, rd, rm);
  }
  void Vrev64(DataType dt, DRegister rd, DRegister rm) {
    Vrev64(al, dt, rd, rm);
  }

  void Vrev64(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrev64(cond, dt, rd, rm);
  }
  void Vrev64(DataType dt, QRegister rd, QRegister rm) {
    Vrev64(al, dt, rd, rm);
  }

  void Vrhadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrhadd(cond, dt, rd, rn, rm);
  }
  void Vrhadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vrhadd(al, dt, rd, rn, rm);
  }

  void Vrhadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrhadd(cond, dt, rd, rn, rm);
  }
  void Vrhadd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vrhadd(al, dt, rd, rn, rm);
  }

  void Vrinta(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrinta(dt1, dt2, rd, rm);
  }

  void Vrinta(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrinta(dt1, dt2, rd, rm);
  }

  void Vrinta(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrinta(dt1, dt2, rd, rm);
  }

  void Vrintm(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrintm(dt1, dt2, rd, rm);
  }

  void Vrintm(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrintm(dt1, dt2, rd, rm);
  }

  void Vrintm(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrintm(dt1, dt2, rd, rm);
  }

  void Vrintn(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrintn(dt1, dt2, rd, rm);
  }

  void Vrintn(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrintn(dt1, dt2, rd, rm);
  }

  void Vrintn(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrintn(dt1, dt2, rd, rm);
  }

  void Vrintp(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrintp(dt1, dt2, rd, rm);
  }

  void Vrintp(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrintp(dt1, dt2, rd, rm);
  }

  void Vrintp(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrintp(dt1, dt2, rd, rm);
  }

  void Vrintr(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrintr(cond, dt1, dt2, rd, rm);
  }
  void Vrintr(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    Vrintr(al, dt1, dt2, rd, rm);
  }

  void Vrintr(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrintr(cond, dt1, dt2, rd, rm);
  }
  void Vrintr(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    Vrintr(al, dt1, dt2, rd, rm);
  }

  void Vrintx(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrintx(cond, dt1, dt2, rd, rm);
  }
  void Vrintx(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    Vrintx(al, dt1, dt2, rd, rm);
  }

  void Vrintx(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrintx(dt1, dt2, rd, rm);
  }

  void Vrintx(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrintx(cond, dt1, dt2, rd, rm);
  }
  void Vrintx(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    Vrintx(al, dt1, dt2, rd, rm);
  }

  void Vrintz(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrintz(cond, dt1, dt2, rd, rm);
  }
  void Vrintz(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    Vrintz(al, dt1, dt2, rd, rm);
  }

  void Vrintz(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vrintz(dt1, dt2, rd, rm);
  }

  void Vrintz(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrintz(cond, dt1, dt2, rd, rm);
  }
  void Vrintz(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    Vrintz(al, dt1, dt2, rd, rm);
  }

  void Vrshl(
      Condition cond, DataType dt, DRegister rd, DRegister rm, DRegister rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrshl(cond, dt, rd, rm, rn);
  }
  void Vrshl(DataType dt, DRegister rd, DRegister rm, DRegister rn) {
    Vrshl(al, dt, rd, rm, rn);
  }

  void Vrshl(
      Condition cond, DataType dt, QRegister rd, QRegister rm, QRegister rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrshl(cond, dt, rd, rm, rn);
  }
  void Vrshl(DataType dt, QRegister rd, QRegister rm, QRegister rn) {
    Vrshl(al, dt, rd, rm, rn);
  }

  void Vrshr(Condition cond,
             DataType dt,
             DRegister rd,
             DRegister rm,
             const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrshr(cond, dt, rd, rm, operand);
  }
  void Vrshr(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vrshr(al, dt, rd, rm, operand);
  }

  void Vrshr(Condition cond,
             DataType dt,
             QRegister rd,
             QRegister rm,
             const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrshr(cond, dt, rd, rm, operand);
  }
  void Vrshr(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vrshr(al, dt, rd, rm, operand);
  }

  void Vrshrn(Condition cond,
              DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrshrn(cond, dt, rd, rm, operand);
  }
  void Vrshrn(DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand) {
    Vrshrn(al, dt, rd, rm, operand);
  }

  void Vrsqrte(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrsqrte(cond, dt, rd, rm);
  }
  void Vrsqrte(DataType dt, DRegister rd, DRegister rm) {
    Vrsqrte(al, dt, rd, rm);
  }

  void Vrsqrte(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrsqrte(cond, dt, rd, rm);
  }
  void Vrsqrte(DataType dt, QRegister rd, QRegister rm) {
    Vrsqrte(al, dt, rd, rm);
  }

  void Vrsqrts(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrsqrts(cond, dt, rd, rn, rm);
  }
  void Vrsqrts(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vrsqrts(al, dt, rd, rn, rm);
  }

  void Vrsqrts(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrsqrts(cond, dt, rd, rn, rm);
  }
  void Vrsqrts(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vrsqrts(al, dt, rd, rn, rm);
  }

  void Vrsra(Condition cond,
             DataType dt,
             DRegister rd,
             DRegister rm,
             const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrsra(cond, dt, rd, rm, operand);
  }
  void Vrsra(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vrsra(al, dt, rd, rm, operand);
  }

  void Vrsra(Condition cond,
             DataType dt,
             QRegister rd,
             QRegister rm,
             const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrsra(cond, dt, rd, rm, operand);
  }
  void Vrsra(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vrsra(al, dt, rd, rm, operand);
  }

  void Vrsubhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vrsubhn(cond, dt, rd, rn, rm);
  }
  void Vrsubhn(DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    Vrsubhn(al, dt, rd, rn, rm);
  }

  void Vseleq(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vseleq(dt, rd, rn, rm);
  }

  void Vseleq(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vseleq(dt, rd, rn, rm);
  }

  void Vselge(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vselge(dt, rd, rn, rm);
  }

  void Vselge(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vselge(dt, rd, rn, rm);
  }

  void Vselgt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vselgt(dt, rd, rn, rm);
  }

  void Vselgt(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vselgt(dt, rd, rn, rm);
  }

  void Vselvs(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vselvs(dt, rd, rn, rm);
  }

  void Vselvs(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    vselvs(dt, rd, rn, rm);
  }

  void Vshl(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vshl(cond, dt, rd, rm, operand);
  }
  void Vshl(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vshl(al, dt, rd, rm, operand);
  }

  void Vshl(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vshl(cond, dt, rd, rm, operand);
  }
  void Vshl(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vshl(al, dt, rd, rm, operand);
  }

  void Vshll(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rm,
             const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vshll(cond, dt, rd, rm, operand);
  }
  void Vshll(DataType dt, QRegister rd, DRegister rm, const DOperand& operand) {
    Vshll(al, dt, rd, rm, operand);
  }

  void Vshr(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vshr(cond, dt, rd, rm, operand);
  }
  void Vshr(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vshr(al, dt, rd, rm, operand);
  }

  void Vshr(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vshr(cond, dt, rd, rm, operand);
  }
  void Vshr(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vshr(al, dt, rd, rm, operand);
  }

  void Vshrn(Condition cond,
             DataType dt,
             DRegister rd,
             QRegister rm,
             const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vshrn(cond, dt, rd, rm, operand);
  }
  void Vshrn(DataType dt, DRegister rd, QRegister rm, const QOperand& operand) {
    Vshrn(al, dt, rd, rm, operand);
  }

  void Vsli(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsli(cond, dt, rd, rm, operand);
  }
  void Vsli(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vsli(al, dt, rd, rm, operand);
  }

  void Vsli(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsli(cond, dt, rd, rm, operand);
  }
  void Vsli(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vsli(al, dt, rd, rm, operand);
  }

  void Vsqrt(Condition cond, DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsqrt(cond, dt, rd, rm);
  }
  void Vsqrt(DataType dt, SRegister rd, SRegister rm) { Vsqrt(al, dt, rd, rm); }

  void Vsqrt(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsqrt(cond, dt, rd, rm);
  }
  void Vsqrt(DataType dt, DRegister rd, DRegister rm) { Vsqrt(al, dt, rd, rm); }

  void Vsra(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsra(cond, dt, rd, rm, operand);
  }
  void Vsra(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vsra(al, dt, rd, rm, operand);
  }

  void Vsra(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsra(cond, dt, rd, rm, operand);
  }
  void Vsra(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vsra(al, dt, rd, rm, operand);
  }

  void Vsri(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsri(cond, dt, rd, rm, operand);
  }
  void Vsri(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vsri(al, dt, rd, rm, operand);
  }

  void Vsri(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsri(cond, dt, rd, rm, operand);
  }
  void Vsri(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vsri(al, dt, rd, rm, operand);
  }

  void Vst1(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vst1(cond, dt, nreglist, operand);
  }
  void Vst1(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vst1(al, dt, nreglist, operand);
  }

  void Vst2(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vst2(cond, dt, nreglist, operand);
  }
  void Vst2(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vst2(al, dt, nreglist, operand);
  }

  void Vst3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vst3(cond, dt, nreglist, operand);
  }
  void Vst3(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vst3(al, dt, nreglist, operand);
  }

  void Vst3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vst3(cond, dt, nreglist, operand);
  }
  void Vst3(DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand) {
    Vst3(al, dt, nreglist, operand);
  }

  void Vst4(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vst4(cond, dt, nreglist, operand);
  }
  void Vst4(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vst4(al, dt, nreglist, operand);
  }

  void Vstm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vstm(cond, dt, rn, write_back, dreglist);
  }
  void Vstm(DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    Vstm(al, dt, rn, write_back, dreglist);
  }
  void Vstm(Condition cond,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    Vstm(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void Vstm(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Vstm(al, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void Vstm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vstm(cond, dt, rn, write_back, sreglist);
  }
  void Vstm(DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    Vstm(al, dt, rn, write_back, sreglist);
  }
  void Vstm(Condition cond,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    Vstm(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void Vstm(Register rn, WriteBack write_back, SRegisterList sreglist) {
    Vstm(al, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void Vstmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vstmdb(cond, dt, rn, write_back, dreglist);
  }
  void Vstmdb(DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vstmdb(al, dt, rn, write_back, dreglist);
  }
  void Vstmdb(Condition cond,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vstmdb(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void Vstmdb(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Vstmdb(al, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void Vstmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vstmdb(cond, dt, rn, write_back, sreglist);
  }
  void Vstmdb(DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vstmdb(al, dt, rn, write_back, sreglist);
  }
  void Vstmdb(Condition cond,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vstmdb(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void Vstmdb(Register rn, WriteBack write_back, SRegisterList sreglist) {
    Vstmdb(al, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void Vstmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vstmia(cond, dt, rn, write_back, dreglist);
  }
  void Vstmia(DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vstmia(al, dt, rn, write_back, dreglist);
  }
  void Vstmia(Condition cond,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vstmia(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void Vstmia(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Vstmia(al, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void Vstmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vstmia(cond, dt, rn, write_back, sreglist);
  }
  void Vstmia(DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vstmia(al, dt, rn, write_back, sreglist);
  }
  void Vstmia(Condition cond,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vstmia(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void Vstmia(Register rn, WriteBack write_back, SRegisterList sreglist) {
    Vstmia(al, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void Vstr(Condition cond,
            DataType dt,
            DRegister rd,
            const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vstr(cond, dt, rd, operand);
  }
  void Vstr(DataType dt, DRegister rd, const MemOperand& operand) {
    Vstr(al, dt, rd, operand);
  }
  void Vstr(Condition cond, DRegister rd, const MemOperand& operand) {
    Vstr(cond, Untyped64, rd, operand);
  }
  void Vstr(DRegister rd, const MemOperand& operand) {
    Vstr(al, Untyped64, rd, operand);
  }

  void Vstr(Condition cond,
            DataType dt,
            SRegister rd,
            const MemOperand& operand) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vstr(cond, dt, rd, operand);
  }
  void Vstr(DataType dt, SRegister rd, const MemOperand& operand) {
    Vstr(al, dt, rd, operand);
  }
  void Vstr(Condition cond, SRegister rd, const MemOperand& operand) {
    Vstr(cond, Untyped32, rd, operand);
  }
  void Vstr(SRegister rd, const MemOperand& operand) {
    Vstr(al, Untyped32, rd, operand);
  }

  void Vsub(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsub(cond, dt, rd, rn, rm);
  }
  void Vsub(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vsub(al, dt, rd, rn, rm);
  }

  void Vsub(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsub(cond, dt, rd, rn, rm);
  }
  void Vsub(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vsub(al, dt, rd, rn, rm);
  }

  void Vsub(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsub(cond, dt, rd, rn, rm);
  }
  void Vsub(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vsub(al, dt, rd, rn, rm);
  }

  void Vsubhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsubhn(cond, dt, rd, rn, rm);
  }
  void Vsubhn(DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    Vsubhn(al, dt, rd, rn, rm);
  }

  void Vsubl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsubl(cond, dt, rd, rn, rm);
  }
  void Vsubl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vsubl(al, dt, rd, rn, rm);
  }

  void Vsubw(
      Condition cond, DataType dt, QRegister rd, QRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vsubw(cond, dt, rd, rn, rm);
  }
  void Vsubw(DataType dt, QRegister rd, QRegister rn, DRegister rm) {
    Vsubw(al, dt, rd, rn, rm);
  }

  void Vswp(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vswp(cond, dt, rd, rm);
  }
  void Vswp(DataType dt, DRegister rd, DRegister rm) { Vswp(al, dt, rd, rm); }
  void Vswp(Condition cond, DRegister rd, DRegister rm) {
    Vswp(cond, kDataTypeValueNone, rd, rm);
  }
  void Vswp(DRegister rd, DRegister rm) {
    Vswp(al, kDataTypeValueNone, rd, rm);
  }

  void Vswp(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vswp(cond, dt, rd, rm);
  }
  void Vswp(DataType dt, QRegister rd, QRegister rm) { Vswp(al, dt, rd, rm); }
  void Vswp(Condition cond, QRegister rd, QRegister rm) {
    Vswp(cond, kDataTypeValueNone, rd, rm);
  }
  void Vswp(QRegister rd, QRegister rm) {
    Vswp(al, kDataTypeValueNone, rd, rm);
  }

  void Vtbl(Condition cond,
            DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vtbl(cond, dt, rd, nreglist, rm);
  }
  void Vtbl(DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm) {
    Vtbl(al, dt, rd, nreglist, rm);
  }

  void Vtbx(Condition cond,
            DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vtbx(cond, dt, rd, nreglist, rm);
  }
  void Vtbx(DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm) {
    Vtbx(al, dt, rd, nreglist, rm);
  }

  void Vtrn(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vtrn(cond, dt, rd, rm);
  }
  void Vtrn(DataType dt, DRegister rd, DRegister rm) { Vtrn(al, dt, rd, rm); }

  void Vtrn(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vtrn(cond, dt, rd, rm);
  }
  void Vtrn(DataType dt, QRegister rd, QRegister rm) { Vtrn(al, dt, rd, rm); }

  void Vtst(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vtst(cond, dt, rd, rn, rm);
  }
  void Vtst(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vtst(al, dt, rd, rn, rm);
  }

  void Vtst(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vtst(cond, dt, rd, rn, rm);
  }
  void Vtst(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vtst(al, dt, rd, rn, rm);
  }

  void Vuzp(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vuzp(cond, dt, rd, rm);
  }
  void Vuzp(DataType dt, DRegister rd, DRegister rm) { Vuzp(al, dt, rd, rm); }

  void Vuzp(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vuzp(cond, dt, rd, rm);
  }
  void Vuzp(DataType dt, QRegister rd, QRegister rm) { Vuzp(al, dt, rd, rm); }

  void Vzip(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vzip(cond, dt, rd, rm);
  }
  void Vzip(DataType dt, DRegister rd, DRegister rm) { Vzip(al, dt, rd, rm); }

  void Vzip(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    vzip(cond, dt, rd, rm);
  }
  void Vzip(DataType dt, QRegister rd, QRegister rm) { Vzip(al, dt, rd, rm); }

  void Yield(Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EnsureEmitFor(kMaxInstructionSizeInBytes);
    ITScope it_scope(this, &cond);
    yield(cond);
  }
  void Yield() { Yield(al); }
  // End of generated code.
 private:
  RegisterList available_;
  VRegisterList available_vfp_;
  MacroAssemblerContext context_;
  Label::Offset checkpoint_;
  LiteralPoolManager literal_pool_manager_;
  VeneerPoolManager veneer_pool_manager_;
#ifdef VIXL_DEBUG
  bool allow_macro_instructions_;
#endif
};

// This scope is used to ensure that the specified size of instructions will be
// emitted contiguously. The assert policy kExtactSize should only be used
// when you use directly the assembler as it's difficult to know exactly how
// many instructions will be emitted by the macro-assembler. Using the assembler
// means that you directly use the assembler instructions (in lower case) from a
// MacroAssembler object.
class CodeBufferCheckScope {
 public:
  // Tell whether or not the scope should assert the amount of code emitted
  // within the scope is consistent with the requested amount.
  enum AssertPolicy {
    kNoAssert,    // No assert required.
    kExactSize,   // The code emitted must be exactly size bytes.
    kMaximumSize  // The code emitted must be at most size bytes.
  };

  CodeBufferCheckScope(MacroAssembler* masm,
                       uint32_t size,
                       AssertPolicy assert_policy = kMaximumSize)
      : masm_(masm) {
    masm->EnsureEmitFor(size);
#ifdef VIXL_DEBUG
    initial_cursor_offset_ = masm->GetCursorOffset();
    size_ = size;
    assert_policy_ = assert_policy;
#else
    USE(assert_policy);
#endif
  }

  ~CodeBufferCheckScope() {
#ifdef VIXL_DEBUG
    switch (assert_policy_) {
      case kNoAssert:
        break;
      case kExactSize:
        VIXL_ASSERT(masm_->GetCursorOffset() - initial_cursor_offset_ == size_);
        break;
      case kMaximumSize:
        VIXL_ASSERT(masm_->GetCursorOffset() - initial_cursor_offset_ <= size_);
        break;
      default:
        VIXL_UNREACHABLE();
    }
#endif
  }

 protected:
  MacroAssembler* masm_;
#ifdef VIXL_DEBUG
  uint32_t initial_cursor_offset_;
  uint32_t size_;
  AssertPolicy assert_policy_;
#endif
};

// Use this scope when you need a one-to-one mapping between methods and
// instructions. This scope prevents the MacroAssembler functions from being
// called and the literal pools and veneers from being emitted (they can only be
// emitted when you create the scope). It also asserts the size of the emitted
// instructions is the specified size (or not greater than the specified size).
// This scope must be used when you want to directly use the assembler. It will
// ensure that the buffer is big enough and that you don't break the pool and
// veneer mechanisms.
class AssemblerAccurateScope : public CodeBufferCheckScope {
 public:
  AssemblerAccurateScope(MacroAssembler* masm,
                         uint32_t size,
                         AssertPolicy policy = kExactSize)
      : CodeBufferCheckScope(masm, size, policy) {
    VIXL_ASSERT(policy != kNoAssert);
#ifdef VIXL_DEBUG
    old_allow_macro_instructions_ = masm->AllowMacroInstructions();
    masm->SetAllowMacroInstructions(false);
#endif
  }

  ~AssemblerAccurateScope() {
#ifdef VIXL_DEBUG
    masm_->SetAllowMacroInstructions(old_allow_macro_instructions_);
#endif
  }

 private:
#ifdef VIXL_DEBUG
  bool old_allow_macro_instructions_;
#endif
};

// This scope utility allows scratch registers to be managed safely. The
// MacroAssembler's GetScratchRegisterList() is used as a pool of scratch
// registers. These registers can be allocated on demand, and will be returned
// at the end of the scope.
//
// When the scope ends, the MacroAssembler's lists will be restored to their
// original state, even if the lists were modified by some other means.
class UseScratchRegisterScope {
 public:
  // This constructor implicitly calls the `Open` function to initialise the
  // scope, so it is ready to use immediately after it has been constructed.
  explicit UseScratchRegisterScope(MacroAssembler* masm)
      : available_(NULL),
        available_vfp_(NULL),
        old_available_(0),
        old_available_vfp_(0) {
    Open(masm);
  }
  // This constructor allows deferred and optional initialisation of the scope.
  // The user is required to explicitly call the `Open` function before using
  // the scope.
  UseScratchRegisterScope()
      : available_(NULL),
        available_vfp_(NULL),
        old_available_(0),
        old_available_vfp_(0) {}

  // This function performs the actual initialisation work.
  void Open(MacroAssembler* masm);

  // The destructor always implicitly calls the `Close` function.
  ~UseScratchRegisterScope() { Close(); }

  // This function performs the cleaning-up work. It must succeed even if the
  // scope has not been opened. It is safe to call multiple times.
  void Close();

  bool IsAvailable(const Register& reg) const;
  bool IsAvailable(const VRegister& reg) const;

  // Take a register from the temp list. It will be returned automatically when
  // the scope ends.
  Register Acquire();
  VRegister AcquireV(unsigned size_in_bits);
  QRegister AcquireQ();
  DRegister AcquireD();
  SRegister AcquireS();

  // Explicitly release an acquired (or excluded) register, putting it back in
  // the temp list.
  void Release(const Register& reg);
  void Release(const VRegister& reg);

  // Make the specified registers available as scratch registers for the
  // duration of this scope.
  void Include(const RegisterList& list);
  void Include(const Register& reg1,
               const Register& reg2 = NoReg,
               const Register& reg3 = NoReg,
               const Register& reg4 = NoReg) {
    Include(RegisterList(reg1, reg2, reg3, reg4));
  }
  void Include(const VRegisterList& list);
  void Include(const VRegister& reg1,
               const VRegister& reg2 = NoVReg,
               const VRegister& reg3 = NoVReg,
               const VRegister& reg4 = NoVReg) {
    Include(VRegisterList(reg1, reg2, reg3, reg4));
  }

  // Make sure that the specified registers are not available in this scope.
  // This can be used to prevent helper functions from using sensitive
  // registers, for example.
  void Exclude(const RegisterList& list);
  void Exclude(const Register& reg1,
               const Register& reg2 = NoReg,
               const Register& reg3 = NoReg,
               const Register& reg4 = NoReg) {
    Exclude(RegisterList(reg1, reg2, reg3, reg4));
  }
  void Exclude(const VRegisterList& list);
  void Exclude(const VRegister& reg1,
               const VRegister& reg2 = NoVReg,
               const VRegister& reg3 = NoVReg,
               const VRegister& reg4 = NoVReg) {
    Exclude(VRegisterList(reg1, reg2, reg3, reg4));
  }

  // Prevent any scratch registers from being used in this scope.
  void ExcludeAll();

 private:
  // Available scratch registers.
  RegisterList* available_;       // kRRegister
  VRegisterList* available_vfp_;  // kVRegister

  // The state of the available lists at the start of this scope.
  uint32_t old_available_;      // kRRegister
  uint64_t old_available_vfp_;  // kVRegister

  VIXL_DEBUG_NO_RETURN UseScratchRegisterScope(const UseScratchRegisterScope&) {
    VIXL_UNREACHABLE();
  }
  VIXL_DEBUG_NO_RETURN void operator=(const UseScratchRegisterScope&) {
    VIXL_UNREACHABLE();
  }
};

class JumpTableBase {
 protected:
  JumpTableBase(int len, int offset_size)
      : table_location_(Label::kMaxOffset),
        branch_location_(Label::kMaxOffset),
        length_(len),
        offset_shift_(WhichPowerOf2(offset_size)),
        presence_(length_) {
    VIXL_ASSERT((length_ >= 0) && (offset_size <= 4));
  }
  virtual ~JumpTableBase() {}

 public:
  int GetTableSizeInBytes() const { return length_ * (1 << offset_shift_); }
  int GetOffsetShift() const { return offset_shift_; }
  int GetLength() const { return length_; }
  Label* GetDefaultLabel() { return &default_; }
  Label* GetEndLabel() { return &end_; }
  void SetBranchLocation(uint32_t branch_location) {
    branch_location_ = branch_location;
  }
  uint32_t GetBranchLocation() const { return branch_location_; }
  void BindTable(uint32_t location) { table_location_ = location; }
  virtual void Link(MacroAssembler* masm,
                    int case_index,
                    uint32_t location) = 0;

  uint32_t GetLocationForCase(int i) {
    VIXL_ASSERT((i >= 0) && (i < length_));
    return table_location_ + (i * (1 << offset_shift_));
  }
  void SetPresenceBitForCase(int i) {
    VIXL_ASSERT((i >= 0) && (i < length_));
    presence_.Set(i);
  }

  void Finalize(MacroAssembler* masm) {
    if (!default_.IsBound()) {
      masm->Bind(&default_);
    }
    masm->Bind(&end_);

    presence_.ForEachBitNotSet(LinkIt(this, masm, default_.GetLocation()));
  }

 private:
  uint32_t table_location_;
  uint32_t branch_location_;
  const int length_;
  const int offset_shift_;
  BitField presence_;
  Label default_;
  Label end_;
  struct LinkIt {
    JumpTableBase* table_;
    MacroAssembler* const masm_;
    const uint32_t location_;
    LinkIt(JumpTableBase* table, MacroAssembler* const masm, uint32_t location)
        : table_(table), masm_(masm), location_(location) {}
    bool execute(int id) const {
      VIXL_ASSERT(id < table_->GetLength());
      table_->Link(masm_, static_cast<int>(id), location_);
      return true;
    }
  };
};

// JumpTable<T>(len): Helper to describe a jump table
// len here describes the number of possible case. Values in [0, n[ can have a
// jump offset. Any other value will assert.
template <typename T>
class JumpTable : public JumpTableBase {
 protected:
  explicit JumpTable(int length) : JumpTableBase(length, sizeof(T)) {}

 public:
  virtual void Link(MacroAssembler* masm, int case_index, uint32_t location) {
    uint32_t position_in_table = GetLocationForCase(case_index);
    uint32_t from = GetBranchLocation();
    int offset = location - from;
    T* case_offset = masm->GetBuffer().GetOffsetAddress<T*>(position_in_table);
    if (masm->IsT32()) {
      *case_offset = offset >> 1;
    } else {
      *case_offset = offset >> 2;
    }
  }
};

class JumpTable8bitOffset : public JumpTable<uint8_t> {
 public:
  explicit JumpTable8bitOffset(int length) : JumpTable<uint8_t>(length) {}
};

class JumpTable16bitOffset : public JumpTable<uint16_t> {
 public:
  explicit JumpTable16bitOffset(int length) : JumpTable<uint16_t>(length) {}
};

class JumpTable32bitOffset : public JumpTable<uint32_t> {
 public:
  explicit JumpTable32bitOffset(int length) : JumpTable<uint32_t>(length) {}
};

}  // namespace aarch32
}  // namespace vixl

#endif  // VIXL_A32_MACRO_ASSEMBLER_A32_H_