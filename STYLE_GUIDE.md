# Sub0Log Code Style Guide

Follows the Sub0Pipeline style guide. Only the deltas are recorded here.

## Naming

Identical to Sub0Pipeline -- lowercase namespaces, PascalCase types, camelCase
free functions, `c`-prefixed constants, trailing-underscore members -- with one
deliberate departure, below.

## Macros

Two categories, named differently on purpose.

**Call-site macros are named like the functions they stand in for**: lowercase,
`sub0log_` prefixed, no shouting.

```cpp
sub0log_debug(Vfs, "read {} at {} for {} bytes", contentId, offset, len);
```

These appear in ordinary application code, often many times in a function, and
the reader's question there is *what is being logged*, not *is this a macro*.
UPPER_SNAKE at every call site is visual noise that earns nothing: the argument
list already reads as a call, the name already carries a library prefix, and the
standard library itself sets this precedent with `assert`, `offsetof` and
`va_arg`. Qt does the same with `qDebug()`.

They are macros for two reasons, both verified rather than assumed:

- The per-call-site descriptor must be a distinct object with static storage
  duration. A function template instantiated on the same argument types at two
  different lines shares one static. `std::source_location` as a defaulted
  parameter yields the right *values* but not a distinct *object*, and
  `__builtin_LINE()` as a default template argument is evaluated at the point of
  declaration by MSVC, collapsing to a single instantiation.
- A macro does not evaluate its arguments when the site is disabled. A function
  call evaluates every argument first, so `sub0log_trace(S, "{}", expensive())`
  would run `expensive()` at `Error` level. This is what makes "compiled into
  every build, disabled cost is a masked load" true rather than aspirational.

**Every other macro stays UPPER_SNAKE with a `SUB0LOG_` prefix** -- feature
flags, configuration defaults, platform guards, internal expansion helpers:

```cpp
#ifndef SUB0LOG_SEGMENT_BYTES
#  define SUB0LOG_SEGMENT_BYTES (4u * 1024u * 1024u)
#endif
```

The rule is about the audience, not the mechanism: a name a library consumer
writes in ordinary code reads as a call; a name that configures the build or
expands inside the library shouts, because there the reader's question really is
*is this the preprocessor*.

## Modern C++ first

Reach for a language feature before a macro, and prefer the newest form the
project's standard supports. C++23 is the baseline; C++26 is not available, so
`_` as a reusable discarded-variable placeholder (P2169) and static reflection
are out of scope until it is.

A macro is justified only where a language feature cannot express the
requirement, and that justification is written down with the evidence -- as
above. "It has always been a macro" is not a reason.
