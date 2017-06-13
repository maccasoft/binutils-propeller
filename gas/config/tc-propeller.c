/* tc-propeller
   Copyright 2011-2013 Parallax Inc.

   This file is part of GAS, the GNU Assembler.

   GAS is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GAS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GAS; see the file COPYING.  If not, write to
   the Free Software Foundation, 51 Franklin Street - Fifth Floor,
   Boston, MA 02110-1301, USA.  */

#include "sysdep.h"
#include "dis-asm.h"
#include "as.h"
#include "struc-symbol.h"
#include "safe-ctype.h"
#include "opcode/propeller.h"
#include "elf/propeller.h"
#include "dwarf2dbg.h"
#include "obstack.h"

/* "always" condition code */
#define CC_ALWAYS   (0xf << 18)

/* condition code mask */
#define CC_MASK     (0xf << 18)

/* A representation for Propeller machine code.  */
struct propeller_code
{
  const char *error;
  int code;
  struct
  {
    bfd_reloc_code_real_type type;
    expressionS exp;
    int pc_rel;
  } reloc;
};


/* These chars start a comment anywhere in a source file (except inside
   another comment.  */
const char comment_chars[] = "'";

/* These chars only start a comment at the beginning of a line.  */
const char line_comment_chars[] = "#/";

const char line_separator_chars[] = ";";

/* Chars that can be used to separate mant from exp in floating point nums.  */
const char EXP_CHARS[] = "eE";

/* Chars that mean this number is a floating point constant.  */
/* as in 0f123.456.  */
/* or    0H1.234E-12 (see exp chars above).  */
const char FLT_CHARS[] = "dDfF";

/* extra characters that could be part of symbols */
const char propeller_symbol_chars[] = ":@";

/* forward declarations */
static void pseudo_fit (int);
static void pseudo_gas (int);
static void pseudo_pasm (int);
static void pseudo_compress (int);
static char *skip_whitespace (char *str);
static char *find_whitespace (char *str);
static char *find_whitespace_or_separator (char *str);
static int  pasm_replace_expression (expressionS *exp);

static int pasm_default = 0;    /* Use PASM addressing if 1 */
static int lmm = 0;             /* Enable LMM pseudo-instructions */
static int compress = 0;        /* Enable compressed (16 bit) instructions */
static int prop2 = 0;           /* Enable Propeller 2 instructions */
static int compress_default = 0; /* default compression mode from command line */
static int elf_flags = 0;       /* machine specific ELF flags */
static int cc_flag;             /* set if a condition code was specified in the current instruction */
static int cc_cleared;          /* set if the condition code field has been cleared in the process of handling inda/indb references */

const pseudo_typeS md_pseudo_table[] = {
  {"fit", pseudo_fit, 0},
  {"res", s_space, 4},
  {"gas", pseudo_gas, 0},
  {"pasm", pseudo_pasm, 0},
  {"compress", pseudo_compress, 0},
  {0, 0, 0},
};

typedef struct regdef {
    const char *name;
    int val;
} Prop_regdef;

Prop_regdef p1_regs[] = {
    { "par", 0x1f0 },
    { "cnt", 0x1f1 },
    { "ina", 0x1f2 },
    { "inb", 0x1f3 },
    { "outa", 0x1f4 },
    { "outb", 0x1f5 },
    { "dira", 0x1f6 },
    { "dirb", 0x1f7 },
    { "ctra", 0x1f8 },
    { "ctrb", 0x1f9 },
    { "frqa", 0x1fa },
    { "frqb", 0x1fb },
    { "phsa", 0x1fc },
    { "phsb", 0x1fd },
    { "vcfg", 0x1fe },
    { "vscl", 0x1ff },
    { 0, 0 } };

Prop_regdef p2_regs[] = {
    { "inda", 0x1f6 },
    { "indb", 0x1f7 },
    { "pina", 0x1f8 },
    { "pinb", 0x1f9 },
    { "pinc", 0x1fa },
    { "pind", 0x1fb },
    { "dira", 0x1fc },
    { "dirb", 0x1fd },
    { "dirc", 0x1fe },
    { "dird", 0x1ff },
    { 0, 0 }
};

static struct hash_control *insn_hash = NULL;
static struct hash_control *cond_hash = NULL;
static struct hash_control *eff_hash = NULL;

const char *md_shortopts = "";

enum {
    OPTION_MD_LMM = OPTION_MD_BASE,
    OPTION_MD_CMM,
    OPTION_MD_P2,
    OPTION_MD_PASM
};

struct option md_longopts[] = {
  {"lmm", no_argument, NULL, OPTION_MD_LMM},
  {"cmm", no_argument, NULL, OPTION_MD_CMM},
  {"p2", no_argument, NULL, OPTION_MD_P2},
  {"pasm", no_argument, NULL, OPTION_MD_PASM},
  {NULL, no_argument, NULL, 0}
};

size_t md_longopts_size = sizeof (md_longopts);

static void
init_defaults (void)
{
  static int first = 1;

  if (first)
    {
      /* set_option(as desired); */
      first = 0;
    }
}

void
md_begin (void)
{
  int i;
  Prop_regdef *regs;

  init_defaults ();

  insn_hash = hash_new ();
  if (insn_hash == NULL)
    as_fatal (_("Virtual memory exhausted"));
  cond_hash = hash_new ();
  if (cond_hash == NULL)
    as_fatal (_("Virtual memory exhausted"));
  eff_hash = hash_new ();
  if (eff_hash == NULL)
    as_fatal (_("Virtual memory exhausted"));

  for (i = 0; i < propeller_num_opcodes; i++){
    int hardware = propeller_opcodes[i].hardware;
    int add = 0;
    if (prop2) {
      bfd_set_arch_mach (stdoutput, bfd_arch_propeller, bfd_mach_prop2);
      if (hardware & PROP_2)
        add = 1;
      if (hardware & PROP_2_LMM && lmm)
        add = 1;
    }
    else {
      bfd_set_arch_mach (stdoutput, bfd_arch_propeller, bfd_mach_prop1);
      if (hardware & PROP_1)
        add = 1;
      if ((hardware & PROP_1_LMM) && lmm)
        add = 1;
    }
    if (add) {
        hash_insert (insn_hash, propeller_opcodes[i].name,
                     (void *) (propeller_opcodes + i));
    }
  }
  for (i = 0; i < propeller_num_conditions; i++)
    hash_insert (cond_hash, propeller_conditions[i].name,
                 (void *) (propeller_conditions + i));
  for (i = 0; i < propeller_num_effects; i++)
    hash_insert (eff_hash, propeller_effects[i].name,
                 (void *) (propeller_effects + i));

  /* insert symbols for predefined registers */
  if (prop2)
      regs = p2_regs;
  else
      regs = p1_regs;
  while (regs->name)
  {
      symbol_table_insert (symbol_new (regs->name, reg_section, regs->val, &zero_address_frag));
      regs++;
  }
  /* make sure data and bss are longword aligned */
  record_alignment(data_section, 2);
  record_alignment(bss_section, 2);
}

long
md_chars_to_number (con, nbytes)
     unsigned char con[];       /* High order byte 1st.  */
     int nbytes;                /* Number of bytes in the input.  */
{
  switch (nbytes)
    {
    case 0:
      return 0;
    case 1:
      return con[0];
    case 2:
      return (con[1] << BITS_PER_CHAR) | con[0];
    case 3:
      return (con[2] << (2*BITS_PER_CHAR)) | (con[1] << BITS_PER_CHAR) | con[0];
    case 4:
      return
        (((con[3] << BITS_PER_CHAR) | con[2]) << (2 * BITS_PER_CHAR))
        | ((con[1] << BITS_PER_CHAR) | con[0]);
    default:
      BAD_CASE (nbytes);
      return 0;
    }
}

/* Fix up some data or instructions after we find out the value of a symbol
   that they reference.  Knows about order of bytes in address.  */

void
md_apply_fix (fixS * fixP, valueT * valP, segT seg ATTRIBUTE_UNUSED)
{
  valueT code;
  valueT mask;
  valueT val = *valP;
  char *buf;
  int shift;
  int rshift;
  int size;

  /* note whether this will delete the relocation */
  if (fixP->fx_addsy == NULL && fixP->fx_pcrel == 0) {
    fixP->fx_done = 1;
  }
  buf = fixP->fx_where + fixP->fx_frag->fr_literal;
  size = fixP->fx_size;
  code = md_chars_to_number ((unsigned char *) buf, size);


  /* On a 64-bit host, silently truncate 'value' to 32 bits for
     consistency with the behaviour on 32-bit hosts.  Remember value
     for emit_reloc.  */
  val &= 0xffffffff;
  val ^= 0x80000000;
  val -= 0x80000000;

  *valP = val;
  fixP->fx_addnumber = val;

  /* Same treatment for fixP->fx_offset.  */
  fixP->fx_offset &= 0xffffffff;
  fixP->fx_offset ^= 0x80000000;
  fixP->fx_offset -= 0x80000000;

  switch (fixP->fx_r_type)
    {
    case BFD_RELOC_PROPELLER_REPINSCNT:
      val -= 1;
      mask = 0x0000003f;
      shift = 0;
      rshift = 0;
      break;
    case BFD_RELOC_PROPELLER_SRC_IMM:
      mask = 0x000001ff;
      shift = 0;
      rshift = 0;
      break;
    case BFD_RELOC_PROPELLER_SRC:
      mask = 0x000001ff;
      shift = 0;
      rshift = 2;
      break;
    case BFD_RELOC_PROPELLER_DST_IMM:
      mask = 0x0003fe00;
      shift = 9;
      rshift = 0;
      break;
    case BFD_RELOC_PROPELLER_DST:
      mask = 0x0003fe00;
      shift = 9;
      rshift = 2;
      break;
    case BFD_RELOC_PROPELLER_23:
      mask = 0x007fffff;
      shift = 0;
      rshift = 0;
      break;
    case BFD_RELOC_32:
      mask = 0xffffffff;
      shift = 0;
      rshift = 0;
      break;
    case BFD_RELOC_PROPELLER_32_DIV4:
      mask = 0xffffffff;
      shift = 0;
      rshift = 2;
      break;
    case BFD_RELOC_16:
      mask = 0x0000ffff;
      shift = 0;
      rshift = 0;
      break;
    case BFD_RELOC_PROPELLER_16_DIV4:
      mask = 0x0000ffff;
      shift = 0;
      rshift = 2;
      break;
    case BFD_RELOC_8:
      mask = 0x000000ff;
      shift = 0;
      rshift = 0;
      break;
    case BFD_RELOC_PROPELLER_8_DIV4:
      mask = 0x000000ff;
      shift = 0;
      rshift = 2;
      break;
    case BFD_RELOC_8_PCREL:
      mask = 0x0000007f;
      shift = 0;
      rshift = 0;
      if ((val & 0x80000000)) {
        /* negative */
        if ( (val & 0xFFFFFF80) == 0xFFFFFF80 ) {
          mask |= 0x80;
          val &= 0xFF;
        }
      }
      break;
    case BFD_RELOC_16_PCREL:
      mask = 0x00007fff;
      shift = 0;
      rshift = 0;
      if ((val & 0x80000000)) {
        /* negative */
        if ( (val & 0xFFFF8000) == 0xFFFF8000 ) {
          mask |= 0x8000;
          val &= 0xFFFF;
        }
      }
      break;
    case BFD_RELOC_PROPELLER_PCREL10:
      mask = 0x000001ff;
      shift = 0;
      rshift = 0;
      if ((val & 0x80000000)) {
        /* negative */
        //fprintf(stderr, "negative val=(%08lx)\n", val);
        val = (-val) & 0xffffffff;
        val |=  0x04000000;  /* toggle add to sub */
        mask |= 0x04000000;
      }
      break;
    case BFD_RELOC_PROPELLER_REPSREL:
      val -= 1;
      mask = 0x0000003f;
      shift = 0;
      rshift = 2;
      break;
    default:
      BAD_CASE (fixP->fx_r_type);
    }

  if (fixP->fx_addsy != NULL){
    val += symbol_get_bfdsym (fixP->fx_addsy)->section->vma;
  } else if (fixP->fx_subsy != NULL) {
    val -= symbol_get_bfdsym (fixP->fx_subsy)->section->vma;
  }

  if (!fixP->fx_done)
    val = 0;

  if( (((val>>rshift) << shift) & 0xffffffff) & ~mask){
    as_bad_where (fixP->fx_file, fixP->fx_line,
                  _("Relocation overflows"));
    //fprintf(stderr, "val=(%08lx), mask=%08lx, shift=%d, rshift=%d\n",
    //      (unsigned long)val, (unsigned long)mask, shift, rshift);
  }

  {
    code &= ~mask;
    code |= ((val>>rshift) << shift) & mask;
  }

  md_number_to_chars (buf, code, size);

}

/* Translate internal representation of relocation info to BFD target
   format.  */

arelent *
tc_gen_reloc (asection * section ATTRIBUTE_UNUSED, fixS * fixp)
{
  arelent *reloc;
  bfd_reloc_code_real_type code;

  reloc = xmalloc (sizeof (*reloc));

  reloc->sym_ptr_ptr = xmalloc (sizeof (asymbol *));
  *reloc->sym_ptr_ptr = symbol_get_bfdsym (fixp->fx_addsy);
  reloc->address = fixp->fx_frag->fr_address + fixp->fx_where;

  reloc->addend = fixp->fx_offset;

  switch (fixp->fx_r_type)
    {
    case BFD_RELOC_32_PCREL:
    case BFD_RELOC_16_PCREL:
    case BFD_RELOC_8_PCREL:
    case BFD_RELOC_PROPELLER_PCREL10:
    case BFD_RELOC_PROPELLER_REPSREL:
      // special hack:
      // on the propeller, all PCREL relocations should be
      // relative to the size of the fixup
      // it's a terrible hack to put this here, it should be
      // in the instruction generation
      reloc->addend -= fixp->fx_size;
      code = fixp->fx_r_type;
      break;
    case BFD_RELOC_32:
    case BFD_RELOC_16:
    case BFD_RELOC_8:
    case BFD_RELOC_PROPELLER_SRC:
    case BFD_RELOC_PROPELLER_SRC_IMM:
    case BFD_RELOC_PROPELLER_DST:
    case BFD_RELOC_PROPELLER_DST_IMM:
    case BFD_RELOC_PROPELLER_23:
    case BFD_RELOC_PROPELLER_REPINSCNT:
    case BFD_RELOC_PROPELLER_REPS:
    case BFD_RELOC_PROPELLER_32_DIV4:
    case BFD_RELOC_PROPELLER_16_DIV4:
    case BFD_RELOC_PROPELLER_8_DIV4:
      code = fixp->fx_r_type;
      break;

    default:
      BAD_CASE (fixp->fx_r_type);
      return NULL;
    }

  reloc->howto = bfd_reloc_type_lookup (stdoutput, code);

  if (reloc->howto == NULL)
    {
      as_bad_where (fixp->fx_file, fixp->fx_line,
                    _
                    ("Can not represent %s relocation in this object file format"),
                    bfd_get_reloc_code_name (code));
      return NULL;
    }

  return reloc;
}

const char *
md_atof (int type, char *litP, int *sizeP)
{
  return ieee_md_atof (type, litP, sizeP, FALSE);
}

/* Pseudo-op processing */
static void
pseudo_fit (int c ATTRIBUTE_UNUSED)
{
  /* does nothing interesting right now, but
     we do parse the expression
  */
  get_absolute_expression ();
  demand_empty_rest_of_line ();
}

/* turn compression on/off */
static void
pseudo_compress (int x ATTRIBUTE_UNUSED)
{
  char *opt;
  char delim;

  delim = get_symbol_name (&opt);

  if (strcasecmp (opt, "on") == 0)
    {
      compress = 1;
    }
  else if (strcasecmp (opt, "off") == 0)
    {
      compress = 0;
    }
  else if (strncmp (opt, "def", 3) == 0)
    {
      compress = compress_default;
    }
  else
    {
      as_bad (_("Unrecognized compress option \"%s\""), opt);
    }
  if (compress == 0)
    {
      /* compression is off, make sure code is aligned */
      frag_align_code (2, 0);
    }
  (void) restore_line_pointer (delim);
  demand_empty_rest_of_line ();
}

/* switch pasm mode off/on */
static void
pseudo_gas (int c ATTRIBUTE_UNUSED)
{
  pasm_default = 0;
}

static void
pseudo_pasm (int c ATTRIBUTE_UNUSED)
{
  pasm_default = 1;
}

/* Instruction processing */
static char *
parse_expression (char *str, struct propeller_code *operand)
{
  char *save_input_line_pointer;
  segT seg;

  save_input_line_pointer = input_line_pointer;
  input_line_pointer = str;
  seg = expression (&operand->reloc.exp);
  if (seg == NULL)
    {
      input_line_pointer = save_input_line_pointer;
      operand->error = _("Error in expression");
      return str;
    }

  str = input_line_pointer;
  input_line_pointer = save_input_line_pointer;

  operand->reloc.pc_rel = 0;

  return str;
}

static char *
skip_whitespace (char *str)
{
  while (*str == ' ' || *str == '\t')
    str++;
  return str;
}

static char *
find_whitespace (char *str)
{
  while (*str != ' ' && *str != '\t' && *str != 0)
    str++;
  return str;
}

static char *
find_whitespace_or_separator (char *str)
{
  while (*str != ' ' && *str != '\t' && *str != 0 && *str != ',')
    str++;
  return str;
}

static char *
parse_separator (char *str, int *error)
{
  str = skip_whitespace (str);
  *error = (*str != ',');
  if (!*error)
    str++;
  return str;
}

static void
lc (char *str)
{
  while (*str)
    {
      *str = TOLOWER (*str);
      str++;
    }
}

/*
 * parse a register specification like r0 or lr
 */

/* match a register name; if it matches, return the character after it */
static char *matchregname(char *str, const char *match)
{
  int len = strlen(match);
  if (!strncmp(str, match, len))
    {
      str += len;
      if (ISDIGIT(*str) || ISALPHA(*str) || *str == '_')
        return NULL;
      return str;
    }
  return NULL;
}

#define SP_REGNUM 16
#define PC_REGNUM 17
#define CC_REGNUM 18
#define FFFFFFFF_REGNUM 19

static struct {
  const char *name;
  int regno;
} lmm_regs[] = {
  { "lr", 15 },
  { "LR", 15 },
  { "sp", SP_REGNUM },
  { "pc", PC_REGNUM },
  { "ccr", CC_REGNUM },
  { "__MASK_FFFFFFFF", FFFFFFFF_REGNUM },
};

#define ARRAYSIZE(x) ((int)(sizeof(x)/sizeof(x[0])))

static char *
parse_regspec (char *str, int *regnum, struct propeller_code *operand, int give_error)
{
  int reg;
  char *newstr;
  int i;

  str = skip_whitespace (str);
  /* check for LMM register names */
  for (i = 0; i < ARRAYSIZE(lmm_regs); i++)
    {
      newstr = matchregname(str, lmm_regs[i].name);
      if (newstr)
        {
          reg = lmm_regs[i].regno;
          if (!compress && reg > PC_REGNUM)
            {
              /* non CMM kernels may have stuff anywhere */
              if (give_error)
                operand->error = _("bad register");
              return NULL;
            }
          *regnum = lmm_regs[i].regno;
          str = newstr;
          return str;
        }
    }

  if ( (*str != 'r' && *str != 'R') || !ISDIGIT(str[1]) )
    {
      if (give_error)
        operand->error = _("expected register number");
      return str;
    }
  str++;
  reg = 0;
  while (ISDIGIT(*str))
    {
      reg = 10*reg + ((*str) - '0');
      str++;
    }
  if (reg > 15 || reg < 0)
    {
      if (give_error)
        operand->error = _("illegal register number");
      return str;
    }
  *regnum = reg;
  return str;
}

static int
check_ptr(char *str){
  int is_ptr_op = 0;
  
  str = skip_whitespace(str);
  
  /* check for prefix operators */
  if (strncmp(str, "++", 2) == 0) {
    is_ptr_op = 1;
  }
  else if (strncmp(str, "--", 2) == 0) {
    is_ptr_op = 1;
  }
  
  /* check for a pointer or index register */
  else {
    str = skip_whitespace(str);
    if (matchregname(str, "ptra")
    ||  matchregname(str, "ptrb")
    ||  matchregname(str, "inda")
    ||  matchregname(str, "indb")) {
      is_ptr_op = 1;
    }
  }
  
  return is_ptr_op;
}

static char *
parse_ptr(char *str, struct propeller_code *operand, struct propeller_code *insn, int format){
  char *newstr;
  int isindx = 0; // 0 for ptra/ptrb, 1 for inda/indb
  int prefix_op = 0;
  int suffix_op = 0;
  int field = 0;
  int ndx = 0;
  int regnum = 0;
  
  str = skip_whitespace(str);
  
  // check for prefix operators
  if (strncmp(str, "++", 2) == 0) {
    prefix_op = 1;
    ndx = 1;
    str += 2;
  }
  else if (strncmp(str, "--", 2) == 0) {
    prefix_op = 1;
    ndx = -1;
    str += 2;
  }
  
  str = skip_whitespace(str);
  
  // parse the pointer name
  if ((newstr = matchregname(str, "ptra")) != NULL) {
    //field |= 0x000;
    str = newstr;
  }
  else if ((newstr = matchregname(str, "ptrb")) != NULL) {
    field |= 0x100;
    str = newstr;
  }
  else if ((newstr = matchregname(str, "inda")) != NULL) {
    regnum = 0x1f6;
    str = newstr;
    isindx = 1;
  }
  else if ((newstr = matchregname(str, "indb")) != NULL) {
    regnum = 0x1f7;
    str = newstr;
    isindx = 1;
  }
  else {
    operand->error = _("Can only use ++ or -- with ptra, ptrb, inda, or indb");
    return str;
  }
    
  str = skip_whitespace(str);
  
  // check for postfix operators
  if (strncmp(str, "++", 2) == 0) {
    suffix_op = 1;
    ndx = 1;
    str += 2;
  }
  else if (strncmp(str, "--", 2) == 0) {
    suffix_op = 1;
    ndx = -1;
    str += 2;
  }
  
  if (prefix_op && suffix_op) {
    operand->error = _("Can't use both prefix and postfix update");
    return str;
  }
  
  /* handle inda/indb in the source field */
  if (isindx) {
  
    if (prefix_op) {
      if (ndx == -1) {
        operand->error = _("Can't use prefix -- with inda or indb");
        return str;
      }
      insn->code |= 3 << 18;
    }
    else if (suffix_op) {
      insn->code |= (ndx == 1 ? 1 : 2) << 18;
    }
    
    operand->reloc.type = BFD_RELOC_NONE;
    operand->reloc.pc_rel = 0;
    operand->reloc.exp.X_op = O_register;
    operand->reloc.exp.X_add_number = regnum;
    insn->code |= operand->reloc.exp.X_add_number;
    
    return str;
  }
  
  /* complete the field */
  if (prefix_op) {
    field |= 0x080;
  }
  else if (suffix_op) {
    field |= 0x0c0;
  }
  
  str = skip_whitespace(str);
  
  // check for an index
  if (*str == '[') {
  
    str = skip_whitespace(++str);
    
    str = parse_expression(str, operand);
    if (operand->error)
      return str;
    
    switch (operand->reloc.exp.X_op)
      {
      case O_constant:
        if (ndx < 0)
          ndx = -operand->reloc.exp.X_add_number;
        else
          ndx = operand->reloc.exp.X_add_number;
        break;
      default:
        operand->error = _("Index must be a constant expression");
        return str;
      }
    
    str = skip_whitespace(str);
    
    if (*str == ']') {
      ++str;
    }
    else {
      operand->error = _("Missing right bracket");
      return str;
    }
  }
  
  // handle the index
  if (ndx < -32 || ndx > 31) {
    operand->error = _("6-bit value out of range");
    return str;
  }
  field |= ndx & 0x3f;
  
  /* build the instruction */
  switch (format) {
  case PROPELLER_OPERAND_PTRS_OPS:
    insn->code |= 0x00400000 | field;
    break;
  case PROPELLER_OPERAND_PTRD_OPS:
    insn->code |= 0x00c00000 | (field << 9);
    break;
  default:
    operand->error = _("Internal error");
    return str;
  }

  return str;
}

static char *
parse_indx(char *str, struct propeller_code *operand, struct propeller_code *insn, int type){
  char *newstr;
  int prefix_op = 0;
  int suffix_op = 0;
  int ndx = 0;
  int regnum;

  str = skip_whitespace(str);
  
  // check for the ++ prefix operator (-- is not allowed here)
  if (strncmp(str, "++", 2) == 0) {
    prefix_op = 1;
    ndx = 1;
    str += 2;
  }
  
  str = skip_whitespace(str);
  
  // parse the index register name
  if ((newstr = matchregname(str, "inda")) != NULL) {
    regnum = 0x1f6;
    str = newstr;
  }
  else if ((newstr = matchregname(str, "indb")) != NULL) {
    regnum = 0x1f7;
    str = newstr;
  }
  else {
    if (prefix_op) {
        operand->error = _("Can only use ++ with inda or indb");
        return str;
    }
    else {
        return NULL;
    }
  }
  
  // make sure a condition code was not given on this instruction
  if (cc_flag) {
        operand->error = _("Condition can not be used with inda or indb");
        return str;
  }
  
  // clear the "always" condition that is set by default
  if (!cc_cleared) {
    insn->code &= ~CC_MASK;
    cc_cleared = 1;
  }
    
  str = skip_whitespace(str);
  
  // check for postfix operators
  if (strncmp(str, "++", 2) == 0) {
    suffix_op = 1;
    ndx = 1;
    str += 2;
  }
  else if (strncmp(str, "--", 2) == 0) {
    suffix_op = 1;
    ndx = -1;
    str += 2;
  }
  
  if (prefix_op && suffix_op) {
    operand->error = _("Can't use both prefix and postfix update");
    return str;
  }
  
  if (prefix_op) {
    insn->code |= 3 << (type == BFD_RELOC_PROPELLER_SRC ? 18 : 20);
  }
  else if (suffix_op) {
    insn->code |= (ndx == 1 ? 1 : 2) << (type == BFD_RELOC_PROPELLER_SRC ? 18 : 20);
  }
    
  operand->reloc.type = BFD_RELOC_NONE;
  operand->reloc.pc_rel = 0;
  operand->reloc.exp.X_op = O_register;
  operand->reloc.exp.X_add_number = regnum;
  insn->code |= operand->reloc.exp.X_add_number << (type == BFD_RELOC_PROPELLER_SRC ? 0 : 9);
    
  return str;
}

static char *
parse_src(char *str, struct propeller_code *operand, struct propeller_code *insn, int format){
  int integer_reloc = 0;
  int pcrel_reloc = 0;
  int immediate = 0;
  int val;
  int pasm_expr = pasm_default;

  str = skip_whitespace (str);
  if (*str == '#')
    {
      if (format == PROPELLER_OPERAND_PTRS_OPS) {
          operand->error = _("Immediate operand not allowed here");
          return str;
      }
      str++;
      str = skip_whitespace(str);
      if (*str == '@') {
          str++;
          pasm_expr = 0;
      } else if (*str == '&') {
          str++;
          pasm_expr = 1;
      }
      insn->code |= 1 << 22;
      if (pasm_expr || (format != PROPELLER_OPERAND_JMP && format != PROPELLER_OPERAND_JMPRET && format != PROPELLER_OPERAND_MOVA))
        {
          integer_reloc = 1;
        }
      immediate = 1;
    }
  else if (compress)
    {
      /* check for registers */
      char *tmp;
      int regnum = -1;
      tmp = parse_regspec (str, &regnum, operand, 0);
      if (regnum != -1)
        {
          str = tmp;
          operand->reloc.type = BFD_RELOC_NONE;
          operand->reloc.pc_rel = 0;
          operand->reloc.exp.X_op = O_register;
          operand->reloc.exp.X_add_number = regnum;
          insn->code |= operand->reloc.exp.X_add_number;
          return str;
        }
    }
    
  if (prop2 && !immediate) {
    char *newstr;
    if ((newstr = parse_indx(str, operand, insn, BFD_RELOC_PROPELLER_SRC)) != NULL) {
      return newstr;
    }
  }
  
  if (format == PROPELLER_OPERAND_BRS) {
    pcrel_reloc = compress ? BFD_RELOC_8_PCREL : BFD_RELOC_PROPELLER_PCREL10;
  }

  str = parse_expression (str, operand);
  if (operand->error)
    return str;
  switch (operand->reloc.exp.X_op)
    {
    case O_constant:
    case O_register:
      val = operand->reloc.exp.X_add_number;
      if (format == PROPELLER_OPERAND_REPD)
      {
          val -= 1;
          if (val & ~0x3f)
          {
              operand->error = _("6-bit constant out of range");
              break;
          }
      }
      else
      {
          if (val & ~0x1ff)
          {
              operand->error = _("9-bit constant out of range");
              break;
          }
      }
      insn->code |= val;
      break;
    case O_symbol:
    case O_add:
    case O_subtract:
      if (pcrel_reloc)
        {
          operand->reloc.type = pcrel_reloc;
          operand->reloc.pc_rel = 1;
        }
      else
        {
	  if (format == PROPELLER_OPERAND_REPD) {
	    operand->reloc.type = BFD_RELOC_PROPELLER_REPINSCNT;
	  } else if (integer_reloc) {
	    operand->reloc.type = BFD_RELOC_PROPELLER_SRC_IMM;
	  } else {
	    operand->reloc.type = BFD_RELOC_PROPELLER_SRC;
	  }
          operand->reloc.pc_rel = 0;
        }
      break;
    case O_illegal:
      operand->error = _("Illegal operand in source");
      break;
    default:
      if (pcrel_reloc)
        operand->error = _("Source operand too complicated for relative instruction");
      else
        {
	  if (format == PROPELLER_OPERAND_REPD) {
	    operand->reloc.type = BFD_RELOC_PROPELLER_REPINSCNT;
	  } else if (integer_reloc) {
	    operand->reloc.type = BFD_RELOC_PROPELLER_SRC_IMM;
	  } else {
	    operand->reloc.type = BFD_RELOC_PROPELLER_SRC;
	  }
          operand->reloc.pc_rel = 0;
        }
      break;
    }
  if (pasm_expr && (operand->reloc.type == BFD_RELOC_PROPELLER_SRC_IMM || operand->reloc.type == BFD_RELOC_PROPELLER_SRC)
      && pasm_replace_expression (&operand->reloc.exp))
  {
      operand->reloc.type = BFD_RELOC_PROPELLER_SRC;
  }
  return str;
}

static char *
parse_src_reloc(char *str, struct propeller_code *operand, int default_reloc, int pcrel, int nbits)
{
  str = parse_expression (str, operand);
  if (operand->error)
    return str;
  switch (operand->reloc.exp.X_op)
    {
    case O_constant:
    case O_register:
      if ((nbits < 32) && (0 != (operand->reloc.exp.X_add_number & ~((1L << nbits)-1))))
        {
          operand->error = _("value out of range");
          break;
        }
      operand->code = operand->reloc.exp.X_add_number;
      operand->reloc.type = BFD_RELOC_NONE;
      break;
    case O_symbol:
    case O_add:
    case O_subtract:
      operand->reloc.type = default_reloc;
      operand->reloc.pc_rel = pcrel;
      break;
    case O_illegal:
      operand->error = _("Illegal operand in source");
      break;
    default:
        {
          operand->reloc.type = default_reloc;
          operand->reloc.pc_rel = pcrel;
        }
      break;
    }
  return str;
}

static char *
parse_src_n(char *str, struct propeller_code *operand, int nbits){

  int default_reloc = BFD_RELOC_PROPELLER_23;
  int c;

  if (nbits == 32)
    default_reloc = BFD_RELOC_32;
  else if (nbits == 16)
    default_reloc = BFD_RELOC_16;
  else if (nbits == 8)
    default_reloc = BFD_RELOC_8;

  str = skip_whitespace (str);
  c = *str++;
  if (c != '#')
    {
      operand->error = _("immediate operand required");
      return str;
    }
  return parse_src_reloc (str, operand, default_reloc, 0, nbits);
}

/*
 * delta is normally 0, but is -1 for 1 based instructions that have repeat
 * counts and such
 */

static char *
parse_src_or_dest(char *str, struct propeller_code *operand, struct propeller_code *insn, int type, int delta){

  int isdest;

  isdest = (type == BFD_RELOC_PROPELLER_DST || type == BFD_RELOC_PROPELLER_DST_IMM);

  str = skip_whitespace (str);
  if (compress)
    {
      /* check for registers */
      char *tmp;
      int regnum = -1;
      tmp = parse_regspec (str, &regnum, operand, 0);
      if (regnum != -1)
        {
          str = tmp;
          operand->reloc.type = BFD_RELOC_NONE;
          operand->reloc.pc_rel = 0;
          operand->reloc.exp.X_op = O_register;
          operand->reloc.exp.X_add_number = regnum;
          insn->code |= (operand->reloc.exp.X_add_number << (isdest ? 9 : 0));
          return str;
        }
    }

  str = parse_expression (str, operand);
  if (operand->error)
    return str;
  switch (operand->reloc.exp.X_op)
    {
    case O_constant:
      operand->reloc.exp.X_add_number += delta;
    case O_register:
      if (operand->reloc.exp.X_add_number & ~0x1ff)
        {
          operand->error = _("9-bit destination out of range");
          break;
        }
      insn->code |= operand->reloc.exp.X_add_number << (isdest ? 9 : 0);
      break;
    case O_symbol:
    case O_add:
    case O_subtract:
      operand->reloc.type = type;
      operand->reloc.pc_rel = 0;
      break;
    case O_illegal:
      operand->error = _(isdest ? "Illegal operand in destination"
			 : "Illegal operand in source");
      break;
    default:
      operand->reloc.type = type;
      operand->reloc.pc_rel = 0;
      break;
    }
  return str;
}

static char *
parse_dest(char *str, struct propeller_code *operand, struct propeller_code *insn){
  int pasm_expr = pasm_default;

  if (prop2) {
    char *newstr;
    if ((newstr = parse_indx(str, operand, insn, BFD_RELOC_PROPELLER_DST)) != NULL) {
      return newstr;
    }
  }
  str = parse_src_or_dest(str, operand, insn, BFD_RELOC_PROPELLER_DST, 0);
  if (pasm_expr) {
      pasm_replace_expression(&operand->reloc.exp);
  }
  return str;
}

static char *
parse_srcimm(char *str, struct propeller_code *operand, struct propeller_code *insn){
  str = skip_whitespace (str);
  if (*str == '#')
    {
      str++;
      insn->code |= 1 << 23;
    }
  return parse_src_or_dest(str, operand, insn, BFD_RELOC_PROPELLER_SRC, 0);
}

static char *
parse_destimm(char *str, struct propeller_code *operand, struct propeller_code *insn, int delta){
  int reloc = BFD_RELOC_PROPELLER_DST;
  str = skip_whitespace (str);
  if (*str == '#')
    {
      str++;
      insn->code |= 1 << 23;
      //reloc = BFD_RELOC_PROPELLER_DST_IMM; // do we sometimes want this??
    }
  else
    {
      delta = 0;
    }
  return parse_src_or_dest(str, operand, insn, reloc, delta);
}

static char *
parse_destimm_imm(char *str, struct propeller_code *op1, struct propeller_code *op2, struct propeller_code *insn, int mask){
  int error;

  str = parse_destimm(str, op1, insn, 0);
  
  str = parse_separator (str, &error);
  if (error)
    {
      op2->error = _("Missing ','");
      return str;
    }
    
  str = skip_whitespace (str);
  if (*str++ != '#')
    {
      op2->error = _("immediate operand required");
      return str;
    }

  str = skip_whitespace(str);
    
  str = parse_expression(str, op2);
  if (op2->error)
    return str;
    
  switch (op2->reloc.exp.X_op)
  {
    case O_constant:
      if (op2->reloc.exp.X_add_number < 0 || op2->reloc.exp.X_add_number > mask)
        {
          op2->error = _("Second operand value out of range");
          return str;
        }
      insn->code |= op2->reloc.exp.X_add_number & mask;
      break;
    default:
      op2->error = _("Must be a constant expression");
      return str;
  }

  return str;
}

static char *
parse_setind_operand(char *str, struct propeller_code *operand, struct propeller_code *insn, int type){

  int incflag = 0;
  int decflag = 0;
  int mask = 0x1ff;
  int fixup = 0;
  int pasm_expr = pasm_default;

  str = skip_whitespace(str);

  // check for operand type
  if (strncmp(str, "#", 1) == 0) {
    str += 1;
    fixup = 1;
  }
  else if (strncmp(str, "++", 2) == 0) {
    incflag = 1;
    str += 2;
    mask = 0xff;
  }
  else if (strncmp(str, "--", 2) == 0) {
    decflag = 1;
    str += 2;
    mask = 0xff;
  }

  str = skip_whitespace (str);
  str = parse_expression (str, operand);
  if (operand->error)
    return str;
  switch (operand->reloc.exp.X_op)
    {
    case O_constant:
      if (operand->reloc.exp.X_add_number & ~mask)
        {
          operand->error = _("9-bit value out of range");
          break;
        }
      if (incflag || decflag) {
        if (decflag) {
          operand->reloc.exp.X_add_number = 512-operand->reloc.exp.X_add_number;
        }
        insn->code |= 1 << (type == BFD_RELOC_PROPELLER_DST ? 21 : 19);
      }
      insn->code |= operand->reloc.exp.X_add_number << (type == BFD_RELOC_PROPELLER_DST ? 9 : 0);
      break;
    case O_register:
      if (incflag || decflag)
        {
          operand->error = _("Must be a constant expression");
          break;
        }
      if (operand->reloc.exp.X_add_number & ~mask)
        {
          operand->error = _("9-bit value out of range");
          break;
        }
      insn->code |= operand->reloc.exp.X_add_number << (type == BFD_RELOC_PROPELLER_DST ? 9 : 0);
      break;
    case O_symbol:
    case O_add:
    case O_subtract:
      if (incflag || decflag)
        {
          operand->error = _("Must be a constant expression");
          break;
        }
      operand->reloc.type = type;
      operand->reloc.pc_rel = 0;
      if (pasm_expr && fixup)
        {
          pasm_replace_expression (&operand->reloc.exp);
        }
      break;
    case O_illegal:
      operand->error = _(type == BFD_RELOC_PROPELLER_DST ? "Illegal operand in destination" : "Illegal operand in source");
      break;
    default:
      operand->reloc.type = type;
      operand->reloc.pc_rel = 0;
      break;
    }
  return str;
}

static char *
parse_repd(char *str, struct propeller_code *op1, struct propeller_code *op2, struct propeller_code *insn){
  int error;

  str = parse_destimm(str, op1, insn, -1);
  
  str = parse_separator (str, &error);
  if (error)
    {
      op2->error = _("Missing ','");
      return str;
    }
    
  str = skip_whitespace (str);
  if (*str != '#')
    {
      op2->error = _("Instruction requires immediate source");
    }
  str = parse_src(str, op2, insn, PROPELLER_OPERAND_REPD);
  return str;
}

static char *
parse_reps(char *str, struct propeller_code *op1, struct propeller_code *op2, struct propeller_code *insn){
  int error;
  
  // condition bits are used for other purposes in this instruction
  // BUG should probably give an error if a condition is used
  insn->code &= ~0x003c0000;
  
  str = skip_whitespace (str);
  if (*str++ != '#')
    {
      op1->error = _("immediate operand required for reps count");
      return str;
    }

  str = skip_whitespace(str);
    
  str = parse_expression(str, op1);
  if (op1->error)
    return str;
    
  switch (op1->reloc.exp.X_op)
  {
    case O_constant:
      --op1->reloc.exp.X_add_number; // value encoded into instruction is one less than the repeat count
      if (op1->reloc.exp.X_add_number < 0 || op1->reloc.exp.X_add_number >= (1 << 14))
        {
          op1->error = _("14-bit value out of range");
          return str;
        }
      insn->code |= (op1->reloc.exp.X_add_number & 0x1fff) << 9;
      insn->code |= (op1->reloc.exp.X_add_number & 0x2000) << (25 - 13);
      break;
    default:
      op1->error = _("Repeat count must be a constant expression");
      return str;
  }

  str = parse_separator (str, &error);
  if (error)
    {
      op2->error = _("Missing ','");
      return str;
    }
  
  str = skip_whitespace (str);
  if (*str == '#')
    {
      str++;
      str = parse_src(str, op2, insn, PROPELLER_OPERAND_REPD);
      if (op2->error)
        return str;
    }
  else if (*str == '@')
    {
      str++;
      str = parse_src_reloc (str, op2, BFD_RELOC_PROPELLER_REPSREL, 1, 6);
    }
  else
    {
      op2->error = _("immediate operand required for reps range");
      return str;
    }

    
  return str;
}

/*
  native instructions are 32 bits like:

  oooo_ooee eICC_CCdd dddd_ddds ssss_ssss

  if CCCC == 1111 (always execute), then store as:

  CCCC_eeeI + 24 bits:(little endian) oooo_oodd dddd_ddds ssss_ssss
*/

static unsigned long
pack_native(unsigned long code)
{
  unsigned long bottom = code & 0x3FFFF;
  unsigned long top = (code >> 26) & 0x3F;
  unsigned long eeeI = (code >> 22) & 0xF;

  bottom = bottom | (top << 18);
  return PREFIX_PACK_NATIVE | eeeI | (bottom << 8);
}


void
md_assemble (char *instruction_string)
{
  const struct propeller_opcode *op;
  const struct propeller_condition *cond;
  const struct propeller_effect *eff;
  unsigned condmask = 0xf;
  struct propeller_code insn, op1, op2, insn2, op3, op4;
  int error;
  int size;
  const char *err = NULL;
  char *str;
  char *p;
  char *to;
  char c;
  int insn_compressed = 0;
  int insn2_compressed = 0;
  unsigned int reloc_prefix = 0;  /* for a compressed instruction */
  int xmov_flag = 0;

  if (ignore_input())
    {
      //fprintf(stderr, "ignore input in md_assemble!\n");
      return;
    }

  /* initialize the condition code flags */
  cc_flag = cc_cleared = 0;

  /* force 4 byte alignment for this section */
  record_alignment(now_seg, 2);

  /* remove carriage returns (convert them to spaces) in case we are
     in dos mode */
  for (p = instruction_string; *p; p++)
    if (*p == '\r') *p = ' ';

#ifdef OBJ_ELF
  /* Tie dwarf2 debug info to the address at the start of the insn.  */
  dwarf2_emit_insn (0);
#endif

  str = skip_whitespace (instruction_string);
  p = find_whitespace (str);
  if (p - str == 0)
    {
      as_bad (_("No instruction found"));
      return;
    }

  c = *p;
  *p = '\0';
  lc (str);
  cond = (struct propeller_condition *) hash_find (cond_hash, str);
  *p = c;
  if (cond)
    {
      char *p2;
      /* Process conditional flag that str points to */
      insn.code = cond->value;
      p = skip_whitespace (p);
      p2 = find_whitespace (p);
      if (p2 - p == 0)
        {
          as_bad (_("No instruction found after condition"));
          return;
        }
      str = p;
      p = p2;
      cc_flag = 1;
    }
  else
    {
      insn.code = CC_ALWAYS;
    }
  condmask = 0xf & (insn.code >> 18);
  c = *p;
  *p = '\0';
  lc (str);
  op = (struct propeller_opcode *) hash_find (insn_hash, str);
  *p = c;
  if (op == 0)
    {
      as_bad (_("Unknown instruction '%s'"), str);
      return;
    }
    
  if (!(op->flags & FLAG_CC)) {
    if (cc_flag) {
      as_bad (_("Condition code not allowed with this instruction"));
      return;
    }
    insn.code = 0;
  }

  insn.error = NULL;
  insn.code |= op->opcode;
  insn.reloc.type = BFD_RELOC_NONE;
  insn2.error = NULL;
  insn2.reloc.type = BFD_RELOC_NONE;
  insn2.code = 0;
  insn2.reloc.type = BFD_RELOC_NONE;
  op1.error = NULL;
  op1.reloc.type = BFD_RELOC_NONE;
  op2.error = NULL;
  op2.reloc.type = BFD_RELOC_NONE;
  op3.error = NULL;
  op3.reloc.type = BFD_RELOC_NONE;
  op4.error = NULL;
  op4.reloc.type = BFD_RELOC_NONE;

  str = p;
  size = 4;

  switch (op->format)
    {
    case PROPELLER_OPERAND_IGNORE:
      /* special case for NOP instruction, since we need to 
       * suppress the condition. */
      insn.code = 0;
      if (compress) { 
        size = 1; 
        insn_compressed = 1;
      }
      break;

    case PROPELLER_OPERAND_NO_OPS:
      str = skip_whitespace (str);
      break;

    case PROPELLER_OPERAND_DEST_ONLY:
      str = parse_dest(str, &op1, &insn);
      break;

    case PROPELLER_OPERAND_DESTIMM_SRCIMM:
      str = parse_destimm(str, &op1, &insn, 0);
      str = parse_separator (str, &error);
      if (error)
        {
          op2.error = _("Missing ','");
          break;
        }
      str = parse_srcimm(str, &op2, &insn);
      break;
    
    case PROPELLER_OPERAND_DESTIMM:
      str = parse_destimm(str, &op1, &insn, 0);
      break;
    
    case PROPELLER_OPERAND_SETINDA:
      str = parse_setind_operand(str, &op1, &insn, BFD_RELOC_PROPELLER_SRC);
      break;
      
    case PROPELLER_OPERAND_SETINDB:
      str = parse_setind_operand(str, &op1, &insn, BFD_RELOC_PROPELLER_DST);
      break;
      
    case PROPELLER_OPERAND_SETINDS:
      str = parse_setind_operand(str, &op1, &insn, BFD_RELOC_PROPELLER_DST);
      str = parse_separator (str, &error);
      if (error)
        {
          op2.error = _("Missing ','");
          break;
        }
      str = parse_setind_operand(str, &op2, &insn, BFD_RELOC_PROPELLER_SRC);
      break;
    
    case PROPELLER_OPERAND_TWO_OPS:
    case PROPELLER_OPERAND_JMPRET:
    case PROPELLER_OPERAND_MOVA:
      str = parse_dest(str, &op1, &insn);
      str = parse_separator (str, &error);
      if (error)
        {
          op2.error = _("Missing ','");
          break;
        }
      str = parse_src(str, &op2, &insn, op->format);
      break;

    case PROPELLER_OPERAND_PTRS_OPS:
      {
          str = parse_dest(str, &op1, &insn);
          str = parse_separator (str, &error);
          if (error)
            {
              op2.error = _("Missing ','");
              break;
            }
          if (check_ptr(str)) {
            str = parse_ptr(str, &op2, &insn, op->format);
          }
          else {
            str = parse_src(str, &op2, &insn, PROPELLER_OPERAND_TWO_OPS);
          }
      }
      break;

    case PROPELLER_OPERAND_PTRD_OPS:
      {
          if (check_ptr(str)) {
            str = parse_ptr(str, &op2, &insn, op->format);
          }
          else {
            str = parse_dest(str, &op2, &insn);
          }
      }
      break;

    case PROPELLER_OPERAND_LDI:
      {
        char *pc;
        str = parse_dest(str, &op1, &insn);
        str = parse_separator (str, &error);
        if (error)
          {
            op3.error = _("Missing ','");
            break;
          }
        if (!lmm)
          {
            as_bad (_("instruction only supported in LMM mode"));
          }
        pc = malloc(3);
        if (pc == NULL)
          as_fatal (_("Virtual memory exhausted"));
        strcpy (pc, "pc");
        parse_src(pc, &op2, &insn, PROPELLER_OPERAND_TWO_OPS);
        str = parse_src_n(str, &op3, 32);
        size = 8;
        if(op3.reloc.exp.X_op == O_constant){
          /* Be sure to adjust this as needed for Prop-2! FIXME */
          if((op3.reloc.exp.X_add_number & 0x003c0000) && (op3.reloc.exp.X_add_number & 0x03800000))
            {
              op3.error = _("value out of range");
              break;
            }
          op3.code = op3.reloc.exp.X_add_number;
        }
        free(pc);
      }
      break;

    case PROPELLER_OPERAND_BRS:
      {
        char *arg;
        char *arg2;
        int len;

        str = skip_whitespace(str);
        len = strlen(str);
        arg = malloc(len+16);
        if (arg == NULL)
          as_fatal (_("Virtual memory exhausted"));
        if (*str == '#') {
          str++;  /* allow optional # in brs */
          len--;
        }
        sprintf(arg, "pc,#%s", str);
        str += len;
        arg2 = parse_dest (arg, &op1, &insn);
        arg2 = parse_separator (arg2, &error);
        if (error)
          {
           op2.error = _("Missing ','");
           break;
          }
        arg2 = parse_src (arg2, &op2, &insn, op->format);
        free (arg);
        /* here op1 contains pc, op2 contains address */
        if (compress)
          {
            unsigned byte0;
            op1.reloc.type = BFD_RELOC_NONE;
            /* extract the condition code */
            byte0 = PREFIX_BRS | ((insn.code >> 18) & 0xf);
            reloc_prefix = 1;
            insn.code = byte0;
            size = 2;
            insn_compressed = 1;
          }
      }
      break;
    case PROPELLER_OPERAND_BRW:
    case PROPELLER_OPERAND_BRL:
      {
        str = skip_whitespace(str);

        if (compress)
          {
            unsigned byte0;
	    if (op->format == PROPELLER_OPERAND_BRW)
	      {
		if (*str == '#') str++; /* skip optional immediate symbol */
		/* parse a 16 bit pc relative destination */
		str = parse_src_reloc (str, &op2, BFD_RELOC_16_PCREL, 1, 16);
		byte0 = PREFIX_BRW | (condmask);
		size = 3;
		reloc_prefix = 1;
	      }
	    else
	      {
		if (condmask != 0xf)
		  {
		    as_bad (_("conditional brl not allowed"));
		  }
		str = parse_src_n (str, &insn2, 32);
		byte0 = PREFIX_MACRO | MACRO_LJMP;
		size = 5;
		reloc_prefix = 0;  /* relocation is in insn2 */
	      }
	    insn.code = byte0;
	    insn_compressed = 1;
	  }
        else
          {
            char arg[16];
            strcpy(arg, "#__LMM_JMP");

            parse_src(arg, &op2, &insn, PROPELLER_OPERAND_JMP);
	    // The address is stored as data after the jmp. If this
	    // is an unconditional jump then no problem, but for
	    // conditionals we have to make sure that the data will
	    // be interpreted as a no-op (i.e. have its condition code
	    // bits set to 0). That's what the 23 relocation does.
	    if (condmask == 0xf) {
	      // unconditional jump
	      str = parse_src_n(str, &insn2, 32);
	    } else {
	      str = parse_src_n(str, &insn2, 23);
	    }
            insn2_compressed = 1;
            size = 8;
            if (!lmm)
              {
                as_bad (_("instruction only supported in LMM mode"));
              }
          }
      }
      break;

    case PROPELLER_OPERAND_XMMIO:
      {
        /* this looks like:
              xmmio rdbyte,r0,r2
           and gets translated into two instructions:
              mov     __TMP0,#(0<<16)+2
              jmpret  __LMM_RDBYTEI_ret, #__LMM_RDBYTEI
        */
        char *arg;
        char *rdwrop;
        int len;
        int regnum;
        char temp0reg[16];

        size = 8;  /* this will be a long instruction */
        str = skip_whitespace (str);
        len = strlen(str);
        arg = malloc(len + 16);
        if (arg == NULL)
          as_fatal (_("Virtual memory exhausted"));
        rdwrop = arg;
        strcpy(arg, "#__LMM_");
        arg += 7;
        while (*str && ISALPHA(*str)) {
          *arg++ = TOUPPER(*str);
          str++;
        }
        *arg++ = 'I';
        *arg = 0;

        /* op1 will be __TMP0; op2 will be an immediate constant built
           out of the strings we see
        */
        strcpy(temp0reg, "__TMP0");
        parse_dest (temp0reg, &op1, &insn);
        str = parse_separator (str, &error);
        if (error)
          {
           op2.error = _("Missing ','");
           break;
          }
        regnum = -1;
        str = parse_regspec (str, &regnum, &op2, 1);
        if (regnum < 0 || regnum > 15)
          {
            op2.error = _("illegal register");
          }
        insn.code |= (1<<22);  // make it an immediate instruction
        insn.code |= (regnum<<4);
        str = parse_separator (str, &error);
        if (error && !op2.error)
          {
            op2.error = _("Missing ','");
            break;
          }
        regnum = -1;
        str = parse_regspec (str, &regnum, &op2, 1);
        if (regnum < 0 || regnum > 15)
          {
            op2.error = _("illegal register");
          }
        insn.code |= (regnum);

        // now set up the CALL instruction
        insn2.code = (prop2 ? 0x1c800000 : 0x5c800000) | (0xf << 18); 
        parse_src(rdwrop, &op4, &insn2, PROPELLER_OPERAND_JMPRET);
        strcat(rdwrop, "_ret");
        parse_dest(rdwrop+1, &op3, &insn2);
        free(rdwrop);
      }
      break;

    case PROPELLER_OPERAND_FCACHE:
      {
        /* this looks like:
              fcache #n
           and gets translated into two instructions:
              jmp  #__LMM_FCACHE
              long n
        */
        if (compress)
          {
            size = 3;
            str = parse_src_n(str, &op2, 16);
            insn.code = MACRO_FCACHE | (op2.code << 8);
            insn_compressed = 1;
            reloc_prefix = 1;
          }
        else
          {
            char *arg;
            arg = malloc(32);
            if (arg == NULL)
              as_fatal (_("Virtual memory exhausted"));
            strcpy(arg, "#__LMM_FCACHE_LOAD");
            parse_src(arg, &op2, &insn, PROPELLER_OPERAND_JMP);
            str = parse_src_n(str, &insn2, 32);
            size = 8;
            insn2_compressed = 1;  /* insn2 is not an instruction */
            free(arg);
            if (!lmm)
              {
                as_bad (_("fcache only supported in LMM mode"));
              }
          }
      }
      break;

    case PROPELLER_OPERAND_MACRO_8:
      {
        /*
              lpushm #n
           and gets translated into two instructions:
              mov  __TMP0,#n
              jmpret __LMM_PUSHM_ret,#__LMM_PUSHM
        */
        if (compress)
          {
            size = 2;
            str = parse_src_n(str, &op2, 8);
            insn.code = op->copc | (op2.code << 8);
            insn_compressed = 1;
            reloc_prefix = 1;
          }
        else
          {
            char *arg;
            const char *macroname = "dummy";

            switch (op->copc) {
            case MACRO_PUSHM:
              macroname = "PUSHM";
              break;
            case MACRO_POPM:
              macroname = "POPM";
              break;
            case MACRO_POPRET:
              macroname = "POPRET";
              break;
            default:
              as_fatal (_("internal error, bad instruction"));
              break;
            }
            arg = malloc(64);
            if (arg == NULL)
              as_fatal (_("Virtual memory exhausted"));
            strcpy(arg, "__TMP0");
            parse_dest(arg, &op1, &insn);
            str = parse_src(str, &op2, &insn, PROPELLER_OPERAND_TWO_OPS);
            sprintf(arg, "__LMM_%s_ret", macroname);
            // now set up the CALL instruction
            insn2.code = (prop2 ? 0x1c800000 : 0x5c800000) | (0xf << 18); 
            parse_dest(arg, &op3, &insn2);
            sprintf(arg, "#__LMM_%s", macroname);
            parse_src(arg, &op4, &insn2, PROPELLER_OPERAND_JMPRET);
            free(arg);

            size = 8;
            if (!lmm)
              {
                as_bad (_("pushm/popm only supported in LMM mode"));
              }
          }
      }
      break;

    case PROPELLER_OPERAND_LRET:
      {
        /*
	   the "lret" macro expands to "mov pc, lr"
        */
        if (compress)
          {
            size = 1;
            insn.code = op->copc;
            insn_compressed = 1;
          }
        else
          {
            char *arg;

            arg = malloc(64);
            if (arg == NULL)
              as_fatal (_("Virtual memory exhausted"));
            strcpy(arg, "pc");
            parse_dest(arg, &op1, &insn);
            strcpy(arg, "lr");
            parse_src(arg, &op2, &insn, PROPELLER_OPERAND_TWO_OPS);
            free(arg);

            size = 4;
          }
      }
      break;

    case PROPELLER_OPERAND_MACRO_0:
      {
        /*
           a single macro like
              lmul
           and gets translated into the instruction
              jmpret __MULSI_ret,#__MULSI
        */
        if (compress)
          {
            size = 1;
            insn.code = op->copc;
            insn_compressed = 1;
          }
        else
          {
            char *arg;
            const char *macroname = "dummy";

            switch (op->copc) {
            case MACRO_RET:
              macroname = "__LMM_lret";
              break;
            case MACRO_MUL:
              macroname = "__MULSI";
              break;
            case MACRO_UDIV:
              macroname = "__UDIVSI";
              break;
            case MACRO_DIV:
              macroname = "__DIVSI";
              break;
            default:
              as_fatal (_("internal error, bad instruction"));
              break;
            }
            arg = malloc(64);
            if (arg == NULL)
              as_fatal (_("Virtual memory exhausted"));
            sprintf(arg, "%s_ret", macroname);
            parse_dest(arg, &op1, &insn);
            sprintf(arg, "#%s", macroname);
            parse_src(arg, &op2, &insn, PROPELLER_OPERAND_JMPRET);
            free(arg);

            size = 4;
          }
      }
      break;

    case PROPELLER_OPERAND_LEASP:
      {
        unsigned destval = 512;
        /*
              leasp rN,#n
           and gets translated into two instructions:
              mov  rN,#n
              add  rN,sp
        */
        int can_compress = 0;
        parse_dest(str, &op1, &insn);
        str = parse_dest(str, &op3, &insn2);
        str = parse_separator (str, &error);
        if (error)
          {
            op1.error = _("Missing ','");
            break;
          }

        if (compress && !op1.error)
          {
            if (op1.reloc.type == BFD_RELOC_NONE) {
              destval = ((insn.code >> 9) & 0x1f);
              if (destval <= 15) {
                can_compress = 1;
              }
            }
          }

        if (can_compress)
          {
            size = 2;
            str = parse_src_n(str, &op2, 8);
            insn.code = PREFIX_LEASP | destval | (op2.code << 8);
            reloc_prefix = 1;
            if (condmask != 0xf) {
              insn.code = insn.code << 8;
              condmask = ~condmask & 0xf;
              insn.code |= (PREFIX_SKIP2 | condmask);
              size++;
              reloc_prefix++;
            }
            insn_compressed = 1;
            insn2.code = 0;
            insn2.reloc.type = BFD_RELOC_NONE;
          }
        else
          {
            char *arg;

            str = parse_src(str, &op2, &insn, PROPELLER_OPERAND_TWO_OPS);
            if (!(insn.code & (1<<22)))
              {
                op2.error = _("leasp only accepts 8 bit immediates");
              }

            arg = malloc(64);
            if (arg == NULL)
              as_fatal (_("Virtual memory exhausted"));
            strcpy(arg, "sp");
            parse_src(arg, &op4, &insn, PROPELLER_OPERAND_TWO_OPS);
            // now set up the ADD instruction
            insn2.code = 0x80800000 | (0xf << 18); 
            free(arg);

            size = 8;
          }
      }
      break;

    case PROPELLER_OPERAND_XMOV:
      {
        /*
              xmov rA,rB,op,rC,rD
           and gets translated into two instructions:
              mov  rA,rB
              op   rC,rD
        */
        str = parse_dest(str, &op1, &insn);
        str = parse_separator (str, &error);
        if (error)
          {
            op1.error = _("Missing ',' in xmov");
            break;
          }
        str = parse_src(str, &op2, &insn, PROPELLER_OPERAND_TWO_OPS);
        str = skip_whitespace (str);
        p = find_whitespace (str);
        if (p - str == 0)
          {
            as_bad (_("No instruction found in xmov"));
            return;
          }
        c = *p;
        *p = '\0';
        lc (str);
        op = (struct propeller_opcode *) hash_find (insn_hash, str);
        *p = c;
        if (op == 0 || op->format != PROPELLER_OPERAND_TWO_OPS)
          {
            as_bad (_("Bad or missing instruction in xmov: '%s'"), str);
            return;
          }
        insn2.code = op->opcode | (condmask << 18);

        /* OK, second instruction now */
        str = p;
        str = parse_dest (str, &op3, &insn2);
        str = parse_separator (str, &error);
        if (error)
          {
            op3.error = _("Missing ',' in xmov op");
            break;
          }
        str = parse_src (str, &op4, &insn2, PROPELLER_OPERAND_TWO_OPS);
        size = 8;
        xmov_flag = 1;
      }
      break;

    case PROPELLER_OPERAND_LCALL:
      {
        /* this looks like:
              lcall #n
           and gets translated into two instructions:
              jmp  #__LMM_CALL
              long n
        */
        char *arg;
        if (compress)
          {
            str = parse_src_n(str, &op2, 16);
            insn.code = MACRO_LCALL | (op2.code << 8);
            if (op2.reloc.type == BFD_RELOC_PROPELLER_23)
              op2.reloc.type = BFD_RELOC_16;
	    if (prop2)
	      {
		// have to divide address by 4 
		switch (op2.reloc.type)
		  {
		  case BFD_RELOC_16:
		    op2.reloc.type = BFD_RELOC_PROPELLER_16_DIV4;
		    break;
		  default:
		    break;
		  }
	      }
            insn_compressed = 1;
            reloc_prefix = 1;
            size = 3;
          }
        else
          {
            arg = malloc(32);
            if (arg == NULL)
              as_fatal (_("Virtual memory exhausted"));
            strcpy(arg, "#__LMM_CALL");
            parse_src(arg, &op2, &insn, PROPELLER_OPERAND_JMP);
            str = parse_src_n(str, &insn2, 32);
            size = 8;
            free(arg);
            if (!lmm)
              {
                as_bad (_("lcall only supported in LMM mode"));
              }
          }
      }
      break;

    case PROPELLER_OPERAND_MVI:
      {
        /* this looks like:
              mvi rN,#n
           and gets translated into two instructions:
              jmp  #__LMM_MVI_rN
              long n
        */
        int reg;

        reg = -1;
        str = parse_regspec (str, &reg, &op1, 1);
        if (reg < 0 || reg > 15)
          {
            op1.error = _("illegal register");
          }
        str = parse_separator (str, &error);
        if (error)
          {
           op2.error = _("Missing ','");
           break;
          }
        if (compress && op->copc == PREFIX_MVIW)
          {
            str = parse_src_n(str, &op2, 16);
          }
        else
          {
            str = parse_src_n(str, &insn2, 32);
          }
        if (compress && !op1.error && !op2.error && !insn2.error)
          {
            if (op->copc == PREFIX_MVIW)
              {
                size = 3;
                insn.code = op->copc | reg;
                insn.code |= (op2.code << 8);
                reloc_prefix = 1;
              }
            else
              {
                size = 5;
                insn.code = op->copc | reg;
              }
            insn_compressed = 1;
          }
        else
          {
            char *arg;
            arg = malloc(32);
            if (arg == NULL)
              as_fatal (_("Virtual memory exhausted"));
            if (reg == 15)
              sprintf(arg, "#__LMM_MVI_lr");
            else
              sprintf(arg, "#__LMM_MVI_r%d", reg);
            parse_src(arg, &op2, &insn, PROPELLER_OPERAND_JMP);
            free(arg);
            
            size = 8;
            if (!lmm)
              {
                as_bad (_("lmvi only supported in LMM mode"));
              }
          }
      }
      break;

    case PROPELLER_OPERAND_SOURCE_ONLY:
    case PROPELLER_OPERAND_JMP:
      str = parse_src(str, &op2, &insn, op->format);
      break;

    case PROPELLER_OPERAND_CALL:
      {
        char *str2, *p2;
        str = skip_whitespace (str);
        if (*str == '#')
          {
            str++;
            insn.code |= 1 << 22;
          }
        str2 = malloc (5 + strlen (str));
        if (str2 == NULL)
          as_fatal (_("Virtual memory exhausted"));
        strcpy (str2, str);
        str = parse_expression (str, &op2);
        if (op2.error)
          break;
        switch (op2.reloc.exp.X_op)
          {
          case O_constant:
          case O_register:
            if (op2.reloc.exp.X_add_number & ~0x1ff)
              {
                op2.error = _("9-bit value out of range");
                break;
              }
            insn.code |= op2.reloc.exp.X_add_number;
            break;
          case O_illegal:
            op1.error = _("Illegal operand in call");
            break;
          default:
            op2.reloc.type = BFD_RELOC_PROPELLER_SRC;
            op2.reloc.pc_rel = 0;
            break;
          }

        p2 = find_whitespace_or_separator (str2);
        *p2 = 0;
        strcat (str2, "_ret");
        parse_expression (str2, &op1);
        free (str2);
        if (op1.error)
          break;
        switch (op1.reloc.exp.X_op)
          {
          case O_symbol:
            op1.reloc.type = BFD_RELOC_PROPELLER_DST;
            op1.reloc.pc_rel = 0;
            break;
          default:
            op1.error = _("Improper call target");
          }
      }
      break;

    case PROPELLER_OPERAND_REPD:
      str = parse_repd(str, &op1, &op2, &insn);
      break;
      
    case PROPELLER_OPERAND_REPS:
      str = parse_reps(str, &op1, &op2, &insn);
      break;
    
    case PROPELLER_OPERAND_JMPTASK:
      str = parse_destimm_imm(str, &op1, &op2, &insn, 0xf);
      break;
      
    case PROPELLER_OPERAND_BIT:
      str = parse_destimm_imm(str, &op1, &op2, &insn, 0x1f);
      break;
      
    default:
      BAD_CASE (op->format);
    }

  /* set the r bit to its default state for this insn */
  if (op->flags & FLAG_R) {
    if (xmov_flag)
      insn2.code |= (op->flags & FLAG_R_DEF ? 1 : 0) << 23;
    else
      insn.code |= (op->flags & FLAG_R_DEF ? 1 : 0) << 23;
  }
  
  /* Find and process any effect flags */
  do
    {
      str = skip_whitespace (str);
      if(*str == 0) break;
      p = find_whitespace_or_separator (str);
      c = *p;
      *p = '\0';
      lc (str);
      eff = (struct propeller_effect *) hash_find (eff_hash, str);
      *p = c;
      if (!eff)
        break;
      str = p;
      if (op->flags & eff->flag) {
        if (xmov_flag) {
          insn2.code |= eff->ormask;
          insn2.code &= eff->andmask;
        } else {
          insn.code |= eff->ormask;
          insn.code &= eff->andmask;
        }
      }
      else {
        as_bad(_("Effect '%s' not allowed with this instruction"), eff->name);
        return;
      }
      str = parse_separator (str, &error);
    }
  while (eff && !error);

  if (op1.error)
    err = op1.error;
  else if (op2.error)
    err = op2.error;
  else if (insn2.error)
    err = insn2.error;
  else if (op3.error)
    err = op3.error;
  else if (op4.error)
    err = op4.error;
  else
    {
      str = skip_whitespace (str);
      if (*str)
        err = _("Too many operands");
    }

  if (err)
    {
      as_bad ("%s", err);
      return;
    }


  /* check for possible compression */
  if (compress && op->compress_type && !insn_compressed) {
    unsigned destval, srcval;
    unsigned immediate;
    unsigned effects, expectedeffects;
    unsigned newcode;
    unsigned movbyte = 0;
    unsigned xopbyte = 0;

    /* first, we cannot compress big instructions with a conditional prefix */
    /* (except xmov) */
    if ( size > 4 && 0xf != condmask && !xmov_flag) {
      goto skip_compress;
    }

    /* make sure dest. is a legal register */
    if (op2.reloc.type != BFD_RELOC_NONE) goto skip_compress;
    destval = (insn.code >> 9) & 0x1ff;

    /* make sure srcval is legal (if a register) or immediate */
    if (op1.reloc.type != BFD_RELOC_NONE) goto skip_compress;
    immediate = (insn.code >> 22) & 0x1;
    srcval = insn.code & 0x1ff;

    effects = (insn.code >> 23) & 0x7;
    if (xmov_flag) {
      if (immediate) {
        as_bad (_("xmov may not have immediate argument for mov"));
        return;
      }
      if (effects != 1) {
        as_bad (_("No effects permitted in xmov"));
        return;
      }
      effects = (insn2.code >> 23) & 0x7;
      if (destval > 15 || srcval > 15)
        {
          as_bad (_("Illegal register in xmov"));
          return;
        }
      movbyte = (destval<<4) | srcval;

      immediate = (insn2.code >> 22) & 1;
      srcval = (insn2.code & 0x1ff);
      destval = (insn2.code >> 9) & 0x1ff;
    }

    /* now compress based on type */
    if (op->compress_type == COMPRESS_XOP)
      {
        /* make sure the effect flags match */
        switch (op->copc) {
        case XOP_CMPU:
        case XOP_CMPS:
          expectedeffects = 6;  /* wz,wc,nr */
          break;
        case XOP_WRB:
        case XOP_WRL:
          expectedeffects = 0;
          break;
        default:
          expectedeffects = 1;  /* just the R field */
          break;
        }
        if (effects != expectedeffects) {
          goto skip_compress;
        }

        /* handle special destination registers */
        if (destval > 15)
          {
            /* only compression with a destination > 15 is
               add sp,#XXX
            */
            if (xmov_flag) goto skip_compress;
            if (destval == SP_REGNUM && immediate && srcval < 128)
              {
                if (op->copc == XOP_ADD)
                  {
                    newcode = MACRO_ADDSP | (srcval << 8);
                    size = 2;
                    goto compress_done;
                  }
                if (op->copc == XOP_SUB)
                  {
                    srcval = (-srcval) & 0xff;
                    newcode = MACRO_ADDSP | (srcval << 8);
                    size = 2;
                    goto compress_done;
                  }
              }

            /* any other operation with a destination other than 0-15 is
               bad news */
            goto skip_compress;
          }

        /* special case -- a source of __MASK_FFFFFFFF can
           be changed to an immediate -1 */
        if (!immediate && srcval == FFFFFFFF_REGNUM)
          {
            immediate = 1;
            srcval = (-1) & 0x00000FFF;
          }

        /* OK, we can compress now */
        if (immediate)
          {
            if (srcval > 15) {
              if (xmov_flag) goto skip_compress;
              newcode = (PREFIX_REGIMM12 | destval);
              xopbyte = ((srcval & 0xff));
              xopbyte |= ((((srcval >> 8)&0xf)) | (op->copc<<4)) << 8;
              size = 3;
            } else {
              /* FIXME: could special case a few things here */
              if (xmov_flag)
                newcode = (PREFIX_XMOVIMM | destval);
              else
                newcode = (PREFIX_REGIMM4 | destval);
              xopbyte = ( ((srcval<<4)|op->copc) );
              size = 2;
            }
          }
        else
          {
            if (srcval > 15) goto skip_compress;
            if (xmov_flag)
              newcode = (PREFIX_XMOVREG | destval);
            else
              newcode = (PREFIX_REGREG | destval);
            xopbyte = ( ((srcval<<4)|op->copc));
            size = 2;
          }

        if (xmov_flag)
          {
            newcode |= movbyte << 8;
            newcode |= xopbyte << 16;
            size++;
          }
        else
          {
            newcode |= xopbyte << 8;
          }
      }
    else if (op->compress_type == COMPRESS_MOV)
      {
        if (destval > 15)
          {
            goto skip_compress;
          }
        /* for mov, only default wr effect can be compressed */
        if (effects != 1) goto skip_compress;
        if (immediate)
          {
            if (xmov_flag) {
              as_bad (_("mov immediate not supported in xmov"));
              return;
            }
            if (srcval == 0 && condmask == 0xf) {
              newcode = (PREFIX_ZEROREG | destval);
              size = 1;
            } else if (srcval <= 255) {
              newcode = (PREFIX_MVIB | destval) | (srcval << 8);
              size = 2;
            } else {
              newcode = (PREFIX_MVIW | destval) | (srcval << 8);
              size = 3;
            }
          }
        else
          {
            if (srcval > 15)
              {
                goto skip_compress;
              }
            if (xmov_flag)
              {
                newcode = MACRO_XMVREG;
                newcode |= (movbyte << 8) | (((destval << 4) | srcval) << 16);
                size = 3;
              }
            else
              {
                newcode = MACRO_MVREG;
                newcode |= (((destval << 4) | srcval) << 8);
                size = 2;
              }
          }
      }
    else
      {
        goto skip_compress;
      }
  compress_done:
    if (condmask != 0xf) {
      newcode = newcode << 8;
      condmask = ~condmask & 0xf;
      if (size == 3)
        newcode |= (PREFIX_SKIP3 | condmask);
      else
        newcode |= (PREFIX_SKIP2 | condmask);
      size++;
    }
    insn.code = newcode;
    insn_compressed = 1;
    /* no relocations required */
    insn.reloc.type = BFD_RELOC_NONE;
    op1.reloc.type = BFD_RELOC_NONE;
    op2.reloc.type = BFD_RELOC_NONE;
    /* no second instruction needed */
    insn2.code = 0;
    insn2.reloc.type = BFD_RELOC_NONE;
  }
 skip_compress:

  /* if the instruction still isn't compressed, we may be able
     to pack it into 4 bytes anyway, so long as the condition
     flags are 0xF
  */
  if (compress && !insn_compressed && size == 4 && condmask == 0xf) {
    insn.code = pack_native(insn.code);
    reloc_prefix = 1;
    insn_compressed = 1;
  }

  {
    unsigned int insn_size;
    unsigned int alloc_size;
    unsigned int bytes_written = 0;
#define CHECK_WRITTEN(n) do { bytes_written += (n); if (bytes_written > alloc_size) abort(); } while (0)

    if (compress && !insn_compressed) {
      /* we are in CMM mode, but failed to compress this instruction;
         we will have to add a NATIVE prefix
      */
      size += !insn_compressed;
      if (insn2.reloc.type != BFD_RELOC_NONE || insn2.code)
        size += !insn2_compressed;
      insn_size = 4;
    } else if (compress) {
      insn_size = size;
    } else {
      insn_size = 4;
    }
    to = frag_more (size);
    alloc_size = size;

    if (compress) {
      if (!insn_compressed) {
        md_number_to_chars (to, MACRO_NATIVE, 1);
        CHECK_WRITTEN(1);
        to++;
      } else if (insn_size > 4) {
        /* handle the rare long compressed forms like mvi */
        md_number_to_chars (to, insn.code, insn_size-4);
        CHECK_WRITTEN(insn_size-4);
        to += (insn_size-4);
        insn_size = size = 4;
        insn = insn2;
        insn2.code = 0;
        insn2.reloc.type = BFD_RELOC_NONE;
      }
    }
    md_number_to_chars (to, insn.code, insn_size);
    CHECK_WRITTEN(insn_size);
    if (insn.reloc.type != BFD_RELOC_NONE)
      fix_new_exp (frag_now, to - frag_now->fr_literal + reloc_prefix, 
                   insn_size - reloc_prefix,
                   &insn.reloc.exp, insn.reloc.pc_rel, insn.reloc.type);
    if (op1.reloc.type != BFD_RELOC_NONE)
      fix_new_exp (frag_now, to - frag_now->fr_literal + reloc_prefix,
                   insn_size - reloc_prefix,
                   &op1.reloc.exp, op1.reloc.pc_rel, op1.reloc.type);
    if (op2.reloc.type != BFD_RELOC_NONE)
      fix_new_exp (frag_now, to - frag_now->fr_literal + reloc_prefix,
                   insn_size - reloc_prefix,
                   &op2.reloc.exp, op2.reloc.pc_rel, op2.reloc.type);
    to += insn_size;

    /* insn2 is never used for real instructions, but is useful */
    /* for some pseudoinstruction for LMM and such. */
    /* note that we never have to do this for compressed instructions */
    if (insn2.reloc.type != BFD_RELOC_NONE || insn2.code)
      {
        if (compress && !insn2_compressed) {
          md_number_to_chars ( to, MACRO_NATIVE, 1 );
          CHECK_WRITTEN(1);
          to++;
        }
        md_number_to_chars (to, insn2.code, 4);
        CHECK_WRITTEN(4);
        if(insn2.reloc.type != BFD_RELOC_NONE){
          fix_new_exp (frag_now, to - frag_now->fr_literal, 4,
                       &insn2.reloc.exp, insn2.reloc.pc_rel, insn2.reloc.type);
        }
        if (op3.reloc.type != BFD_RELOC_NONE)
          fix_new_exp (frag_now, to - frag_now->fr_literal, 4,
                       &op3.reloc.exp, op3.reloc.pc_rel, op3.reloc.type);
        if (op4.reloc.type != BFD_RELOC_NONE)
          fix_new_exp (frag_now, to - frag_now->fr_literal, 4,
                       &op4.reloc.exp, op4.reloc.pc_rel, op4.reloc.type);
        to += 4;
      }
  }
  if (insn_compressed)
    elf_flags |= EF_PROPELLER_COMPRESS;
}

int
md_estimate_size_before_relax (fragS * fragP ATTRIBUTE_UNUSED,
                               segT segment ATTRIBUTE_UNUSED)
{
  return 0;
}

void
md_convert_frag (bfd * headers ATTRIBUTE_UNUSED,
                 segT seg ATTRIBUTE_UNUSED, fragS * fragP ATTRIBUTE_UNUSED)
{
}

void
propeller_frob_label (symbolS * sym)
{
  unsigned int flag = 0;
  static const int null_flag = 0;

  if (compress) {
    flag |= PROPELLER_OTHER_COMPRESSED;
  }
  /* reset the tc marker for all newly created symbols */
  if (flag) {
    symbol_set_tc (sym, (int *)&null_flag);
    S_SET_OTHER (sym, S_GET_OTHER (sym) | flag); 
  }
}

int md_short_jump_size = 4;
int md_long_jump_size = 4;

void
md_create_short_jump (char *ptr ATTRIBUTE_UNUSED,
                      addressT from_addr ATTRIBUTE_UNUSED,
                      addressT to_addr ATTRIBUTE_UNUSED,
                      fragS * frag ATTRIBUTE_UNUSED,
                      symbolS * to_symbol ATTRIBUTE_UNUSED)
{
}

void
md_create_long_jump (char *ptr ATTRIBUTE_UNUSED,
                     addressT from_addr ATTRIBUTE_UNUSED,
                     addressT to_addr ATTRIBUTE_UNUSED,
                     fragS * frag ATTRIBUTE_UNUSED,
                     symbolS * to_symbol ATTRIBUTE_UNUSED)
{
}


/* Invocation line includes a switch not recognized by the base assembler.
   See if it's a processor-specific option.  */
int
md_parse_option (int c, const char *arg)
{
  (void)arg;
  switch (c)
    {
    case OPTION_MD_LMM:
      lmm = 1;
      break;
    case OPTION_MD_CMM:
      compress = compress_default = 1;
      lmm = 1; /* -cmm implies -lmm */
      break;
    case OPTION_MD_P2:
      prop2 = 1;
      break;
    case OPTION_MD_PASM:
      pasm_default = 1;
      break;
    default:
      return 0;
    }
  return 1;
}

void
md_show_usage (FILE * stream)
{
  fprintf (stream, "\
Propeller options\n\
  --lmm\t\tEnable LMM instructions.\n\
  --cmm\t\tEnable compressed instructions.\n\
  --p2\t\tEnable Propeller 2 instructions.\n\
");
}

symbolS *
md_undefined_symbol (char *name ATTRIBUTE_UNUSED)
{
  return 0;
}

/*
 * round up a section's size to the appropriate boundary
 */
valueT
md_section_align (segT segment, valueT size)
{
  int align = bfd_get_section_alignment (stdoutput, segment);
  valueT mask = ((valueT) 1 << align) - 1;

  return (size + mask) & ~mask;
}

long
md_pcrel_from (fixS * fixP)
{
  return fixP->fx_frag->fr_address + fixP->fx_where + fixP->fx_size;
}

/* any special processing for the ELF output file */
void
propeller_elf_final_processing (void)
{
  /* set various flags in the elf header if necessary */
  if (0 == EF_PROPELLER_GET_ABI(elf_flags))
    EF_PROPELLER_PUT_ABI(elf_flags, DEFAULT_PROPELLER_ABI);
  elf_elfheader (stdoutput)->e_flags |= elf_flags;
}

//static int propeller_pasm_cons_reloc;
/*
 * constant parsing code
 */
TC_PARSE_CONS_RETURN_TYPE
propeller_cons (expressionS *exp, int size)
{
    int propeller_pasm_cons_reloc;
    (void)size;

    propeller_pasm_cons_reloc = pasm_default;
    SKIP_WHITESPACE ();
    if (input_line_pointer[0] == '@')
      {
        propeller_pasm_cons_reloc = 0;
        input_line_pointer++;
      }
    else if (input_line_pointer[0] == '&')
      {
        propeller_pasm_cons_reloc = 1;
        input_line_pointer++;
      }
    expression (exp);
    return propeller_pasm_cons_reloc;
}

/* This is called by emit_expr via TC_CONS_FIX_NEW when creating a
   reloc for a cons.  */

void
propeller_cons_fix_new (struct frag *frag, int where, unsigned int nbytes, struct expressionS *exp, int propeller_pasm_cons_reloc)
{
  bfd_reloc_code_real_type r;

  r = (nbytes == 1 ? BFD_RELOC_8 :
       (nbytes == 2 ? BFD_RELOC_16 : BFD_RELOC_32));

  if (propeller_pasm_cons_reloc && pasm_replace_expression (exp))
    {
      r = (nbytes == 1 ? BFD_RELOC_PROPELLER_8_DIV4 :
           (nbytes == 2 ? BFD_RELOC_PROPELLER_16_DIV4 : BFD_RELOC_PROPELLER_32_DIV4));

    }
  fix_new_exp (frag, where, (int) nbytes, exp, 0, r);
}

/*
 * replace constants in an expression to make it PASM compatible;
 * since pasm uses longword addressing, we have to multiply offsets
 * by 4 to convert to byte addressing (e.g. n+1 -> n+4)
 * If the expression is a simple immediate constant, make no changes and
 *   return 0
 * Returns 1 if changes were made, 0 if no changes necessary
 */
static int pasm_replace_expression (expressionS *exp)
{
    switch (exp->X_op)
    {
    case O_constant:
    case O_register:
        /* make no change */
        return 0;
    default:
        break;
    }
    exp->X_add_number *= 4;
    return 1;
}


typedef struct prop_localsym {
    struct prop_localsym *next;
    char *name;
    int defined;
    int value;
} Prop_LocalSym;

static Prop_LocalSym *colonsyms = 0;
static int colonval = 0;

/*
 * free and clear the list of local symbols
 * also reports any that were never defined
 */
static void
clear_colonsyms(void)
{
    Prop_LocalSym *cur, *next;
    cur = colonsyms;
    while (cur) {
        next = cur->next;
        if (!cur->defined) {
            as_bad (_("Local symbol `%s' never defined"), cur->name);
        }
        xfree(cur->name);
        xfree(cur);
        cur = next;
    }
    colonsyms = 0;
    colonval = 0;
}

/*
 * look up a local sym, allocating it if necessary
 */
static Prop_LocalSym *
lookup_colonsym(const char *s)
{
    Prop_LocalSym *r;

    for (r = colonsyms; r; r = r->next) {
        if (!strcmp(s, r->name)) {
            return r;
        }
    }
    /* if we get here, allocate a new one */
    r = (Prop_LocalSym *)xmalloc (sizeof(*r));
    r->name = xstrdup (s);
    r->defined = 0;
    r->value = colonval++;
    r->next = colonsyms;
    colonsyms = r;
    return r;
}

/*
 * read a name from *input_ptr; include the ':'
 */

static char *
get_colon_name(char *input_ptr)
{
    char *name = input_ptr++;
    char save_c;
    while (is_part_of_name (*input_ptr))
        input_ptr++;
    save_c = *input_ptr;
    *input_ptr = 0;
    name = xstrdup (name);
    *input_ptr = save_c;
    return name;
}

static char *
handle_colon(char *s, int start_of_line)
{
    char *p;
    char *name;
    Prop_LocalSym *sym;
    char buf[80];
    int i;

    name = get_colon_name (s);
    sym = lookup_colonsym (name);
    if (start_of_line) {
        if (sym->defined) {
            as_bad (_("Symbol `%s' redefined"), name);
            xfree (name);
        }
        sym->defined = 1;
    }

    // now rewrite s into "val$:"
    p = s;
    s += strlen(name);
#if 0
    while (*s != '\n' && ISSPACE (*s)) {
        s++;
    }
#endif
    sprintf (buf, "%d", sym->value);
    if ((size_t)(strlen(buf)+1+start_of_line) > (size_t)(s - p)) {
        as_bad (_("Not enough space for temporary label `%s'"), name);
    } else {
        for (i = 0; buf[i]; i++) {
            *p++ = buf[i];
        }
        *p++ = '$';
        if (start_of_line)
            *p++ = ':';
#if 0
        while (!ISSPACE (*p) && p != s) {
            *p++ = ' ';
        }
#else
        // don't insert any extraneous space
        while(*s) {
            *p++ = *s++;
        }
        *p++ = 0;
#endif
    }
    xfree (name);

    return s;
}

static void
erase_line(void)
{
    char *s;

    s = input_line_pointer;
    while (*s && *s != '\n') {
        *s++ = ' ';
    }
}

/*
 * case-insensitive compare
 */
static int
matchword(const char *line, const char *word)
{
    int a, b;

    while (*word) {
        a = *line++;
        b = *word++;
        a = TOUPPER (a);
        b = TOUPPER (b);
        if (a != b)
            return 0;
    }
    return *line == 0 || *line == '\n' || ISSPACE(*line);
}

/*
 * re-write a line to adapt PASM style local labels (":foo") into
 * GAS style ones ("1$")
 * Also processes multi-line PASM style comments { ... },
 * handles the CON and DAT declarations,
 * ignores PUB and PRI spin code,
 * converts "current location" from $ to .
 */

static int is_spin_file = 0;
static int skip_spin_code;
static int in_comment;
static int in_quote;

void
propeller_start_line_hook (void)
{
    char *s = input_line_pointer;

    if (!pasm_default)
        return;

    if (*s != ':' && is_name_beginner (*s))
      {
        clear_colonsyms();
      }

    /* check some things at start of line */
    if (matchword (s, "con") || matchword (s, "dat")) {
        // erase the word, but keep processing the line
        // in case they put code after it
        s[0] = s[1] = s[2] = ' ';
        is_spin_file = 1;
        skip_spin_code = 0;
    }
    if (is_spin_file
        && (matchword (s, "pub")
            || matchword (s, "pri")
            || matchword (s, "var")
            || matchword (s, "obj")
            ))
      {
        skip_spin_code = 1;
      }
    if (skip_spin_code)
      {
        erase_line ();
        return;
      }

    /* process the rest of the line */
    while (*s && *s != '\n')
      {
        if (in_comment)
          {
            if (*s == '{')
                in_comment++;
            else if (*s == '}')
                in_comment--;
            *s++ = ' ';
          }
        else if (in_quote)
          {
            if (*s == '"')
                in_quote = 0;
            s++;
          }
        else if (*s == ':' && is_part_of_name (s[1]))
          {
            s = handle_colon(s, s == input_line_pointer);
          }
        else if (s > input_line_pointer && s[0] == '$' 
                   && !ISALNUM (s[1]) && !ISALNUM (s[-1]) )
          {
            /* PASM uses '$' as the location counter, but also
               in hex constants, and we use it for local labels
            */
            *s++ = '.';
          }
        else if (*s == '"')
          {
            in_quote = 1;
            s++;
          }
        else if (*s == '{')
          {
            in_comment = 1;
            *s++ = ' ';
          }
        else
          {
            s++;
          }
      }

    if (in_quote)
      {
        as_bad (_("Unterminated quote"));
        in_quote = 0;
      }
}
