# simply-cpp-cbor

`sc-cbor` reads and writes CBOR, the Concise Binary Object Representation
([RFC 8949](https://www.rfc-editor.org/rfc/rfc8949)): every data item as an `sc::cbor::value`,
decoded with the checks untrusted input needs and encoded in the preferred or the core
deterministic serialization. It shows CBOR as diagnostic notation and converts it to and from
JSON. It's our own code, with nothing but sc-core under it.

**Documentation:** the [simply-cpp wiki](https://github.com/roelofrossouw/simply-cpp/wiki) has the [sc-cbor reference](https://github.com/roelofrossouw/simply-cpp/wiki/Reference-sc-cbor).

```cpp
#include <sc_cbor.h>

const auto item = sc::cbor::decode(bytes);            // the whole input, one data item
item.find(3)->integer();                               // a map's value for the key 3
item.at("fmt").text();                                 // std::out_of_range when it's not there
sc::cbor::diagnostic(item);                            // {1: 2, 3: -7, -1: 1, -2: h'...'}

const auto cose = sc::cbor::value::map_of({{1, 2}, {3, -7}, {-2, sc::cbor::value::bytes(x)}});
const auto out = sc::cbor::encode(cose);               // preferred serialization
const auto same = sc::cbor::encode(cose, {.deterministic = true});   // keys sorted (4.2.1)

// Untrusted input that should be small and plain (WebAuthn, COSE):
sc::cbor::decode(bytes, sc::cbor::decode_options::strict());

nlohmann::json json = sc::cbor::to_json(item);         // section 6.1; from_json() back
```

| Type | | |
|---|---|---|
| `sc::cbor::value` | integers, `double`, `bool`, `nullptr`, text; `bytes()`, `negative()`, `tagged()`, `undefined()`, `simple_value()`, `array_of()`, `map_of()`; `type()`, `is_*()`, `integer()`, `unsigned_integer()`, `argument()`, `floating()`, `boolean()`, `byte_string()`, `text()`, `items()`, `entries()`, `tag()`, `content()`, `simple_number()`, `find()`, `at()`, `item()`, `size()`, `==` | a data item |
| `sc::cbor::array`, `sc::cbor::map` | vectors of values, of key/value pairs | a map keeps its order |
| `sc::cbor::decode_options` | `max_depth`, `max_items`, `max_length`, `indefinite_lengths`, `tags`, `duplicate_keys`, `text_keys_and_integers_only`, `check_utf8`, `check_standard_tags`; `strict()` | what decoding takes |
| `sc::cbor::encode_options` | `deterministic` | |
| `sc::cbor::error` | `what()`, `offset()` | where the input went wrong |

| Function | |
|---|---|
| `decode(bytes, options)` | the whole input as one item |
| `decode_prefix(bytes, used, options)` | the item at the start, and how many bytes it took |
| `decode_sequence(bytes, options)` | a CBOR sequence (RFC 8742) |
| `encode(value, options)` | preferred serialization, or core deterministic |
| `diagnostic(value)` | section 8's notation |
| `to_json(value)`, `from_json(json)` | section 6 |

- **Decoding** checks well-formedness (section 5.2: every one of Appendix F's examples is
  refused) and validity (5.3): text must be UTF-8, a map may not have a key twice (unless
  `duplicate_keys`, which keeps the last value), and tags 0 to 5, 24 and 32 to 36 must hold
  what they're defined to. Indefinite lengths decode to the same values as definite ones.
  `max_depth`, `max_items` and `max_length` bound what a hostile input can make it allocate
  or recurse into; `strict()` also refuses indefinite lengths and tags, and keys other than
  integers and text.
- **Integers** cover CBOR's whole range: 0 to 2^64-1 and -1 to -2^64 (`argument()` gives
  the head's number for the ones `integer()` can't hold). Bignums are tags 2 and 3.
- **Encoding** always writes the preferred serialization (section 4.1): the shortest head,
  definite lengths, and a float in the narrowest of half, single and double precision that
  keeps it exactly (NaN payloads too). `deterministic` also sorts map keys by their
  encodings' bytes and writes every NaN as `f97e00` (4.2.1).
- **Equality** is the same data item: the same type and content, floats by their bits (NaN
  equals NaN, 0.0 isn't -0.0), maps in the same order.
- **JSON** (section 6.1): bytes become base64url (base64 or base16 under tags 22 and 23),
  bignums numbers when they fit, other tags their content, NaN, infinities and undefined
  null, keys that aren't text their diagnostic notation.

The tests run RFC 8949's Appendix A examples both ways, Appendix F's not-well-formed
examples, and a fuzz run of random input.

## Building

`scripts/install.sh` builds, tests and installs the module; `sc-cbor-demo` shows a data
item, given in hex or read from a file, as diagnostic notation, JSON or its deterministic
encoding.

## License

Apache License 2.0 (see `LICENSE` and `NOTICE`).
