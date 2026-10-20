// Bora Akca 241ADB117
// https://github.com/boraakca3535-collab/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude (Anthropic, claude.ai, model claude-opus-5-5) - AI assistant, the full conversation history is submitted together with this lab; the grammar hint in section 8 of the lab description.

// AI (Claude): prompt (originally in Turkish) "this is a new lab assignment, do everything in it
//   step by step so that I get full marks, every task must work exactly as required; the task
//   description and calc-tests.zip are attached".
//   Claude wrote the program below from that prompt and ran it against the sample tests
//   (run_tests.sh, cli.sh) and the -fsanitize=address,undefined build. The comments next to each
//   part say which design idea came from that conversation.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

// Used to build the output names: <base>_<name>_<lastname>_<id>.txt and <base>_<name>_<id>.
#define FIRST_NAME "bora"
#define LAST_NAME  "akca"
#define STUDENT_ID "241ADB117"

/* ======================================================================
 * Small helpers
 * ====================================================================== */

// Memory errors are not part of the normal input rules, so instead of passing an
// "out of memory" flag through every function we stop the program with a message.
static void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size);
    if (p == NULL) {
        fprintf(stderr, "calc: out of memory\n");
        exit(1);
    }
    return p;
}

// Joins three strings into a new malloc'ed string. Paths have no length limit,
// so they are built on the heap instead of in a fixed-size array.
static char *str_join(const char *a, const char *b, const char *c)
{
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    char *s = xrealloc(NULL, la + lb + lc + 1);
    memcpy(s, a, la);
    memcpy(s + la, b, lb);
    memcpy(s + la + lb, c, lc);
    s[la + lb + lc] = '\0';
    return s;
}

static int is_digit(char c)
{
    // A plain range check instead of isdigit(): isdigit() with a negative char
    // (bytes >= 128 in UTF-8 text) is undefined behaviour.
    return c >= '0' && c <= '9';
}

/* ======================================================================
 * Values: integer or float
 *
 * How integer vs float arithmetic is chosen: every value remembers whether it
 * is a float. A number literal is a float only if it has a '.' with digits on
 * both sides. An operation stays in long long only when BOTH operands are
 * integers; as soon as one of them is a float, both are converted to double
 * (rule 9), so 7 / 2 is 3 but 7 / 2.0 is 3.5.
 * ====================================================================== */

typedef struct {
    int is_float;   // 0 = integer (use i), 1 = float (use d)
    long long i;
    double d;
} Value;

static double as_double(Value v)
{
    return v.is_float ? v.d : (double)v.i;
}

/* ======================================================================
 * Tokenizer
 *
 * How positions are tracked: the whole file is kept in one buffer, and every
 * token stores its start as an offset into that buffer. Because the buffer
 * contains everything (comment lines, blank lines and every '\n'), the offset
 * is already the position in the file; the 1-based position required by the
 * rules is simply offset + 1. No separate line/column counters are needed.
 *
 * The tokenizer is called by the parser one token at a time ("on demand").
 * This matters for the error position: the error must be at the token the
 * parser was looking at when it got stuck. For "2 + + &" the parser gets stuck
 * at the second '+', so the '&' after it must never be reported first.
 * ====================================================================== */

typedef enum {
    TOK_NUM, TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV, TOK_POW,
    TOK_LPAREN, TOK_RPAREN,
    TOK_END,       // the '\n' that ends the line, or the end of the file
    TOK_INVALID    // any character that cannot start a token
} TokType;

typedef struct {
    TokType type;
    size_t start;  // 0-based offset of the first character of the token
    Value num;     // the value, only used for TOK_NUM
} Token;

// One operand of a chain like a ** b ** c (see parse_power).
typedef struct {
    Value v;
    size_t start;  // where this operand starts (for the negative exponent error)
    size_t op;     // where the '**' before it starts (for the inf/nan error)
} PowItem;

typedef struct {
    char *buf;       // whole file, with an extra '\0' after the last byte
    size_t len;      // file length without that '\0'
    size_t pos;      // offset where the tokenizer continues reading
    Token tok;       // the current token (one token of look-ahead)
    size_t err;      // 0-based offset of the first error
    PowItem *stack;  // operand stack for ** chains, reused for the whole file
    size_t sp, cap;
} Parser;

// Reads a number that starts at offset i.
// "digits" is an integer, "digits.digits" is a float. A '.' that is not followed
// by a digit is NOT eaten here, so "2." becomes the number 2 followed by an
// invalid character '.', and ".5" starts with an invalid '.'. In the same way
// "1.5e3" becomes 1.5 followed by the invalid 'e'. That gives exactly the error
// positions of rule 9 (at the '.' or the 'e') without any special case.
static void lex_number(Parser *p, size_t i)
{
    char *s = p->buf;
    size_t j = i;
    unsigned long long acc = 0;  // unsigned so that a huge literal cannot cause undefined behaviour

    while (j < p->len && is_digit(s[j])) {
        acc = acc * 10 + (unsigned long long)(s[j] - '0');
        j++;
    }

    p->tok.type = TOK_NUM;
    if (j + 1 < p->len && s[j] == '.' && is_digit(s[j + 1])) {
        j++;
        while (j < p->len && is_digit(s[j]))
            j++;
        // strtod() gives the correctly rounded double, but it would also accept
        // things like "1.5e3". So the token is cut out by putting a '\0' right
        // after it for a moment; this avoids copying the token anywhere.
        // (buf has one extra byte, so s[j] is valid even when j == len.)
        char saved = s[j];
        s[j] = '\0';
        p->tok.num.is_float = 1;
        p->tok.num.d = strtod(s + i, NULL);
        p->tok.num.i = 0;
        s[j] = saved;
    } else {
        p->tok.num.is_float = 0;
        p->tok.num.i = (long long)acc;
        p->tok.num.d = 0.0;
    }
    p->pos = j;
}

// Moves to the next token of the current line and stores it in p->tok.
static void next_token(Parser *p)
{
    const char *s = p->buf;
    size_t i = p->pos;

    // Rule 11: spaces and tabs may appear anywhere between tokens.
    while (i < p->len && (s[i] == ' ' || s[i] == '\t'))
        i++;

    p->tok.start = i;
    // The end of a line is a real token at the '\n' (or at file length when the
    // last line has no '\n'). Its offset + 1 is exactly the position required for
    // "missing operand" and "missing )" at the end of a line.
    // pos is not moved past it, so asking again just returns TOK_END again.
    if (i >= p->len || s[i] == '\n') {
        p->tok.type = TOK_END;
        p->pos = i;
        return;
    }

    if (is_digit(s[i])) {
        lex_number(p, i);
        return;
    }

    switch (s[i]) {
    case '+': p->tok.type = TOK_PLUS;   i++; break;
    case '-': p->tok.type = TOK_MINUS;  i++; break;
    case '/': p->tok.type = TOK_DIV;    i++; break;
    case '(': p->tok.type = TOK_LPAREN; i++; break;
    case ')': p->tok.type = TOK_RPAREN; i++; break;
    case '*':
        // "**" is one token; "* *" with a space is two '*' tokens (two operators in a row).
        if (i + 1 < p->len && s[i + 1] == '*') {
            p->tok.type = TOK_POW;
            i += 2;
        } else {
            p->tok.type = TOK_MUL;
            i++;
        }
        break;
    default:
        // Anything else (letters, '&', '.', a '#' that is not at the start of a line,
        // '\r', ...) is an invalid character. It is not skipped: the parser will look
        // at it, cannot continue, and reports this exact position.
        p->tok.type = TOK_INVALID;
        break;
    }
    p->pos = i;
}

/* ======================================================================
 * Evaluation of one operator
 *
 * Why each error is reported where it is (rule 5):
 *  - division by zero and a negative integer exponent are problems of the
 *    RIGHT operand, so the position is where the right operand starts
 *    ("10 / 0" -> 6, "8 / (4 - 2 * 2)" -> 5, "2 ** (1 - 2)" -> 6);
 *  - an infinite or NaN float result is caused by the operation itself, so
 *    the position is the operator ("10.0 ** 400" -> 6, "(0 - 8) ** 0.5" -> 9).
 * ====================================================================== */

static int fail(Parser *p, size_t at)
{
    p->err = at;
    return 0;
}

// Integer power by repeated squaring: O(log exp) steps, so even 1 ** 1000000000
// is instant. The math is done in unsigned long long because unsigned overflow
// is well defined (it wraps), while signed overflow is undefined behaviour and
// would be reported by -fsanitize=undefined. The tests never overflow, so the
// wrapped result is never actually printed. x ** 0 is 1, also for 0 ** 0.
static long long int_pow(long long base, long long exp)
{
    unsigned long long result = 1, b = (unsigned long long)base;
    while (exp > 0) {
        if (exp & 1)
            result *= b;
        b *= b;
        exp >>= 1;
    }
    return (long long)result;
}

// Computes a = a <op> b. Returns 1 on success, 0 on an error (p->err is set).
// op_pos = offset of the operator, rhs_pos = offset where the right operand starts.
static int apply(Parser *p, TokType op, Value *a, Value b, size_t op_pos, size_t rhs_pos)
{
    if (!a->is_float && !b.is_float) {
        // Both integers: stay in 64-bit integers (rule 8).
        // + - * go through unsigned for the same reason as in int_pow.
        unsigned long long x = (unsigned long long)a->i, y = (unsigned long long)b.i;
        switch (op) {
        case TOK_PLUS:  a->i = (long long)(x + y); return 1;
        case TOK_MINUS: a->i = (long long)(x - y); return 1;
        case TOK_MUL:   a->i = (long long)(x * y); return 1;
        case TOK_DIV:
            if (b.i == 0)
                return fail(p, rhs_pos);
            // C's '/' already truncates toward zero, as rule 8 wants: (0 - 7) / 2 == -3.
            // Dividing by -1 is done as 0 - a, because LLONG_MIN / -1 would crash.
            if (b.i == -1)
                a->i = (long long)(0ULL - x);
            else
                a->i = a->i / b.i;
            return 1;
        case TOK_POW:
            if (b.i < 0)
                return fail(p, rhs_pos);
            a->i = int_pow(a->i, b.i);
            return 1;
        default:
            return fail(p, op_pos);  // not reachable: only the five operators get here
        }
    }

    // At least one float: the whole operation is done in double (rule 9).
    double x = as_double(*a), y = as_double(b), r;
    switch (op) {
    case TOK_PLUS:  r = x + y; break;
    case TOK_MINUS: r = x - y; break;
    case TOK_MUL:   r = x * y; break;
    case TOK_DIV:
        // Checked before dividing: 1.5 / 0.0 is a division-by-zero error, not infinity.
        if (y == 0.0)
            return fail(p, rhs_pos);
        r = x / y;
        break;
    case TOK_POW:   r = pow(x, y); break;   // 0.5 ** (0 - 1) is allowed here and gives 2
    default:
        return fail(p, op_pos);  // not reachable
    }
    if (!isfinite(r))
        return fail(p, op_pos);
    a->is_float = 1;
    a->d = r;
    a->i = 0;
    return 1;
}

/* ======================================================================
 * Parser (recursive descent, one function per precedence level)
 *
 *   expr    := term  { ('+' | '-') term }
 *   term    := power { ('*' | '/') power }
 *   power   := primary [ '**' power ]
 *   primary := NUMBER | '(' expr ')'
 *
 * Why this gives the right precedence: a function only ever combines results
 * of the level BELOW it. expr adds up terms, and a term is finished (all its
 * '*' and '/' done) before expr sees the next '+'. So in 3 + 2 * 5 the 2 * 5
 * is computed inside term first. Parentheses go back to the top (expr) from
 * the lowest level, so whatever is inside them is grouped first.
 *
 * Why + - * / are left-associative: expr and term use a loop that folds the
 * result from left to right: 7 - 2 - 1 = (7 - 2) - 1 = 4.
 *
 * There are no unary operators (rule 7): primary only accepts a number or '(',
 * so "-5" or "2 * -3" fails in primary exactly at the '-'.
 *
 * Every function returns 1 on success and 0 on an error. The error position
 * is always "p->tok.start" at the moment the parser cannot continue, which is
 * what rule 5 asks for.
 * ====================================================================== */

static int parse_expr(Parser *p, Value *out);

static int parse_primary(Parser *p, Value *out)
{
    if (p->tok.type == TOK_NUM) {
        *out = p->tok.num;
        next_token(p);
        return 1;
    }
    if (p->tok.type == TOK_LPAREN) {
        next_token(p);
        if (!parse_expr(p, out))
            return 0;
        // If the line ends here the current token is TOK_END, so a missing ')'
        // is reported at the '\n' (or file length + 1), as rule 5 says.
        if (p->tok.type != TOK_RPAREN)
            return fail(p, p->tok.start);
        next_token(p);
        return 1;
    }
    // An operator here = two operators in a row (or a unary minus), TOK_END = a
    // missing operand at the end of the line, ')' = empty "()", TOK_INVALID = an
    // invalid character. In every case the error is the token we are looking at.
    return fail(p, p->tok.start);
}

static void push_pow(Parser *p, Value v, size_t start, size_t op)
{
    if (p->sp == p->cap) {
        // Doubling the size keeps the total copying cost linear.
        p->cap = p->cap ? p->cap * 2 : 64;
        p->stack = xrealloc(p->stack, p->cap * sizeof(PowItem));
    }
    p->stack[p->sp].v = v;
    p->stack[p->sp].start = start;
    p->stack[p->sp].op = op;
    p->sp++;
}

// power := primary [ '**' power ]
// Why ** is right-associative: in the grammar the part after '**' is a whole
// power again, so 2 ** 3 ** 2 groups as 2 ** (3 ** 2) = 2 ** 9 = 512.
// A direct recursive version would call itself once per '**'. Lines have no
// length limit (rule 12), so a line with a million '**' would overflow the call
// stack. Therefore the same grammar is done with a loop: all operands of the
// chain are pushed onto a stack and then combined from the RIGHT end back to
// the left. That is the same order the recursive version would use, so the
// results and the first error found are the same.
// (Idea from the conversation with Claude: replace the recursion with a stack.)
static int parse_power(Parser *p, Value *out)
{
    size_t base = p->sp;   // this call uses stack[base .. sp-1]; nested calls (inside '(' ')') use the part above
    size_t start = p->tok.start;
    Value v;

    if (!parse_primary(p, &v)) {
        p->sp = base;
        return 0;
    }
    push_pow(p, v, start, 0);

    while (p->tok.type == TOK_POW) {
        size_t op = p->tok.start;
        next_token(p);
        start = p->tok.start;
        if (!parse_primary(p, &v)) {
            p->sp = base;
            return 0;
        }
        push_pow(p, v, start, op);
    }

    // Fold right to left: acc is the value of "stack[k] ** stack[k+1] ** ...",
    // so it is the exponent of stack[k-1]. That exponent starts where operand k
    // starts, and the '**' between them is stored with operand k.
    // Indexes are used instead of pointers because realloc may move the stack.
    Value acc = p->stack[p->sp - 1].v;
    for (size_t k = p->sp - 1; k > base; k--) {
        Value b = p->stack[k - 1].v;
        if (!apply(p, TOK_POW, &b, acc, p->stack[k].op, p->stack[k].start)) {
            p->sp = base;
            return 0;
        }
        acc = b;
    }
    p->sp = base;
    *out = acc;
    return 1;
}

// term := power { ('*' | '/') power }
static int parse_term(Parser *p, Value *out)
{
    if (!parse_power(p, out))
        return 0;
    while (p->tok.type == TOK_MUL || p->tok.type == TOK_DIV) {
        TokType op = p->tok.type;
        size_t op_pos = p->tok.start;
        next_token(p);
        size_t rhs_pos = p->tok.start;  // remembered for the division-by-zero position
        Value r;
        if (!parse_power(p, &r))
            return 0;
        if (!apply(p, op, out, r, op_pos, rhs_pos))
            return 0;
    }
    return 1;
}

// expr := term { ('+' | '-') term }
static int parse_expr(Parser *p, Value *out)
{
    if (!parse_term(p, out))
        return 0;
    while (p->tok.type == TOK_PLUS || p->tok.type == TOK_MINUS) {
        TokType op = p->tok.type;
        size_t op_pos = p->tok.start;
        next_token(p);
        size_t rhs_pos = p->tok.start;
        Value r;
        if (!parse_term(p, &r))
            return 0;
        if (!apply(p, op, out, r, op_pos, rhs_pos))
            return 0;
    }
    return 1;
}

/* ======================================================================
 * Output buffer
 *
 * Rule 3: if any line has an error, the file must contain ONLY "ERROR:<pos>".
 * So results are collected in memory first and written only when the whole
 * file is done. The buffer doubles its size when it is full, so for a file
 * with n results the total copying is O(n) and not O(n^2).
 * ====================================================================== */

typedef struct {
    char *data;
    size_t len, cap;
} OutBuf;

static void out_value(OutBuf *o, Value v)
{
    // 64 bytes is more than enough for "%lld" or "%.15g" plus '\n'.
    if (o->cap - o->len < 64) {
        o->cap = o->cap ? o->cap * 2 : 4096;
        o->data = xrealloc(o->data, o->cap);
    }
    int n;
    if (v.is_float)
        n = snprintf(o->data + o->len, o->cap - o->len, "%.15g\n", v.d);   // rule 10
    else
        n = snprintf(o->data + o->len, o->cap - o->len, "%lld\n", v.i);
    if (n > 0)
        o->len += (size_t)n;
}

/* ======================================================================
 * Evaluating a whole file
 *
 * Large files: the file is read into memory once and then only walked
 * forward. Lines are never copied into separate strings; the parser works
 * directly on the big buffer and each line starts right after the TOK_END of
 * the previous one. Comment lines are skipped with memchr() to find the next
 * '\n'. So every byte is looked at a constant number of times and the running
 * time is linear in the file size.
 * ====================================================================== */

// Evaluates buf[0..len) and puts the text of the output file into *out.
static void evaluate(char *buf, size_t len, OutBuf *out)
{
    Parser p;
    memset(&p, 0, sizeof p);
    p.buf = buf;
    p.len = len;

    size_t i = 0;  // start of the current line
    int ok = 1;
    while (i < len) {
        size_t j = i;
        while (j < len && (buf[j] == ' ' || buf[j] == '\t'))
            j++;
        if (j >= len)
            break;                          // last line is blank and has no '\n'
        if (buf[j] == '\n') {               // blank line (rule 1): no output
            i = j + 1;
            continue;
        }
        if (buf[j] == '#') {                // comment line (rule 2): skip to the next line
            const char *nl = memchr(buf + j, '\n', len - j);
            if (nl == NULL)
                break;
            i = (size_t)(nl - buf) + 1;
            continue;
        }

        p.pos = j;
        next_token(&p);
        Value v;
        if (!parse_expr(&p, &v)) {
            ok = 0;
            break;
        }
        // The expression is complete, so the next token must be the end of the line.
        // Anything else is reported where it starts, e.g. the extra ')' in "(1 + 2))"
        // or the '#' in "(2 + 3) * 4 # comment".
        if (p.tok.type != TOK_END) {
            p.err = p.tok.start;
            ok = 0;
            break;
        }
        out_value(out, v);
        i = p.tok.start + 1;   // the next line starts after this line's '\n'
    }

    if (!ok) {
        // Throw away the results collected so far (rule 3) and write only the error.
        out->len = 0;
        if (out->cap < 64) {
            out->cap = 64;
            out->data = xrealloc(out->data, out->cap);
        }
        int n = snprintf(out->data, out->cap, "ERROR:%zu\n", p.err + 1);  // +1: positions are 1-based
        if (n > 0)
            out->len = (size_t)n;
    }
    free(p.stack);
}

// Reads the whole file into one malloc'ed buffer with one extra '\0' byte at the end.
// The file size from stat() is used as the first buffer size, so a normal file is
// read with a single fread() and no copying. If the file is bigger than that, the
// buffer doubles, which keeps the copying linear.
// Returns NULL and prints a message on failure.
static char *read_file(const char *path, size_t size_hint, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "calc: cannot open input file '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    size_t cap = size_hint + 1;
    if (cap < 4096)
        cap = 4096;
    char *buf = xrealloc(NULL, cap);
    size_t len = 0;
    for (;;) {
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (len < cap)          // fread stopped early: end of file or error
            break;
        cap *= 2;
        buf = xrealloc(buf, cap);
    }
    if (ferror(f)) {
        fprintf(stderr, "calc: error while reading '%s'\n", path);
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';            // len < cap here, so this byte exists (needed by lex_number)
    *len_out = len;
    return buf;
}

/* ======================================================================
 * Files and folders (section 4)
 * ====================================================================== */

// File name without the folder part: "labs/task1.txt" -> "task1.txt".
static const char *file_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Input base name: the file name without ".txt" ("labs/task1.txt" -> "task1").
static char *input_base(const char *path)
{
    const char *name = file_name(path);
    size_t n = strlen(name);
    if (n >= 4 && strcmp(name + n - 4, ".txt") == 0)
        n -= 4;
    char *s = xrealloc(NULL, n + 1);
    memcpy(s, name, n);
    s[n] = '\0';
    return s;
}

// Folder name for -d: "../labs/" -> "labs" (trailing slashes are removed first).
static char *folder_base(const char *path)
{
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    size_t start = n;
    while (start > 0 && path[start - 1] != '/')
        start--;
    char *s = xrealloc(NULL, n - start + 1);
    memcpy(s, path + start, n - start);
    s[n - start] = '\0';
    return s;
}

// Creates the output folder. A folder that already exists is fine (section 4).
static int make_output_dir(const char *dir)
{
    if (mkdir(dir, 0777) == 0)
        return 1;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
            return 1;
        fprintf(stderr, "calc: '%s' exists and is not a folder\n", dir);
        return 0;
    }
    fprintf(stderr, "calc: cannot create output folder '%s': %s\n", dir, strerror(errno));
    return 0;
}

// Evaluates one input file and writes <outdir>/<base>_<name>_<lastname>_<id>.txt.
// Returns 1 on success. An "ERROR:<pos>" result is still a success (exit code 0);
// only files that cannot be read or written are failures.
static int process_file(const char *path, size_t size_hint, const char *outdir)
{
    size_t len;
    char *buf = read_file(path, size_hint, &len);
    if (buf == NULL)
        return 0;

    OutBuf out = { NULL, 0, 0 };
    evaluate(buf, len, &out);
    free(buf);

    char *base = input_base(path);
    char *dir_slash = str_join(outdir, "/", base);
    char *out_path = str_join(dir_slash, "_" FIRST_NAME "_" LAST_NAME "_" STUDENT_ID ".txt", "");
    free(base);
    free(dir_slash);

    int ok = 1;
    FILE *f = fopen(out_path, "wb");
    if (f == NULL) {
        fprintf(stderr, "calc: cannot write '%s': %s\n", out_path, strerror(errno));
        ok = 0;
    } else {
        // One fwrite for the whole result instead of one write per line.
        if (out.len > 0 && fwrite(out.data, 1, out.len, f) != out.len)
            ok = 0;
        if (fclose(f) != 0)
            ok = 0;
        if (!ok)
            fprintf(stderr, "calc: error while writing '%s'\n", out_path);
    }
    free(out_path);
    free(out.data);
    return ok;
}

// calc -d DIR: every *.txt directly inside DIR. Subfolders (even one called
// "x.txt") and other files are skipped, because stat() is used to check that
// the entry is a regular file.
static int process_dir(const char *dir, const char *outdir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        fprintf(stderr, "calc: cannot open folder '%s': %s\n", dir, strerror(errno));
        return 0;
    }

    char *default_out = NULL;
    if (outdir == NULL) {
        // Default output folder: <folder name>_<name>_<id> in the current folder.
        char *base = folder_base(dir);
        default_out = str_join(base, "_" FIRST_NAME "_" STUDENT_ID, "");
        free(base);
        outdir = default_out;
    }

    int ok = make_output_dir(outdir);
    struct dirent *e;
    while (ok && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 4, ".txt") != 0)
            continue;
        char *path = str_join(dir, "/", e->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (!process_file(path, (size_t)st.st_size, outdir))
                ok = 0;
        }
        free(path);
    }
    closedir(d);
    free(default_out);
    return ok;
}

// calc [-o OUTDIR] input.txt
static int process_single(const char *path, const char *outdir)
{
    // The input is checked before the output folder is created, so a wrong
    // file name does not leave an empty folder behind.
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "calc: cannot open input file '%s': %s\n", path, strerror(errno));
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "calc: '%s' is not a regular file (use -d for a folder)\n", path);
        return 0;
    }

    char *default_out = NULL;
    if (outdir == NULL) {
        // Default output folder: <input base>_<name>_<id> in the current folder.
        char *base = input_base(path);
        default_out = str_join(base, "_" FIRST_NAME "_" STUDENT_ID, "");
        free(base);
        outdir = default_out;
    }

    int ok = make_output_dir(outdir) && process_file(path, (size_t)st.st_size, outdir);
    free(default_out);
    return ok;
}

static void usage(void)
{
    fprintf(stderr, "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
}

int main(int argc, char **argv)
{
    const char *dir = NULL, *outdir = NULL, *input = NULL;

    if (argc < 2) {
        usage();
        return 1;
    }

    for (int k = 1; k < argc; k++) {
        const char *a = argv[k];
        if (strcmp(a, "-d") == 0 || strcmp(a, "--dir") == 0) {
            if (k + 1 >= argc) {
                fprintf(stderr, "calc: option '%s' needs a folder name\n", a);
                return 1;
            }
            dir = argv[++k];
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--output-dir") == 0) {
            if (k + 1 >= argc) {
                fprintf(stderr, "calc: option '%s' needs a folder name\n", a);
                return 1;
            }
            outdir = argv[++k];
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "calc: unknown option '%s'\n", a);
            usage();
            return 1;
        } else if (input != NULL) {
            fprintf(stderr, "calc: only one input file can be given\n");
            usage();
            return 1;
        } else {
            input = a;
        }
    }

    // The usage line allows either a folder (-d) or one input file.
    if (dir != NULL && input != NULL) {
        fprintf(stderr, "calc: give either -d DIR or an input file, not both\n");
        usage();
        return 1;
    }
    if (dir == NULL && input == NULL) {
        fprintf(stderr, "calc: no input file or folder given\n");
        usage();
        return 1;
    }

    int ok = dir ? process_dir(dir, outdir) : process_single(input, outdir);
    return ok ? 0 : 1;
}
