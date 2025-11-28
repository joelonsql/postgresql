/*-------------------------------------------------------------------------
 *
 * parser.c
 *		Main entry point/driver for PostgreSQL grammar
 *
 * This should match src/backend/parser/parser.c, except that we do not
 * need to bother with re-entrant interfaces.
 *
 * Note: ECPG doesn't report error location like the backend does.
 * This file will need work if we ever want it to.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/ecpg/preproc/parser.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "preproc_extern.h"
#include "preproc.h"


static bool have_lookahead;		/* is lookahead info valid? */
static int	lookahead_token;	/* one-token lookahead */
static YYSTYPE lookahead_yylval;	/* yylval for lookahead token */
static YYLTYPE lookahead_yylloc;	/* yylloc for lookahead token */
static char *lookahead_yytext;	/* start current token */

/*
 * Extended lookahead buffer for KEY_LA disambiguation.
 * We use a simple linked list since ECPG doesn't have access to backend List.
 */
typedef struct EcpgBufferedToken
{
	int			token;
	YYSTYPE		yylval;
	YYLTYPE		yylloc;
	char	   *yytext;
	struct EcpgBufferedToken *next;
} EcpgBufferedToken;

static EcpgBufferedToken *lookahead_buffer_head = NULL;
static EcpgBufferedToken *lookahead_buffer_tail = NULL;

static int	base_yylex_location(void);
static bool check_uescapechar(unsigned char escape);
static bool ecpg_isspace(char ch);
static bool peek_fk_join_after_parens(void);


/*
 * Intermediate filter between parser and base lexer (base_yylex in scan.l).
 *
 * This filter is needed because in some cases the standard SQL grammar
 * requires more than one token lookahead.  We reduce these cases to one-token
 * lookahead by replacing tokens here, in order to keep the grammar LALR(1).
 *
 * Using a filter is simpler than trying to recognize multiword tokens
 * directly in scan.l, because we'd have to allow for comments between the
 * words.  Furthermore it's not clear how to do that without re-introducing
 * scanner backtrack, which would cost more performance than this filter
 * layer does.
 *
 * We also use this filter to convert UIDENT and USCONST sequences into
 * plain IDENT and SCONST tokens.  While that could be handled by additional
 * productions in the main grammar, it's more efficient to do it like this.
 */
int
filtered_base_yylex(void)
{
	int			cur_token;
	int			next_token;
	YYSTYPE		cur_yylval;
	YYLTYPE		cur_yylloc;
	char	   *cur_yytext;

	/* First, return any buffered tokens from extended lookahead */
	if (lookahead_buffer_head != NULL)
	{
		EcpgBufferedToken *bt = lookahead_buffer_head;

		cur_token = bt->token;
		base_yylval = bt->yylval;
		base_yylloc = bt->yylloc;
		base_yytext = bt->yytext;
		lookahead_buffer_head = bt->next;
		if (lookahead_buffer_head == NULL)
			lookahead_buffer_tail = NULL;
		free(bt);
		return cur_token;
	}

	/* Get next token --- we might already have it */
	if (have_lookahead)
	{
		cur_token = lookahead_token;
		base_yylval = lookahead_yylval;
		base_yylloc = lookahead_yylloc;
		base_yytext = lookahead_yytext;
		have_lookahead = false;
	}
	else
		cur_token = base_yylex_location();

	/*
	 * If this token isn't one that requires lookahead, just return it.
	 */
	switch (cur_token)
	{
		case FORMAT:
		case KEY:
		case NOT:
		case NULLS_P:
		case WITH:
		case WITHOUT:
		case UIDENT:
		case USCONST:
			break;
		default:
			return cur_token;
	}

	/* Save and restore lexer output variables around the call */
	cur_yylval = base_yylval;
	cur_yylloc = base_yylloc;
	cur_yytext = base_yytext;

	/* Get next token, saving outputs into lookahead variables */
	next_token = base_yylex_location();

	lookahead_token = next_token;
	lookahead_yylval = base_yylval;
	lookahead_yylloc = base_yylloc;
	lookahead_yytext = base_yytext;

	base_yylval = cur_yylval;
	base_yylloc = cur_yylloc;
	base_yytext = cur_yytext;

	have_lookahead = true;

	/* Replace cur_token if needed, based on lookahead */
	switch (cur_token)
	{
		case FORMAT:
			/* Replace FORMAT by FORMAT_LA if it's followed by JSON */
			switch (next_token)
			{
				case JSON:
					cur_token = FORMAT_LA;
					break;
			}
			break;

		case KEY:
			/*
			 * KEY followed by '(' could be:
			 * - Foreign key join: KEY (cols) -> ref (cols)
			 * - Table alias with column renaming: tbl key(cols) ON ...
			 *
			 * Peek ahead using the tokenizer to find matching ')' and
			 * check if '->' or '<-' follows.
			 */
			if (next_token == '(')
			{
				if (peek_fk_join_after_parens())
					cur_token = KEY_LA;
				/* else cur_token stays KEY (identifier for alias) */
			}
			break;

		case NOT:
			/* Replace NOT by NOT_LA if it's followed by BETWEEN, IN, etc */
			switch (next_token)
			{
				case BETWEEN:
				case IN_P:
				case LIKE:
				case ILIKE:
				case SIMILAR:
					cur_token = NOT_LA;
					break;
			}
			break;

		case NULLS_P:
			/* Replace NULLS_P by NULLS_LA if it's followed by FIRST or LAST */
			switch (next_token)
			{
				case FIRST_P:
				case LAST_P:
					cur_token = NULLS_LA;
					break;
			}
			break;

		case WITH:
			/* Replace WITH by WITH_LA if it's followed by TIME or ORDINALITY */
			switch (next_token)
			{
				case TIME:
				case ORDINALITY:
					cur_token = WITH_LA;
					break;
			}
			break;

		case WITHOUT:
			/* Replace WITHOUT by WITHOUT_LA if it's followed by TIME */
			switch (next_token)
			{
				case TIME:
					cur_token = WITHOUT_LA;
					break;
			}
			break;
		case UIDENT:
		case USCONST:
			/* Look ahead for UESCAPE */
			if (next_token == UESCAPE)
			{
				/* Yup, so get third token, which had better be SCONST */
				const char *escstr;

				/*
				 * Again save and restore lexer output variables around the
				 * call
				 */
				cur_yylval = base_yylval;
				cur_yylloc = base_yylloc;
				cur_yytext = base_yytext;

				/* Get third token */
				next_token = base_yylex_location();

				if (next_token != SCONST)
					mmerror(PARSE_ERROR, ET_ERROR, "UESCAPE must be followed by a simple string literal");

				/*
				 * Save and check escape string, which the scanner returns
				 * with quotes
				 */
				escstr = base_yylval.str;
				if (strlen(escstr) != 3 || !check_uescapechar(escstr[1]))
					mmerror(PARSE_ERROR, ET_ERROR, "invalid Unicode escape character");

				base_yylval = cur_yylval;
				base_yylloc = cur_yylloc;
				base_yytext = cur_yytext;

				/* Combine 3 tokens into 1 */
				base_yylval.str = make3_str(base_yylval.str,
											" UESCAPE ",
											escstr);
				base_yylloc = loc_strdup(base_yylval.str);

				/* Clear have_lookahead, thereby consuming all three tokens */
				have_lookahead = false;
			}

			if (cur_token == UIDENT)
				cur_token = IDENT;
			else if (cur_token == USCONST)
				cur_token = SCONST;
			break;
	}

	return cur_token;
}

/*
 * Call base_yylex() and fill in base_yylloc.
 *
 * pgc.l does not worry about setting yylloc, and given what we want for
 * that, trying to set it there would be pretty inconvenient.  What we
 * want is: if the returned token has type <str>, then duplicate its
 * string value as yylloc; otherwise, make a downcased copy of yytext.
 * The downcasing is ASCII-only because all that we care about there
 * is producing uniformly-cased output of keywords.  (That's mostly
 * cosmetic, but there are places in ecpglib that expect to receive
 * downcased keywords, plus it keeps us regression-test-compatible
 * with the pre-v18 implementation of ecpg.)
 */
static int
base_yylex_location(void)
{
	int			token = base_yylex();

	switch (token)
	{
			/* List a token here if pgc.l assigns to base_yylval.str for it */
		case Op:
		case CSTRING:
		case CPP_LINE:
		case CVARIABLE:
		case BCONST:
		case SCONST:
		case USCONST:
		case XCONST:
		case FCONST:
		case IDENT:
		case UIDENT:
		case IP:
			/* Duplicate the <str> value */
			base_yylloc = loc_strdup(base_yylval.str);
			break;
		default:
			/* Else just use the input, i.e., yytext */
			base_yylloc = loc_strdup(base_yytext);
			/* Apply an ASCII-only downcasing */
			for (unsigned char *ptr = (unsigned char *) base_yylloc; *ptr; ptr++)
			{
				if (*ptr >= 'A' && *ptr <= 'Z')
					*ptr += 'a' - 'A';
			}
			break;
	}
	return token;
}

/*
 * Helper to append a token to the lookahead buffer linked list.
 */
static void
buffer_token(int token, YYSTYPE yylval, YYLTYPE yylloc, char *yytext)
{
	EcpgBufferedToken *bt = (EcpgBufferedToken *) malloc(sizeof(EcpgBufferedToken));

	bt->token = token;
	bt->yylval = yylval;
	bt->yylloc = yylloc;
	bt->yytext = yytext;
	bt->next = NULL;

	if (lookahead_buffer_tail != NULL)
		lookahead_buffer_tail->next = bt;
	else
		lookahead_buffer_head = bt;
	lookahead_buffer_tail = bt;
}

/*
 * peek_fk_join_after_parens
 *
 * Peek ahead in the token stream to determine if the current position
 * represents a foreign key join constraint: (cols) -> ref (cols) or
 * (cols) <- ref (cols).
 *
 * Assumes we've already seen KEY and the next token is '(' (in the
 * one-token lookahead). Uses the actual tokenizer (base_yylex_location)
 * to peek ahead, buffering tokens in lookahead_buffer for later consumption.
 *
 * Returns true if '->' or '<-' follows the closing ')', indicating
 * a foreign key join (KEY_LA). Returns false otherwise (KEY as alias).
 */
static bool
peek_fk_join_after_parens(void)
{
	int		paren_depth = 1;	/* Already saw '(' in lookahead */
	int		token;

	/*
	 * The '(' is already in the one-token lookahead. Buffer it first.
	 */
	buffer_token('(', lookahead_yylval, lookahead_yylloc, lookahead_yytext);

	/* Clear the one-token lookahead since we're taking over */
	have_lookahead = false;

	/*
	 * Scan tokens until we find the matching ')'.
	 * Buffer each token for later consumption.
	 */
	while (paren_depth > 0)
	{
		token = base_yylex_location();

		/* Buffer this token */
		buffer_token(token, base_yylval, base_yylloc, base_yytext);

		if (token == '(')
			paren_depth++;
		else if (token == ')')
			paren_depth--;
		else if (token == 0)	/* EOF */
			return false;
	}

	/*
	 * Now peek at the next token to see if it's '->' or '<-'.
	 * This token also gets buffered.
	 */
	token = base_yylex_location();

	buffer_token(token, base_yylval, base_yylloc, base_yytext);

	/*
	 * Check for FK join indicators:
	 * - RIGHT_ARROW (->)
	 * - '<' which is the start of <- sequence (LEFT_ARROW_LESS)
	 */
	return (token == RIGHT_ARROW || token == '<' || token == LEFT_ARROW_LESS);
}

/*
 * check_uescapechar() and ecpg_isspace() should match their equivalents
 * in pgc.l.
 */

/* is 'escape' acceptable as Unicode escape character (UESCAPE syntax) ? */
static bool
check_uescapechar(unsigned char escape)
{
	if (isxdigit(escape)
		|| escape == '+'
		|| escape == '\''
		|| escape == '"'
		|| ecpg_isspace(escape))
		return false;
	else
		return true;
}

/*
 * ecpg_isspace() --- return true if flex scanner considers char whitespace
 */
static bool
ecpg_isspace(char ch)
{
	if (ch == ' ' ||
		ch == '\t' ||
		ch == '\n' ||
		ch == '\r' ||
		ch == '\f')
		return true;
	return false;
}
