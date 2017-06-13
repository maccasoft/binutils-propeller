/* Target-dependent code for Parallax Propeller

   Copyright (C) 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2007, 2008, 2009,
   2010, 2011, 2012 Free Software Foundation, Inc.
   Copyright 2011, 2012 Parallax Inc.

   Contributed by Ken Rose, rose@acm.org

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#include "defs.h"
#include "frame.h"
#include "frame-unwind.h"
#include "frame-base.h"
#include "block.h"
#include "dwarf2-frame.h"
#include "trad-frame.h"
#include "symtab.h"
#include "gdbtypes.h"
#include "gdbcmd.h"
#include "gdbcore.h"
#include "value.h"
#include "inferior.h"
#include "dis-asm.h"  
#include "symfile.h"
#include "objfiles.h"
#include "arch-utils.h"
#include "regcache.h"
#include "reggroups.h"

#include "target.h"
#include "opcode/propeller.h"
#include "elf/propeller.h"
#include "elf-bfd.h"
#include "bfd-in2.h"

#define PROPELLER_NUM_REGS 19
#define PROPELLER_CCR_REGNUM 18
#define PROPELLER_PC_REGNUM 17
#define PROPELLER_SP_REGNUM 16
#define PROPELLER_LR_REGNUM 15
#define PROPELLER_FP_REGNUM 14

#define PROPELLER_R0_REGNUM 0
#define PROPELLER_R1_REGNUM 1

#define NUM_ARG_REGS 6

#define PROPELLER_CMM_BIT 0x40

struct gdbarch_tdep {
  int elf_flags;
  unsigned int call_ins;
};

static const char *propeller_register_names[] = {
  "r0",
  "r1",
  "r2",
  "r3",
  "r4",
  "r5",
  "r6",
  "r7",
  "r8",
  "r9",
  "r10",
  "r11",
  "r12",
  "r13",
  "r14",
  "r15",
  "sp",
  "pc",
  "cc",
};

void _initialize_propeller_tdep (void);

/* Macros for setting and testing a bit in a minimal symbol that marks
   it as CMM code.  The MSB of the minimal symbol's "info" field
   is used for this purpose.

   MSYMBOL_SET_SPECIAL	Actually sets the "special" bit.
   MSYMBOL_IS_SPECIAL   Tests the "special" bit in a minimal symbol.  */

#define MSYMBOL_SET_SPECIAL(msym)				\
	MSYMBOL_TARGET_FLAG_1 (msym) = 1

#define MSYMBOL_IS_SPECIAL(msym)				\
	MSYMBOL_TARGET_FLAG_1 (msym)

static void
propeller_elf_make_msymbol_special(asymbol *sym, struct minimal_symbol *msym)
{
  int is_cmm;
  Elf_Internal_Sym *elfsym;

  elfsym = &((elf_symbol_type *)sym)->internal_elf_sym;
  is_cmm = (elfsym->st_other & PROPELLER_OTHER_COMPRESSED) != 0;
  if (is_cmm)
    MSYMBOL_SET_SPECIAL (msym);

}

/* Return the GDB type object for the "standard" data type of data in
   register N.  This should be int for all registers except
   PC, which should be a pointer to a function.
   
   Note, for registers that contain addresses return pointer to
   void, not pointer to char, because we don't want to attempt to
   print the string after printing the address.  */

static struct type *
propeller_register_type (struct gdbarch *gdbarch, int regnum)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  if (regnum == gdbarch_pc_regnum (gdbarch))
    return builtin_type (gdbarch)->builtin_func_ptr;

  return builtin_type (gdbarch)->builtin_int32;
}

static const char *
propeller_register_name (struct gdbarch *gdbarch, int regnum)
{
  if (regnum < 0 || regnum >= ARRAY_SIZE (propeller_register_names))
    internal_error (__FILE__, __LINE__,
		    _("propeller_register_name: illegal register number %d"),
		    regnum);
  else
    return propeller_register_names[regnum];
}

struct propeller_frame_cache
{
  /* Base address.  */
  CORE_ADDR base;
  CORE_ADDR args;
  CORE_ADDR reg_bytes_saved;
  CORE_ADDR sp_offset;
  CORE_ADDR pc;

  /* Saved registers.  */
  CORE_ADDR saved_regs[PROPELLER_NUM_REGS];
  CORE_ADDR saved_sp;

  /* Stack space reserved for local variables.  */
  long locals;
};

/* Allocate and initialize a frame cache.  */
static struct propeller_frame_cache *
propeller_alloc_frame_cache (void)
{
  struct propeller_frame_cache *cache;
  int i;

  cache = FRAME_OBSTACK_ZALLOC (struct propeller_frame_cache);

  /* Base address.  */
  cache->base = 0;
  cache->sp_offset = 0;
  cache->pc = 0;
  cache->args = 0;
  cache->reg_bytes_saved = 0;

  /* Saved registers.  We initialize these to -1 since
     zero is a valid offset  */
  for (i = 0; i < PROPELLER_NUM_REGS; i++)
    cache->saved_regs[i] = -1;

  /* Frameless until proven otherwise.  */
  cache->locals = -1;

  return cache;
}

/*
 * determine whether instructions at memaddr are encoded using CMM (compressed)
 * or regular uncompressed instructions
 */
static int
propeller_pc_is_cmm (struct gdbarch *gdbarch, CORE_ADDR memaddr)
{
  struct bound_minimal_symbol sym;

  sym = lookup_minimal_symbol_by_pc (memaddr);
  if (sym.minsym)
    return MSYMBOL_IS_SPECIAL (sym.minsym);

  if (target_has_registers)
    {
      CORE_ADDR ccr;

      ccr = get_frame_register_unsigned (get_current_frame (), PROPELLER_CCR_REGNUM);
      return (ccr & PROPELLER_CMM_BIT) != 0;
    }

  /* assume regular instructions if we can't find it */
  return 0;

}

#define SUB4_P(op) ((op & 0xfffc01ff) == 0x84fc0004)
#define SUB_P(op) ((op & 0xfffc0000) == 0x84fc0000)
#define MOVE_P(op) ((op & 0xffbc0000) == 0xa0bc0000)
#define WRLONG_P(op) ((op & 0xfffc0000) == 0x083c0000)
#define GET_DST(op) ((op >> 9) & 0x1ff)
#define GET_SRC(op) (op & 0x1ff)

static int
CALL_P(unsigned int op, struct gdbarch *arch)
{
  return ((op & 0xfffc0000) == gdbarch_tdep(arch)->call_ins);
}

/* Do a full analysis of the prologue at PC and update CACHE
   accordingly.  Bail out early if CURRENT_PC is reached.  Return the
   address where the analysis stopped.

   We (intend to) handle all cases that can be generated by gcc.

   There are two main forms of prologue, one with a save-multiple, and
   one without.

With:
    a0fcee2e 	mov	1dc <__TMP0>, #46 
    5cfcd462 	jmpret	1a8 <__LMM_PUSHM_ret>, #188 <__LMM_PUSHM> 
    a0bc1c10 	mov	38  <r14>, 40 <sp> 
    84fc206c 	sub	40  <sp>, #108 

    a0bc0e0e 	mov	1c  <r7>, 38 <r14> 
    84fc0e08 	sub	1c  <r7>, #8 
    083c0007 	wrlong	0   <r0>, 1c <r7> 

    a0bc0e0e 	mov	1c  <r7>, 38 <r14> 
    84fc0e04 	sub	1c  <r7>, #4 
    083c0207 	wrlong	4   <r1>, 1c <r7> 

Without:
    84fc2004 	sub	40 <sp>, #4 
    083c1c10 	wrlong	38 <r14>, 40 <sp> 
    a0bc1c10 	mov	38 <r14>, 40 <sp> 
    84fc2014 	sub	40 <sp>, #20 

    a0bc0e0e 	mov	1c <r7>,  38 <r14> 
    84fc0e0c 	sub	1c <r7>, #12 
    083c0007 	wrlong	0  <r0>,  1c <r7> 

    a0bc0e0e 	mov	1c <r7>,  38 <r14> 
    84fc0e08 	sub	1c <r7>, #8 
    083c0207 	wrlong	4  <r1>,  1c <r7> 

    a0bc0e0e 	mov	1c <r7>,  38 <r14> 
    84fc0e04 	sub	1c <r7>, #4 
    083c0407 	wrlong	8  <r2>,  1c <r7> 

  Note the repeats of "mov, sub, wrlong", which stash incoming
  parameters into the frame.

*/

static CORE_ADDR
propeller_analyze_prologue (struct gdbarch *gdbarch,
			    CORE_ADDR pc,
			    CORE_ADDR current_pc,
			    struct propeller_frame_cache *cache){
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  unsigned int op;
  CORE_ADDR base_pc = pc;

  if(pc >= current_pc)
    return current_pc;

  op = read_memory_unsigned_integer (pc, 4, byte_order);

  if(SUB4_P(op)) {
    int reg;
    pc += 4;
    op = read_memory_unsigned_integer (pc, 4, byte_order);
    if(WRLONG_P(op)){
      reg = GET_DST(op);
      cache->sp_offset += 4;
      cache->saved_regs[reg] = cache->sp_offset;
      if(reg == 15){
	cache->saved_regs[PROPELLER_PC_REGNUM] = cache->sp_offset;
      }
      pc += 4;
      op = read_memory_unsigned_integer (pc, 4, byte_order);
      cache->reg_bytes_saved = 4;
      if(MOVE_P(op)){
	pc += 4;
	op = read_memory_unsigned_integer (pc, 4, byte_order);
	if(SUB_P(op)){
	  cache->locals = GET_SRC(op) + 4;
	  pc += 4;
	} else {
	  return base_pc;
	}
      } else {
	return base_pc;
      }
    } else {
      return base_pc;
    }
  } else if(MOVE_P(op)){
    int reg;
    int count;
    reg = GET_SRC(op) & 0xf;
    count = (GET_SRC(op) & 0xf0) >> 4;
    op = read_memory_unsigned_integer (pc+4, 4, byte_order);
    if(CALL_P(op, gdbarch)){
      pc += 8;
      cache->locals = 4 * count;
      cache-> reg_bytes_saved = 4 * count;
      for(; count; count--){
	cache->sp_offset += 4;
	if(reg == 15){
	  // do the PC, too
	  cache->saved_regs[PROPELLER_PC_REGNUM] = cache->sp_offset;
	}
	cache->saved_regs[reg++] = cache->sp_offset;
      }
      op = read_memory_unsigned_integer (pc, 4, byte_order);
      if(MOVE_P(op)){
	pc += 4;
	op = read_memory_unsigned_integer (pc, 4, byte_order);
	if(SUB_P(op)){
	  cache->locals += GET_SRC(op);
	  pc += 4;
	} else {
	  return base_pc;
	}
      } else {
	return base_pc;
      }
    } else {
      return base_pc;
    }
  }
  while(pc < current_pc){
    int offset;
    // some number of parameters will be written into the frame.
    op = read_memory_unsigned_integer (pc, 4, byte_order);
    if(MOVE_P(op)){
    } else {
      // Not part of the prologue.
      break;
    }
    op = read_memory_unsigned_integer (pc+4, 4, byte_order);
    if(SUB_P(op)){
      offset = GET_SRC(op);
    } else {
      // Not part of the prologue.
      break;
    }
    op = read_memory_unsigned_integer (pc+8, 4, byte_order);
    if(WRLONG_P(op)) {
      int reg;
      // FIXME track where the value went
      reg = GET_DST(op);
      if(offset > cache->args) cache->args = offset;
      cache->saved_regs[reg] = cache->sp_offset + offset;
      if(reg == 15){
	cache->saved_regs[PROPELLER_PC_REGNUM] = cache->sp_offset;
      }
      pc += 12;
    } else {
      // Not part of the prologue.
      break;
    }
  }

  if (pc >= current_pc)
    return current_pc;
  return pc;
}

/* Return PC of first real instruction.  */

static CORE_ADDR
propeller_skip_prologue (struct gdbarch *gdbarch, CORE_ADDR start_pc)
{
  struct propeller_frame_cache cache;
  CORE_ADDR pc, func_addr;
  int op;

  /* See if we can determine the end of the prologue via the symbol table.
     If so, then return either PC, or the PC after the prologue, whichever
     is greater. */
  if (find_pc_partial_function(start_pc, NULL, &func_addr, NULL))
    {
      CORE_ADDR post_prologue_pc = skip_prologue_using_sal (gdbarch, func_addr);
      struct compunit_symtab *cust = find_pc_compunit_symtab (func_addr);

      /* GCC always emits a line note before the prologue and another
	 one after, even if the two are at the same address or on the
	 same line.  Take advantage of this so that we do not need to
	 know every instruction that might appear in the prologue.  We
	 will have producer information for most binaries; if it is
	 missing (e.g. for -gstabs), assuming the GNU tools.  */
      if (post_prologue_pc
	  && (cust == NULL
	      || COMPUNIT_PRODUCER (cust) == NULL
	      || strncmp (COMPUNIT_PRODUCER (cust), "GNU ", sizeof ("GNU ") - 1) == 0
	      || strncmp (COMPUNIT_PRODUCER (cust), "clang ", sizeof ("clang ") - 1) == 0))
	return post_prologue_pc;
    }
  cache.locals = -1;
  if (propeller_pc_is_cmm (gdbarch, start_pc))
    {
      fprintf(stderr, "CMM PROLOGUE CODE NEEDED\n");
      pc = 0;
    }
  else
    {
      pc = propeller_analyze_prologue (gdbarch, start_pc, (CORE_ADDR) -1, &cache);
    }
  if (cache.locals < 0)
    return start_pc;
  return pc;
}

static CORE_ADDR
propeller_unwind_pc (struct gdbarch *gdbarch, struct frame_info *next_frame)
{
  ULONGEST pc;
  pc = frame_unwind_register_unsigned (next_frame, PROPELLER_PC_REGNUM);
  return gdbarch_addr_bits_remove (gdbarch, pc);}

static CORE_ADDR
propeller_unwind_sp (struct gdbarch *gdbarch, struct frame_info *this_frame)
{
  ULONGEST sp;
  sp = frame_unwind_register_unsigned (this_frame, PROPELLER_SP_REGNUM);
  return gdbarch_addr_bits_remove (gdbarch, sp);}

static void
propeller_virtual_frame_pointer (struct gdbarch *gdbarch, 
			    CORE_ADDR pc, int *reg, LONGEST *offset)
{
  *reg = PROPELLER_FP_REGNUM;
  *offset = 8;
}

/* Normal frames.  */

static struct propeller_frame_cache *
propeller_frame_cache (struct frame_info *this_frame, void **this_cache)
{
  struct gdbarch *gdbarch = get_frame_arch (this_frame);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  struct propeller_frame_cache *cache;
  gdb_byte buf[4];
  int i;

  if (*this_cache)
    return (struct propeller_frame_cache *)*this_cache;
#if 0
  { CORE_ADDR sp;
    if (this_frame != NULL)
      sp = get_frame_register_signed (this_frame, + PROPELLER_SP_REGNUM);
    else
      sp = 0;
  }
#endif
  cache = propeller_alloc_frame_cache ();
  *this_cache = cache;

  /* In principle, for normal frames, %fp holds the frame pointer,
     which holds the base address for the current stack frame.
     However, for functions that don't need it, the frame pointer is
     optional.  For these "frameless" functions the frame pointer is
     actually the frame pointer of the calling frame.  Signal
     trampolines are just a special case of a "frameless" function.
     They (usually) share their frame pointer with the frame that was
     in progress when the signal occurred.  */

  get_frame_register (this_frame, PROPELLER_SP_REGNUM, buf);
  cache->base = extract_unsigned_integer (buf, 4, byte_order);
  if (cache->base == 0)
    return cache;

  cache->pc = get_frame_func (this_frame);
  if (cache->pc != 0)
    propeller_analyze_prologue (get_frame_arch (this_frame), cache->pc,
			   get_frame_pc (this_frame), cache);

  if (cache->locals < 0)
    {
      /* We didn't find a valid frame, which means that CACHE->base
	 currently holds the frame pointer for our calling frame.  If
	 we're at the start of a function, or somewhere half-way its
	 prologue, the function's frame probably hasn't been fully
	 setup yet.  Try to reconstruct the base address for the stack
	 frame by looking at the stack pointer.  For truly "frameless"
	 functions this might work too.  */

      get_frame_register (this_frame, PROPELLER_SP_REGNUM, buf);
      cache->base = extract_unsigned_integer (buf, 4, byte_order);
      cache->locals = 0;
    }

  /* Now that we have the base address for the stack frame we can
     calculate the value of %sp in the calling frame.  */
  cache->saved_sp = cache->base + cache->locals;

  /* Adjust all the saved registers such that they contain addresses
     instead of offsets.  */
  for (i = 0; i < PROPELLER_NUM_REGS; i++){
    if (cache->saved_regs[i] != -1){
      cache->saved_regs[i] = cache->base + cache->locals - cache->saved_regs[i];
    }
  }
  return cache;
}

static void
propeller_frame_this_id (struct frame_info *this_frame, void **this_cache,
		    struct frame_id *this_id)
{
  struct propeller_frame_cache *cache = propeller_frame_cache (this_frame, this_cache);

  /* This marks the outermost frame.  */
  if (cache->base == 0)
    return;

  /* See the end of propeller_push_dummy_call.  */
  *this_id = frame_id_build (cache->base + 4, cache->pc);
}

static struct value *
propeller_frame_prev_register (struct frame_info *this_frame, void **this_cache,
			  int regnum)
{
  struct propeller_frame_cache *cache = propeller_frame_cache (this_frame, this_cache);

  gdb_assert (regnum >= 0);
  /* if asked to unwind the PC, then we need to return the LR instead */
  if (regnum == PROPELLER_PC_REGNUM) {
    CORE_ADDR lr;
    lr = frame_unwind_register_unsigned (this_frame, PROPELLER_LR_REGNUM);
    return frame_unwind_got_constant (this_frame, regnum, lr);
  }
  if (regnum == PROPELLER_SP_REGNUM && cache->saved_sp){
    return frame_unwind_got_constant (this_frame, regnum, cache->saved_sp);
  }
  if (regnum < PROPELLER_NUM_REGS && cache->saved_regs[regnum] != -1){
    return frame_unwind_got_memory (this_frame, regnum,
				    cache->saved_regs[regnum]);
  }
  return frame_unwind_got_register (this_frame, regnum, regnum);
}

//struct frame_unwind
//{
//  /* The frame's type.  Should this instead be a collection of
//     predicates that test the frame for various attributes?  */
//  enum frame_type type;
//  /* Should an attribute indicating the frame's address-in-block go
//     here?  */
//  frame_unwind_stop_reason_ftype *stop_reason;
//  frame_this_id_ftype *this_id;
//  frame_prev_register_ftype *prev_register;
//  const struct frame_data *unwind_data;
//  frame_sniffer_ftype *sniffer;
//  frame_dealloc_cache_ftype *dealloc_cache;
//  frame_prev_arch_ftype *prev_arch;
//};

static const struct frame_unwind propeller_frame_unwind =
{
  NORMAL_FRAME,
  default_frame_unwind_stop_reason,
  propeller_frame_this_id,
  propeller_frame_prev_register,
  NULL,
  default_frame_sniffer,
  NULL,
  NULL
};

static CORE_ADDR
propeller_frame_base_address (struct frame_info *this_frame, void **this_cache)
{
  struct propeller_frame_cache *cache = propeller_frame_cache (this_frame, this_cache);
  return cache->base;
}

static CORE_ADDR
propeller_frame_local_address (struct frame_info *this_frame, void **this_cache)
{
  struct propeller_frame_cache *cache = propeller_frame_cache (this_frame, this_cache);

  return cache->base;
}

static CORE_ADDR
propeller_frame_arg_address (struct frame_info *this_frame, void **this_cache)
{
  struct propeller_frame_cache *cache = propeller_frame_cache (this_frame, this_cache);

  return cache->base + cache->locals - cache->args - cache->reg_bytes_saved;
}

static const struct frame_base propeller_frame_base =
{
  &propeller_frame_unwind,
  propeller_frame_base_address,
  propeller_frame_local_address,
  propeller_frame_arg_address
};


static CORE_ADDR
propeller_push_dummy_call (struct gdbarch *gdbarch, struct value *function,
		      struct regcache *regcache, CORE_ADDR bp_addr, int nargs,
		      struct value **args, CORE_ADDR sp, int struct_return,
		      CORE_ADDR struct_addr)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  gdb_byte buf[4];
  int i;
  int arg_regs = 0;

  /* Push arguments in reverse order.  */
  for (i = nargs - 1; i >= 0; i--)
    if(i < NUM_ARG_REGS){
      memcpy(buf, value_contents(args[i]), 4);
      regcache_cooked_write(regcache, i, value_contents(args[i]));
    } else
    {
      struct type *value_type = value_enclosing_type (args[i]);
      int len = TYPE_LENGTH (value_type);
      int container_len = (len + 3) & ~3;
      int offset;

      /* Non-scalars bigger than 4 bytes are left aligned, others are
	 right aligned.  */
      if ((TYPE_CODE (value_type) == TYPE_CODE_STRUCT
	   || TYPE_CODE (value_type) == TYPE_CODE_UNION
	   || TYPE_CODE (value_type) == TYPE_CODE_ARRAY)
	  && len > 4)
	offset = 0;
      else
	offset = container_len - len;
      sp -= container_len;
      write_memory (sp + offset, value_contents_all (args[i]), len);
    }

  /* Store struct value address.  */
  if (struct_return)
    {
      store_unsigned_integer (buf, 4, byte_order, struct_addr);
      regcache_cooked_write (regcache, 0, buf);
    }

  /* Store return address.  */
  store_unsigned_integer (buf, 4, byte_order, bp_addr);
  regcache_cooked_write (regcache, 15, buf);

  /* Finally, update the stack pointer...  */
  store_unsigned_integer (buf, 4, byte_order, sp);
  regcache_cooked_write (regcache, PROPELLER_SP_REGNUM, buf);

  /* ...and fake a frame pointer.  */
  regcache_cooked_write (regcache, PROPELLER_FP_REGNUM, buf);

  /* DWARF2/GCC uses the stack address *before* the function call as a
     frame's CFA.  */
  return sp + 4;
}

static struct frame_id
propeller_dummy_id (struct gdbarch *gdbarch, struct frame_info *this_frame)
{
  CORE_ADDR fp;

  fp = get_frame_register_unsigned (this_frame, PROPELLER_FP_REGNUM);

  /* See the end of propeller_push_dummy_call.  */
  return frame_id_build (fp + 4, get_frame_pc (this_frame));
}

/* little endian version of P1 breakpoint instruction */
static const gdb_byte bpt_p1[4] = {0x14, 0x00, 0x7c, 0x5c};
/* P2 breakpoint instruction */
static const gdb_byte bpt_p2[4] = {0x14, 0x00, 0x7c, 0x1c};
/* CMM breakpoint instruction (both P1 and P2) */
static const gdb_byte bpt_cmm[1] = { 0x01 };

static const gdb_byte *propeller_breakpoint_from_pc(struct gdbarch *arch, CORE_ADDR *addr, int *x){
  int flags;


  if (propeller_pc_is_cmm (arch, *addr))
    {
      *x = 1;
      return bpt_cmm;
    }

  flags = gdbarch_tdep (arch)->elf_flags;

  *x = 4;
  if ((flags & EF_PROPELLER_MACH) == EF_PROPELLER_PROP2)
    return bpt_p2;
  return bpt_p1;
}

/* There is a fair number of calling conventions that are in somewhat
   wide use.  The 68000/08/10 don't support an FPU, not even as a
   coprocessor.  All function return values are stored in %d0/%d1.
   Structures are returned in a static buffer, a pointer to which is
   returned in %d0.  This means that functions returning a structure
   are not re-entrant.  To avoid this problem some systems use a
   convention where the caller passes a pointer to a buffer in %a1
   where the return values is to be stored.  This convention is the
   default, and is implemented in the function m68k_return_value.

   The 68020/030/040/060 do support an FPU, either as a coprocessor
   (68881/2) or built-in (68040/68060).  That's why System V release 4
   (SVR4) instroduces a new calling convention specified by the SVR4
   psABI.  Integer values are returned in %d0/%d1, pointer return
   values in %a0 and floating values in %fp0.  When calling functions
   returning a structure the caller should pass a pointer to a buffer
   for the return value in %a0.  This convention is implemented in the
   function m68k_svr4_return_value, and by appropriately setting the
   struct_value_regnum member of `struct gdbarch_tdep'.

   GNU/Linux returns values in the same way as SVR4 does, but uses %a1
   for passing the structure return value buffer.

   GCC can also generate code where small structures are returned in
   %d0/%d1 instead of in memory by using -freg-struct-return.  This is
   the default on NetBSD a.out, OpenBSD and GNU/Linux and several
   embedded systems.  This convention is implemented by setting the
   struct_return member of `struct gdbarch_tdep' to reg_struct_return.  */

/* Read a function return value of TYPE from REGCACHE, and copy that
   into VALBUF.  */

static void
propeller_extract_return_value (struct type *type, struct regcache *regcache,
			   gdb_byte *valbuf)
{
  int len = TYPE_LENGTH (type);
  gdb_byte buf[4];

  if (len <= 4)
    {
      regcache_raw_read (regcache, PROPELLER_R0_REGNUM, buf);
      memcpy (valbuf, buf + (4 - len), len);
    }
  else if (len <= 8)
    {
      regcache_raw_read (regcache, PROPELLER_R0_REGNUM, buf);
      memcpy (valbuf, buf + (8 - len), len - 4);
      regcache_raw_read (regcache, PROPELLER_R1_REGNUM, valbuf + (len - 4));
    }
  else
    internal_error (__FILE__, __LINE__,
		    _("Cannot extract return value of %d bytes long."), len);
}

/* Write a function return value of TYPE from VALBUF into REGCACHE.  */

static void
propeller_store_return_value (struct type *type, struct regcache *regcache,
			 const gdb_byte *valbuf)
{
  int len = TYPE_LENGTH (type);

  if (len <= 4)
    regcache_raw_write_part (regcache, PROPELLER_R0_REGNUM, 4 - len, len, valbuf);
  else if (len <= 8)
    {
      regcache_raw_write_part (regcache, PROPELLER_R0_REGNUM, 8 - len,
			       len - 4, valbuf);
      regcache_raw_write (regcache, PROPELLER_R1_REGNUM, valbuf + (len - 4));
    }
  else
    internal_error (__FILE__, __LINE__,
		    _("Cannot store return value of %d bytes long."), len);
}

/* Return non-zero if TYPE, which is assumed to be a structure or
   union type, should be returned in registers for architecture
   GDBARCH.  */

static int
propeller_reg_struct_return_p (struct gdbarch *gdbarch, struct type *type)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  enum type_code code = TYPE_CODE (type);
  int len = TYPE_LENGTH (type);

  gdb_assert (code == TYPE_CODE_STRUCT || code == TYPE_CODE_UNION);

   return (len == 1 || len == 2 || len == 4 || len == 8);
}

/* Determine, for architecture GDBARCH, how a return value of TYPE
   should be returned.  If it is supposed to be returned in registers,
   and READBUF is non-zero, read the appropriate value from REGCACHE,
   and copy it into READBUF.  If WRITEBUF is non-zero, write the value
   from WRITEBUF into REGCACHE.  */

static enum return_value_convention
propeller_return_value (struct gdbarch *gdbarch, struct value *function,
		   struct type *type, struct regcache *regcache,
		   gdb_byte *readbuf, const gdb_byte *writebuf)
{
  struct type *func_type = function ? value_type(function) : NULL;
  enum type_code code = TYPE_CODE (type);

  /* GCC returns a `long double' in memory too.  */
  if (((code == TYPE_CODE_STRUCT || code == TYPE_CODE_UNION)
       && !propeller_reg_struct_return_p (gdbarch, type))
      || (code == TYPE_CODE_FLT && TYPE_LENGTH (type) == 12))
    {
      /* The default on m68k is to return structures in static memory.
         Consequently a function must return the address where we can
         find the return value.  */

      if (readbuf)
	{
	  ULONGEST addr;

	  regcache_raw_read_unsigned (regcache, PROPELLER_R0_REGNUM, &addr);
	  read_memory (addr, readbuf, TYPE_LENGTH (type));
	}

      return RETURN_VALUE_ABI_RETURNS_ADDRESS;
    }

  if (readbuf)
    propeller_extract_return_value (type, regcache, readbuf);
  if (writebuf)
    propeller_store_return_value (type, regcache, writebuf);

  return RETURN_VALUE_REGISTER_CONVENTION;
}

static struct gdbarch *
propeller_gdbarch_init (struct gdbarch_info info,
			struct gdbarch_list *arches)
{
  struct gdbarch *gdbarch;
  struct gdbarch_tdep *tdep;
  int elf_flags;

  /* Extract the elf_flags if available.  */
  if (info.abfd != NULL
      && bfd_get_flavour (info.abfd) == bfd_target_elf_flavour)
    elf_flags = elf_elfheader (info.abfd)->e_flags;
  else
    elf_flags = 0;

  /* Try to find a pre-existing architecture.  */
  for (arches = gdbarch_list_lookup_by_info (arches, &info);
       arches != NULL;
       arches = gdbarch_list_lookup_by_info (arches->next, &info))
    {
      if (gdbarch_tdep (arches->gdbarch)->elf_flags != elf_flags)
	continue;

      return arches->gdbarch;
    }

  /* Need a new architecture.  Fill in a target specific vector.  */
  tdep = (struct gdbarch_tdep *) xmalloc (sizeof (struct gdbarch_tdep));
  gdbarch = gdbarch_alloc (&info, tdep);
  tdep->elf_flags = elf_flags;
  if ((elf_flags & 0x3) == 2) {
    tdep->call_ins = 0x1cfc0000;
  } else {
    tdep->call_ins = 0x5cfc0000;
  }
  //  tdep->prologue = propeller_prologue;
  set_gdbarch_addr_bit (gdbarch, 32);
  //  set_gdbarch_num_pseudo_regs (gdbarch, PROPELLER_NUM_PSEUDO_REGS);
  set_gdbarch_pc_regnum (gdbarch, PROPELLER_PC_REGNUM);
  set_gdbarch_num_regs (gdbarch, PROPELLER_NUM_REGS);

  /* Initially set everything according to the ABI.
     Use 32-bit integers since it will be the case for most
     programs.  The size of these types should normally be set
     according to the dwarf2 debug information.  */
  set_gdbarch_short_bit (gdbarch, 16);
  set_gdbarch_int_bit (gdbarch, 32);
  set_gdbarch_float_bit (gdbarch, 32);
  set_gdbarch_double_bit (gdbarch, 64);
  set_gdbarch_long_double_bit (gdbarch, 64);
  set_gdbarch_long_bit (gdbarch, 32);
  set_gdbarch_ptr_bit (gdbarch, 32);
  set_gdbarch_long_long_bit (gdbarch, 64);

  /* Characters are unsigned.  */
  set_gdbarch_char_signed (gdbarch, 0);

  set_gdbarch_unwind_pc (gdbarch, propeller_unwind_pc);
  set_gdbarch_unwind_sp (gdbarch, propeller_unwind_sp);

  /* Set register info.  */
  set_gdbarch_fp0_regnum (gdbarch, -1);

  set_gdbarch_sp_regnum (gdbarch, PROPELLER_SP_REGNUM);
  set_gdbarch_register_name (gdbarch, propeller_register_name);
  set_gdbarch_register_type (gdbarch, propeller_register_type);
  set_gdbarch_virtual_frame_pointer (gdbarch, propeller_virtual_frame_pointer);
  //  set_gdbarch_pseudo_register_read (gdbarch, propeller_pseudo_register_read);
  //  set_gdbarch_pseudo_register_write (gdbarch, propeller_pseudo_register_write);

  set_gdbarch_push_dummy_call (gdbarch, propeller_push_dummy_call);

  set_gdbarch_return_value (gdbarch, propeller_return_value);
  set_gdbarch_skip_prologue (gdbarch, propeller_skip_prologue);
  set_gdbarch_inner_than (gdbarch, core_addr_lessthan);
  set_gdbarch_breakpoint_from_pc (gdbarch, propeller_breakpoint_from_pc);
  set_gdbarch_print_insn (gdbarch, print_insn_propeller);

  //
  // Minsymbol frobbing
  //
  set_gdbarch_elf_make_msymbol_special (gdbarch, propeller_elf_make_msymbol_special);

  //  propeller_add_reggroups (gdbarch);
  //  set_gdbarch_register_reggroup_p (gdbarch, propeller_register_reggroup_p);
  //  set_gdbarch_print_registers_info (gdbarch, propeller_print_registers_info);

  /* Hook in the DWARF CFI frame unwinder.  */
  //  dwarf2_append_unwinders (gdbarch);

  frame_unwind_append_unwinder (gdbarch, &propeller_frame_unwind);
  frame_base_set_default (gdbarch, &propeller_frame_base);
  
  /* Methods for saving / extracting a dummy frame's ID.  The ID's
     stack address must match the SP value returned by
     PUSH_DUMMY_CALL, and saved by generic_save_dummy_frame_tos.  */
  set_gdbarch_dummy_id (gdbarch, propeller_dummy_id);

  set_gdbarch_believe_pcc_promotion (gdbarch, 1);

#if 0
  /* Don't set the target's PC; startup code does it for us. */
  set_gdbarch_load_writes_pc(gdbarch, 0);
#endif
  return gdbarch;
}

static void
propeller_dump_tdep (struct gdbarch *gdbarch, struct ui_file *file)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  if (tdep == NULL)
    return;
}

void
_initialize_propeller_tdep (void)
{
  gdbarch_register (bfd_arch_propeller, propeller_gdbarch_init, propeller_dump_tdep);
} 
