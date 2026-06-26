# charcoal

Charcoal is a small embeddable scripting language that compiles to its own bytecode and runs on a register virtual machine. I started it because I wanted to actually understand how a language goes from text you type to instructions a machine runs, and the only way I've ever really learned something is by building the whole thing myself and watching where it breaks.

The thing I care about most here is safety. A scripting VM spends its whole life running input it didn't write, so the moment you trust a byte you shouldn't, you've handed an attacker the keys. The whole pipeline treats untrusted input as hostile from the first byte to the last, and I lean on AddressSanitizer, UndefinedBehaviorSanitizer, and fuzzing to prove it rather than just hoping.

A program takes the same trip every time: `source → lex → parse → compile → serialize → load → verify → run`. The interesting split is that compiling produces a `.cbc` file, and running one starts over from an untrusted buffer — it loads, verifies, and only then executes. The verifier is the gate the interpreter trusts, so once a module passes it, every opcode is known, every operand is in range, and every jump lands on a real instruction boundary.

## Building

You'll need CMake and a C++17 compiler.

```
cmake -S . -B build
cmake --build build
```

I do all my real work with the sanitizers on, and I'd recommend you do too:

```
cmake -S . -B build -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
cmake --build build
```

Run the test suite with `./build/charcoal_tests` — it prints `ok` when everything passes, which is the whole point. The `charcoal` binary itself does two things, compile a `.char` source file to bytecode and run the bytecode:

```
charcoal compile prog.char -o prog.cbc
charcoal run prog.cbc
```

A missing file, garbage bytes, or a syntax error all come back as a clear message on stderr and a non-zero exit code. Nothing crashes, which is sort of the recurring theme of this whole project.

## The .char language

It's small on purpose. Here's most of it in one go:

```
fn dbl(a) { return a + a; }
print dbl(21);              // 42

x = [10, 20, 30];
print x[1];                // 20

m = { "a" = 5 };
print m.a;                 // 5

i = 3;
while (i) { i = i - 1; }
print i;                   // 0

if (0) { print 1; } else { print 2; }   // 2
```

You get integers, doubles, strings, booleans, nil, arrays, and maps. Functions are top-level, take parameters, and can call each other and recurse. There's `if`/`else`, `while`, assignment, the four arithmetic operators with normal precedence, indexing with `[]`, field access with `.`, and `print`. One quirk worth calling out: maps use `=` between key and value (`{ "a" = 5 }`) instead of the colon you might expect, because the lexer has no colon token yet. That's it for the language right now, and honestly it's enough to exercise every interesting part of the VM.

## The .cbc format

A `.cbc` file is the compiled module — a constant pool and a function table behind a small header. Everything is little-endian, and it's written byte-by-byte on purpose so the layout doesn't depend on the host's struct padding or endianness. The loader bounds-checks every read against the file size, so a truncated or lying file is rejected, never followed off a cliff.

### Header

| Field         | Type        | Notes                                  |
|---------------|-------------|----------------------------------------|
| magic         | 4 bytes     | `'C' 'B' 'C' 0x01`                     |
| version       | u16         | currently `1`                          |
| flags         | u16         | reserved, currently `0`                |
| const_count   | u32         | number of constants in the pool        |
| func_count    | u32         | number of functions in the table       |
| entry_func    | u32         | index into the function table to run   |

### Constant pool

`const_count` entries, each a one-byte tag followed by a payload that depends on the tag:

| Tag        | Value | Payload                                 |
|------------|-------|-----------------------------------------|
| `Int`      | 0     | i64 (8 bytes)                           |
| `Double`   | 1     | f64 bit pattern (8 bytes)               |
| `Str`      | 2     | u32 length, then that many raw bytes    |
| `Nil`      | 3     | nothing                                 |
| `Bool`     | 4     | u8 (`0` or `1`)                         |

### Function table

`func_count` entries, each laid out as:

| Field        | Type | Notes                                            |
|--------------|------|--------------------------------------------------|
| name_const   | u32  | index of a `Str` constant holding the name       |
| num_params   | u8   | parameter count                                  |
| num_regs     | u8   | size of the register window (≥ num_params)       |
| max_stack    | u16  | advisory for now                                 |
| code_len     | u32  | length of the bytecode that follows              |
| code         | bytes| `code_len` bytes of instructions                 |

### Bytecode

Instructions are a one-byte opcode followed by fixed-width operands: `r` is a u8 register, `k` is a u16 constant-pool index, `n` is a u8 count, and `o` is an i16 jump offset measured from the byte right after the operand.

| Opcode          | Hex  | Operands | Meaning                              |
|-----------------|------|----------|--------------------------------------|
| `HALT`          | 0x00 | —        | stop                                 |
| `LOAD_CONST`    | 0x01 | r, k     | `r = consts[k]`                      |
| `LOAD_NIL`      | 0x02 | r        | `r = nil`                            |
| `MOVE`          | 0x03 | r, r2    | `r = r2`                             |
| `ADD`/`SUB`/`MUL`/`DIV` | 0x10–0x13 | r, r2, r3 | `r = r2 op r3`              |
| `NEW_ARRAY`     | 0x20 | r, n     | `r = array of length n`              |
| `ARRAY_GET`     | 0x21 | r, r2, r3| `r = r2[r3]`                         |
| `ARRAY_SET`     | 0x22 | r, r2, r3| `r[r2] = r3`                         |
| `NEW_OBJECT`    | 0x23 | r        | `r = empty map`                      |
| `GET_PROP`      | 0x24 | r, r2, k | `r = r2.(consts[k])`                 |
| `SET_PROP`      | 0x25 | r, k, r2 | `r.(consts[k]) = r2`                 |
| `CALL`          | 0x30 | r, k, n  | call function named `consts[k]` with n args at `r`, result in `r` |
| `RET`           | 0x31 | r        | return `r`                           |
| `JUMP`          | 0x40 | o        | `pc += o`                            |
| `JUMP_IF_FALSE` | 0x41 | r, o     | jump if `r` is falsey                |
| `PRINT`         | 0x50 | r        | print `r`                            |

## Status

Charcoal runs the whole pipeline above end to end, with a unit suite and two
fuzzing harnesses kept passing under AddressSanitizer and UndefinedBehaviorSanitizer.
It's a work in progress and the language is intentionally small; I add features
when I have a use for them rather than for completeness.
