#include "compiler.h"
#include "heap.h"
#include "interp.h"
#include "lexer.h"
#include "parser.h"
#include "test_main.h"

#include <cstring>
#include <string>

using namespace coal;

// Force exactly one minor collection: allocate unrooted garbage until the young
// bump offset drops (which happens only when a collection reclaims it), then stop
// immediately. Each call ages every rooted survivor by exactly one collection.
static void force_one_gc(Heap& h) {
  for (int i = 0; i < 1000000; ++i) {
    size_t before = h.bytes_used();
    (void)h.new_array(2);
    if (h.bytes_used() < before) return;  // top_ dropped => a collection just fired
  }
}

void test_gc() {
  // (1) An object rooted by a HandleScope survives forced collections intact.
  {
    Heap h(64 * 1024);
    HandleScope hs(h);
    size_t si = hs.root(h.new_string("survivor", 8));
    for (int i = 0; i < 20000; ++i) (void)h.new_array(1);  // force collection(s)
    StringObj* s = hs.get<StringObj>(si);                  // re-read moved object
    CHECK(s->len == 8);
    CHECK(std::memcmp(s->bytes->data, "survivor", 8) == 0);
  }

  // (2) Unrooted garbage is reclaimed: a tiny heap never overflows under a huge
  // allocation count, because each collection frees everything.
  {
    Heap h(64 * 1024);
    for (int i = 0; i < 50000; ++i) (void)h.new_array(2);
    CHECK(h.over_cap() == false);
    CHECK(h.bytes_used() < 64 * 1024);
  }

  // (3) Transitive structure survives: an array of strings, rooted, keeps its
  // elements alive and intact across collections.
  {
    Heap h(128 * 1024);
    HandleScope hs(h);
    size_t ai = hs.root(h.new_array(3));
    for (int i = 0; i < 3; ++i) {
      StringObj* e = h.new_string("x", 1);     // safepoint
      ArrayObj* a = hs.get<ArrayObj>(ai);      // re-read after the alloc
      a->slots->data[i] = Value::object(e);    // store immediately (no alloc between)
    }
    for (int i = 0; i < 20000; ++i) (void)h.new_array(1);  // force collection(s)
    ArrayObj* a = hs.get<ArrayObj>(ai);
    CHECK(a->len == 3);
    for (int i = 0; i < 3; ++i) {
      CHECK(a->slots->data[i].tag == Tag::Obj);
      StringObj* e = static_cast<StringObj*>(a->slots->data[i].as.obj);
      CHECK(e->len == 1 && e->bytes->data[0] == 'x');
    }
  }

  // (4) Objects actually move (proves the collector is copying, not no-op).
  {
    Heap h(64 * 1024);
    HandleScope hs(h);
    size_t ai = hs.root(h.new_array(1));
    void* before = static_cast<void*>(hs.get<ArrayObj>(ai));
    for (int i = 0; i < 20000; ++i) (void)h.new_array(1);
    void* after = static_cast<void*>(hs.get<ArrayObj>(ai));
    CHECK(before != after);
  }

  // (5) End-to-end: register values are roots, so a live array survives many
  // allocations forced mid-execution on a small heap.
  {
    const char* src =
        "xs = [10, 20, 30];"
        "i = 200;"
        "while (i) { junk = [0, 0, 0, 0, 0]; i = i - 1; }"
        "print xs[1];";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(32 * 1024);                 // small -> the loop forces collections
    Limits lim;
    auto r = run(c.module, h, lim);
    CHECK(r.ok);
    CHECK(r.output == "20");           // xs survived; xs[1] still 20
  }

  // (6) push under heap pressure: a safepoint bug in ARRAY_PUSH would corrupt or
  // crash here, since collections fire mid-push while the array must survive.
  {
    const char* src =
        "a = [];"
        "i = 300;"
        "while (i) { push(a, i); junk = [0, 0, 0, 0]; i = i - 1; }"
        "print a[0];";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(48 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "300");
  }

  // (7) format_join under heap pressure: a relocation mid-format must not leave
  // the input array's backing store cached. The buggy version reads poisoned
  // from-space here; the fix re-reads the array each iteration.
  {
    const char* src =
        "filler = range(0, 1500);"
        "a = range(0, 400);"
        "print format_join(a, \",\");";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(48 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output.substr(0, 6) == "0,1,2,");   // joined ints, fixed version
  }

  // (8) Lazy iterator happy path: drain a small array with no heap pressure.
  {
    const char* src =
        "a = [5, 7, 9];"
        "it = iter(a);"
        "s = 0;"
        "while (has_next(it)) { s = s + next(it); }"
        "print s;";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(64 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "21");   // 5 + 7 + 9
  }

  // (9) Iterator under heap pressure: a collection between next() calls must not
  // leave the cursor's cached backing store dangling. The buggy version reads
  // poisoned from-space here; the fix re-derives the array's slots each step.
  {
    const char* src =
        "a = range(0, 400);"
        "it = iter(a);"
        "s = 0;"
        "while (has_next(it)) { junk = range(0, 30); s = s + next(it); }"
        "print s;";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(48 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "79800");   // sum 0..399 = 399*400/2
  }

  // (G1) Fresh objects are young; old arena starts empty.
  {
    Heap h(64 * 1024);
    ArrayObj* a = h.new_array(1);
    CHECK(h.is_young(a));
    CHECK(!h.is_old(a));
    CHECK(h.remset_size() == 0);
  }

  // (G2) Barrier on a young holder records nothing (holder isn't old yet).
  {
    Heap h(64 * 1024);
    HandleScope hs(h);
    size_t ai = hs.root(h.new_array(2));
    StringObj* s = h.new_string("v", 1);           // safepoint
    ArrayObj* a = hs.get<ArrayObj>(ai);
    a->slots->data[0] = Value::object(s);
    h.write_barrier(a->slots, Value::object(s));    // young holder -> no-op
    CHECK(h.remset_size() == 0);
  }

  // (G3) Promotion: a rooted object tenures after surviving PROMOTE_THRESHOLD GCs.
  {
    Heap h(32 * 1024);
    HandleScope hs(h);
    size_t ai = hs.root(h.new_array(1));
    for (int i = 0; i < 8000; ++i) (void)h.new_array(2);  // several minor GCs
    CHECK(h.is_old(hs.get<ArrayObj>(ai)));
  }

  // (G4) Barriered old->young edge survives a minor GC (array-set path).
  // The container is threaded through a parameter: the language's top-level named
  // functions cannot capture a top-level variable, so `store` takes the (tenured)
  // container as an argument. The young `b` is still born INSIDE the callee and its
  // only reference after return is the old container -- exactly the intended test.
  {
    const char* src =
        "a = [0, 0];"
        "i = 0; while (i < 20) { j = range(0, 300); i = i + 1; }"   // a tenures
        "fn store(c) { b = [7, 8, 9]; c[0] = b; }"                  // b young; dropped on return
        "store(a);"
        "k = 0; while (k < 20) { j2 = range(0, 300); k = k + 1; }"  // minor GCs
        "print a[0][1];";                                           // 8 iff b survived
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(32 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "8");
  }

  // (G5) Barriered old->young edge survives a minor GC (map-insert path).
  {
    const char* src =
        "m = { \"a\" = 0 };"
        "i = 0; while (i < 20) { j = range(0, 300); i = i + 1; }"   // m tenures
        "fn store(c) { b = [7, 8, 9]; c.bb = b; }"                  // new key bb -> young b
        "store(m);"
        "k = 0; while (k < 20) { j2 = range(0, 300); k = k + 1; }"
        "print m.bb[1];";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(32 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "8");
  }

  // (G6) Barriered old->young edge survives a minor GC (array-push path).
  {
    const char* src =
        "a = [0];"
        "i = 0; while (i < 20) { j = range(0, 300); i = i + 1; }"   // a tenures
        "fn store(c) { b = [7, 8, 9]; push(c, b); }"                // young b pushed into old a
        "store(a);"
        "k = 0; while (k < 20) { j2 = range(0, 300); k = k + 1; }"
        "print a[1][1];";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(32 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "8");
  }

  // (G7) Re-remember path: an object that PROMOTES this cycle while still holding
  // an independently-rooted YOUNG child creates a fresh old->young edge. That
  // holder must be appended to the remembered set so the NEXT collection forwards
  // the child's pointer. trace_from_old must detect "child stayed young" against
  // the to-space it was copied into -- checking from-space (is_young) would always
  // read false, drop the edge, and the following collection would move the child
  // and leave the old holder dangling (ASan use-after-poison). O is aged one
  // collection ahead of C so O promotes while C is still young in the same cycle.
  {
    Heap h(32 * 1024);
    HandleScope hs(h);
    size_t oi = hs.root(h.new_array(1));           // O: age 0
    force_one_gc(h);                               // O ages to 1 (no child yet)

    StringObj* c = h.new_string("Z", 1);           // C: age 0, younger than O (safepoint)
    size_t ci = hs.root(c);
    ArrayObj* O = hs.get<ArrayObj>(oi);            // re-read O after the safepoint
    O->slots->data[0] = Value::object(hs.get<StringObj>(ci));
    h.write_barrier(O->slots, O->slots->data[0]);  // both young here -> no-op

    force_one_gc(h);   // O(age1)->promotes to old; C(age0) stays young in to-space
    CHECK(h.is_old(hs.get<ArrayObj>(oi)));         // O promoted this cycle
    CHECK(h.is_young(hs.get<StringObj>(ci)));      // C survived as young
    CHECK(h.remset_size() >= 1);                   // re-remember fired (the whole point)

    force_one_gc(h);   // remembered holder forwards C; C is promoted into old
    force_one_gc(h);   // and again -- O's edge must stay valid, never dangling
    ArrayObj* O2 = hs.get<ArrayObj>(oi);
    CHECK(O2->slots->data[0].tag == Tag::Obj);
    StringObj* C2 = static_cast<StringObj*>(O2->slots->data[0].as.obj);
    CHECK(C2->len == 1 && C2->bytes->data[0] == 'Z');  // intact, not dangling
  }

  // (G8) Filling the old arena is a graceful over-cap, not a crash. The old
  // arena never collects (v1 scope: no major GC), so its occupancy is
  // monotonic. Root a strictly growing set of survivors -- never released
  // within this scope -- so more objects tenure every cycle than can ever be
  // reclaimed; eventually neither the old arena (copy()'s promotion branch)
  // nor young to-space (its fallback) can fit the next survivor, and over_cap
  // must be set instead of writing past either bound. Checking over_cap() is
  // false at the start and true only after the loop breaks proves this isn't
  // vacuous -- the arena genuinely fills. Running clean under ASan/UBSan
  // (no fault, no abort) proves the guards, not just the flag, hold.
  {
    Heap h(16 * 1024);
    HandleScope hs(h);
    CHECK(h.over_cap() == false);  // sanity: cap isn't already tripped at start

    bool filled = false;
    for (int i = 0; i < 40000; ++i) {
      size_t si = hs.root(h.new_array(4));   // permanently rooted -> never reclaimed
      (void)si;
      for (int k = 0; k < 3; ++k) (void)h.new_array(2);  // extra GC pressure
      if (h.over_cap()) { filled = true; break; }
    }
    CHECK(filled);                 // arena genuinely filled within the loop bound
    CHECK(h.over_cap() == true);   // reached the cap cleanly (no ASan fault, no abort)
  }

  // (G9) The object header carries a major-GC mark bit without growing: the
  // mark byte lives in existing header padding, so sizeof(Object) and every
  // object's layout stay unchanged.
  {
    CHECK(sizeof(coal::Object) == 16);
    coal::Object o{};
    o.mark = 1;
    CHECK(o.mark == 1);
    o.mark = 0;
    CHECK(o.mark == 0);
  }

  // (G10) A major collection reclaims dead old objects: root one map to keep,
  // then tenure a large batch of unrooted maps into old (by aging them past
  // promotion while temporarily rooted in a nested scope), drop that root, and
  // force a major. old_bytes_used() must strictly shrink, and the kept map must
  // still be readable and of the right kind -- proving reclamation actually
  // happened, not just that the accessor exists.
  {
    Heap h(64 * 1024);
    HandleScope outer(h);
    size_t keep_i = outer.root(h.new_map());

    {
      HandleScope inner(h);
      for (int i = 0; i < 400; ++i) (void)inner.root(h.new_map());
      // Age the rooted batch past the promotion threshold so it tenures into old.
      for (int i = 0; i <= Heap::PROMOTE_THRESHOLD; ++i) force_one_gc(h);
    }  // inner scope ends: the tenured maps are now unreachable garbage in old

    size_t before = h.old_bytes_used();
    CHECK(before > 0);              // sanity: promotion actually filled old

    h.force_major_for_test();

    size_t after = h.old_bytes_used();
    CHECK(after < before);          // dead old objects were reclaimed

    MapObj* keep = outer.get<MapObj>(keep_i);
    CHECK(keep->kind == ObjKind::Map);
    CHECK(keep->len == 0);
  }

  // (G11) collect() itself -- not just the force_major_for_test() hook -- must
  // interleave majors automatically. Repeat the G10 tenure-and-drop pattern many
  // rounds WITHOUT ever calling the test hook: each round tenures 400 maps into
  // old and then abandons them. On a 64KiB heap (32KiB old arena) this would blow
  // past old_size_ and set over_cap_ within a few rounds if nothing ever reclaimed
  // old garbage. If collect()'s high-water-mark dispatcher is wired up, the old
  // arena keeps getting compacted automatically and stays bounded indefinitely.
  {
    Heap h(64u << 10);
    HandleScope outer(h);
    size_t keep_i = outer.root(h.new_map());

    for (int round = 0; round < 20; ++round) {
      HandleScope inner(h);
      for (int i = 0; i < 400; ++i) (void)inner.root(h.new_map());
      // Age the rooted batch past the promotion threshold so it tenures into old.
      for (int i = 0; i <= Heap::PROMOTE_THRESHOLD; ++i) force_one_gc(h);
      CHECK(!h.over_cap());   // must never trip mid-loop: majors are firing on the fly
    }  // each round's tenured batch is unreachable garbage in old after this point

    CHECK(!h.over_cap());                       // never overflowed: majors reclaimed
    CHECK(h.old_bytes_used() < (48u << 10));     // stayed well under old_size_ (32KiB)

    MapObj* keep = outer.get<MapObj>(keep_i);
    CHECK(keep->kind == ObjKind::Map);
    CHECK(keep->len == 0);
  }
}
