#include "compiler.h"
#include "heap.h"
#include "interp.h"
#include "lexer.h"
#include "parser.h"
#include "test_main.h"

#include <cstring>
#include <string>

using namespace coal;

static RunResult run_src(const char* s, size_t n) {
  auto l = lex(s, n);
  auto p = parse(l.tokens);
  auto c = compile(p.program);
  if (!c.ok) { RunResult r; r.error = c.error; return r; }
  Heap h(8u << 20);
  return run(c.module, h);
}

#define SRC(x) run_src(x, sizeof(x) - 1)
#define OUT(x, expect) { auto r = SRC(x); CHECK(r.ok); CHECK(r.output == expect); }
#define ERRS(x)        { auto r = SRC(x); CHECK(!r.ok); }

void test_native() {
  // --- type / convert ---
  OUT("print type(3);", "int");
  OUT("print type(3.5);", "double");
  OUT("print type(true);", "bool");
  OUT("print type(nil);", "nil");
  OUT("print type([1]);", "array");
  OUT("print type(\"x\");", "string");
  OUT("print type({\"a\"=1});", "map");
  OUT("print to_string(42);", "42");
  OUT("print to_int(\"42\");", "42");
  OUT("print to_int(3.9);", "3");
  OUT("print to_float(\"1.5\") == 1.5;", "true");
  OUT("print is_str(\"x\");", "true");
  OUT("print is_array(3);", "false");
  ERRS("print to_int(\"nope\");");

  // --- math ---
  OUT("print abs(0 - 7);", "7");
  OUT("print max(3, 9);", "9");
  OUT("print min(3, 9);", "3");
  OUT("print floor(3.7);", "3");
  OUT("print ceil(3.2);", "4");
  OUT("print round(2.5);", "3");
  OUT("print sign(0 - 4);", "-1");
  OUT("print sqrt(9.0) == 3.0;", "true");
  OUT("print pow(2, 10) == 1024.0;", "true");
  ERRS("print sqrt(0 - 1);");

  // --- string ---
  OUT("print len(\"hello\");", "5");
  OUT("print substr(\"hello\", 1, 3);", "ell");
  OUT("print substr(\"hi\", 0, 99);", "hi");    // clamp, no OOB
  OUT("print substr(\"hi\", 0, 0);", "");       // empty, no crash
  OUT("print index_of(\"hello\", \"ll\");", "2");
  OUT("print contains(\"hello\", \"xy\");", "false");
  OUT("print starts_with(\"hello\", \"he\");", "true");
  OUT("print ends_with(\"hello\", \"lo\");", "true");
  OUT("print upper(\"aB\");", "AB");
  OUT("print lower(\"aB\");", "ab");
  OUT("print char_at(\"hi\", 1);", "105");      // 'i'
  OUT("print ord(\"A\");", "65");
  OUT("print chr(66);", "B");
  OUT("print repeat(\"ab\", 3);", "ababab");
  OUT("print str_concat(\"foo\", \"bar\");", "foobar");
  OUT("print trim(\"  hi  \");", "hi");
  OUT("print join([\"a\", \"b\"], \"-\");", "a-b");
  ERRS("print char_at(\"hi\", 9);");            // OOB -> runtime error
  ERRS("print repeat(\"x\", 0 - 1);");          // negative count

  // split -> array of strings
  OUT("a = split(\"a,b,c\", \",\"); print len(a);", "3");
  OUT("a = split(\"a,b,c\", \",\"); print a[0];", "a");
  OUT("a = split(\"a,b,c\", \",\"); print a[2];", "c");

  // --- array ---
  OUT("a = [1, 2, 3]; print len(a);", "3");
  OUT("a = [1, 2, 3]; print pop(a);", "3");
  OUT("a = [1, 2, 3]; pop(a); print len(a);", "2");
  OUT("a = [1, 2, 3]; print remove(a, 0);", "1");
  OUT("a = [1, 2, 3]; remove(a, 0); print a[0];", "2");
  OUT("a = [3, 1, 2]; sort(a); print a[0];", "1");
  OUT("a = [3, 1, 2]; sort(a); print a[2];", "3");
  OUT("a = slice([1, 2, 3, 4], 1, 2); print len(a);", "2");
  OUT("a = slice([1, 2, 3, 4], 1, 2); print a[0];", "2");
  OUT("a = [1, 2, 3]; reverse(a); print a[0];", "3");
  OUT("a = [1, 2]; insert(a, 1, 9); print a[1];", "9");
  OUT("a = [1, 2]; insert(a, 1, 9); print len(a);", "3");
  OUT("a = [10, 20, 30]; print arr_index_of(a, 20);", "1");
  OUT("a = [10, 20, 30]; print arr_index_of(a, 99);", "-1");
  ERRS("a = []; pop(a);");                       // empty
  ERRS("a = [1]; remove(a, 5);");                // OOB
  ERRS("a = [1, \"x\"]; sort(a);");              // non-number element

  // --- map ---
  OUT("m = {\"x\"=1, \"y\"=2}; print len(keys(m));", "2");
  OUT("m = {\"x\"=1, \"y\"=2}; print has(m, \"x\");", "true");
  OUT("m = {\"x\"=1, \"y\"=2}; print has(m, \"z\");", "false");
  OUT("m = {\"x\"=5}; a = values(m); print a[0];", "5");
  OUT("m = {\"x\"=1, \"y\"=2}; print remove_key(m, \"x\");", "true");
  OUT("m = {\"x\"=1, \"y\"=2}; remove_key(m, \"x\"); print len(m);", "1");

  // --- builtins resolve over user functions; a user fn still works otherwise ---
  OUT("fn dbl(a) { return a + a; } print dbl(21);", "42");

  // --- GC pressure: multi-alloc builtins stay correct while collections fire ---
  // split builds an array + N strings; force collections after, read back.
  {
    const char* src =
        "parts = split(\"a,b,c,d,e\", \",\");"
        "i = 200;"
        "while (i) { junk = [0, 0, 0, 0, 0]; i = i - 1; }"
        "print parts[4];";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(32 * 1024);  // small -> the loop forces collections
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "e");
  }

  // insert under heap pressure: the grow path must re-read the array.
  {
    const char* src =
        "a = [];"
        "i = 200;"
        "while (i) { insert(a, 0, i); junk = [0, 0, 0, 0]; i = i - 1; }"
        "print a[0];";   // last inserted at front == 1
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(48 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "1");
  }

  // keys() allocates an array then copies map entries: re-read after the alloc.
  {
    const char* src =
        "m = {\"alpha\"=1, \"beta\"=2, \"gamma\"=3};"
        "ks = keys(m);"
        "i = 200;"
        "while (i) { junk = [0, 0, 0, 0, 0]; i = i - 1; }"
        "print ks[0];";   // first key survives the collections
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(32 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "alpha");
  }

  // slice() allocates then copies source elements: re-read source after alloc.
  {
    const char* src =
        "s = slice([100, 200, 300, 400], 1, 2);"
        "i = 200;"
        "while (i) { junk = [0, 0, 0, 0, 0]; i = i - 1; }"
        "print s[0] + s[1];";   // 200 + 300
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(32 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "500");
  }

  // --- A2b: stdlib batch 2 ---
  // convert / predicates
  OUT("print to_string(42);", "42");
  OUT("print to_int(\"42\") + 1;", "43");
  OUT("print is_nil(nil);", "true");
  OUT("print is_array([1]);", "true");
  OUT("print is_map({\"a\" = 1});", "true");
  OUT("print is_int(3);", "true");
  OUT("print is_bool(1 < 2);", "true");
  { auto r = SRC("print to_int(\"nope\");"); CHECK(!r.ok); }  // bad parse -> error

  // math
  OUT("print clamp(15, 0, 10);", "10");
  OUT("print clamp(-3, 0, 10);", "0");
  OUT("print gcd(12, 18);", "6");
  OUT("print lcm(4, 6);", "12");
  OUT("print trunc(3.9);", "3");
  { auto r = SRC("print log(0);"); CHECK(!r.ok); }            // domain error
  { auto r = SRC("print clamp(1, 10, 0);"); CHECK(!r.ok); }   // lo > hi

  // string
  OUT("print replace(\"a.b.c\", \".\", \"-\");", "a-b-c");
  OUT("print last_index_of(\"abcabc\", \"bc\");", "4");
  OUT("print pad_left(\"7\", 3, \"0\");", "007");
  OUT("print pad_right(\"7\", 3, \".\");", "7..");
  OUT("print count_sub(\"aaaa\", \"aa\");", "2");             // non-overlapping
  OUT("print capitalize(\"hELLO\");", "Hello");
  OUT("print reverse_str(\"abc\");", "cba");
  OUT("print len(lines(\"a\nb\nc\"));", "3");
  OUT("print is_empty(\"\");", "true");
  { auto r = SRC("print replace(\"x\", \"\", \"y\");"); CHECK(!r.ok); }  // empty from

  // array
  OUT("print len(range(0, 5));", "5");
  OUT("a = range(0, 5); print a[0] + a[4];", "4");
  OUT("a = range(10, 0, 0 - 2); print a[0] + a[1];", "18");   // 10 + 8
  OUT("a = fill(3, 7); print a[0] + a[1] + a[2];", "21");
  OUT("a = concat_arr([1, 2], [3, 4]); print a[0] + a[3];", "5");  // 1 + 4
  OUT("print sum([1, 2, 3, 4]);", "10");
  OUT("print product([1, 2, 3, 4]);", "24");
  OUT("print min_of([5, 2, 8]);", "2");
  OUT("print max_of([5, 2, 8]);", "8");
  OUT("print first([9, 8, 7]);", "9");
  OUT("print last([9, 8, 7]);", "7");
  OUT("a = take([1, 2, 3, 4], 2); print len(a) + a[1];", "4");    // 2 + 2
  OUT("a = drop([1, 2, 3, 4], 1); print len(a) + a[0];", "5");    // 3 + 2
  OUT("print count_of([1, 2, 1, 1], 1);", "3");
  { auto r = SRC("print first([]);"); CHECK(!r.ok); }            // empty
  { auto r = SRC("print sum([1, \"x\"]);"); CHECK(!r.ok); }      // non-number

  // map (dynamic keys via set/get)
  OUT("m = {}; set(m, \"x\", 5); print get(m, \"x\");", "5");
  OUT("m = {}; set(m, \"x\", 1); print get(m, \"y\");", "nil");
  OUT("a = {\"x\" = 1}; b = {\"y\" = 2}; m = merge(a, b); print get(m, \"x\") + get(m, \"y\");", "3");
  OUT("m = {\"a\" = 1, \"b\" = 2}; e = entries(m); print len(e) + len(e[0]);", "4");  // 2 entries, pair len 2

  // set under heap pressure: dynamic map growth survives collections
  {
    const char* src =
        "m = {};"
        "set(m, \"keep\", 123);"
        "i = 200;"
        "while (i) { junk = [0, 0, 0, 0, 0]; i = i - 1; }"
        "print get(m, \"keep\");";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(32 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "123");
  }

  // ===================== batch 3 =====================

  // math
  OUT("print factorial(5);", "120");
  OUT("print factorial(0);", "1");
  OUT("print is_prime(7);", "true");
  OUT("print is_prime(8);", "false");
  OUT("print is_prime(1);", "false");
  OUT("print fib(10);", "55");
  OUT("print round(cbrt(27.0));", "3");          // avoid exact float compare
  OUT("print round(log_base(8.0, 2.0));", "3");
  OUT("print mean([2, 4, 6]) == 4.0;", "true");
  ERRS("print factorial(0 - 1);");
  ERRS("print factorial(25);");          // overflow guard
  ERRS("print fib(200);");               // overflow guard
  ERRS("print log_base(8.0, 1.0);");     // invalid base

  // array reductions / predicates
  OUT("print any([0, 0, 3]);", "true");
  OUT("print any([0, 0, 0]);", "false");
  OUT("print all([1, 2, 3]);", "true");
  OUT("print all([1, 0, 3]);", "false");
  OUT("print includes([1, 2, 3], 2);", "true");
  OUT("print includes([1, 2, 3], 9);", "false");
  OUT("print index_min([5, 1, 3]);", "1");
  OUT("print index_max([5, 1, 3]);", "0");

  // array transforms
  OUT("a = zip([1, 2, 3], [4, 5]); print len(a);", "2");          // min length
  OUT("a = zip([1, 2], [4, 5]); print a[1][0] + a[1][1];", "7");  // 2 + 5
  OUT("a = enumerate([7, 8, 9]); print a[2][0] + a[2][1];", "11"); // index 2 + value 9
  OUT("a = flatten([[1, 2], [3], [4, 5]]); print len(a);", "5");
  OUT("a = flatten([[1, 2], [3]]); print a[2];", "3");
  OUT("a = unique([1, 2, 2, 3, 1, 3]); print len(a);", "3");
  OUT("a = unique([1, 2, 2, 1]); print a[1];", "2");
  OUT("a = rotate([1, 2, 3, 4], 1); print a[0];", "2");
  OUT("a = rotate([1, 2, 3, 4], 0 - 1); print a[0];", "4");       // negative shift
  OUT("a = chunk([1, 2, 3, 4, 5], 2); print len(a);", "3");       // 2,2,1
  OUT("a = chunk([1, 2, 3, 4, 5], 2); print len(a[2]);", "1");    // last chunk shorter
  ERRS("print chunk([1, 2], 0);");                                // bad size

  // string predicates
  OUT("print is_digit(\"123\");", "true");
  OUT("print is_digit(\"12a\");", "false");
  OUT("print is_alpha(\"abcXYZ\");", "true");
  OUT("print is_alnum(\"a1b2\");", "true");
  OUT("print is_space(\"  \t\");", "true");
  OUT("print is_digit(\"\");", "false");          // empty -> false

  // string transforms
  OUT("print swapcase(\"aBcD\");", "AbCd");
  OUT("print title_case(\"hello world\");", "Hello World");
  OUT("print zfill(\"42\", 5);", "00042");
  OUT("print center(\"hi\", 6, \"-\");", "--hi--");
  OUT("a = chars(\"abc\"); print len(a);", "3");
  OUT("a = chars(\"abc\"); print a[1];", "b");
  OUT("a = to_bytes(\"AB\"); print a[0] + a[1];", "131");          // 65 + 66
  OUT("print format(\"{} + {} = {}\", 2, 3, 5);", "2 + 3 = 5");
  OUT("print format(\"no args here\");", "no args here");

  // map transforms
  OUT("m = {\"a\"=\"x\", \"b\"=\"y\"}; inv = invert(m); print get(inv, \"x\");", "a");
  OUT("m = {\"a\"=1, \"b\"=2, \"c\"=3}; p = pick(m, [\"a\", \"c\"]); print len(p);", "2");
  OUT("m = {\"a\"=1, \"b\"=2}; p = pick(m, [\"a\", \"z\"]); print len(p);", "1");  // missing key skipped

  // GC pressure on batch-3 multi-alloc builtins.
  {
    const char* src =
        "z = zip([1, 2, 3], [10, 20, 30]);"
        "ch = chars(\"hello\");"
        "fl = flatten([[1, 2], [3, 4, 5]]);"
        "i = 200;"
        "while (i) { junk = [0, 0, 0, 0, 0]; i = i - 1; }"
        "print z[2][1] + len(ch) + fl[4];";   // 30 + 5 + 5 == 40
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(48 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "40");
  }

  // invert under heap pressure: dynamic map growth across collections.
  {
    const char* src =
        "m = {\"a\"=\"1\", \"b\"=\"2\", \"c\"=\"3\", \"d\"=\"4\", \"e\"=\"5\"};"
        "inv = invert(m);"
        "i = 200;"
        "while (i) { junk = [0, 0, 0, 0]; i = i - 1; }"
        "print get(inv, \"5\");";   // value "5" inverts back to key "e"
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(48 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "e");
  }

  // ===================== batch 4 =====================

  // math: inverse trig / hyperbolic (compare rounded to avoid float equality)
  OUT("print round(acos(0.0) * 1000);", "1571");   // pi/2
  OUT("print round(atan(1.0) * 1000);", "785");    // pi/4
  OUT("print round(sinh(0.0));", "0");
  OUT("print round(cosh(0.0));", "1");
  OUT("print round(tanh(0.0));", "0");
  ERRS("print asin(2.0);");      // domain
  ERRS("print acosh(0.5);");     // domain (x < 1)
  ERRS("print atanh(1.0);");     // domain (|x| >= 1)

  // math: misc
  OUT("print fmod(7.0, 3.0) == 1.0;", "true");
  ERRS("print fmod(1.0, 0.0);");
  OUT("print copysign(3.0, 0 - 1.0) == 0 - 3.0;", "true");
  OUT("print ldexp(1.0, 4) == 16.0;", "true");
  OUT("print is_nan(0.0);", "false");
  OUT("print is_inf(1.0);", "false");
  OUT("print is_finite(3);", "true");
  OUT("print round(lerp(0.0, 10.0, 0.5));", "5");
  OUT("print smoothstep(0.0, 1.0, 0.5) == 0.5;", "true");
  OUT("print isqrt(17);", "4");
  OUT("print isqrt(16);", "4");
  ERRS("print isqrt(0 - 1);");
  OUT("print mod_floor(0 - 7, 3);", "2");
  OUT("print mod_floor(7, 0 - 3);", "-2");
  ERRS("print mod_floor(5, 0);");
  OUT("print bit_count(7);", "3");
  OUT("print bit_count(0);", "0");
  OUT("print next_pow2(17);", "32");
  OUT("print next_pow2(16);", "16");
  OUT("a = divmod(17, 5); print a[0];", "3");
  OUT("a = divmod(17, 5); print a[1];", "2");
  ERRS("print divmod(1, 0);");

  // string predicates / transforms
  OUT("print is_upper(\"ABC\");", "true");
  OUT("print is_upper(\"AbC\");", "false");
  OUT("print is_upper(\"123\");", "false");   // no letters
  OUT("print is_lower(\"abc\");", "true");
  OUT("print strip_prefix(\"foobar\", \"foo\");", "bar");
  OUT("print strip_prefix(\"foobar\", \"xyz\");", "foobar");
  OUT("print strip_suffix(\"foobar\", \"bar\");", "foo");
  OUT("print left(\"hello\", 3);", "hel");
  OUT("print right(\"hello\", 3);", "llo");
  OUT("print left(\"hi\", 99);", "hi");        // clamp
  OUT("print count_char(\"banana\", \"a\");", "3");
  OUT("a = find_all(\"abcabc\", \"bc\"); print len(a);", "2");
  OUT("a = find_all(\"abcabc\", \"bc\"); print a[0] + a[1];", "5");  // 1 + 4
  OUT("print replace_first(\"a.b.c\", \".\", \"-\");", "a-b.c");
  OUT("print rot13(\"abc\");", "nop");
  OUT("print rot13(rot13(\"Hello\"));", "Hello");  // involution
  ERRS("print find_all(\"x\", \"\");");        // empty pattern

  // array transforms / reductions
  OUT("a = compact([1, nil, 2, nil, 3]); print len(a);", "3");
  OUT("a = compact([1, nil, 2]); print a[1];", "2");
  OUT("a = cumsum([1, 2, 3, 4]); print a[3];", "10");
  OUT("a = cumsum([1, 2, 3]); print a[0] + a[1] + a[2];", "10");   // 1 + 3 + 6
  OUT("print dot([1, 2, 3], [4, 5, 6]);", "32");
  ERRS("print dot([1, 2], [1, 2, 3]);");       // unequal length
  OUT("a = intersperse([1, 2, 3], 0); print len(a);", "5");
  OUT("a = intersperse([1, 2, 3], 0); print a[1];", "0");
  OUT("a = reverse_copy([1, 2, 3]); print a[0];", "3");
  OUT("a = [3, 1, 2]; s = sorted(a); print s[0];", "1");
  OUT("a = [3, 1, 2]; sorted(a); print a[0];", "3");   // source unchanged
  OUT("print is_sorted([1, 2, 3]);", "true");
  OUT("print is_sorted([1, 3, 2]);", "false");
  OUT("a = tail([1, 2, 3]); print a[0];", "2");
  OUT("a = tail([1, 2, 3]); print len(a);", "2");
  OUT("a = init([1, 2, 3]); print a[1];", "2");
  OUT("a = init([1, 2, 3]); print len(a);", "2");
  ERRS("print tail([]);");

  // map utilities
  OUT("m = {\"a\"=1}; print get_or(m, \"a\", 99);", "1");
  OUT("m = {\"a\"=1}; print get_or(m, \"z\", 99);", "99");
  OUT("m = {\"a\"=1, \"b\"=2}; print has_value(m, 2);", "true");
  OUT("m = {\"a\"=1}; print has_value(m, 9);", "false");
  OUT("m = {\"a\"=1, \"b\"=2, \"c\"=3}; o = omit(m, [\"b\"]); print len(o);", "2");
  OUT("m = {\"a\"=1, \"b\"=2}; o = omit(m, [\"b\"]); print has(o, \"a\");", "true");
  OUT("m = {\"a\"=1, \"b\"=2}; o = omit(m, [\"b\"]); print has(o, \"b\");", "false");

  // conversion / predicate
  OUT("print hex(255);", "0xff");
  OUT("print hex(0);", "0x0");
  OUT("print hex(0 - 255);", "-0xff");
  OUT("print bin(5);", "0b101");
  OUT("print is_function(3);", "false");
  OUT("fn f(x) { return x; } print is_function(f);", "true");

  // GC pressure on a batch-4 multi-alloc builtin (omit grows a fresh map).
  {
    const char* src =
        "m = {\"a\"=1, \"b\"=2, \"c\"=3, \"d\"=4, \"e\"=5};"
        "o = omit(m, [\"c\"]);"
        "i = 200;"
        "while (i) { junk = [0, 0, 0, 0]; i = i - 1; }"
        "print get(o, \"e\");";   // surviving entry after collections
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(48 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "5");
  }
}
