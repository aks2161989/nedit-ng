
#ifndef COMPILE_H_
#define COMPILE_H_

#include "RegexError.h"
#include <bitset>
#include <vector>
#include <boost/utility/string_view.hpp>

class Regex;

// Array sizes for arrays used by function init_ansi_classes.
constexpr int WHITE_SPACE_SIZE = 16;
constexpr int ALNUM_CHAR_SIZE  = 256;

// Global work variables for 'CompileRE'.
struct ParseContext {
	boost::string_view::iterator Reg_Parse;                         // Input scan ptr (scans user's regex)
	boost::string_view           InputString;
	std::vector<uint8_t>        Code;
	const char *                Meta_Char;
	size_t                      Reg_Size;                          // Size of compiled regex code.
	std::bitset<64>             Closed_Parens;                     // Bit flags indicating () closure.
	std::bitset<64>             Paren_Has_Width;                   // Bit flags indicating ()'s that are known to not match the empty string
	uint8_t                     Num_Braces;                        // Number of general {m,n} constructs. {m,n} quantifiers of SIMPLE atoms are not included in this count.
	uint8_t                     Total_Paren;                       // Parentheses, (),  counter.
	bool                        FirstPass;
	bool                        Is_Case_Insensitive;
	bool                        Match_Newline;
	bool                        Enable_Counting_Quantifier = true;
	char                        White_Space[WHITE_SPACE_SIZE];     // Arrays used by
	char                        Word_Char[ALNUM_CHAR_SIZE];        // functions
	char                        Letter_Char[ALNUM_CHAR_SIZE];      // init_ansi_classes () and shortcut_escape ().
	char                        Brace_Char;
};

extern ParseContext pContext;

#endif
