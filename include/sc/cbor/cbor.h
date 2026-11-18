#pragma once

// CBOR, the Concise Binary Object Representation (RFC 8949): a data item as a value, decoded from
// bytes and encoded to them, with diagnostic notation (section 8) and the JSON conversions of
// section 6.
//
//     const auto item = sc::cbor::decode(bytes);                // throws sc::cbor::error
//     if (const auto *alg = item.find(3)) std::cout << alg->integer();
//     const auto out = sc::cbor::encode(sc::cbor::value::map_of({{1, 2}, {3, -7}}));
//     std::cout << sc::cbor::diagnostic(item);                  // {1: 2, 3: -7}
//
// Decoding checks well-formedness (section 5.2) and validity (5.3: UTF-8 text, no duplicate map
// keys, the standard tags' content), within limits a caller sets; decode_options::strict() is for
// untrusted input that should be small and plain (no indefinite lengths, no tags). Encoding uses
// the preferred serialization (4.1: shortest heads, shortest exact float, definite lengths);
// encode_options::deterministic sorts map keys too (4.2.1, core deterministic encoding).

#include <nlohmann/json.hpp>

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sc::cbor {
    // Input that isn't well-formed or valid CBOR, or that a limit refuses. offset() is where in the
    // input it was found.
    class error : public std::runtime_error {
    public:
        error(const std::string &message, const std::size_t offset) : std::runtime_error(message), offset_(offset) {}

        [[nodiscard]] std::size_t offset() const { return offset_; }

    private:
        std::size_t offset_;
    };

    class value;
    using array = std::vector<value>;
    // A map keeps its entries in the order they came (or were added).
    using map = std::vector<std::pair<value, value>>;

    // What a data item is: a major type, or for major type 7 a float or a simple value.
    enum class type { unsigned_integer, negative_integer, bytes, text, array, map, tag, simple, floating };

    // The simple values with names (section 3.3); others are numbers 0 to 19 and 32 to 255.
    namespace simple {
        constexpr std::uint8_t false_value = 20, true_value = 21, null = 22, undefined = 23;
    }

    class value {
    public:
        // Null.
        value();
        // Integers: unsigned_integer from 0 up, negative_integer below.
        value(int number);
        value(long number);
        value(long long number);
        value(unsigned int number);
        value(unsigned long number);
        value(unsigned long long number);
        value(double number);
        value(bool truth);
        value(std::nullptr_t);
        // Text (UTF-8). Bytes are made with value::bytes(), so a string isn't taken for them.
        value(std::string text);
        value(std::string_view text);
        value(const char *text);
        value(cbor::array items);
        value(cbor::map entries);

        [[nodiscard]] static value bytes(std::string data);
        // A negative integer -1 - argument, for those below INT64_MIN (down to -2^64).
        [[nodiscard]] static value negative(std::uint64_t argument);
        [[nodiscard]] static value tagged(std::uint64_t tag, value content);
        [[nodiscard]] static value undefined();
        // Simple value number (0 to 19, 32 to 255; 20 to 23 are false, true, null, undefined).
        [[nodiscard]] static value simple_value(std::uint8_t number);
        [[nodiscard]] static value array_of(std::initializer_list<value> items);
        [[nodiscard]] static value map_of(std::initializer_list<std::pair<value, value>> entries);

        [[nodiscard]] cbor::type type() const { return type_; }
        [[nodiscard]] bool is_integer() const { return type_ == type::unsigned_integer || type_ == type::negative_integer; }
        [[nodiscard]] bool is_bytes() const { return type_ == type::bytes; }
        [[nodiscard]] bool is_text() const { return type_ == type::text; }
        [[nodiscard]] bool is_array() const { return type_ == type::array; }
        [[nodiscard]] bool is_map() const { return type_ == type::map; }
        [[nodiscard]] bool is_tag() const { return type_ == type::tag; }
        [[nodiscard]] bool is_float() const { return type_ == type::floating; }
        [[nodiscard]] bool is_bool() const;
        [[nodiscard]] bool is_null() const;
        [[nodiscard]] bool is_undefined() const;

        // The value, or std::logic_error when it isn't that kind (or an integer doesn't fit).
        [[nodiscard]] std::int64_t integer() const;
        [[nodiscard]] std::uint64_t unsigned_integer() const;
        // An integer's head argument: the number itself, or n for -1 - n.
        [[nodiscard]] std::uint64_t argument() const;
        [[nodiscard]] double floating() const; // a float, or an integer as a double
        [[nodiscard]] bool boolean() const;
        [[nodiscard]] const std::string &byte_string() const;
        [[nodiscard]] const std::string &text() const;
        [[nodiscard]] const cbor::array &items() const;
        [[nodiscard]] cbor::array &items();
        [[nodiscard]] const cbor::map &entries() const;
        [[nodiscard]] cbor::map &entries();
        [[nodiscard]] std::uint64_t tag() const;
        [[nodiscard]] const value &content() const; // a tag's
        [[nodiscard]] std::uint8_t simple_number() const;

        // A map's value for a key, nullptr when there's none or this isn't a map.
        [[nodiscard]] const value *find(const value &key) const;
        // A map's value for a key, std::out_of_range when there's none; an array's item, the same.
        // (Two names, so at(1) on a map with integer keys can't mean the second item.)
        [[nodiscard]] const value &at(const value &key) const;
        [[nodiscard]] const value &item(std::size_t index) const;
        [[nodiscard]] std::size_t size() const; // items, entries, bytes or text bytes

        // The same data item: same type and the same content, floats compared by their bits (so
        // NaN equals NaN and 0.0 doesn't equal -0.0); maps in the same order.
        friend bool operator==(const value &a, const value &b);

    private:
        cbor::type type_ = cbor::type::simple;
        std::uint64_t number_ = simple::null; // an integer's argument, a tag, a simple value
        double floating_ = 0;
        std::string string_;                  // bytes or text
        cbor::array items_;                   // array items, or a tag's content as its one item
        cbor::map entries_;
    };

    struct decode_options {
        std::size_t max_depth = 256;           // arrays, maps and tags nested
        std::size_t max_items = 1 << 22;       // array items and map entries, all levels together
        std::size_t max_length = 1 << 26;      // one byte or text string, chunks together
        bool indefinite_lengths = true;        // section 3.2.1-3.2.3
        bool tags = true;                      // section 3.4; off: any tag is an error
        bool duplicate_keys = false;           // section 5.6: on keeps the first one's place and the last value
        bool text_keys_and_integers_only = false; // map keys must be integers or text (COSE, CTAP)
        bool check_utf8 = true;                // text must be UTF-8 (5.3.1)
        bool check_standard_tags = true;       // tags 0 to 5, 21 to 24, 32 to 36 hold what they must (5.3.2)

        // For untrusted input expected to be small and plain, as WebAuthn's: 16 levels, 4096
        // items, 64 KiB strings, no indefinite lengths, no tags, keys integers or text only.
        [[nodiscard]] static decode_options strict();
    };

    // The whole input as one data item: an error when anything follows it.
    [[nodiscard]] value decode(std::string_view input, const decode_options &options = {});
    // The data item at the start of input; used gets the bytes it took.
    [[nodiscard]] value decode_prefix(std::string_view input, std::size_t &used, const decode_options &options = {});
    // A CBOR sequence (RFC 8742): data items one after the other, none or more.
    [[nodiscard]] std::vector<value> decode_sequence(std::string_view input, const decode_options &options = {});

    struct encode_options {
        // Core deterministic encoding (4.2.1): map keys sorted by their encodings' bytes, and NaN
        // as 0xf97e00. Without it, maps keep their order.
        bool deterministic = false;
    };

    // Preferred serialization: shortest heads, definite lengths, floats in the shortest of half,
    // single and double that keeps the value exactly.
    [[nodiscard]] std::string encode(const value &item, const encode_options &options = {});

    // Diagnostic notation (section 8): 1, -7, h'0102', "text", [1, 2], {1: 2}, 0("2013-03-21T20:04:00Z"),
    // 1.5, NaN, Infinity, true, null, undefined, simple(16).
    [[nodiscard]] std::string diagnostic(const value &item);

    // To JSON (section 6.1): bytes as base64url, bignums (tags 2 and 3) as numbers when they fit,
    // other tags as their content, NaN and infinities null, undefined null, map keys that aren't
    // text as their diagnostic notation.
    [[nodiscard]] nlohmann::json to_json(const value &item);
    // From JSON (section 6.2): integers as integers, other numbers as floats, the rest as is.
    [[nodiscard]] value from_json(const nlohmann::json &json);
}
