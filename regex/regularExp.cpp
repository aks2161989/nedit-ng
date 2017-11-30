/*------------------------------------------------------------------------*
 * 'CompileRE', 'ExecRE', and 'substituteRE' -- regular expression parsing
 *
 * This is a HIGHLY ALTERED VERSION of Henry Spencer's 'regcomp' and
 * 'regexec' code adapted for NEdit.
 *
 * .-------------------------------------------------------------------.
 * | ORIGINAL COPYRIGHT NOTICE:                                        |
 * |                                                                   |
 * | Copyright (c) 1986 by University of Toronto.                      |
 * | Written by Henry Spencer.  Not derived from licensed software.    |
 * |                                                                   |
 * | Permission is granted to anyone to use this software for any      |
 * | purpose on any computer system, and to redistribute it freely,    |
 * | subject to the following restrictions:                            |
 * |                                                                   |
 * | 1. The author is not responsible for the consequences of use of   |
 * |      this software, no matter how awful, even if they arise       |
 * |      from defects in it.                                          |
 * |                                                                   |
 * | 2. The origin of this software must not be misrepresented, either |
 * |      by explicit claim or by omission.                            |
 * |                                                                   |
 * | 3. Altered versions must be plainly marked as such, and must not  |
 * |      be misrepresented as being the original software.            |
 * '-------------------------------------------------------------------'
 *
 * This is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version. In addition, you may distribute version of this program linked to
 * Motif or Open Motif. See README for details.
 *
 * This software is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * software; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307 USA
 *
 *
 * BEWARE that some of this code is subtly aware of the way operator
 * precedence is structured in regular expressions.  Serious changes in
 * regular-expression syntax might require a total rethink.
 *   -- Henry Spencer
 * (Yes, it did!) -- Christopher Conrad, Dec. 1999
 *
 * January, 1994, Mark Edel
 *    Consolidated files, changed names of external functions to avoid
 *    potential conflicts with native regcomp and regexec functions, changed
 *    error reporting to NEdit form, added multi-line and reverse searching,
 *    and added \n \t \u \U \l \L.
 *
 * June, 1996, Mark Edel
 *    Bug in NEXT macro, didn't work for expressions which compiled to over
 *    256 bytes.
 *
 * December, 1999, Christopher Conrad
 *    Reformatted code for readability, improved error output, added octal and
 *    hexadecimal escapes, added back-references (\1-\9), added positive look
 *    ahead: (?=...), added negative lookahead: (?!...),  added non-capturing
 *    parentheses: (?:...), added case insensitive constructs (?i...) and
 *    (?I...), added newline matching constructs (?n...) and (?N...), added
 *    regex comments: (?#...), added shortcut escapes: \d\D\l\L\s\S\w\W\y\Y.
 *    Added "not a word boundary" anchor \B.
 *
 * July, 2002, Eddy De Greef
 *    Added look behind, both positive (?<=...) and negative (?<!...) for
 *    bounded-length patterns.
 *
 * November, 2004, Eddy De Greef
 *    Added constrained matching (allowing specification of the logical end
 *    of the string iso. matching till \0), and fixed several (probably
 *    very old) string overrun errors that could easily result in crashes,
 *    especially in client code.
 */

#include "regularExp.h"
#include "Opcodes.h"
#include "ParseContext.h"
#include "ExecuteContext.h"
#include "util/raise.h"

#include <bitset>
#include <cassert>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

/* The next_ptr () function can consume up to 30% of the time during matching
   because it is called an immense number of times (an average of 25
   next_ptr() calls per match() call was witnessed for Perl syntax
   highlighting). Therefore it is well worth removing some of the function
   call overhead by selectively inlining the next_ptr() calls. Moreover,
   the inlined code can be simplified for matching because one of the tests,
   only necessary during compilation, can be left out.
   The net result of using this inlined version at two critical places is
   a 25% speedup (again, witnesses on Perl syntax highlighting). */

#define NEXT_PTR(in_ptr, out_ptr)                   \
    do {                                            \
        next_ptr_offset = GET_OFFSET(in_ptr);       \
        if (next_ptr_offset == 0)                   \
            out_ptr = nullptr;                      \
        else {                                      \
            if (GET_OP_CODE(in_ptr) == BACK)        \
                out_ptr = in_ptr - next_ptr_offset; \
            else                                    \
                out_ptr = in_ptr + next_ptr_offset; \
        }                                           \
    } while(0)

ExecuteContext eContext;
ParseContext pContext;

namespace {

/* The first byte of the regexp internal 'program' is a magic number to help
   gaurd against corrupted data; the compiled regex code really begins in the
   second byte. */

constexpr uint8_t MAGIC = 0234;

/* The "internal use only" fields in `regexp.h' are present to pass info from
 * `CompileRE' to `ExecRE' which permits the execute phase to run lots faster on
 * simple cases.  They are:
 *
 *   match_start     Character that must begin a match; '\0' if none obvious.
 *   anchor          Is the match anchored (at beginning-of-line only)?
 *
 * `match_start' and `anchor' permit very fast decisions on suitable starting
 * points for a match, considerably reducing the work done by ExecRE. */


/* A node is one char of opcode followed by two chars of NEXT pointer plus
 * any operands.  NEXT pointers are stored as two 8-bit pieces, high order
 * first.  The value is a positive offset from the opcode of the node
 * containing it.  An operand, if any, simply follows the node.  (Note that
 * much of the code generation knows about this implicit relationship.)
 *
 * Using two bytes for NEXT_PTR_SIZE is vast overkill for most things,
 * but allows patterns to get big without disasters. */

constexpr int OP_CODE_SIZE  = 1;
constexpr int NEXT_PTR_SIZE = 2;
constexpr int INDEX_SIZE    = 1;
constexpr int LENGTH_SIZE   = 4;
constexpr int NODE_SIZE     = NEXT_PTR_SIZE + OP_CODE_SIZE;

// Flags to be passed up and down via function parameters during compile.

constexpr int WORST       = 0; // Worst case. No assumptions can be made.
constexpr int HAS_WIDTH   = 1; // Known never to match null string.
constexpr int SIMPLE      = 2; // Simple enough to be STAR/PLUS operand.

constexpr int NO_PAREN    = 0; // Only set by initial call to "chunk".
constexpr int PAREN       = 1; // Used for normal capturing parentheses.
constexpr int NO_CAPTURE  = 2; // Non-capturing parentheses (grouping only).
constexpr int INSENSITIVE = 3; // Case insensitive parenthetical construct
constexpr int SENSITIVE   = 4; // Case sensitive parenthetical construct
constexpr int NEWLINE     = 5; // Construct to match newlines in most cases
constexpr int NO_NEWLINE  = 6; // Construct to match newlines normally

const auto REG_INFINITY   = 0UL;
const auto REG_ZERO       = 0UL;
const auto REG_ONE        = 1UL;

// Flags for function shortcut_escape()

constexpr int CHECK_ESCAPE       = 0; // Check an escape sequence for validity only.
constexpr int CHECK_CLASS_ESCAPE = 1; // Check the validity of an escape within a character class
constexpr int EMIT_CLASS_BYTES	 = 2; // Emit equivalent character class bytes, e.g \d=0123456789
constexpr int EMIT_NODE          = 3; // Emit the appropriate node.



/* Number of bytes to offset from the beginning of the regex program to the start
   of the actual compiled regex code, i.e. skipping over the MAGIC number and
   the two counters at the front.  */

constexpr int REGEX_START_OFFSET = 3;

// Largest size a compiled regex can be. Probably could be 65535UL.
constexpr auto MAX_COMPILED_SIZE  = 32767UL;



// Address of this used as flag.
uint8_t Compute_Size;

}

struct len_range {
	long lower;
	long upper;
};

template <class T>
char literal_escape(T ch);

template <class T>
uint8_t *emit_node(T op_code);

template <class T>
void emit_byte(T ch);

template <class T>
void emit_class_byte(T ch);

template <class T>
uint8_t *shortcut_escape(T ch, int *flag_param, int emit);

template <class T>
char numeric_escape(T ch, const char **parse);

static bool init_ansi_classes();
static uint8_t *alternative(int *flag_param, len_range *range_param);
static uint8_t *atom(int *flag_param, len_range *range_param);
static uint8_t *back_ref(const char *c, int *flag_param, int emit);
static uint8_t *chunk(int paren, int *flag_param, len_range *range_param);
static uint8_t *emit_special(uint8_t op_code, unsigned long test_val, size_t index);
static uint8_t *insert(uint8_t op, uint8_t *insert_pos, long min, long max, size_t index);
static uint8_t *next_ptr(uint8_t *ptr);
static uint8_t *piece(int *flag_param, len_range *range_param);
static void branch_tail(uint8_t *ptr, int offset, uint8_t *val);
static void offset_tail(uint8_t *ptr, int offset, uint8_t *val);
static void reg_error(const char *str);
static void tail(uint8_t *search_from, uint8_t *point_to);

namespace {

const char ASCII_Digits[]      = "0123456789"; // Same for all locales.
const char Default_Meta_Char[] = "{.*+?[(|)^<>$";

// NOTE(eteran): duplicated from utils.h
template <int (&F) (int)>
int safe_ctype (int c) {
    return F(static_cast<unsigned char>(c));
}

uint8_t GET_OP_CODE(uint8_t *p) {
	return *p;
}

uint8_t *OPERAND(uint8_t *p) {
	return p + NODE_SIZE;
}

uint16_t GET_OFFSET(uint8_t *p) {
    return static_cast<uint16_t>(((p[1] & 0xff) << 8) + (p[2] & 0xff));
}

template <class T>
constexpr uint8_t PUT_OFFSET_L(T v) {
    return static_cast<uint8_t>((v >> 8) & 0xff);
}

template <class T>
constexpr uint8_t PUT_OFFSET_R(T v) {
    return static_cast<uint8_t>(v & 0xff);
}

uint8_t GET_LOWER(uint8_t *p) {
    return static_cast<uint8_t>(((p[NODE_SIZE + 0] & 0xff) << 8) + ((p[NODE_SIZE + 1]) & 0xff));
}

uint8_t GET_UPPER(uint8_t *p) {
    return static_cast<uint8_t>(((p[NODE_SIZE + 2] & 0xff) << 8) + ((p[NODE_SIZE + 3]) & 0xff));
}

// Utility definitions.
bool IS_QUANTIFIER(char c) {
    return c == '*' || c == '+' || c == '?' || c == pContext.Brace_Char;
}

template <class T>
unsigned int U_CHAR_AT(T *p) {
	return static_cast<unsigned int>(*p);
}

bool AT_END_OF_STRING(const char *ptr) {

    if(eContext.End_Of_String != nullptr && ptr >= eContext.End_Of_String) {
        return true;
    }

    return false;
}

// Default table for determining whether a character is a word delimiter.

std::bitset<256> Default_Delimiters;


bool isDelimiter(int ch) {
    unsigned int n = static_cast<unsigned int>(ch);
    if(n < eContext.Current_Delimiters.size()) {
        return eContext.Current_Delimiters[n];
    }

    return false;
}

}

// Forward declarations of functions used by 'ExecRE'

static bool attempt(regexp *prog, const char *string);
static int match(uint8_t *prog, int *branch_index_param);
static unsigned long greedy(uint8_t *p, long max);
static std::bitset<256> makeDelimiterTable(view::string_view delimiters);


/*----------------------------------------------------------------------*
 * CompileRE
 *
 * Compiles a regular expression into the internal format used by
 * 'ExecRE'.
 *
 * The default behaviour wrt. case sensitivity and newline matching can
 * be controlled through the defaultFlags argument (Markus Schwarzenberg).
 * Future extensions are possible by using other flag bits.
 * Note that currently only the case sensitivity flag is effectively used.
 *
 * Beware that the optimization and preparation code in here knows about
 * some of the structure of the compiled regexp.
 *----------------------------------------------------------------------*/
regexp::regexp(view::string_view exp, int defaultFlags) {

	// NOTE(eteran): previously uninitialized
    std::fill_n(startp.begin(), NSUBEXP, nullptr);
    std::fill_n(endp.begin(), NSUBEXP, nullptr);
	extentpBW = nullptr;
	extentpFW = nullptr;
	top_branch = 0;

    int flags_local;
	len_range range_local;

    if (pContext.Enable_Counting_Quantifier) {
        pContext.Brace_Char = '{';
        pContext.Meta_Char = &Default_Meta_Char[0];
	} else {
        pContext.Brace_Char = '*';                  // Bypass the '{' in
        pContext.Meta_Char = &Default_Meta_Char[1]; // Default_Meta_Char
	}

	// Initialize arrays used by function 'shortcut_escape'.
	if (!init_ansi_classes()) {
        raise<regex_error>("internal error #1, 'CompileRE'");
	}

    pContext.Code_Emit_Ptr = &Compute_Size;
    pContext.Reg_Size = 0UL;

	/* We can't allocate space until we know how big the compiled form will be,
	   but we can't compile it (and thus know how big it is) until we've got a
	   place to put the code.  So we cheat: we compile it twice, once with code
	   generation turned off and size counting turned on, and once "for real".
	   This also means that we don't allocate space until we are sure that the
	   thing really will compile successfully, and we never have to move the
	   code and thus invalidate pointers into it.  (Note that it has to be in
	   one piece because free() must be able to free it all.) */

    for (int pass = 1; pass <= 2; pass++) {
		/*-------------------------------------------*
		 * FIRST  PASS: Determine size and legality. *
		 * SECOND PASS: Emit code.                   *
		 *-------------------------------------------*/

		/*  Schwarzenberg:
		 *  If defaultFlags = 0 use standard defaults:
		 *    Is_Case_Insensitive: Case sensitive is the default
		 *    Match_Newline:       Newlines are NOT matched by default
		 *                         in character classes
		 */
        pContext.Is_Case_Insensitive = ((defaultFlags & REDFLT_CASE_INSENSITIVE) ? 1 : 0);
        pContext.Match_Newline = 0; /* ((defaultFlags & REDFLT_MATCH_NEWLINE)   ? 1 : 0);
		                      Currently not used. Uncomment if needed. */

        pContext.Reg_Parse       = exp.begin();
        pContext.Reg_Parse_End   = exp.end();
        pContext.Total_Paren     = 1;
        pContext.Num_Braces      = 0;
        pContext.Closed_Parens   = 0;
        pContext.Paren_Has_Width = 0;

		emit_byte(MAGIC);
		emit_byte('%'); // Placeholder for num of capturing parentheses.
		emit_byte('%'); // Placeholder for num of general {m,n} constructs.

		if (chunk(NO_PAREN, &flags_local, &range_local) == nullptr) {
            raise<regex_error>("internal error #10, 'CompileRE'");
		}

		if (pass == 1) {
            if (pContext.Reg_Size >= MAX_COMPILED_SIZE) {
				/* Too big for NEXT pointers NEXT_PTR_SIZE bytes long to span.
				   This is a real issue since the first BRANCH node usually points
				   to the end of the compiled regex code. */

                raise<regex_error>("regexp > %lu bytes", MAX_COMPILED_SIZE);
			}

			// Allocate memory.

            program = new uint8_t[pContext.Reg_Size];

            pContext.Code_Emit_Ptr = this->program;
		}
	}

    this->program[1] = static_cast<uint8_t>(pContext.Total_Paren - 1);
    this->program[2] = static_cast<uint8_t>(pContext.Num_Braces);

	/*----------------------------------------*
	 * Dig out information for optimizations. *
	 *----------------------------------------*/

	this->match_start = '\0'; // Worst-case defaults.
	this->anchor = 0;

	// First BRANCH.

    uint8_t *scan = (this->program + REGEX_START_OFFSET);

	if (GET_OP_CODE(next_ptr(scan)) == END) { // Only one top-level choice.
		scan = OPERAND(scan);

		// Starting-point info.

		if (GET_OP_CODE(scan) == EXACTLY) {
			this->match_start = *OPERAND(scan);

		} else if (PLUS <= GET_OP_CODE(scan) && GET_OP_CODE(scan) <= LAZY_PLUS) {

			/* Allow x+ or x+? at the start of the regex to be
			   optimized. */

			if (GET_OP_CODE(scan + NODE_SIZE) == EXACTLY) {
				this->match_start = *OPERAND(scan + NODE_SIZE);
			}
		} else if (GET_OP_CODE(scan) == BOL) {
			this->anchor++;
		}
	}
}

/**
 * @brief regexp::~regexp
 */
regexp::~regexp() {
	delete [] program;
}

/*----------------------------------------------------------------------*
 * chunk                                                                *
 *                                                                      *
 * Process main body of regex or process a parenthesized "thing".       *
 *                                                                      *
 * Caller must absorb opening parenthesis.                              *
 *                                                                      *
 * Combining parenthesis handling with the base level of regular        *
 * expression is a trifle forced, but the need to tie the tails of the  *
 * branches to what follows makes it hard to avoid.                     *
 *----------------------------------------------------------------------*/
uint8_t *chunk(int paren, int *flag_param, len_range *range_param) {

	uint8_t *ret_val = nullptr;
	uint8_t *this_branch;
	uint8_t *ender = nullptr;
	size_t this_paren = 0;
    int flags_local;
    int first = 1;
    int zero_width;
    int old_sensitive = pContext.Is_Case_Insensitive;
    int old_newline = pContext.Match_Newline;
	len_range range_local;
	int look_only = 0;
	uint8_t *emit_look_behind_bounds = nullptr;

	*flag_param = HAS_WIDTH; // Tentatively.
	range_param->lower = 0;  // Idem
	range_param->upper = 0;

	// Make an OPEN node, if parenthesized.

	if (paren == PAREN) {
        if (pContext.Total_Paren >= NSUBEXP) {
            raise<regex_error>("number of ()'s > %d", static_cast<int>(NSUBEXP));
		}

        this_paren = pContext.Total_Paren;
        pContext.Total_Paren++;
		ret_val = emit_node(OPEN + this_paren);
	} else if (paren == POS_AHEAD_OPEN || paren == NEG_AHEAD_OPEN) {
		*flag_param = WORST; // Look ahead is zero width.
		look_only = 1;
		ret_val = emit_node(paren);
	} else if (paren == POS_BEHIND_OPEN || paren == NEG_BEHIND_OPEN) {
		*flag_param = WORST; // Look behind is zero width.
		look_only = 1;
		// We'll overwrite the zero length later on, so we save the ptr
        ret_val = emit_special(paren, 0, 0);
		emit_look_behind_bounds = ret_val + NODE_SIZE;
	} else if (paren == INSENSITIVE) {
        pContext.Is_Case_Insensitive = 1;
	} else if (paren == SENSITIVE) {
        pContext.Is_Case_Insensitive = 0;
	} else if (paren == NEWLINE) {
        pContext.Match_Newline = 1;
	} else if (paren == NO_NEWLINE) {
        pContext.Match_Newline = 0;
	}

	// Pick up the branches, linking them together.

	do {
		this_branch = alternative(&flags_local, &range_local);

		if (this_branch == nullptr)
            return nullptr;

		if (first) {
			first = 0;
			*range_param = range_local;
			if (ret_val == nullptr)
				ret_val = this_branch;
		} else if (range_param->lower >= 0) {
			if (range_local.lower >= 0) {
				if (range_local.lower < range_param->lower)
					range_param->lower = range_local.lower;
				if (range_local.upper > range_param->upper)
					range_param->upper = range_local.upper;
			} else {
				range_param->lower = -1; // Branches have different lengths
				range_param->upper = -1;
			}
		}

		tail(ret_val, this_branch); // Connect BRANCH -> BRANCH.

		/* If any alternative could be zero width, consider the whole
		   parenthisized thing to be zero width. */

		if (!(flags_local & HAS_WIDTH))
			*flag_param &= ~HAS_WIDTH;

		// Are there more alternatives to process?

        if (*pContext.Reg_Parse != '|')
			break;

        ++pContext.Reg_Parse;
	} while (1);

	// Make a closing node, and hook it on the end.

	if (paren == PAREN) {
		ender = emit_node(CLOSE + this_paren);

	} else if (paren == NO_PAREN) {
		ender = emit_node(END);

	} else if (paren == POS_AHEAD_OPEN || paren == NEG_AHEAD_OPEN) {
		ender = emit_node(LOOK_AHEAD_CLOSE);

	} else if (paren == POS_BEHIND_OPEN || paren == NEG_BEHIND_OPEN) {
		ender = emit_node(LOOK_BEHIND_CLOSE);

	} else {
		ender = emit_node(NOTHING);
	}

	tail(ret_val, ender);

	// Hook the tails of the branch alternatives to the closing node.

	for (this_branch = ret_val; this_branch != nullptr;) {
		branch_tail(this_branch, NODE_SIZE, ender);
		this_branch = next_ptr(this_branch);
	}

	// Check for proper termination.

    if (paren != NO_PAREN && *pContext.Reg_Parse++ != ')') {
        raise<regex_error>("missing right parenthesis ')'");
    } else if (paren == NO_PAREN && pContext.Reg_Parse != pContext.Reg_Parse_End) {
        if (*pContext.Reg_Parse == ')') {
            raise<regex_error>("missing left parenthesis '('");
		} else {
            raise<regex_error>("junk on end"); // "Can't happen" - NOTREACHED
		}
	}

	// Check whether look behind has a fixed size

	if (emit_look_behind_bounds) {
		if (range_param->lower < 0) {
            raise<regex_error>("look-behind does not have a bounded size");
		}
		if (range_param->upper > 65535L) {
            raise<regex_error>("max. look-behind size is too large (>65535)");
		}
        if (pContext.Code_Emit_Ptr != &Compute_Size) {
			*emit_look_behind_bounds++ = PUT_OFFSET_L(range_param->lower);
			*emit_look_behind_bounds++ = PUT_OFFSET_R(range_param->lower);
			*emit_look_behind_bounds++ = PUT_OFFSET_L(range_param->upper);
			*emit_look_behind_bounds = PUT_OFFSET_R(range_param->upper);
		}
	}

	// For look ahead/behind, the length must be set to zero again
	if (look_only) {
		range_param->lower = 0;
		range_param->upper = 0;
	}

	zero_width = 0;

	/* Set a bit in Closed_Parens to let future calls to function 'back_ref'
	   know that we have closed this set of parentheses. */

    if (paren == PAREN && this_paren <= pContext.Closed_Parens.size()) {
        pContext.Closed_Parens[this_paren] = true;

		/* Determine if a parenthesized expression is modified by a quantifier
		   that can have zero width. */

        if (*pContext.Reg_Parse == '?' || *pContext.Reg_Parse == '*') {
			zero_width++;
        } else if (*pContext.Reg_Parse == '{' && pContext.Brace_Char == '{') {
            if (pContext.Reg_Parse[1] == ',' || pContext.Reg_Parse[1] == '}') {
				zero_width++;
            } else if (pContext.Reg_Parse[1] == '0') {
                int i = 2;

                while (pContext.Reg_Parse[i] == '0') {
					i++;
                }

                if (pContext.Reg_Parse[i] == ',') {
					zero_width++;
                }
			}
		}
	}

	/* If this set of parentheses is known to never match the empty string, set
	   a bit in Paren_Has_Width to let future calls to function back_ref know
	   that this set of parentheses has non-zero width.  This will allow star
	   (*) or question (?) quantifiers to be aplied to a back-reference that
	   refers to this set of parentheses. */

    if ((*flag_param & HAS_WIDTH) && paren == PAREN && !zero_width && this_paren <= pContext.Paren_Has_Width.size()) {

        pContext.Paren_Has_Width[this_paren] = true;
	}

    pContext.Is_Case_Insensitive = old_sensitive;
    pContext.Match_Newline = old_newline;

    return ret_val;
}

/*----------------------------------------------------------------------*
 * alternative
 *
 * Processes one alternative of an '|' operator.  Connects the NEXT
 * pointers of each regex atom together sequentialy.
 *----------------------------------------------------------------------*/

uint8_t *alternative(int *flag_param, len_range *range_param) {

	uint8_t *ret_val;
	uint8_t *chain;
	uint8_t *latest;
	int flags_local;
	len_range range_local;

	*flag_param = WORST;    // Tentatively.
	range_param->lower = 0; // Idem
	range_param->upper = 0;

	ret_val = emit_node(BRANCH);
	chain = nullptr;

	/* Loop until we hit the start of the next alternative, the end of this set
	   of alternatives (end of parentheses), or the end of the regex. */

    while (*pContext.Reg_Parse != '|' && *pContext.Reg_Parse != ')' && pContext.Reg_Parse != pContext.Reg_Parse_End) {
		latest = piece(&flags_local, &range_local);

		if(!latest)
            return nullptr; // Something went wrong.

		*flag_param |= flags_local & HAS_WIDTH;
		if (range_local.lower < 0) {
			// Not a fixed length
			range_param->lower = -1;
			range_param->upper = -1;
		} else if (range_param->lower >= 0) {
			range_param->lower += range_local.lower;
			range_param->upper += range_local.upper;
		}

		if (chain) { // Connect the regex atoms together sequentialy.
			tail(chain, latest);
		}

		chain = latest;
	}

	if(!chain) { // Loop ran zero times.
        emit_node(NOTHING);
	}

    return ret_val;
}

/*----------------------------------------------------------------------*
 * piece - something followed by possible '*', '+', '?', or "{m,n}"
 *
 * Note that the branching code sequences used for the general cases of
 * *, +. ?, and {m,n} are somewhat optimized:  they use the same
 * NOTHING node as both the endmarker for their branch list and the
 * body of the last branch. It might seem that this node could be
 * dispensed with entirely, but the endmarker role is not redundant.
 *----------------------------------------------------------------------*/

uint8_t *piece(int *flag_param, len_range *range_param) {

	uint8_t *ret_val;
	uint8_t *next;
	uint8_t op_code;
	unsigned long min_max[2] = {REG_ZERO, REG_INFINITY};
	int flags_local, i, brace_present = 0;
	bool lazy = false;
	bool comma_present = false;
	int digit_present[2] = {0, 0};
	len_range range_local;

	ret_val = atom(&flags_local, &range_local);

	if (ret_val == nullptr)
        return nullptr; // Something went wrong.

    op_code = *pContext.Reg_Parse;

	if (!IS_QUANTIFIER(op_code)) {
		*flag_param = flags_local;
		*range_param = range_local;
        return  ret_val;
	} else if (op_code == '{') { // {n,m} quantifier present
		brace_present++;
        ++pContext.Reg_Parse;

		/* This code will allow specifying a counting range in any of the
		   following forms:

		   {m,n}  between m and n.
		   {,n}   same as {0,n} or between 0 and infinity.
		   {m,}   same as {m,0} or between m and infinity.
		   {m}    same as {m,m} or exactly m.
		   {,}    same as {0,0} or between 0 and infinity or just '*'.
		   {}     same as {0,0} or between 0 and infinity or just '*'.

		   Note that specifying a max of zero, {m,0} is not allowed in the regex
		   itself, but it is implemented internally that way to support '*', '+',
		   and {min,} constructs and signals an unlimited number. */

		for (i = 0; i < 2; i++) {
			/* Look for digits of number and convert as we go.  The numeric maximum
			   value for max and min of 65,535 is due to using 2 bytes to store
			   each value in the compiled regex code. */

            while (safe_ctype<isdigit>(*pContext.Reg_Parse)) {
				// (6553 * 10 + 6) > 65535 (16 bit max)

                if ((min_max[i] == 6553UL && (*pContext.Reg_Parse - '0') <= 5) || (min_max[i] <= 6552UL)) {

                    min_max[i] = (min_max[i] * 10UL) + (unsigned long)(*pContext.Reg_Parse - '0');
                    ++pContext.Reg_Parse;

					digit_present[i]++;
				} else {
					if (i == 0) {
                        raise<regex_error>("min operand of {%lu%c,???} > 65535", min_max[0], *pContext.Reg_Parse);
					} else {
                        raise<regex_error>("max operand of {%lu,%lu%c} > 65535", min_max[0], min_max[1], *pContext.Reg_Parse);
					}
				}
			}

            if (!comma_present && *pContext.Reg_Parse == ',') {
				comma_present = true;
                ++pContext.Reg_Parse;
			}
		}

		/* A max of zero can not be specified directly in the regex since it would
		   signal a max of infinity.  This code specifically disallows '{0,0}',
		   '{,0}', and '{0}' which really means nothing to humans but would be
		   interpreted as '{0,infinity}' or '*' if we didn't make this check. */

		if (digit_present[0] && (min_max[0] == REG_ZERO) && !comma_present) {

            raise<regex_error>("{0} is an invalid range");
		} else if (digit_present[0] && (min_max[0] == REG_ZERO) && digit_present[1] && (min_max[1] == REG_ZERO)) {

            raise<regex_error>("{0,0} is an invalid range");
		} else if (digit_present[1] && (min_max[1] == REG_ZERO)) {
			if (digit_present[0]) {
                raise<regex_error>("{%lu,0} is an invalid range", min_max[0]);
			} else {
                raise<regex_error>("{,0} is an invalid range");
			}
		}

		if (!comma_present)
			min_max[1] = min_max[0]; // {x} means {x,x}

        if (*pContext.Reg_Parse != '}') {
            raise<regex_error>("{m,n} specification missing right '}'");

		} else if (min_max[1] != REG_INFINITY && min_max[0] > min_max[1]) {
			// Disallow a backward range.

            raise<regex_error>("{%lu,%lu} is an invalid range", min_max[0], min_max[1]);
		}
	}

    ++pContext.Reg_Parse;

	// Check for a minimal matching (non-greedy or "lazy") specification.

    if (*pContext.Reg_Parse == '?') {
		lazy = true;
        ++pContext.Reg_Parse;
	}

	// Avoid overhead of counting if possible

	if (op_code == '{') {
		if (min_max[0] == REG_ZERO && min_max[1] == REG_INFINITY) {
			op_code = '*';
		} else if (min_max[0] == REG_ONE && min_max[1] == REG_INFINITY) {
			op_code = '+';
		} else if (min_max[0] == REG_ZERO && min_max[1] == REG_ONE) {
			op_code = '?';
		} else if (min_max[0] == REG_ONE && min_max[1] == REG_ONE) {
			/* "x{1,1}" is the same as "x".  No need to pollute the compiled
			    regex with such nonsense. */

			*flag_param = flags_local;
			*range_param = range_local;
            return ret_val;
        } else if (pContext.Num_Braces > (int)UINT8_MAX) {
            raise<regex_error>("number of {m,n} constructs > %d", UINT8_MAX);
		}
	}

	if (op_code == '+')
		min_max[0] = REG_ONE;
	if (op_code == '?')
		min_max[1] = REG_ONE;

	/* It is dangerous to apply certain quantifiers to a possibly zero width
	   item. */

	if (!(flags_local & HAS_WIDTH)) {
		if (brace_present) {
            raise<regex_error>("{%lu,%lu} operand could be empty", min_max[0], min_max[1]);
		} else {
            raise<regex_error>("%c operand could be empty", op_code);
		}
	}

	*flag_param = (min_max[0] > REG_ZERO) ? (WORST | HAS_WIDTH) : WORST;
	if (range_local.lower >= 0) {
		if (min_max[1] != REG_INFINITY) {
			range_param->lower = range_local.lower * min_max[0];
			range_param->upper = range_local.upper * min_max[1];
		} else {
			range_param->lower = -1; // Not a fixed-size length
			range_param->upper = -1;
		}
	} else {
		range_param->lower = -1; // Not a fixed-size length
		range_param->upper = -1;
	}

	/*---------------------------------------------------------------------*
	 *          Symbol  Legend  For  Node  Structure  Diagrams
	 *---------------------------------------------------------------------*
	 * (...) = general grouped thing
	 * B     = (B)ranch,  K = bac(K),  N = (N)othing
	 * I     = (I)nitialize count,     C = Increment (C)ount
	 * T~m   = (T)est against mini(m)um- go to NEXT pointer if >= operand
	 * T~x   = (T)est against ma(x)imum- go to NEXT pointer if >= operand
	 * '~'   = NEXT pointer, \___| = forward pointer, |___/ = Backward pointer
	 *---------------------------------------------------------------------*/

	if (op_code == '*' && (flags_local & SIMPLE)) {
		insert((lazy ? LAZY_STAR : STAR), ret_val, 0UL, 0UL, 0);

	} else if (op_code == '+' && (flags_local & SIMPLE)) {
		insert(lazy ? LAZY_PLUS : PLUS, ret_val, 0UL, 0UL, 0);

	} else if (op_code == '?' && (flags_local & SIMPLE)) {
		insert(lazy ? LAZY_QUESTION : QUESTION, ret_val, 0UL, 0UL, 0);

	} else if (op_code == '{' && (flags_local & SIMPLE)) {
		insert(lazy ? LAZY_BRACE : BRACE, ret_val, min_max[0], min_max[1], 0);

	} else if ((op_code == '*' || op_code == '+') && lazy) {
		/*  Node structure for (x)*?    Node structure for (x)+? construct.
		 *  construct.                  (Same as (x)*? except for initial
		 *                              forward jump into parenthesis.)
		 *
		 *                                  ___6____
		 *   _______5_______               /________|______
		 *  | _4__        1_\             /| ____   |     _\
		 *  |/    |       / |\           / |/    |  |    / |\
		 *  B~ N~ B~ (...)~ K~ N~       N~ B~ N~ B~ (...)~ K~ N~
		 *      \  \___2_______|               \  \___________|
		 *       \_____3_______|                \_____________|
		 *
		 */

		tail(ret_val, emit_node(BACK));              // 1
		(void)insert(BRANCH, ret_val, 0UL, 0UL, 0);  // 2,4
		(void)insert(NOTHING, ret_val, 0UL, 0UL, 0); // 3

		next = emit_node(NOTHING); // 2,3

		offset_tail(ret_val, NODE_SIZE, next);        // 2
		tail(ret_val, next);                          // 3
		insert(BRANCH, ret_val, 0UL, 0UL, 0);         // 4,5
		tail(ret_val, ret_val + (2 * NODE_SIZE));     // 4
		offset_tail(ret_val, 3 * NODE_SIZE, ret_val); // 5

		if (op_code == '+') {
			insert(NOTHING, ret_val, 0UL, 0UL, 0);    // 6
			tail(ret_val, ret_val + (4 * NODE_SIZE)); // 6
		}
	} else if (op_code == '*') {
		/* Node structure for (x)* construct.
		 *      ____1_____
		 *     |          \
		 *     B~ (...)~ K~ B~ N~
		 *      \      \_|2 |\_|
		 *       \__3_______|  4
		 */

		insert(BRANCH, ret_val, 0UL, 0UL, 0);             // 1,3
		offset_tail(ret_val, NODE_SIZE, emit_node(BACK)); // 2
		offset_tail(ret_val, NODE_SIZE, ret_val);         // 1
		tail(ret_val, emit_node(BRANCH));                 // 3
		tail(ret_val, emit_node(NOTHING));                // 4
	} else if (op_code == '+') {
		/* Node structure for (x)+ construct.
		 *
		 *      ____2_____
		 *     |          \
		 *     (...)~ B~ K~ B~ N~
		 *          \_|\____|\_|
		 *          1     3    4
		 */

		next = emit_node(BRANCH); // 1

		tail(ret_val, next);               // 1
		tail(emit_node(BACK), ret_val);    // 2
		tail(next, emit_node(BRANCH));     // 3
		tail(ret_val, emit_node(NOTHING)); // 4
	} else if (op_code == '?' && lazy) {
		/* Node structure for (x)?? construct.
		 *       _4__        1_
		 *      /    |       / |
		 *     B~ N~ B~ (...)~ N~
		 *         \  \___2____|
		 *          \_____3____|
		 */

		(void)insert(BRANCH, ret_val, 0UL, 0UL, 0);  // 2,4
		(void)insert(NOTHING, ret_val, 0UL, 0UL, 0); // 3

		next = emit_node(NOTHING); // 1,2,3

		offset_tail(ret_val, 2 * NODE_SIZE, next);  // 1
		offset_tail(ret_val, NODE_SIZE, next);      // 2
		tail(ret_val, next);                        // 3
		insert(BRANCH, ret_val, 0UL, 0UL, 0);       // 4
		tail(ret_val, (ret_val + (2 * NODE_SIZE))); // 4

	} else if (op_code == '?') {
		/* Node structure for (x)? construct.
		 *       ___1____  _2
		 *      /        |/ |
		 *     B~ (...)~ B~ N~
		 *             \__3_|
		 */

		insert(BRANCH, ret_val, 0UL, 0UL, 0); // 1
		tail(ret_val, emit_node(BRANCH));     // 1

		next = emit_node(NOTHING); // 2,3

		tail(ret_val, next);                   // 2
		offset_tail(ret_val, NODE_SIZE, next); // 3
	} else if (op_code == '{' && min_max[0] == min_max[1]) {
		/* Node structure for (x){m}, (x){m}?, (x){m,m}, or (x){m,m}? constructs.
		 * Note that minimal and maximal matching mean the same thing when we
		 * specify the minimum and maximum to be the same value.
		 *       _______3_____
		 *      |    1_  _2   \
		 *      |    / |/ |    \
		 *   I~ (...)~ C~ T~m K~ N~
		 *    \_|          \_____|
		 *     5              4
		 */

        tail(ret_val, emit_special(INC_COUNT, 0UL, pContext.Num_Braces));         // 1
        tail(ret_val, emit_special(TEST_COUNT, min_max[0], pContext.Num_Braces)); // 2
		tail(emit_node(BACK), ret_val);                                  // 3
		tail(ret_val, emit_node(NOTHING));                               // 4

        next = insert(INIT_COUNT, ret_val, 0UL, 0UL, pContext.Num_Braces); // 5

		tail(ret_val, next); // 5

        pContext.Num_Braces++;
	} else if (op_code == '{' && lazy) {
		if (min_max[0] == REG_ZERO && min_max[1] != REG_INFINITY) {
			/* Node structure for (x){0,n}? or {,n}? construct.
			 *       _________3____________
			 *    8_| _4__        1_  _2   \
			 *    / |/    |       / |/ |    \
			 *   I~ B~ N~ B~ (...)~ C~ T~x K~ N~
			 *          \  \            \__7__|
			 *           \  \_________6_______|
			 *            \______5____________|
			 */

            tail(ret_val, emit_special(INC_COUNT, 0UL, pContext.Num_Braces)); // 1

            next = emit_special(TEST_COUNT, min_max[0], pContext.Num_Braces); // 2,7

			tail(ret_val, next);                                  // 2
            insert(BRANCH,  ret_val, 0UL, 0UL, pContext.Num_Braces);   // 4,6
            insert(NOTHING, ret_val, 0UL, 0UL, pContext.Num_Braces);   // 5
            insert(BRANCH,  ret_val, 0UL, 0UL, pContext.Num_Braces);   // 3,4,8
			tail(emit_node(BACK), ret_val);                       // 3
			tail(ret_val, ret_val + (2 * NODE_SIZE));             // 4

			next = emit_node(NOTHING); // 5,6,7

			offset_tail(ret_val, NODE_SIZE, next);     // 5
			offset_tail(ret_val, 2 * NODE_SIZE, next); // 6
			offset_tail(ret_val, 3 * NODE_SIZE, next); // 7

            next = insert(INIT_COUNT, ret_val, 0UL, 0UL, pContext.Num_Braces); // 8

			tail(ret_val, next); // 8

		} else if (min_max[0] > REG_ZERO && min_max[1] == REG_INFINITY) {
			/* Node structure for (x){m,}? construct.
			 *       ______8_________________
			 *      |         _______3_____  \
			 *      | _7__   |    1_  _2   \  \
			 *      |/    |  |    / |/ |    \  \
			 *   I~ B~ N~ B~ (...)~ C~ T~m K~ K~ N~
			 *    \_____\__\_|          \_4___|  |
			 *       9   \  \_________5__________|
			 *            \_______6______________|
			 */

            tail(ret_val, emit_special(INC_COUNT, 0UL, pContext.Num_Braces)); // 1

            next = emit_special(TEST_COUNT, min_max[0], pContext.Num_Braces); // 2,4

			tail(ret_val, next);                         // 2
			tail(emit_node(BACK), ret_val);              // 3
			tail(ret_val, emit_node(BACK));              // 4
            insert(BRANCH,  ret_val, 0UL, 0UL, 0);  // 5,7
            insert(NOTHING, ret_val, 0UL, 0UL, 0); // 6

			next = emit_node(NOTHING); // 5,6

			offset_tail(ret_val, NODE_SIZE, next);                   // 5
			tail(ret_val, next);                                     // 6
            insert(BRANCH, ret_val, 0UL, 0UL, 0);              // 7,8
			tail(ret_val, ret_val + (2 * NODE_SIZE));                // 7
			offset_tail(ret_val, 3 * NODE_SIZE, ret_val);            // 8
            insert(INIT_COUNT, ret_val, 0UL, 0UL, pContext.Num_Braces); // 9
			tail(ret_val, ret_val + INDEX_SIZE + (4 * NODE_SIZE));   // 9

		} else {
			/* Node structure for (x){m,n}? construct.
			 *       ______9_____________________
			 *      |         _____________3___  \
			 *      | __8_   |    1_  _2       \  \
			 *      |/    |  |    / |/ |        \  \
			 *   I~ B~ N~ B~ (...)~ C~ T~x T~m K~ K~ N~
			 *    \_____\__\_|          \   \__4__|  |
			 *      10   \  \            \_7_________|
			 *            \  \_________6_____________|
			 *             \_______5_________________|
			 */

            tail(ret_val, emit_special(INC_COUNT, 0UL, pContext.Num_Braces)); // 1

            next = emit_special(TEST_COUNT, min_max[1], pContext.Num_Braces); // 2,7

			tail(ret_val, next); // 2

            next = emit_special(TEST_COUNT, min_max[0], pContext.Num_Braces); // 4

			tail(emit_node(BACK), ret_val);              // 3
			tail(next, emit_node(BACK));                 // 4
			(void)insert(BRANCH, ret_val, 0UL, 0UL, 0);  // 6,8
			(void)insert(NOTHING, ret_val, 0UL, 0UL, 0); // 5
			(void)insert(BRANCH, ret_val, 0UL, 0UL, 0);  // 8,9

			next = emit_node(NOTHING); // 5,6,7

			offset_tail(ret_val, NODE_SIZE, next);                 // 5
			offset_tail(ret_val, 2 * NODE_SIZE, next);             // 6
			offset_tail(ret_val, 3 * NODE_SIZE, next);             // 7
			tail(ret_val, ret_val + (2 * NODE_SIZE));              // 8
			offset_tail(next, -NODE_SIZE, ret_val);                // 9
            insert(INIT_COUNT, ret_val, 0UL, 0UL, pContext.Num_Braces);     // 10
			tail(ret_val, ret_val + INDEX_SIZE + (4 * NODE_SIZE)); // 10
		}

        pContext.Num_Braces++;
	} else if (op_code == '{') {
		if (min_max[0] == REG_ZERO && min_max[1] != REG_INFINITY) {
			/* Node structure for (x){0,n} or (x){,n} construct.
			 *
			 *       ___3____________
			 *      |       1_  _2   \   5_
			 *      |       / |/ |    \  / |
			 *   I~ B~ (...)~ C~ T~x K~ B~ N~
			 *    \_|\            \_6___|__|
			 *    7   \________4________|
			 */

            tail(ret_val, emit_special(INC_COUNT, 0UL, pContext.Num_Braces)); // 1

            next = emit_special(TEST_COUNT, min_max[1], pContext.Num_Braces); // 2,6

			tail(ret_val, next);                        // 2
			(void)insert(BRANCH, ret_val, 0UL, 0UL, 0); // 3,4,7
			tail(emit_node(BACK), ret_val);             // 3

			next = emit_node(BRANCH); // 4,5

			tail(ret_val, next);                   // 4
			tail(next, emit_node(NOTHING));        // 5,6
			offset_tail(ret_val, NODE_SIZE, next); // 6

            next = insert(INIT_COUNT, ret_val, 0UL, 0UL, pContext.Num_Braces); // 7

			tail(ret_val, next); // 7

		} else if (min_max[0] > REG_ZERO && min_max[1] == REG_INFINITY) {
			/* Node structure for (x){m,} construct.
			 *       __________4________
			 *      |    __3__________  \
			 *     _|___|    1_  _2   \  \    _7
			 *    / | 8 |    / |/ |    \  \  / |
			 *   I~ B~  (...)~ C~ T~m K~ K~ B~ N~
			 *       \             \_5___|  |
			 *        \__________6__________|
			 */

            tail(ret_val, emit_special(INC_COUNT, 0UL, pContext.Num_Braces)); // 1

            next = emit_special(TEST_COUNT, min_max[0], pContext.Num_Braces); // 2

			tail(ret_val, next);                        // 2
			tail(emit_node(BACK), ret_val);             // 3
            insert(BRANCH, ret_val, 0UL, 0UL, 0); // 4,6

			next = emit_node(BACK); // 4

			tail(next, ret_val);                   // 4
			offset_tail(ret_val, NODE_SIZE, next); // 5
			tail(ret_val, emit_node(BRANCH));      // 6
			tail(ret_val, emit_node(NOTHING));     // 7

            insert(INIT_COUNT, ret_val, 0UL, 0UL, pContext.Num_Braces); // 8

			tail(ret_val, ret_val + INDEX_SIZE + (2 * NODE_SIZE)); // 8

		} else {
			/* Node structure for (x){m,n} construct.
			 *       _____6________________
			 *      |   _____________3___  \
			 *    9_|__|    1_  _2       \  \    _8
			 *    / |  |    / |/ |        \  \  / |
			 *   I~ B~ (...)~ C~ T~x T~m K~ K~ B~ N~
			 *       \            \   \__4__|  |  |
			 *        \            \_7_________|__|
			 *         \_________5_____________|
			 */

            tail(ret_val, emit_special(INC_COUNT, 0UL, pContext.Num_Braces)); // 1

            next = emit_special(TEST_COUNT, min_max[1], pContext.Num_Braces); // 2,4

			tail(ret_val, next); // 2

            next = emit_special(TEST_COUNT, min_max[0], pContext.Num_Braces); // 4

			tail(emit_node(BACK), ret_val);             // 3
			tail(next, emit_node(BACK));                // 4
            insert(BRANCH, ret_val, 0UL, 0UL, 0); // 5,6

			next = emit_node(BRANCH); // 5,8

			tail(ret_val, next);                    // 5
			offset_tail(next, -NODE_SIZE, ret_val); // 6

			next = emit_node(NOTHING); // 7,8

			offset_tail(ret_val, NODE_SIZE, next); // 7

			offset_tail(next, -NODE_SIZE, next);                     // 8
            insert(INIT_COUNT, ret_val, 0UL, 0UL, pContext.Num_Braces); // 9
			tail(ret_val, ret_val + INDEX_SIZE + (2 * NODE_SIZE));   // 9
		}

        pContext.Num_Braces++;
	} else {
		/* We get here if the IS_QUANTIFIER macro is not coordinated properly
		   with this function. */

        raise<regex_error>("internal error #2, 'piece'");
	}

    if (IS_QUANTIFIER(*pContext.Reg_Parse)) {
		if (op_code == '{') {
            raise<regex_error>("nested quantifiers, {m,n}%c", *pContext.Reg_Parse);
		} else {
            raise<regex_error>("nested quantifiers, %c%c", op_code, *pContext.Reg_Parse);
		}
	}

    return ret_val;
}

/*----------------------------------------------------------------------*
 * atom
 *
 * Process one regex item at the lowest level
 *
 * OPTIMIZATION:  Lumps a continuous sequence of ordinary characters
 * together so that it can turn them into a single EXACTLY node, which
 * is smaller to store and faster to run.
 *----------------------------------------------------------------------*/

uint8_t *atom(int *flag_param, len_range *range_param) {

	uint8_t *ret_val;
	uint8_t test;
	int flags_local;
	len_range range_local;

	*flag_param = WORST;    // Tentatively.
	range_param->lower = 0; // Idem
	range_param->upper = 0;

	/* Process any regex comments, e.g. '(?# match next token->)'.  The
	   terminating right parenthesis can not be escaped.  The comment stops at
	   the first right parenthesis encountered (or the end of the regex
	   string)... period.  Handles multiple sequential comments,
	   e.g. '(?# one)(?# two)...'  */

    while (*pContext.Reg_Parse == '(' && pContext.Reg_Parse[1] == '?' && *(pContext.Reg_Parse + 2) == '#') {

        pContext.Reg_Parse += 3;

        while (*pContext.Reg_Parse != ')' && pContext.Reg_Parse != pContext.Reg_Parse_End) {
            ++pContext.Reg_Parse;
		}

        if (*pContext.Reg_Parse == ')') {
            ++pContext.Reg_Parse;
		}

        if (*pContext.Reg_Parse == ')' || *pContext.Reg_Parse == '|' || pContext.Reg_Parse == pContext.Reg_Parse_End) {
			/* Hit end of regex string or end of parenthesized regex; have to
			 return "something" (i.e. a NOTHING node) to avoid generating an
			 error. */

			ret_val = emit_node(NOTHING);

            return ret_val;
		}
	}

    if(pContext.Reg_Parse == pContext.Reg_Parse_End) {
        // Supposed to be caught earlier.
        raise<regex_error>("internal error #3, 'atom'");
	}

    switch (*pContext.Reg_Parse++) {
	case '^':
		ret_val = emit_node(BOL);
		break;

	case '$':
		ret_val = emit_node(EOL);
		break;

	case '<':
		ret_val = emit_node(BOWORD);
		break;

	case '>':
		ret_val = emit_node(EOWORD);
		break;

	case '.':
        if (pContext.Match_Newline) {
			ret_val = emit_node(EVERY);
		} else {
			ret_val = emit_node(ANY);
		}

		*flag_param |= (HAS_WIDTH | SIMPLE);
		range_param->lower = 1;
		range_param->upper = 1;
		break;

	case '(':
        if (*pContext.Reg_Parse == '?') { // Special parenthetical expression
            ++pContext.Reg_Parse;
			range_local.lower = 0; // Make sure it is always used
			range_local.upper = 0;

            if (*pContext.Reg_Parse == ':') {
                ++pContext.Reg_Parse;
				ret_val = chunk(NO_CAPTURE, &flags_local, &range_local);
            } else if (*pContext.Reg_Parse == '=') {
                ++pContext.Reg_Parse;
				ret_val = chunk(POS_AHEAD_OPEN, &flags_local, &range_local);
            } else if (*pContext.Reg_Parse == '!') {
                ++pContext.Reg_Parse;
				ret_val = chunk(NEG_AHEAD_OPEN, &flags_local, &range_local);
            } else if (*pContext.Reg_Parse == 'i') {
                ++pContext.Reg_Parse;
				ret_val = chunk(INSENSITIVE, &flags_local, &range_local);
            } else if (*pContext.Reg_Parse == 'I') {
                ++pContext.Reg_Parse;
				ret_val = chunk(SENSITIVE, &flags_local, &range_local);
            } else if (*pContext.Reg_Parse == 'n') {
                ++pContext.Reg_Parse;
				ret_val = chunk(NEWLINE, &flags_local, &range_local);
            } else if (*pContext.Reg_Parse == 'N') {
                ++pContext.Reg_Parse;
				ret_val = chunk(NO_NEWLINE, &flags_local, &range_local);
            } else if (*pContext.Reg_Parse == '<') {
                ++pContext.Reg_Parse;
                if (*pContext.Reg_Parse == '=') {
                    ++pContext.Reg_Parse;
					ret_val = chunk(POS_BEHIND_OPEN, &flags_local, &range_local);
                } else if (*pContext.Reg_Parse == '!') {
                    ++pContext.Reg_Parse;
					ret_val = chunk(NEG_BEHIND_OPEN, &flags_local, &range_local);
				} else {
                    raise<regex_error>("invalid look-behind syntax, \"(?<%c...)\"", *pContext.Reg_Parse);
				}
			} else {
                raise<regex_error>("invalid grouping syntax, \"(?%c...)\"", *pContext.Reg_Parse);
			}
		} else { // Normal capturing parentheses
			ret_val = chunk(PAREN, &flags_local, &range_local);
		}

		if (ret_val == nullptr)
            return nullptr; // Something went wrong.

		// Add HAS_WIDTH flag if it was set by call to chunk.

		*flag_param |= flags_local & HAS_WIDTH;
		*range_param = range_local;

		break;

	case '|':
	case ')':
        raise<regex_error>("internal error #3, 'atom'"); // Supposed to be
	                                                    // caught earlier.
	case '?':
	case '+':
	case '*':
        raise<regex_error>("%c follows nothing", pContext.Reg_Parse[-1]);

	case '{':
        if (pContext.Enable_Counting_Quantifier) {
            raise<regex_error>("{m,n} follows nothing");
		} else {
			ret_val = emit_node(EXACTLY); // Treat braces as literals.
			emit_byte('{');
			emit_byte('\0');
			range_param->lower = 1;
			range_param->upper = 1;
		}

		break;

	case '[': {
		unsigned int second_value;
		unsigned int last_value;
		uint8_t last_emit = 0;

		// Handle characters that can only occur at the start of a class.

        if (*pContext.Reg_Parse == '^') { // Complement of range.
			ret_val = emit_node(ANY_BUT);
            ++pContext.Reg_Parse;

			/* All negated classes include newline unless escaped with
			   a "(?n)" switch. */

            if (!pContext.Match_Newline)
				emit_byte('\n');
		} else {
			ret_val = emit_node(ANY_OF);
		}

        if (*pContext.Reg_Parse == ']' || *pContext.Reg_Parse == '-') {
			/* If '-' or ']' is the first character in a class,
			   it is a literal character in the class. */

            last_emit = *pContext.Reg_Parse;
            emit_byte(*pContext.Reg_Parse);
            ++pContext.Reg_Parse;
		}

		// Handle the rest of the class characters.

        while (pContext.Reg_Parse != pContext.Reg_Parse_End && *pContext.Reg_Parse != ']') {
            if (*pContext.Reg_Parse == '-') { // Process a range, e.g [a-z].
                ++pContext.Reg_Parse;

                if (*pContext.Reg_Parse == ']' || pContext.Reg_Parse == pContext.Reg_Parse_End) {
					/* If '-' is the last character in a class it is a literal
					   character.  If 'Reg_Parse' points to the end of the
					   regex string, an error will be generated later. */

					emit_byte('-');
					last_emit = '-';
				} else {
					/* We must get the range starting character value from the
					   emitted code since it may have been an escaped
					   character.  'second_value' is set one larger than the
					   just emitted character value.  This is done since
					   'second_value' is used as the start value for the loop
					   that emits the values in the range.  Since we have
					   already emitted the first character of the class, we do
					   not want to emit it again. */

					second_value = ((unsigned int)last_emit) + 1;

                    if (*pContext.Reg_Parse == '\\') {
						/* Handle escaped characters within a class range.
						   Specifically disallow shortcut escapes as the end of
						   a class range.  To allow this would be ambiguous
						   since shortcut escapes represent a set of characters,
						   and it would not be clear which character of the
						   class should be treated as the "last" character. */

                        ++pContext.Reg_Parse;

                        if ((test = numeric_escape(*pContext.Reg_Parse, &pContext.Reg_Parse))) {
                            last_value = static_cast<unsigned int>(test);
                        } else if ((test = literal_escape(*pContext.Reg_Parse))) {
                            last_value = static_cast<unsigned int>(test);
                        } else if (shortcut_escape(*pContext.Reg_Parse, nullptr, CHECK_CLASS_ESCAPE)) {
                            raise<regex_error>("\\%c is not allowed as range operand", *pContext.Reg_Parse);
						} else {
                            raise<regex_error>("\\%c is an invalid char class escape sequence", *pContext.Reg_Parse);
						}
					} else {
                        last_value = U_CHAR_AT(pContext.Reg_Parse);
					}

                    if (pContext.Is_Case_Insensitive) {
                        second_value = static_cast<unsigned int>(safe_ctype<tolower>(second_value));
                        last_value   = static_cast<unsigned int>(safe_ctype<tolower>(last_value));
					}

					/* For case insensitive, something like [A-_] will
					   generate an error here since ranges are converted to
					   lower case. */

					if (second_value - 1 > last_value) {
                        raise<regex_error>("invalid [] range");
					}

					/* If only one character in range (e.g [a-a]) then this
					   loop is not run since the first character of any range
					   was emitted by the previous iteration of while loop. */

					for (; second_value <= last_value; second_value++) {
						emit_class_byte(second_value);
					}

                    last_emit = static_cast<uint8_t>(last_value);

                    ++pContext.Reg_Parse;

				} // End class character range code.
            } else if (*pContext.Reg_Parse == '\\') {
                ++pContext.Reg_Parse;

                if ((test = numeric_escape(*pContext.Reg_Parse, &pContext.Reg_Parse)) != '\0') {
					emit_class_byte(test);

					last_emit = test;
                } else if ((test = literal_escape(*pContext.Reg_Parse)) != '\0') {
					emit_byte(test);
					last_emit = test;
                } else if (shortcut_escape(*pContext.Reg_Parse, nullptr, CHECK_CLASS_ESCAPE)) {

                    if (pContext.Reg_Parse[1] == '-') {
						/* Specifically disallow shortcut escapes as the start
						   of a character class range (see comment above.) */

                        raise<regex_error>("\\%c not allowed as range operand", *pContext.Reg_Parse);
					} else {
						/* Emit the bytes that are part of the shortcut
						   escape sequence's range (e.g. \d = 0123456789) */

                        shortcut_escape(*pContext.Reg_Parse, nullptr, EMIT_CLASS_BYTES);
					}
				} else {
                    raise<regex_error>("\\%c is an invalid char class escape sequence", *pContext.Reg_Parse);
				}

                ++pContext.Reg_Parse;

				// End of class escaped sequence code
			} else {
                emit_class_byte(*pContext.Reg_Parse); // Ordinary class character.

                last_emit = *pContext.Reg_Parse;
                ++pContext.Reg_Parse;
			}
        } // End of while (Reg_Parse != Reg_Parse_End && *pContext.Reg_Parse != ']')

        if (*pContext.Reg_Parse != ']')
            raise<regex_error>("missing right ']'");

		emit_byte('\0');

		/* NOTE: it is impossible to specify an empty class.  This is
		   because [] would be interpreted as "begin character class"
		   followed by a literal ']' character and no "end character class"
		   delimiter (']').  Because of this, it is always safe to assume
		   that a class HAS_WIDTH. */

        ++pContext.Reg_Parse;
		*flag_param |= HAS_WIDTH | SIMPLE;
		range_param->lower = 1;
		range_param->upper = 1;
	}

	break; // End of character class code.

	case '\\':
        if ((ret_val = shortcut_escape(*pContext.Reg_Parse, flag_param, EMIT_NODE))) {

            ++pContext.Reg_Parse;
			range_param->lower = 1;
			range_param->upper = 1;
			break;

        } else if ((ret_val = back_ref(pContext.Reg_Parse, flag_param, EMIT_NODE))) {
			/* Can't make any assumptions about a back-reference as to SIMPLE
			   or HAS_WIDTH.  For example (^|<) is neither simple nor has
			   width.  So we don't flip bits in flag_param here. */

            ++pContext.Reg_Parse;
			// Back-references always have an unknown length
			range_param->lower = -1;
			range_param->upper = -1;
			break;
		}
		/* fallthrough */

	/* At this point it is apparent that the escaped character is not a
	   shortcut escape or back-reference.  Back up one character to allow
	   the default code to include it as an ordinary character. */

	/* Fall through to Default case to handle literal escapes and numeric
	   escapes. */

        /* fallthrough */
	default:
        --pContext.Reg_Parse; /* If we fell through from the above code, we are now
		                pointing at the back slash (\) character. */
		{
			const char *parse_save;
			int len = 0;

            if (pContext.Is_Case_Insensitive) {
				ret_val = emit_node(SIMILAR);
			} else {
				ret_val = emit_node(EXACTLY);
			}

			/* Loop until we find a meta character, shortcut escape, back
			   reference, or end of regex string. */

            for (; pContext.Reg_Parse != pContext.Reg_Parse_End && !strchr(pContext.Meta_Char, static_cast<int>(*pContext.Reg_Parse)); len++) {

				/* Save where we are in case we have to back
				   this character out. */

                parse_save = pContext.Reg_Parse;

                if (*pContext.Reg_Parse == '\\') {
                    ++pContext.Reg_Parse; // Point to escaped character

                    if ((test = numeric_escape(*pContext.Reg_Parse, &pContext.Reg_Parse))) {
                        if (pContext.Is_Case_Insensitive) {
							emit_byte(tolower(test));
						} else {
							emit_byte(test);
						}
                    } else if ((test = literal_escape(*pContext.Reg_Parse))) {
						emit_byte(test);
                    } else if (back_ref(pContext.Reg_Parse, nullptr, CHECK_ESCAPE)) {
						// Leave back reference for next 'atom' call

                        --pContext.Reg_Parse;
						break;
                    } else if (shortcut_escape(*pContext.Reg_Parse, nullptr, CHECK_ESCAPE)) {
						// Leave shortcut escape for next 'atom' call

                        --pContext.Reg_Parse;
						break;
					} else {
						/* None of the above calls generated an error message
						   so generate our own here. */

                        raise<regex_error>("\\%c is an invalid escape sequence", *pContext.Reg_Parse);

					}

                    ++pContext.Reg_Parse;
				} else {
					// Ordinary character

                    if (pContext.Is_Case_Insensitive) {
                        emit_byte(tolower(*pContext.Reg_Parse));
					} else {
                        emit_byte(*pContext.Reg_Parse);
					}

                    ++pContext.Reg_Parse;
				}

				/* If next regex token is a quantifier (?, +. *, or {m,n}) and
				   our EXACTLY node so far is more than one character, leave the
				   last character to be made into an EXACTLY node one character
				   wide for the multiplier to act on.  For example 'abcd* would
				   have an EXACTLY node with an 'abc' operand followed by a STAR
				   node followed by another EXACTLY node with a 'd' operand. */

                if (IS_QUANTIFIER(*pContext.Reg_Parse) && len > 0) {
                    pContext.Reg_Parse = parse_save; // Point to previous regex token.

                    if (pContext.Code_Emit_Ptr == &Compute_Size) {
                        pContext.Reg_Size--;
					} else {
                        pContext.Code_Emit_Ptr--; // Write over previously emitted byte.
					}

					break;
				}
			}

			if (len <= 0)
                raise<regex_error>("internal error #4, 'atom'");

			*flag_param |= HAS_WIDTH;

			if (len == 1)
				*flag_param |= SIMPLE;

			range_param->lower = len;
			range_param->upper = len;

			emit_byte('\0');
		}
    }

    return ret_val;
}

/*----------------------------------------------------------------------*
 * emit_node
 *
 * Emit (if appropriate) the op code for a regex node atom.
 *
 * The NEXT pointer is initialized to nullptr.
 *
 * Returns a pointer to the START of the emitted node.
 *----------------------------------------------------------------------*/
template <class T>
uint8_t *emit_node(T op_code) {

    uint8_t *ret_val = pContext.Code_Emit_Ptr; // Return address of start of node

    if (ret_val == &Compute_Size) {
        pContext.Reg_Size += NODE_SIZE;
	} else {
        uint8_t *ptr = ret_val;
        *ptr++ = static_cast<uint8_t>(op_code);
		*ptr++ = '\0'; // Null "NEXT" pointer.
		*ptr++ = '\0';

        pContext.Code_Emit_Ptr = ptr;
	}

	return ret_val;
}

/*----------------------------------------------------------------------*
 * emit_byte
 *
 * Emit (if appropriate) a byte of code (usually part of an operand.)
 *----------------------------------------------------------------------*/
template <class T>
void emit_byte(T ch) {

    if (pContext.Code_Emit_Ptr == &Compute_Size) {
        pContext.Reg_Size++;
	} else {
        *pContext.Code_Emit_Ptr++ = static_cast<uint8_t>(ch);
	}
}

/*----------------------------------------------------------------------*
 * emit_class_byte
 *
 * Emit (if appropriate) a byte of code (usually part of a character
 * class operand.)
 *----------------------------------------------------------------------*/
template <class T>
void emit_class_byte(T ch) {

    if (pContext.Code_Emit_Ptr == &Compute_Size) {
        pContext.Reg_Size++;

        if (pContext.Is_Case_Insensitive && safe_ctype<isalpha>(ch))
            pContext.Reg_Size++;
    } else if (pContext.Is_Case_Insensitive && safe_ctype<isalpha>(ch)) {
		/* For case insensitive character classes, emit both upper and lower case
		   versions of alphabetical characters. */

        *pContext.Code_Emit_Ptr++ = static_cast<uint8_t>(safe_ctype<tolower>(ch));
        *pContext.Code_Emit_Ptr++ = static_cast<uint8_t>(safe_ctype<toupper>(ch));
	} else {
        *pContext.Code_Emit_Ptr++ = static_cast<uint8_t>(ch);
	}
}

/*----------------------------------------------------------------------*
 * emit_special
 *
 * Emit nodes that need special processing.
 *----------------------------------------------------------------------*/

uint8_t *emit_special(uint8_t op_code, unsigned long test_val, size_t index) {

    uint8_t *ret_val = &Compute_Size;
	uint8_t *ptr;

    if (pContext.Code_Emit_Ptr == &Compute_Size) {
		switch (op_code) {
		case POS_BEHIND_OPEN:
		case NEG_BEHIND_OPEN:
            pContext.Reg_Size += LENGTH_SIZE; // Length of the look-behind match
            pContext.Reg_Size += NODE_SIZE;   // Make room for the node
			break;

		case TEST_COUNT:
            pContext.Reg_Size += NEXT_PTR_SIZE; // Make room for a test value.
			/* fallthrough */
		case INC_COUNT:
            pContext.Reg_Size += INDEX_SIZE; // Make room for an index value.
			/* fallthrough */
		default:
            pContext.Reg_Size += NODE_SIZE; // Make room for the node.
		}
	} else {
		ret_val = emit_node(op_code); // Return the address for start of node.
        ptr = pContext.Code_Emit_Ptr;

		if (op_code == INC_COUNT || op_code == TEST_COUNT) {
			*ptr++ = (uint8_t)index;

			if (op_code == TEST_COUNT) {
				*ptr++ = PUT_OFFSET_L(test_val);
				*ptr++ = PUT_OFFSET_R(test_val);
			}
		} else if (op_code == POS_BEHIND_OPEN || op_code == NEG_BEHIND_OPEN) {
			*ptr++ = PUT_OFFSET_L(test_val);
			*ptr++ = PUT_OFFSET_R(test_val);
			*ptr++ = PUT_OFFSET_L(test_val);
			*ptr++ = PUT_OFFSET_R(test_val);
		}

        pContext.Code_Emit_Ptr = ptr;
	}

    return ret_val;
}

/*----------------------------------------------------------------------*
 * insert
 *
 * Insert a node in front of already emitted node(s).  Means relocating
 * the operand.  Code_Emit_Ptr points one byte past the just emitted
 * node and operand.  The parameter 'insert_pos' points to the location
 * where the new node is to be inserted.
 *----------------------------------------------------------------------*/

uint8_t *insert(uint8_t op, uint8_t *insert_pos, long min, long max, size_t index) {

	uint8_t *src;
	uint8_t *dst;
	uint8_t *place;
	int insert_size = NODE_SIZE;

	if (op == BRACE || op == LAZY_BRACE) {
		// Make room for the min and max values.

		insert_size += (2 * NEXT_PTR_SIZE);
	} else if (op == INIT_COUNT) {
		// Make room for an index value .

		insert_size += INDEX_SIZE;
	}

    if (pContext.Code_Emit_Ptr == &Compute_Size) {
        pContext.Reg_Size += insert_size;
        return &Compute_Size;
	}

    src = pContext.Code_Emit_Ptr;
    pContext.Code_Emit_Ptr += insert_size;
    dst = pContext.Code_Emit_Ptr;

	// Relocate the existing emitted code to make room for the new node.

	while (src > insert_pos)
		*--dst = *--src;

	place = insert_pos; // Where operand used to be.
	*place++ = op;      // Inserted operand.
	*place++ = '\0';    // NEXT pointer for inserted operand.
	*place++ = '\0';

	if (op == BRACE || op == LAZY_BRACE) {
		*place++ = PUT_OFFSET_L(min);
		*place++ = PUT_OFFSET_R(min);

		*place++ = PUT_OFFSET_L(max);
		*place++ = PUT_OFFSET_R(max);
	} else if (op == INIT_COUNT) {
        *place++ = static_cast<uint8_t>(index);
	}

	return place; // Return a pointer to the start of the code moved.
}

/*----------------------------------------------------------------------*
 * tail - Set the next-pointer at the end of a node chain.
 *----------------------------------------------------------------------*/
void tail(uint8_t *search_from, uint8_t *point_to) {

	uint8_t *scan;
	uint8_t *next;

    if (search_from == &Compute_Size) {
		return;
    }

	// Find the last node in the chain (node with a null NEXT pointer)

	scan = search_from;

	for (;;) {
		next = next_ptr(scan);

        if (!next) {
			break;
        }

		scan = next;
	}

    long offset;
	if (GET_OP_CODE(scan) == BACK) {
        offset = scan - point_to;
	} else {
        offset = point_to - scan;
	}

	// Set NEXT pointer

	*(scan + 1) = PUT_OFFSET_L(offset);
	*(scan + 2) = PUT_OFFSET_R(offset);
}

/*--------------------------------------------------------------------*
 * offset_tail
 *
 * Perform a tail operation on (ptr + offset).
 *--------------------------------------------------------------------*/

void offset_tail(uint8_t *ptr, int offset, uint8_t *val) {

    if (ptr == &Compute_Size || ptr == nullptr)
		return;

	tail(ptr + offset, val);
}

/*--------------------------------------------------------------------*
 * branch_tail
 *
 * Perform a tail operation on (ptr + offset) but only if 'ptr' is a
 * BRANCH node.
 *--------------------------------------------------------------------*/

void branch_tail(uint8_t *ptr, int offset, uint8_t *val) {

    if (ptr == &Compute_Size || ptr == nullptr || GET_OP_CODE(ptr) != BRANCH) {
		return;
	}

	tail(ptr + offset, val);
}

/*--------------------------------------------------------------------*
 * shortcut_escape
 *
 * Implements convenient escape sequences that represent entire
 * character classes or special location assertions (similar to escapes
 * supported by Perl)
 *                                                  _
 *    \d     Digits                  [0-9]           |
 *    \D     NOT a digit             [^0-9]          | (Examples
 *    \l     Letters                 [a-zA-Z]        |  at left
 *    \L     NOT a Letter            [^a-zA-Z]       |    are
 *    \s     Whitespace              [ \t\n\r\f\v]   |    for
 *    \S     NOT Whitespace          [^ \t\n\r\f\v]  |     C
 *    \w     "Word" character        [a-zA-Z0-9_]    |   Locale)
 *    \W     NOT a "Word" character  [^a-zA-Z0-9_]  _|
 *
 *    \B     Matches any character that is NOT a word-delimiter
 *
 *    Codes for the "emit" parameter:
 *
 *    EMIT_NODE
 *       Emit a shortcut node.  Shortcut nodes have an implied set of
 *       class characters.  This helps keep the compiled regex string
 *       small.
 *
 *    EMIT_CLASS_BYTES
 *       Emit just the equivalent characters of the class.  This makes
 *       the escape usable from within a class, e.g. [a-fA-F\d].  Only
 *       \d, \D, \s, \S, \w, and \W can be used within a class.
 *
 *    CHECK_ESCAPE
 *       Only verify that this is a valid shortcut escape.
 *
 *    CHECK_CLASS_ESCAPE
 *       Same as CHECK_ESCAPE but only allows characters valid within
 *       a class.
 *
 *--------------------------------------------------------------------*/
template <class T>
uint8_t *shortcut_escape(T ch, int *flag_param, int emit) {

    const char *clazz = nullptr;
	static const char codes[] = "ByYdDlLsSwW";
    auto ret_val = reinterpret_cast<uint8_t *>(1); // Assume success.
	const char *valid_codes;

	if (emit == EMIT_CLASS_BYTES || emit == CHECK_CLASS_ESCAPE) {
		valid_codes = codes + 3; // \B, \y and \Y are not allowed in classes
	} else {
		valid_codes = codes;
	}

    if (!strchr(valid_codes, static_cast<int>(ch))) {
		return nullptr; // Not a valid shortcut escape sequence
	} else if (emit == CHECK_ESCAPE || emit == CHECK_CLASS_ESCAPE) {
		return ret_val; // Just checking if this is a valid shortcut escape.
	}

    switch (ch) {
	case 'd':
	case 'D':
		if (emit == EMIT_CLASS_BYTES) {
			clazz = ASCII_Digits;
		} else if (emit == EMIT_NODE) {
            ret_val = (safe_ctype<islower>(ch) ? emit_node(DIGIT) : emit_node(NOT_DIGIT));
		}

		break;

	case 'l':
	case 'L':
		if (emit == EMIT_CLASS_BYTES) {
            clazz = pContext.Letter_Char;
		} else if (emit == EMIT_NODE) {
            ret_val = (safe_ctype<islower>(ch) ? emit_node(LETTER) : emit_node(NOT_LETTER));
		}

		break;

	case 's':
	case 'S':
		if (emit == EMIT_CLASS_BYTES) {
            if (pContext.Match_Newline)
				emit_byte('\n');

            clazz = pContext.White_Space;
		} else if (emit == EMIT_NODE) {
            if (pContext.Match_Newline) {
                ret_val = (safe_ctype<islower>(ch) ? emit_node(SPACE_NL) : emit_node(NOT_SPACE_NL));
			} else {
                ret_val = (safe_ctype<islower>(ch) ? emit_node(SPACE) : emit_node(NOT_SPACE));
			}
		}

		break;

	case 'w':
	case 'W':
		if (emit == EMIT_CLASS_BYTES) {
            clazz = pContext.Word_Char;
		} else if (emit == EMIT_NODE) {
            ret_val = (safe_ctype<islower>(ch) ? emit_node(WORD_CHAR) : emit_node(NOT_WORD_CHAR));
		}

		break;

	/* Since the delimiter table is not available at regex compile time \B,
	   \Y and \Y can only generate a node.  At run time, the delimiter table
	   will be available for these nodes to use. */

	case 'y':

		if (emit == EMIT_NODE) {
			ret_val = emit_node(IS_DELIM);
		} else {
            raise<regex_error>("internal error #5 'shortcut_escape'");
		}

		break;

	case 'Y':

		if (emit == EMIT_NODE) {
			ret_val = emit_node(NOT_DELIM);
		} else {
            raise<regex_error>("internal error #6 'shortcut_escape'");
		}

		break;

	case 'B':

		if (emit == EMIT_NODE) {
			ret_val = emit_node(NOT_BOUNDARY);
		} else {
            raise<regex_error>("internal error #7 'shortcut_escape'");
		}

		break;

	default:
		/* We get here if there isn't a case for every character in
		   the string "codes" */

        raise<regex_error>("internal error #8 'shortcut_escape'");
	}

    if (emit == EMIT_NODE && ch != 'B') {
		*flag_param |= (HAS_WIDTH | SIMPLE);
	}

	if (clazz) {
		// Emit bytes within a character class operand.

		while (*clazz != '\0') {
			emit_byte(*clazz++);
		}
	}

	return ret_val;
}

/*--------------------------------------------------------------------*
 * numeric_escape
 *
 * Implements hex and octal numeric escape sequence syntax.
 *
 * Hexadecimal Escape: \x##    Max of two digits  Must have leading 'x'.
 * Octal Escape:       \0###   Max of three digits and not greater
 *                             than 377 octal.  Must have leading zero.
 *
 * Returns the actual character value or nullptr if not a valid hex or
 * octal escape.  raise<regex_error> is called if \x0, \x00, \0, \00, \000, or
 * \0000 is specified.
 *--------------------------------------------------------------------*/
template <class T>
char numeric_escape(T ch, const char **parse) {

	static const char digits[] = "fedcbaFEDCBA9876543210";

    static const unsigned int digit_val[] = {
        15, 14, 13, 12, 11, 10,            // Lower case Hex digits
        15, 14, 13, 12, 11, 10,            // Upper case Hex digits
        9,  8,  7,  6,  5,  4,  3, 2, 1, 0 // Decimal Digits
    };

	const char *scan;
	const char *pos_ptr;
	const char *digit_str;
	unsigned int value = 0;
	unsigned int radix = 8;
	int width = 3; // Can not be bigger than \0377
	int pos_delta = 14;
    int i;

    switch (ch) {
	case '0':
		digit_str = digits + pos_delta; // Only use Octal digits, i.e. 0-7.
		break;

	case 'x':
	case 'X':
		width = 2; // Can not be bigger than \0377
		radix = 16;
		pos_delta = 0;
		digit_str = digits; // Use all of the digit characters.

		break;

	default:
		return '\0'; // Not a numeric escape
	}

	scan = *parse;
	scan++; // Only change *parse on success.

	pos_ptr = strchr(digit_str, static_cast<int>(*scan));

	for (i = 0; pos_ptr != nullptr && (i < width); i++) {
        const long pos = (pos_ptr - digit_str) + pos_delta;
		value = (value * radix) + digit_val[pos];

		/* If this digit makes the value over 255, treat this digit as a literal
		   character instead of part of the numeric escape.  For example, \0777
		   will be processed as \077 (an 'M') and a literal '7' character, NOT
		   511 decimal which is > 255. */

		if (value > 255) {
			// Back out calculations for last digit processed.

			value -= digit_val[pos];
			value /= radix;

			break; /* Note that scan will not be incremented and still points to
			          the digit that caused overflow.  It will be decremented by
			          the "else" below to point to the last character that is
			          considered to be part of the octal escape. */
		}

		scan++;
		pos_ptr = strchr(digit_str, static_cast<int>(*scan));
	}

	// Handle the case of "\0" i.e. trying to specify a nullptr character.

	if (value == 0) {
        if (ch == '0') {
            raise<regex_error>("\\00 is an invalid octal escape");
		} else {
            raise<regex_error>("\\%c0 is an invalid hexadecimal escape", ch);
		}
	} else {
		// Point to the last character of the number on success.

		scan--;
		*parse = scan;
	}

    return static_cast<uint8_t>(value);
}

/*--------------------------------------------------------------------*
 * literal_escape
 *
 * Recognize escaped literal characters (prefixed with backslash),
 * and translate them into the corresponding character.
 *
 * Returns the proper character value or nullptr if not a valid literal
 * escape.
 *--------------------------------------------------------------------*/
template <class T>
char literal_escape(T ch) {

    static const uint8_t valid_escape[] = {
        'a', 'b', 'e', 'f', 'n', 'r', 't', 'v', '(', ')', '-', '[', ']', '<',
        '>', '{', '}', '.', '\\', '|', '^', '$', '*', '+', '?', '&', '\0'
    };

    static const uint8_t value[] = {
        '\a', '\b', 0x1B, // Escape character in ASCII character set.
        '\f', '\n', '\r', '\t', '\v', '(', ')', '-', '[', ']', '<', '>', '{',
        '}', '.', '\\', '|', '^', '$', '*', '+', '?', '&', '\0'
    };

    for (int i = 0; valid_escape[i] != '\0'; i++) {
        if (static_cast<uint8_t>(ch) == valid_escape[i]) {
            return static_cast<char>(value[i]);
        }
	}

	return '\0';
}

/*--------------------------------------------------------------------*
 * back_ref
 *
 * Process a request to match a previous parenthesized thing.
 * Parenthetical entities are numbered beginning at 1 by counting
 * opening parentheses from left to to right.  \0 would represent
 * whole match, but would confuse numeric_escape as an octal escape,
 * so it is forbidden.
 *
 * Constructs of the form \~1, \~2, etc. are cross-regex back
 * references and are used in syntax highlighting patterns to match
 * text previously matched by another regex. *** IMPLEMENT LATER ***
 *--------------------------------------------------------------------*/
uint8_t *back_ref(const char *c, int *flag_param, int emit) {

    int c_offset = 0;
    const int is_cross_regex = 0;

	uint8_t *ret_val;

	// Implement cross regex backreferences later.

	/* if (*c == (uint8_t) ('~')) {
	   c_offset++;
	   is_cross_regex++;
	} */

    int paren_no = (*(c + c_offset) - '0');

    if (!safe_ctype<isdigit>(*(c + c_offset)) || /* Only \1, \2, ... \9 are supported. */
            paren_no == 0) {                     /* Should be caught by numeric_escape. */

		return nullptr;
	}

	// Make sure parentheses for requested back-reference are complete.

    if (!is_cross_regex && !pContext.Closed_Parens[paren_no]) {
        raise<regex_error>("\\%d is an illegal back reference", paren_no);
	}

	if (emit == EMIT_NODE) {
		if (is_cross_regex) {
            ++pContext.Reg_Parse; /* Skip past the '~' in a cross regex back reference.
			                We only do this if we are emitting code. */

            if (pContext.Is_Case_Insensitive) {
				ret_val = emit_node(X_REGEX_BR_CI);
			} else {
				ret_val = emit_node(X_REGEX_BR);
			}
		} else {
            if (pContext.Is_Case_Insensitive) {
				ret_val = emit_node(BACK_REF_CI);
			} else {
				ret_val = emit_node(BACK_REF);
			}
		}

        emit_byte(static_cast<uint8_t>(paren_no));

        if (is_cross_regex || pContext.Paren_Has_Width[paren_no]) {
			*flag_param |= HAS_WIDTH;
		}
	} else if (emit == CHECK_ESCAPE) {
        ret_val = reinterpret_cast<uint8_t *>(1);
	} else {
		ret_val = nullptr;
	}

	return ret_val;
}

/*======================================================================*
 *  Regex execution related code
 *======================================================================*/

/**
 * @brief regexp::execute
 * @param string
 * @param reverse
 * @return
 */
bool regexp::execute(view::string_view string, bool reverse) {
	return execute(string, 0, reverse);
}

/**
 * @brief regexp::execute
 * @param string
 * @param offset
 * @param reverse
 * @return
 */
bool regexp::execute(view::string_view string, size_t offset, bool reverse) {
	return execute(string, offset, nullptr, reverse);
}

/**
 * @brief regexp::execute
 * @param string
 * @param offset
 * @param delimiters
 * @param reverse
 * @return
 */
bool regexp::execute(view::string_view string, size_t offset, const char *delimiters, bool reverse) {
	return execute(string, offset, string.size(), delimiters, reverse);
}

/**
 * @brief regexp::execute
 * @param string
 * @param offset
 * @param end_offset
 * @param delimiters
 * @param reverse
 * @return
 */
bool regexp::execute(view::string_view string, size_t offset, size_t end_offset, const char *delimiters, bool reverse) {
	return execute(
		string,
		offset,
		end_offset,
		(offset     == 0            ) ? '\0' : string[offset - 1],
		(end_offset == string.size()) ? '\0' : string[end_offset],
		delimiters,
		reverse);
}

/**
 * @brief regexp::execute
 * @param string
 * @param offset
 * @param end_offset
 * @param prev
 * @param succ
 * @param delimiters
 * @param reverse
 * @return
 */
bool regexp::execute(view::string_view string, size_t offset, size_t end_offset, char prev, char succ, const char *delimiters, bool reverse) {
	assert(offset <= end_offset);
	assert(end_offset <= string.size());
	return ExecRE(
		&string[offset],
		&string[end_offset],
		reverse,
		prev,
		succ,
		delimiters,
		&string[0],
		&string[string.size()]);
}

/*
 * ExecRE - match a 'regexp' structure against a string
 *
 * If 'end' is non-nullptr, matches may not BEGIN past end, but may extend past
 * it.  If reverse is true, 'end' must be specified, and searching begins at
 * 'end'.  "isbol" should be set to true if the beginning of the string is the
 * actual beginning of a line (since 'ExecRE' can't look backwards from the
 * beginning to find whether there was a newline before).  Likewise, "isbow"
 * asks whether the string is preceded by a word delimiter.  End of string is
 * always treated as a word and line boundary (there may be cases where it
 * shouldn't be, in which case, this should be changed).  "delimit" (if
 * non-null) specifies a null-terminated string of characters to be considered
 * word delimiters matching "<" and ">".  if "delimit" is nullptr, the default
 * delimiters (as set in SetREDefaultWordDelimiters) are used.
 * Look_behind_to indicates the position till where it is safe to
 * perform look-behind matches. If set, it should be smaller than or equal
 * to the start position of the search (pointed at by string). If it is nullptr,
 * it defaults to the start position.
 * Finally, match_to indicates the logical end of the string, till where
 * matches are allowed to extend. Note that look-ahead patterns may look
 * past that boundary. If match_to is set to nullptr, the terminating \0 is
 * assumed to correspond to the logical boundary. Match_to, if set, must be
 * larger than or equal to end, if set.
 */


/*

Notes: look_behind_to <= string <= end <= match_to

look_behind_to string            end           match_to
|              |                 |             |
+--------------+-----------------+-------------+
|  Look Behind | String Contents | Look Ahead  |
+--------------+-----------------+-------------+

*/
bool regexp::ExecRE(const char *string, const char *end, bool reverse, char prev_char, char succ_char, const char *delimiters, const char *look_behind_to, const char *match_to) {

    assert(string);

	// Check validity of program.
	if (U_CHAR_AT(this->program) != MAGIC) {
		reg_error("corrupted program");
		return false;
	}

	const char *str;
	bool ret_val = false;

	// If caller has supplied delimiters, make a delimiter table
    eContext.Current_Delimiters = delimiters ? makeDelimiterTable(delimiters) : Default_Delimiters;

	// Remember the logical end of the string.
    eContext.End_Of_String = match_to;

	if (!end && reverse) {
		for (end = string; !AT_END_OF_STRING(end); end++) {
		}
		succ_char = '\n';
	} else if(!end) {
		succ_char = '\n';
	}

	// Remember the beginning of the string for matching BOL
    eContext.Start_Of_String = string;
    eContext.Look_Behind_To  = (look_behind_to ? look_behind_to : string);

    eContext.Prev_Is_BOL   = (prev_char == '\n') || (prev_char == '\0');
    eContext.Succ_Is_EOL   = (succ_char == '\n') || (succ_char == '\0');
    eContext.Prev_Is_Delim = eContext.Current_Delimiters[static_cast<uint8_t>(prev_char)];
    eContext.Succ_Is_Delim = eContext.Current_Delimiters[static_cast<uint8_t>(succ_char)];

    pContext.Total_Paren = this->program[1];
    pContext.Num_Braces  = this->program[2];

	// Reset the recursion detection flag
    eContext.Recursion_Limit_Exceeded = false;

	// Allocate memory for {m,n} construct counting variables if need be.
    if (pContext.Num_Braces > 0) {
        eContext.BraceCounts = new uint32_t[pContext.Num_Braces];
	} else {
        eContext.BraceCounts = nullptr;
	}

	/* Initialize the first nine (9) capturing parentheses start and end
	   pointers to point to the start of the search string.  This is to prevent
	   crashes when later trying to reference captured parens that do not exist
	   in the compiled regex.  We only need to do the first nine since users
	   can only specify \1, \2, ... \9. */
    std::fill_n(this->startp.begin(), 9, string);
    std::fill_n(this->endp.begin(),   9, string);

	if (!reverse) { // Forward Search
		if (this->anchor) {
			// Search is anchored at BOL

			if (attempt(this, string)) {
				ret_val = true;
				goto SINGLE_RETURN;
			}

            for (str = string; !AT_END_OF_STRING(str) && str != end && !eContext.Recursion_Limit_Exceeded; str++) {

				if (*str == '\n') {
					if (attempt(this, str + 1)) {
						ret_val = true;
						break;
					}
				}
			}

			goto SINGLE_RETURN;

		} else if (this->match_start != '\0') {
			// We know what char match must start with.

            for (str = string; !AT_END_OF_STRING(str) && str != end && !eContext.Recursion_Limit_Exceeded; str++) {

                if (*str == static_cast<uint8_t>(this->match_start)) {
					if (attempt(this, str)) {
						ret_val = true;
						break;
					}
				}
			}

			goto SINGLE_RETURN;
		} else {
			// General case

            for (str = string; !AT_END_OF_STRING(str) && str != end && !eContext.Recursion_Limit_Exceeded; str++) {

				if (attempt(this, str)) {
					ret_val = true;
					break;
				}
			}

			// Beware of a single $ matching \0
            if (!eContext.Recursion_Limit_Exceeded && !ret_val && AT_END_OF_STRING(str) && str != end) {
				if (attempt(this, str)) {
					ret_val = true;
				}
			}

			goto SINGLE_RETURN;
		}
	} else { // Search reverse, same as forward, but loops run backward

		// Make sure that we don't start matching beyond the logical end
        if (eContext.End_Of_String != nullptr && end > eContext.End_Of_String) {
            end = eContext.End_Of_String;
		}

		if (this->anchor) {
			// Search is anchored at BOL

            for (str = (end - 1); str >= string && !eContext.Recursion_Limit_Exceeded; str--) {

				if (*str == '\n') {
					if (attempt(this, str + 1)) {
						ret_val = true;
						goto SINGLE_RETURN;
					}
				}
			}

            if (!eContext.Recursion_Limit_Exceeded && attempt(this, string)) {
				ret_val = true;
				goto SINGLE_RETURN;
			}

			goto SINGLE_RETURN;
		} else if (this->match_start != '\0') {
			// We know what char match must start with.

            for (str = end; str >= string && !eContext.Recursion_Limit_Exceeded; str--) {

                if (*str == static_cast<uint8_t>(this->match_start)) {
					if (attempt(this, str)) {
						ret_val = true;
						break;
					}
				}
			}

			goto SINGLE_RETURN;
		} else {
			// General case

            for (str = end; str >= string && !eContext.Recursion_Limit_Exceeded; str--) {

				if (attempt(this, str)) {
					ret_val = true;
					break;
				}
			}
		}
	}

SINGLE_RETURN:
    delete [] eContext.BraceCounts;

    if (eContext.Recursion_Limit_Exceeded) {
		return false;
	}

	return ret_val;
}

/*--------------------------------------------------------------------*
 * init_ansi_classes
 *
 * Generate character class sets using locale aware ANSI C functions.
 *
 *--------------------------------------------------------------------*/
bool init_ansi_classes() {

    static bool initialized  = false;


	if (!initialized) {
		initialized = true; // Only need to generate character sets once.

        constexpr int Underscore = '_';
        constexpr int Newline    = '\n';

        int word_count   = 0;
        int letter_count = 0;
        int space_count  = 0;

        for (int i = 1; i < UINT8_MAX; i++) {
            if (safe_ctype<isalnum>(i) || i == Underscore) {
                pContext.Word_Char[word_count++] = static_cast<char>(i);
			}

			if (safe_ctype<isalpha>(i)) {
                pContext.Letter_Char[letter_count++] = static_cast<char>(i);
			}

			/* Note: Whether or not newline is considered to be whitespace is
			   handled by switches within the original regex and is thus omitted
			   here. */

            if (safe_ctype<isspace>(i) && (i != Newline)) {
                pContext.White_Space[space_count++] = static_cast<char>(i);
			}

			/* Make sure arrays are big enough.  ("- 2" because of zero array
			   origin and we need to leave room for the nullptr terminator.) */

			if (word_count > (ALNUM_CHAR_SIZE - 2) || space_count > (WHITE_SPACE_SIZE - 2) || letter_count > (ALNUM_CHAR_SIZE - 2)) {

				reg_error("internal error #9 'init_ansi_classes'");
                return false;
			}
		}

        pContext.Word_Char[word_count]    = '\0';
        pContext.Letter_Char[word_count]  = '\0';
        pContext.White_Space[space_count] = '\0';
	}

    return true;
}

/*----------------------------------------------------------------------*
 * attempt - try match at specific point, returns: 0 failure, 1 success
 *----------------------------------------------------------------------*/
static bool attempt(regexp *prog, const char *string) {

	int branch_index = 0; // Must be set to zero !

    eContext.Reg_Input     = string;
    eContext.Start_Ptr_Ptr = prog->startp.begin();
    eContext.End_Ptr_Ptr   = prog->endp.begin();

	// Reset the recursion counter.
    eContext.Recursion_Count = 0;

	// Overhead due to capturing parentheses.
    eContext.Extent_Ptr_BW = string;
    eContext.Extent_Ptr_FW = nullptr;

    std::fill_n(prog->startp.begin(), pContext.Total_Paren + 1, nullptr);
    std::fill_n(prog->endp.begin(),   pContext.Total_Paren + 1, nullptr);

	if (match((prog->program + REGEX_START_OFFSET), &branch_index)) {
		prog->startp[0]  = string;
        prog->endp[0]    = eContext.Reg_Input;     // <-- One char AFTER
        prog->extentpBW  = eContext.Extent_Ptr_BW; //     matched string!
        prog->extentpFW  = eContext.Extent_Ptr_FW;
		prog->top_branch = branch_index;

        return true;
    }

    return false;
}

/*----------------------------------------------------------------------*
 * match - main matching routine
 *
 * Conceptually the strategy is simple: check to see whether the
 * current node matches, call self recursively to see whether the rest
 * matches, and then act accordingly.  In practice we make some effort
 * to avoid recursion, in particular by going through "ordinary" nodes
 * (that don't need to know whether the rest of the match failed) by a
 * loop instead of by recursion.  Returns 0 failure, 1 success.
 *----------------------------------------------------------------------*/
#define MATCH_RETURN(X)    \
    do {                   \
        --eContext.Recursion_Count; \
        return (X);        \
    } while(0)

#define CHECK_RECURSION_LIMIT()       \
    do {                              \
        if (eContext.Recursion_Limit_Exceeded) \
            MATCH_RETURN(0);          \
    } while(0)

static int match(uint8_t *prog, int *branch_index_param) {

	uint8_t *scan; // Current node.
	uint8_t *next;          // Next node.
	int next_ptr_offset; // Used by the NEXT_PTR () macro

    if (++eContext.Recursion_Count > REGEX_RECURSION_LIMIT) {
        if (!eContext.Recursion_Limit_Exceeded) // Prevent duplicate errors
			reg_error("recursion limit exceeded, please respecify expression");
        eContext.Recursion_Limit_Exceeded = true;
		MATCH_RETURN(0);
	}

	scan = prog;

	while (scan) {
		NEXT_PTR(scan, next);

		switch (GET_OP_CODE(scan)) {
		case BRANCH: {
			const char *save;

			if (GET_OP_CODE(next) != BRANCH) { // No choice.
				next = OPERAND(scan);          // Avoid recursion.
			} else {
                int branch_index_local = 0;

				do {
                    save = eContext.Reg_Input;

					if (match(OPERAND(scan), nullptr)) {
						if (branch_index_param)
							*branch_index_param = branch_index_local;
						MATCH_RETURN(1);
					}

                    CHECK_RECURSION_LIMIT();

					++branch_index_local;

                    eContext.Reg_Input = save; // Backtrack.
					NEXT_PTR(scan, scan);
				} while (scan != nullptr && GET_OP_CODE(scan) == BRANCH);

				MATCH_RETURN(0); // NOT REACHED
			}
		}

		break;

		case EXACTLY: {
			uint8_t *opnd = OPERAND(scan);

			// Inline the first character, for speed.
            if (*opnd != *eContext.Reg_Input) {
				MATCH_RETURN(0);
			}

            const auto str = reinterpret_cast<const char *>(opnd);
            const size_t len = strlen(str);

            if (eContext.End_Of_String != nullptr && eContext.Reg_Input + len > eContext.End_Of_String) {
				MATCH_RETURN(0);
			}

            if (len > 1 && strncmp(str, eContext.Reg_Input, len) != 0) {
				MATCH_RETURN(0);
			}

            eContext.Reg_Input += len;
		}

		break;

		case SIMILAR: {
			uint8_t test;
            uint8_t *opnd = OPERAND(scan);

			/* Note: the SIMILAR operand was converted to lower case during
			   regex compile. */

			while ((test = *opnd++) != '\0') {
                if (AT_END_OF_STRING(eContext.Reg_Input) || tolower(*eContext.Reg_Input++) != test) {
					MATCH_RETURN(0);
				}
			}
		}

		break;

		case BOL: // '^' (beginning of line anchor)
            if (eContext.Reg_Input == eContext.Start_Of_String) {
                if (eContext.Prev_Is_BOL)
					break;
            } else if (static_cast<int>(*(eContext.Reg_Input - 1)) == '\n') {
				break;
			}

			MATCH_RETURN(0);

		case EOL: // '$' anchor matches end of line and end of string
            if (*eContext.Reg_Input == '\n' || (AT_END_OF_STRING(eContext.Reg_Input) && eContext.Succ_Is_EOL)) {
				break;
			}

			MATCH_RETURN(0);

		case BOWORD: // '<' (beginning of word anchor)
			         /* Check to see if the current character is not a delimiter
			            and the preceding character is. */
			{
				int prev_is_delim;
                if (eContext.Reg_Input == eContext.Start_Of_String) {
                    prev_is_delim = eContext.Prev_Is_Delim;
				} else {
                    prev_is_delim = isDelimiter(*(eContext.Reg_Input - 1));
				}
				if (prev_is_delim) {
					int current_is_delim;
                    if (AT_END_OF_STRING(eContext.Reg_Input)) {
                        current_is_delim = eContext.Succ_Is_Delim;
					} else {
                        current_is_delim = isDelimiter(*eContext.Reg_Input);
					}
					if (!current_is_delim)
						break;
				}
			}

			MATCH_RETURN(0);

		case EOWORD: // '>' (end of word anchor)
			         /* Check to see if the current character is a delimiter
			        and the preceding character is not. */
			{
				int prev_is_delim;
                if (eContext.Reg_Input == eContext.Start_Of_String) {
                    prev_is_delim = eContext.Prev_Is_Delim;
				} else {
                    prev_is_delim = isDelimiter(*(eContext.Reg_Input - 1));
				}
				if (!prev_is_delim) {
					int current_is_delim;
                    if (AT_END_OF_STRING(eContext.Reg_Input)) {
                        current_is_delim = eContext.Succ_Is_Delim;
					} else {
                        current_is_delim = isDelimiter(*eContext.Reg_Input);
					}
					if (current_is_delim)
						break;
				}
			}

			MATCH_RETURN(0);

		case NOT_BOUNDARY: // \B (NOT a word boundary)
		{
			int prev_is_delim;
			int current_is_delim;
            if (eContext.Reg_Input == eContext.Start_Of_String) {
                prev_is_delim = eContext.Prev_Is_Delim;
			} else {
                prev_is_delim = isDelimiter(*(eContext.Reg_Input - 1));
			}
            if (AT_END_OF_STRING(eContext.Reg_Input)) {
                current_is_delim = eContext.Succ_Is_Delim;
			} else {
                current_is_delim = isDelimiter(*eContext.Reg_Input);
			}
			if (!(prev_is_delim ^ current_is_delim))
				break;
		}

			MATCH_RETURN(0);

		case IS_DELIM: // \y (A word delimiter character.)
            if (isDelimiter(*eContext.Reg_Input) && !AT_END_OF_STRING(eContext.Reg_Input)) {
                eContext.Reg_Input++;
				break;
			}

			MATCH_RETURN(0);

		case NOT_DELIM: // \Y (NOT a word delimiter character.)
            if (!isDelimiter(*eContext.Reg_Input) && !AT_END_OF_STRING(eContext.Reg_Input)) {
                eContext.Reg_Input++;
				break;
			}

			MATCH_RETURN(0);

		case WORD_CHAR: // \w (word character; alpha-numeric or underscore)
            if ((safe_ctype<isalnum>(*eContext.Reg_Input) || *eContext.Reg_Input == '_') && !AT_END_OF_STRING(eContext.Reg_Input)) {
                eContext.Reg_Input++;
				break;
			}

			MATCH_RETURN(0);

		case NOT_WORD_CHAR: // \W (NOT a word character)
            if (safe_ctype<isalnum>(*eContext.Reg_Input) || *eContext.Reg_Input == '_' || *eContext.Reg_Input == '\n' || AT_END_OF_STRING(eContext.Reg_Input))
				MATCH_RETURN(0);

            eContext.Reg_Input++;
			break;

		case ANY: // '.' (matches any character EXCEPT newline)
            if (AT_END_OF_STRING(eContext.Reg_Input) || *eContext.Reg_Input == '\n')
				MATCH_RETURN(0);

            eContext.Reg_Input++;
			break;

		case EVERY: // '.' (matches any character INCLUDING newline)
            if (AT_END_OF_STRING(eContext.Reg_Input))
				MATCH_RETURN(0);

            eContext.Reg_Input++;
			break;

		case DIGIT: // \d, same as [0123456789]
            if (!safe_ctype<isdigit>(*eContext.Reg_Input) || AT_END_OF_STRING(eContext.Reg_Input))
				MATCH_RETURN(0);

            eContext.Reg_Input++;
			break;

		case NOT_DIGIT: // \D, same as [^0123456789]
            if (safe_ctype<isdigit>(*eContext.Reg_Input) || *eContext.Reg_Input == '\n' || AT_END_OF_STRING(eContext.Reg_Input))
				MATCH_RETURN(0);

            eContext.Reg_Input++;
			break;

		case LETTER: // \l, same as [a-zA-Z]
            if (!safe_ctype<isalpha>(*eContext.Reg_Input) || AT_END_OF_STRING(eContext.Reg_Input))
				MATCH_RETURN(0);

            eContext.Reg_Input++;
			break;

		case NOT_LETTER: // \L, same as [^0123456789]
            if (safe_ctype<isalpha>(*eContext.Reg_Input) || *eContext.Reg_Input == '\n' || AT_END_OF_STRING(eContext.Reg_Input))
				MATCH_RETURN(0);

            eContext.Reg_Input++;
			break;

		case SPACE: // \s, same as [ \t\r\f\v]
            if (!safe_ctype<isspace>(*eContext.Reg_Input) || *eContext.Reg_Input == '\n' || AT_END_OF_STRING(eContext.Reg_Input))
				MATCH_RETURN(0);

            eContext.Reg_Input++;
			break;

		case SPACE_NL: // \s, same as [\n \t\r\f\v]
            if (!safe_ctype<isspace>(*eContext.Reg_Input) || AT_END_OF_STRING(eContext.Reg_Input))
				MATCH_RETURN(0);

            eContext.Reg_Input++;
			break;

		case NOT_SPACE: // \S, same as [^\n \t\r\f\v]
            if (safe_ctype<isspace>(*eContext.Reg_Input) || AT_END_OF_STRING(eContext.Reg_Input))
				MATCH_RETURN(0);

            eContext.Reg_Input++;
			break;

		case NOT_SPACE_NL: // \S, same as [^ \t\r\f\v]
            if ((safe_ctype<isspace>(*eContext.Reg_Input) && *eContext.Reg_Input != '\n') || AT_END_OF_STRING(eContext.Reg_Input))
				MATCH_RETURN(0);

            eContext.Reg_Input++;
			break;

		case ANY_OF: // [...] character class.
            if (AT_END_OF_STRING(eContext.Reg_Input))
				MATCH_RETURN(0); /* Needed because strchr ()
				                    considers \0 as a member
				                    of the character set. */

            if (strchr(reinterpret_cast<char *>(OPERAND(scan)), *eContext.Reg_Input) == nullptr) {
				MATCH_RETURN(0);
			}

            eContext.Reg_Input++;
			break;

		case ANY_BUT: /* [^...] Negated character class-- does NOT normally
		              match newline (\n added usually to operand at compile
		              time.) */

            if (AT_END_OF_STRING(eContext.Reg_Input))
				MATCH_RETURN(0); // See comment for ANY_OF.

            if (strchr(reinterpret_cast<char *>(OPERAND(scan)), *eContext.Reg_Input) != nullptr) {
				MATCH_RETURN(0);
			}

            eContext.Reg_Input++;
			break;

		case NOTHING:
		case BACK:
			break;

		case STAR:
		case PLUS:
		case QUESTION:
		case BRACE:

		case LAZY_STAR:
		case LAZY_PLUS:
		case LAZY_QUESTION:
		case LAZY_BRACE: {
			unsigned long num_matched = REG_ZERO;
			unsigned long min = ULONG_MAX;
			unsigned long max = REG_ZERO;
			const char *save;
			uint8_t next_char;
			uint8_t *next_op;
			bool lazy = false;

			/* Lookahead (when possible) to avoid useless match attempts
			   when we know what character comes next. */

			if (GET_OP_CODE(next) == EXACTLY) {
				next_char = *OPERAND(next);
			} else {
				next_char = '\0'; // i.e. Don't know what next character is.
			}

			next_op = OPERAND(scan);

			switch (GET_OP_CODE(scan)) {
			case LAZY_STAR:
				lazy = true;
				/* fallthrough */
            case STAR:
				min = REG_ZERO;
				max = ULONG_MAX;
				break;

			case LAZY_PLUS:
				lazy = true;
				/* fallthrough */
			case PLUS:
				min = REG_ONE;
				max = ULONG_MAX;
				break;

			case LAZY_QUESTION:
				lazy = true;
				/* fallthrough */
			case QUESTION:
				min = REG_ZERO;
				max = REG_ONE;
				break;

			case LAZY_BRACE:
				lazy = true;
				/* fallthrough */
			case BRACE:
				min = static_cast<unsigned long>(GET_OFFSET(scan + NEXT_PTR_SIZE));

				max = static_cast<unsigned long>(GET_OFFSET(scan + (2 * NEXT_PTR_SIZE)));

				if (max <= REG_INFINITY) {
					max = ULONG_MAX;
				}

				next_op = OPERAND(scan + (2 * NEXT_PTR_SIZE));
			}

            save = eContext.Reg_Input;

			if (lazy) {
				if (min > REG_ZERO)
                    num_matched = greedy(next_op, min);
			} else {
				num_matched = greedy(next_op, max);
			}

			while (min <= num_matched && num_matched <= max) {
                if (next_char == '\0' || next_char == *eContext.Reg_Input) {
					if (match(next, nullptr))
						MATCH_RETURN(1);

                    CHECK_RECURSION_LIMIT();
				}

				// Couldn't or didn't match.

				if (lazy) {
					if (!greedy(next_op, 1))
						MATCH_RETURN(0);

					num_matched++; // Inch forward.
				} else if (num_matched > REG_ZERO) {
					num_matched--; // Back up.
				} else if (min == REG_ZERO && num_matched == REG_ZERO) {
					break;
				}

                eContext.Reg_Input = save + num_matched;
			}

			MATCH_RETURN(0);
		}

		break;

		case END:
            if (eContext.Extent_Ptr_FW == nullptr || (eContext.Reg_Input - eContext.Extent_Ptr_FW) > 0) {
                eContext.Extent_Ptr_FW = eContext.Reg_Input;
			}

			MATCH_RETURN(1); // Success!
			break;

		case INIT_COUNT:
            eContext.BraceCounts[*OPERAND(scan)] = REG_ZERO;
			break;

		case INC_COUNT:
            eContext.BraceCounts[*OPERAND(scan)]++;
			break;

		case TEST_COUNT:
            if (eContext.BraceCounts[*OPERAND(scan)] < static_cast<unsigned long>(GET_OFFSET(scan + NEXT_PTR_SIZE + INDEX_SIZE))) {

				next = scan + NODE_SIZE + INDEX_SIZE + NEXT_PTR_SIZE;
			}

			break;

		case BACK_REF:
		case BACK_REF_CI:
			// case X_REGEX_BR:
			// case X_REGEX_BR_CI: *** IMPLEMENT LATER
			{
				const char *captured;
				const char *finish;
				int paren_no;

                paren_no = static_cast<int>(*OPERAND(scan));

				/* if (GET_OP_CODE (scan) == X_REGEX_BR || GET_OP_CODE (scan) == X_REGEX_BR_CI) {

                   if (eContext.Cross_Regex_Backref == nullptr) MATCH_RETURN (0);

                   captured = eContext.Cross_Regex_Backref->startp [paren_no];
                   finish   = eContext.Cross_Regex_Backref->endp   [paren_no];
				} else { */
                captured = eContext.Back_Ref_Start[paren_no];
                finish = eContext.Back_Ref_End[paren_no];
				// }

				if ((captured != nullptr) && (finish != nullptr)) {
					if (captured > finish)
						MATCH_RETURN(0);

					if (GET_OP_CODE(scan) == BACK_REF_CI /* ||
                      GET_OP_CODE (scan) == X_REGEX_BR_CI*/) {

						while (captured < finish) {
                            if (AT_END_OF_STRING(eContext.Reg_Input) || tolower(*captured++) != tolower(*eContext.Reg_Input++)) {
								MATCH_RETURN(0);
							}
						}
					} else {
						while (captured < finish) {
                            if (AT_END_OF_STRING(eContext.Reg_Input) || *captured++ != *eContext.Reg_Input++)
								MATCH_RETURN(0);
						}
					}

					break;
				} else {
					MATCH_RETURN(0);
				}
			}

		case POS_AHEAD_OPEN:
		case NEG_AHEAD_OPEN: {
			const char *save;
			const char *saved_end;
			int answer;

            save = eContext.Reg_Input;

			/* Temporarily ignore the logical end of the string, to allow
			   lookahead past the end. */
            saved_end = eContext.End_Of_String;
            eContext.End_Of_String = nullptr;

			answer = match(next, nullptr); // Does the look-ahead regex match?

            CHECK_RECURSION_LIMIT();

			if ((GET_OP_CODE(scan) == POS_AHEAD_OPEN) ? answer : !answer) {
				/* Remember the last (most to the right) character position
				   that we consume in the input for a successful match.  This
				   is info that may be needed should an attempt be made to
				   match the exact same text at the exact same place.  Since
				   look-aheads backtrack, a regex with a trailing look-ahead
				   may need more text than it matches to accomplish a
				   re-match. */

                if (eContext.Extent_Ptr_FW == nullptr || (eContext.Reg_Input - eContext.Extent_Ptr_FW) > 0) {
                    eContext.Extent_Ptr_FW = eContext.Reg_Input;
				}

                eContext.Reg_Input = save;          // Backtrack to look-ahead start.
                eContext.End_Of_String = saved_end; // Restore logical end.

				/* Jump to the node just after the (?=...) or (?!...)
				   Construct. */

				next = next_ptr(OPERAND(scan)); // Skip 1st branch
				// Skip the chain of branches inside the look-ahead
				while (GET_OP_CODE(next) == BRANCH)
					next = next_ptr(next);
				next = next_ptr(next); // Skip the LOOK_AHEAD_CLOSE
			} else {
                eContext.Reg_Input = save;          // Backtrack to look-ahead start.
                eContext.End_Of_String = saved_end; // Restore logical end.

				MATCH_RETURN(0);
			}
		}

		break;

		case POS_BEHIND_OPEN:
		case NEG_BEHIND_OPEN: {
			const char *save;
			int offset;
			int upper;
			int lower;
			int found = 0;
			const char *saved_end;

            save = eContext.Reg_Input;
            saved_end = eContext.End_Of_String;

			/* Prevent overshoot (greedy matching could end past the
			   current position) by tightening the matching boundary.
			   Lookahead inside lookbehind can still cross that boundary. */
            eContext.End_Of_String = eContext.Reg_Input;

			lower = GET_LOWER(scan);
			upper = GET_UPPER(scan);

			/* Start with the shortest match first. This is the most
			   efficient direction in general.
			   Note! Negative look behind is _very_ tricky when the length
			   is not constant: we have to make sure the expression doesn't
			   match for _any_ of the starting positions. */
			for (offset = lower; offset <= upper; ++offset) {
                eContext.Reg_Input = save - offset;

                if (eContext.Reg_Input < eContext.Look_Behind_To) {
					// No need to look any further
					break;
				}

                int answer = match(next, nullptr); // Does the look-behind regex match?

                CHECK_RECURSION_LIMIT();

				/* The match must have ended at the current position;
				   otherwise it is invalid */
                if (answer && eContext.Reg_Input == save) {
					// It matched, exactly far enough
					found = 1;

					/* Remember the last (most to the left) character position
					   that we consume in the input for a successful match.
					   This is info that may be needed should an attempt be
					   made to match the exact same text at the exact same
					   place. Since look-behind backtracks, a regex with a
					   leading look-behind may need more text than it matches
					   to accomplish a re-match. */

                    if (eContext.Extent_Ptr_BW == nullptr || (eContext.Extent_Ptr_BW - (save - offset)) > 0) {
                        eContext.Extent_Ptr_BW = save - offset;
					}

					break;
				}
			}

			// Always restore the position and the logical string end.
            eContext.Reg_Input = save;
            eContext.End_Of_String = saved_end;

			if ((GET_OP_CODE(scan) == POS_BEHIND_OPEN) ? found : !found) {
				/* The look-behind matches, so we must jump to the next
				   node. The look-behind node is followed by a chain of
				   branches (contents of the look-behind expression), and
				   terminated by a look-behind-close node. */
				next = next_ptr(OPERAND(scan) + LENGTH_SIZE); // 1st branch
				// Skip the chained branches inside the look-ahead
				while (GET_OP_CODE(next) == BRANCH)
					next = next_ptr(next);
				next = next_ptr(next); // Skip LOOK_BEHIND_CLOSE
			} else {
				// Not a match
				MATCH_RETURN(0);
			}
		} break;

		case LOOK_AHEAD_CLOSE:
		case LOOK_BEHIND_CLOSE:
			MATCH_RETURN(1); /* We have reached the end of the look-ahead or
			          look-behind which implies that we matched it,
			  so return TRUE. */
		default:
			if ((GET_OP_CODE(scan) > OPEN) && (GET_OP_CODE(scan) < OPEN + NSUBEXP)) {

				int no;
				const char *save;

				no = GET_OP_CODE(scan) - OPEN;
                save = eContext.Reg_Input;

				if (no < 10) {
                    eContext.Back_Ref_Start[no] = save;
                    eContext.Back_Ref_End[no] = nullptr;
				}

				if (match(next, nullptr)) {
					/* Do not set 'Start_Ptr_Ptr' if some later invocation (think
					   recursion) of the same parentheses already has. */

                    if (eContext.Start_Ptr_Ptr[no] == nullptr)
                        eContext.Start_Ptr_Ptr[no] = save;

					MATCH_RETURN(1);
				} else {
					MATCH_RETURN(0);
				}
			} else if ((GET_OP_CODE(scan) > CLOSE) && (GET_OP_CODE(scan) < CLOSE + NSUBEXP)) {

				int no;
				const char *save;

				no = GET_OP_CODE(scan) - CLOSE;
                save = eContext.Reg_Input;

				if (no < 10)
                    eContext.Back_Ref_End[no] = save;

				if (match(next, nullptr)) {
					/* Do not set 'End_Ptr_Ptr' if some later invocation of the
					   same parentheses already has. */

                    if (eContext.End_Ptr_Ptr[no] == nullptr)
                        eContext.End_Ptr_Ptr[no] = save;

					MATCH_RETURN(1);
				} else {
					MATCH_RETURN(0);
				}
			} else {
				reg_error("memory corruption, 'match'");

				MATCH_RETURN(0);
			}

			break;
		}

		scan = next;
	}

	/* We get here only if there's trouble -- normally "case END" is
	   the terminating point. */

	reg_error("corrupted pointers, 'match'");

	MATCH_RETURN(0);
}

/*----------------------------------------------------------------------*
 * greedy
 *
 * Repeatedly match something simple up to "max" times. If max <= 0
 * then match as much as possible (max = infinity).  Uses unsigned long
 * variables to maximize the amount of text matchable for unbounded
 * qualifiers like '*' and '+'.  This will allow at least 4,294,967,295
 * matches (4 Gig!) for an ANSI C compliant compiler.  If you are
 * applying a regex to something bigger than that, you shouldn't be
 * using NEdit!
 *
 * Returns the actual number of matches.
 *----------------------------------------------------------------------*/
static unsigned long greedy(uint8_t *p, long max) {

	unsigned long count = REG_ZERO;

    const char *input_str = eContext.Reg_Input;
	uint8_t *operand = OPERAND(p); // Literal char or start of class characters.
    unsigned long max_cmp = (max > 0) ? static_cast<unsigned long>(max) : ULONG_MAX;

	switch (GET_OP_CODE(p)) {
	case ANY:
		/* Race to the end of the line or string. Dot DOESN'T match
		   newline. */

		while (count < max_cmp && *input_str != '\n' && !AT_END_OF_STRING(input_str)) {
			count++;
			input_str++;
		}

		break;

	case EVERY:
		// Race to the end of the line or string. Dot DOES match newline.

		while (count < max_cmp && !AT_END_OF_STRING(input_str)) {
			count++;
			input_str++;
		}

		break;

	case EXACTLY: // Count occurrences of single character operand.
		while (count < max_cmp && *operand == *input_str && !AT_END_OF_STRING(input_str)) {
			count++;
			input_str++;
		}

		break;

	case SIMILAR: // Case insensitive version of EXACTLY
		while (count < max_cmp && *operand == tolower(*input_str) && !AT_END_OF_STRING(input_str)) {
			count++;
			input_str++;
		}

		break;

	case ANY_OF: // [...] character class.
		while (count < max_cmp && strchr(reinterpret_cast<char *>(operand), *input_str) != nullptr && !AT_END_OF_STRING(input_str)) {

			count++;
			input_str++;
		}

		break;

	case ANY_BUT: /* [^...] Negated character class- does NOT normally
	                 match newline (\n added usually to operand at compile
	                 time.) */

		while (count < max_cmp && strchr(reinterpret_cast<char *>(operand), *input_str) == nullptr && !AT_END_OF_STRING(input_str)) {

			count++;
			input_str++;
		}

		break;

	case IS_DELIM: /* \y (not a word delimiter char)
	                   NOTE: '\n' and '\0' are always word delimiters. */

		while (count < max_cmp && isDelimiter(*input_str) && !AT_END_OF_STRING(input_str)) {
			count++;
			input_str++;
		}

		break;

	case NOT_DELIM: /* \Y (not a word delimiter char)
	                   NOTE: '\n' and '\0' are always word delimiters. */

		while (count < max_cmp && !isDelimiter(*input_str) && !AT_END_OF_STRING(input_str)) {
			count++;
			input_str++;
		}

		break;

	case WORD_CHAR: // \w (word character, alpha-numeric or underscore)
		while (count < max_cmp && (safe_ctype<isalnum>(*input_str) || *input_str == static_cast<uint8_t>('_')) && !AT_END_OF_STRING(input_str)) {

			count++;
			input_str++;
		}

		break;

	case NOT_WORD_CHAR: // \W (NOT a word character)
		while (count < max_cmp && !safe_ctype<isalnum>(*input_str) && *input_str != static_cast<uint8_t>('_') && *input_str != static_cast<uint8_t>('\n') && !AT_END_OF_STRING(input_str)) {

			count++;
			input_str++;
		}

		break;

	case DIGIT: // same as [0123456789]
		while (count < max_cmp && safe_ctype<isdigit>(*input_str) && !AT_END_OF_STRING(input_str)) {
			count++;
			input_str++;
		}

		break;

	case NOT_DIGIT: // same as [^0123456789]
		while (count < max_cmp && !safe_ctype<isdigit>(*input_str) && *input_str != '\n' && !AT_END_OF_STRING(input_str)) {

			count++;
			input_str++;
		}

		break;

	case SPACE: // same as [ \t\r\f\v]-- doesn't match newline.
		while (count < max_cmp && safe_ctype<isspace>(*input_str) && *input_str != '\n' && !AT_END_OF_STRING(input_str)) {

			count++;
			input_str++;
		}

		break;

	case SPACE_NL: // same as [\n \t\r\f\v]-- matches newline.
		while (count < max_cmp && safe_ctype<isspace>(*input_str) && !AT_END_OF_STRING(input_str)) {

			count++;
			input_str++;
		}

		break;

	case NOT_SPACE: // same as [^\n \t\r\f\v]-- doesn't match newline.
		while (count < max_cmp && !safe_ctype<isspace>(*input_str) && !AT_END_OF_STRING(input_str)) {

			count++;
			input_str++;
		}

		break;

	case NOT_SPACE_NL: // same as [^ \t\r\f\v]-- matches newline.
		while (count < max_cmp && (!safe_ctype<isspace>(*input_str) || *input_str == '\n') && !AT_END_OF_STRING(input_str)) {

			count++;
			input_str++;
		}

		break;

	case LETTER: // same as [a-zA-Z]
		while (count < max_cmp && safe_ctype<isalpha>(*input_str) && !AT_END_OF_STRING(input_str)) {

			count++;
			input_str++;
		}

		break;

	case NOT_LETTER: // same as [^a-zA-Z]
		while (count < max_cmp && !safe_ctype<isalpha>(*input_str) && *input_str != '\n' && !AT_END_OF_STRING(input_str)) {

			count++;
			input_str++;
		}

		break;

	default:
		/* Called inappropriately.  Only atoms that are SIMPLE should
		   generate a call to greedy.  The above cases should cover
		   all the atoms that are SIMPLE. */

		reg_error("internal error #10 'greedy'");
		count = 0U; // Best we can do.
	}

	// Point to character just after last matched character.

    eContext.Reg_Input = input_str;

    return count;
}

/*----------------------------------------------------------------------*
 * next_ptr - compute the address of a node's "NEXT" pointer.
 * Note: a simplified inline version is available via the NEXT_PTR() macro,
 *       but that one is only to be used at time-critical places (see the
 *       description of the macro).
 *----------------------------------------------------------------------*/

static uint8_t *next_ptr(uint8_t *ptr) {

    if (ptr == &Compute_Size) {
        return nullptr;
    }

    const int offset = GET_OFFSET(ptr);

    if (offset == 0) {
        return nullptr;
    }

	if (GET_OP_CODE(ptr) == BACK) {
		return (ptr - offset);
	} else {
		return (ptr + offset);
	}
}

/*
**  SubstituteRE - Perform substitutions after a 'regexp' match.
**
**  This function cleanly shortens results of more than max length to max.
**  To give the caller a chance to react to this the function returns false
**  on any error. The substitution will still be executed.
*/
bool regexp::SubstituteRE(view::string_view source, std::string &dest) const {

    char test;

    if (U_CHAR_AT(this->program) != MAGIC) {
        reg_error("damaged regexp passed to 'SubstituteRE'");
        return false;
    }

    auto src = source.begin();
    auto dst = std::back_inserter(dest);

    while (src != source.end()) {

        char c = *src++;

        char chgcase = '\0';
        int paren_no = -1;

        if (c == '\\') {
            // Process any case altering tokens, i.e \u, \U, \l, \L.

            if (*src == 'u' || *src == 'U' || *src == 'l' || *src == 'L') {
                chgcase = *src++;

                if (src == source.end()) {
                    break;
                }

                c = *src++;
            }
        }

        if (c == '&') {
            paren_no = 0;
        } else if (c == '\\') {
            /* Can not pass register variable '&src' to function 'numeric_escape'
               so make a non-register copy that we can take the address of. */

            decltype(src) src_alias = src;

            if ('1' <= *src && *src <= '9') {
                paren_no = *src++ - '0';

            } else if ((test = literal_escape(*src)) != '\0') {
                c = test;
                src++;

            } else if ((test = numeric_escape(*src, &src_alias)) != '\0') {
                c   = test;
                src = src_alias;
                src++;

                /* NOTE: if an octal escape for zero is attempted (e.g. \000), it
                   will be treated as a literal string. */
            } else if (src == source.end()) {
                /* If '\' is the last character of the replacement string, it is
                   interpreted as a literal backslash. */

                c = '\\';
            } else {
                c = *src++; // Allow any escape sequence (This is
            }               // INCONSISTENT with the 'CompileRE'
        }                   // mind set of issuing an error!

        if (paren_no < 0) { // Ordinary character.
            *dst++ = c;
        } else if (this->startp[paren_no] != nullptr && this->endp[paren_no]) {

            /* The tokens \u and \l only modify the first character while the
             * tokens \U and \L modify the entire string. */
            switch(chgcase) {
            case 'u':
                {
                    int count = 0;
                    std::transform(this->startp[paren_no], this->endp[paren_no], dst, [&count](char ch) -> int {
                        if(count++ == 0) {
                            return safe_ctype<toupper>(ch);
                        } else {
                            return ch;
                        }
                    });
                }
                break;
            case 'U':
                std::transform(this->startp[paren_no], this->endp[paren_no], dst, [](char ch) {
                    return safe_ctype<toupper>(ch);
                });
                break;
            case 'l':
                {
                    int count = 0;
                    std::transform(this->startp[paren_no], this->endp[paren_no], dst, [&count](char ch) -> int {
                        if(count++ == 0) {
                            return safe_ctype<tolower>(ch);
                        } else {
                            return ch;
                        }
                    });
                }
                break;
            case 'L':
                std::transform(this->startp[paren_no], this->endp[paren_no], dst, [](char ch) {
                    return safe_ctype<tolower>(ch);
                });
                break;
            default:
                std::copy(this->startp[paren_no], this->endp[paren_no], dst);
                break;
            }

        }
    }


    return true;
}

/*----------------------------------------------------------------------*
 * reg_error
 *----------------------------------------------------------------------*/

static void reg_error(const char *str) {

	fprintf(stderr, "nedit: Internal error processing regular expression (%s)\n", str);
}

/*----------------------------------------------------------------------*
 * makeDelimiterTable
 *
 * Translate a null-terminated string of delimiters into a 256 byte
 * lookup table for determining whether a character is a delimiter or
 * not.
 *----------------------------------------------------------------------*/
static std::bitset<256> makeDelimiterTable(view::string_view delimiters) {

	std::bitset<256> table;

	for(char ch : delimiters) {
        table[static_cast<size_t>(ch)] = true;
	}

	table['\0'] = true; // These
	table['\t'] = true; // characters
	table['\n'] = true; // are always
	table[' ']  = true; // delimiters.

	return table;
}

/*----------------------------------------------------------------------*
 * SetREDefaultWordDelimiters
 *
 * Builds a default delimiter table that persists across 'ExecRE' calls.
 *----------------------------------------------------------------------*/

void SetREDefaultWordDelimiters(view::string_view delimiters) {
	Default_Delimiters = makeDelimiterTable(delimiters);
}
