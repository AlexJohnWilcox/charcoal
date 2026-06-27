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
}
