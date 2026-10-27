#include "sc_cbor.h"

#include <base64.h>
#include <utf8.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <set>

namespace sc::cbor {
    namespace {
        // --- floats

        // A half-precision float's bits as a double (RFC 8949 Appendix D).
        double from_half(const std::uint16_t bits) {
            const int exponent = bits >> 10 & 0x1f, mantissa = bits & 0x3ff;
            double out;
            if (exponent == 0) out = std::ldexp(mantissa, -24);
            else if (exponent != 31) out = std::ldexp(mantissa + 1024, exponent - 25);
            else out = mantissa == 0 ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
            return bits & 0x8000 ? -out : out;
        }

        // The half-precision bits for a double that one holds exactly, or nothing.
        bool to_half(const double number, std::uint16_t &bits) {
            if (std::isnan(number)) return false; // NaN's payload is kept only in the double's own width
            const std::uint16_t sign = std::signbit(number) ? 0x8000 : 0;
            if (std::isinf(number)) {
                bits = sign | 0x7c00;
                return true;
            }
            const double magnitude = std::fabs(number);
            if (magnitude == 0) {
                bits = sign;
                return true;
            }
            int exponent = 0;
            const double fraction = std::frexp(magnitude, &exponent); // magnitude = fraction * 2^exponent, fraction in [0.5, 1)
            if (exponent >= -13 && exponent <= 16) {
                // Normal: 1.m * 2^(exponent - 1), m 10 bits.
                const double scaled = std::ldexp(fraction, 11);
                if (scaled != std::floor(scaled)) return false;
                bits = static_cast<std::uint16_t>(sign | (exponent + 14) << 10 | (static_cast<int>(scaled) - 1024));
                return true;
            }
            if (exponent < -13 && exponent >= -23) {
                // Subnormal: m * 2^-24.
                const double scaled = std::ldexp(magnitude, 24);
                if (scaled != std::floor(scaled)) return false;
                bits = static_cast<std::uint16_t>(sign | static_cast<int>(scaled));
                return true;
            }
            return false;
        }

        // --- reading

        struct reader {
            std::string_view input;
            const decode_options &options;
            std::size_t at = 0;
            std::size_t items = 0;

            [[noreturn]] void fail(const std::string &message, const std::size_t where) const { throw error("CBOR: " + message, where); }

            std::uint8_t byte() {
                if (at >= input.size()) fail("cut short", at);
                return static_cast<std::uint8_t>(input[at++]);
            }

            // A head's argument for additional information 0 to 27; 28 to 30 are reserved, 31 the
            // caller's (indefinite length, or break).
            std::uint64_t argument(const std::uint8_t info, const std::size_t head_at) {
                if (info < 24) return info;
                if (info > 27) fail("reserved additional information " + std::to_string(info), head_at);
                const std::size_t size = std::size_t{1} << (info - 24);
                if (input.size() - at < size) fail("cut short", at);
                std::uint64_t out = 0;
                for (std::size_t i = 0; i < size; ++i) out = out << 8 | static_cast<std::uint8_t>(input[at++]);
                return out;
            }

            void count(const std::uint64_t more, const std::size_t where) {
                if (more > options.max_items || items + more > options.max_items)
                    fail("more than " + std::to_string(options.max_items) + " array items and map entries", where);
                items += static_cast<std::size_t>(more);
            }

            std::string string_of(const std::uint64_t length, const std::size_t where) {
                if (length > options.max_length) fail("a string longer than " + std::to_string(options.max_length) + " bytes", where);
                if (input.size() - at < length) fail("cut short", at);
                std::string out{input.substr(at, static_cast<std::size_t>(length))};
                at += static_cast<std::size_t>(length);
                return out;
            }

            // Byte or text string: definite, or indefinite as definite chunks of the same major type.
            std::string string_item(const int major, const std::uint8_t info, const std::size_t head_at) {
                if (info != 31) return string_of(argument(info, head_at), head_at);
                if (!options.indefinite_lengths) fail("an indefinite length", head_at);
                std::string out;
                while (true) {
                    const auto chunk_at = at;
                    const auto head = byte();
                    if (head == 0xff) break;
                    if (head >> 5 != major) fail("a chunk of another type in an indefinite length string", chunk_at);
                    if ((head & 0x1f) == 31) fail("an indefinite length chunk inside an indefinite length string", chunk_at);
                    const auto chunk = string_of(argument(head & 0x1f, chunk_at), chunk_at);
                    // Each chunk of text must be UTF-8 on its own.
                    if (major == 3 && options.check_utf8 && !utf8::is_valid(chunk)) fail("text that isn't UTF-8", chunk_at);
                    if (out.size() + chunk.size() > options.max_length) fail("a string longer than " + std::to_string(options.max_length) + " bytes", chunk_at);
                    out += chunk;
                }
                return out;
            }

            bool at_break() {
                if (at >= input.size()) fail("cut short", at);
                if (static_cast<std::uint8_t>(input[at]) != 0xff) return false;
                ++at;
                return true;
            }

            void check_key(const value &key, std::set<std::string> &seen, const std::size_t where) {
                if (options.text_keys_and_integers_only && !key.is_integer() && !key.is_text()) fail("a map key that isn't an integer or text", where);
                if (!options.duplicate_keys && !seen.insert(encode(key)).second) fail("a map key twice", where);
            }

            void check_tag(const std::uint64_t tag, const value &content, const std::size_t where) {
                if (!options.check_standard_tags) return;
                const auto wrong = [&](const char *what) { fail("tag " + std::to_string(tag) + " holds " + what, where); };
                switch (tag) {
                    case 0: case 32: case 33: case 34: case 35: case 36:
                        if (!content.is_text()) wrong("something other than text");
                        break;
                    case 1:
                        if (!content.is_integer() && !content.is_float()) wrong("something other than a number");
                        break;
                    case 2: case 3: case 24:
                        if (!content.is_bytes()) wrong("something other than bytes");
                        break;
                    case 4: case 5: {
                        if (!content.is_array() || content.size() != 2 || !content.item(0).is_integer() ||
                            !(content.item(1).is_integer() || (content.item(1).is_tag() && (content.item(1).tag() == 2 || content.item(1).tag() == 3))))
                            wrong("something other than [exponent, mantissa]");
                        break;
                    }
                    default: break;
                }
            }

            value item(const std::size_t depth) {
                const auto head_at = at;
                const auto head = byte();
                const int major = head >> 5;
                const auto info = static_cast<std::uint8_t>(head & 0x1f);
                switch (major) {
                    case 0: return value{static_cast<unsigned long long>(argument(info, head_at))};
                    case 1: return value::negative(argument(info, head_at));
                    case 2: return value::bytes(string_item(2, info, head_at));
                    case 3: {
                        auto text = string_item(3, info, head_at);
                        if (info != 31 && options.check_utf8 && !utf8::is_valid(text)) fail("text that isn't UTF-8", head_at);
                        return value{std::move(text)};
                    }
                    case 4:
                    case 5: {
                        if (depth >= options.max_depth) fail("nested deeper than " + std::to_string(options.max_depth), head_at);
                        const bool indefinite = info == 31;
                        if (indefinite && !options.indefinite_lengths) fail("an indefinite length", head_at);
                        const auto length = indefinite ? 0 : argument(info, head_at);
                        if (!indefinite) count(length, head_at);
                        if (major == 4) {
                            array out;
                            if (!indefinite) {
                                out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(length, input.size() - at)));
                                for (std::uint64_t i = 0; i < length; ++i) out.push_back(item(depth + 1));
                            } else {
                                while (!at_break()) {
                                    count(1, at);
                                    out.push_back(item(depth + 1));
                                }
                            }
                            return value{std::move(out)};
                        }
                        map out;
                        std::set<std::string> seen;
                        const auto entry = [&] {
                            const auto key_at = at;
                            auto key = item(depth + 1);
                            check_key(key, seen, key_at);
                            if (indefinite && at < input.size() && static_cast<std::uint8_t>(input[at]) == 0xff) fail("a break between a map key and its value", at);
                            auto content = item(depth + 1);
                            if (options.duplicate_keys) {
                                const auto same = std::ranges::find_if(out, [&](const auto &e) { return e.first == key; });
                                if (same != out.end()) {
                                    same->second = std::move(content);
                                    return;
                                }
                            }
                            out.emplace_back(std::move(key), std::move(content));
                        };
                        if (!indefinite) {
                            out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(length, input.size() - at)));
                            for (std::uint64_t i = 0; i < length; ++i) entry();
                        } else {
                            while (!at_break()) {
                                count(1, at);
                                entry();
                            }
                        }
                        return value{std::move(out)};
                    }
                    case 6: {
                        if (!options.tags) fail("a tag", head_at);
                        if (info == 31) fail("an indefinite length tag", head_at);
                        const auto tag = argument(info, head_at);
                        if (depth >= options.max_depth) fail("nested deeper than " + std::to_string(options.max_depth), head_at);
                        auto content = item(depth + 1);
                        check_tag(tag, content, head_at);
                        return value::tagged(tag, std::move(content));
                    }
                    default: {
                        if (info < 24) return value::simple_value(info);
                        switch (info) {
                            case 24: {
                                const auto number = byte();
                                if (number < 32) fail("a two-byte simple value below 32", head_at);
                                return value::simple_value(number);
                            }
                            case 25: return value{from_half(static_cast<std::uint16_t>(argument(25, head_at)))};
                            case 26: {
                                const auto bits = static_cast<std::uint32_t>(argument(26, head_at));
                                float out;
                                std::memcpy(&out, &bits, sizeof out);
                                return value{static_cast<double>(out)};
                            }
                            case 27: {
                                const auto bits = argument(27, head_at);
                                double out;
                                std::memcpy(&out, &bits, sizeof out);
                                return value{out};
                            }
                            case 31: fail("a break outside an indefinite length item", head_at);
                            default: fail("reserved additional information " + std::to_string(info), head_at);
                        }
                    }
                }
            }
        };

        // --- writing

        void head(std::string &out, const int major, const std::uint64_t argument) {
            const auto type = static_cast<std::uint8_t>(major << 5);
            if (argument < 24) {
                out += static_cast<char>(type | argument);
                return;
            }
            const int size = argument <= 0xff ? 1 : argument <= 0xffff ? 2 : argument <= 0xffffffff ? 4 : 8;
            out += static_cast<char>(type | (size == 1 ? 24 : size == 2 ? 25 : size == 4 ? 26 : 27));
            for (int i = size - 1; i >= 0; --i) out += static_cast<char>(argument >> (8 * i) & 0xff);
        }

        void write_float(std::string &out, const double number, const bool deterministic) {
            std::uint16_t half = 0;
            if (std::isnan(number)) {
                // The narrowest width that keeps its sign and payload (a plain quiet NaN: 0xf97e00);
                // deterministic encoding writes every NaN as that one.
                std::uint64_t bits;
                std::memcpy(&bits, &number, sizeof bits);
                const auto payload = bits & 0x000fffffffffffffull;
                if (deterministic) half = 0x7e00;
                else if ((payload & 0x3ffffffffffull) == 0) half = static_cast<std::uint16_t>((bits >> 48 & 0x8000) | 0x7c00 | payload >> 42);
                if (half != 0) {
                    out += static_cast<char>(0xf9);
                    out += static_cast<char>(half >> 8);
                    out += static_cast<char>(half & 0xff);
                    return;
                }
                if ((payload & 0x1fffffffull) == 0) {
                    const auto single = static_cast<std::uint32_t>((bits >> 32 & 0x80000000u) | 0x7f800000u | payload >> 29);
                    out += static_cast<char>(0xfa);
                    for (int i = 3; i >= 0; --i) out += static_cast<char>(single >> (8 * i) & 0xff);
                    return;
                }
            } else if (to_half(number, half)) {
                out += static_cast<char>(0xf9);
                out += static_cast<char>(half >> 8);
                out += static_cast<char>(half & 0xff);
                return;
            }
            const auto single = static_cast<float>(number);
            if (!std::isnan(number) && static_cast<double>(single) == number) {
                std::uint32_t bits;
                std::memcpy(&bits, &single, sizeof bits);
                out += static_cast<char>(0xfa);
                for (int i = 3; i >= 0; --i) out += static_cast<char>(bits >> (8 * i) & 0xff);
                return;
            }
            std::uint64_t bits;
            std::memcpy(&bits, &number, sizeof bits);
            out += static_cast<char>(0xfb);
            for (int i = 7; i >= 0; --i) out += static_cast<char>(bits >> (8 * i) & 0xff);
        }

        void write(std::string &out, const value &item, const encode_options &options) {
            switch (item.type()) {
                case type::unsigned_integer: head(out, 0, item.argument()); break;
                case type::negative_integer: head(out, 1, item.argument()); break;
                case type::bytes: head(out, 2, item.byte_string().size()); out += item.byte_string(); break;
                case type::text: head(out, 3, item.text().size()); out += item.text(); break;
                case type::array:
                    head(out, 4, item.items().size());
                    for (const auto &each: item.items()) write(out, each, options);
                    break;
                case type::map: {
                    head(out, 5, item.entries().size());
                    if (!options.deterministic) {
                        for (const auto &[key, content]: item.entries()) {
                            write(out, key, options);
                            write(out, content, options);
                        }
                        break;
                    }
                    std::vector<std::pair<std::string, std::string>> encoded;
                    encoded.reserve(item.entries().size());
                    for (const auto &[key, content]: item.entries()) {
                        std::string k, c;
                        write(k, key, options);
                        write(c, content, options);
                        encoded.emplace_back(std::move(k), std::move(c));
                    }
                    std::ranges::sort(encoded, [](const auto &a, const auto &b) { return a.first < b.first; });
                    for (const auto &[k, c]: encoded) out += k + c;
                    break;
                }
                case type::tag: head(out, 6, item.tag()); write(out, item.content(), options); break;
                case type::floating: write_float(out, item.floating(), options.deterministic); break;
                case type::simple: {
                    const auto number = item.simple_number();
                    if (number < 24) out += static_cast<char>(0xe0 | number);
                    else {
                        out += static_cast<char>(0xf8);
                        out += static_cast<char>(number);
                    }
                    break;
                }
            }
        }

        // --- diagnostic notation

        std::string hex(const std::string_view bytes) {
            static constexpr char digits[] = "0123456789abcdef";
            std::string out;
            out.reserve(bytes.size() * 2);
            for (const auto c: bytes) {
                out += digits[static_cast<std::uint8_t>(c) >> 4];
                out += digits[static_cast<std::uint8_t>(c) & 0xf];
            }
            return out;
        }

        std::string json_text(const std::string_view text) { return nlohmann::json(std::string{text}).dump(); }

        std::string float_text(const double number) {
            if (std::isnan(number)) return "NaN";
            if (std::isinf(number)) return number > 0 ? "Infinity" : "-Infinity";
            char buffer[64];
            const auto [end, problem] = std::to_chars(buffer, buffer + sizeof buffer, number);
            std::string out{buffer, end};
            // A point always (1.0, 1.0e+300), and an exponent without leading zeros (e-8, not e-08).
            const auto e = out.find('e');
            auto mantissa = out.substr(0, e);
            if (mantissa.find('.') == std::string::npos) mantissa += ".0";
            if (e == std::string::npos) return mantissa;
            auto exponent = out.substr(e + 1);
            const auto sign = exponent.front() == '-' || exponent.front() == '+' ? std::string(1, exponent.front()) : std::string{"+"};
            if (exponent.front() == '-' || exponent.front() == '+') exponent.erase(0, 1);
            while (exponent.size() > 1 && exponent.front() == '0') exponent.erase(0, 1);
            return mantissa + "e" + sign + exponent;
        }

        std::string negative_text(const std::uint64_t argument) {
            if (argument == std::numeric_limits<std::uint64_t>::max()) return "-18446744073709551616";
            return "-" + std::to_string(argument + 1);
        }

        void diagnose(std::string &out, const value &item) {
            switch (item.type()) {
                case type::unsigned_integer: out += std::to_string(item.argument()); break;
                case type::negative_integer: out += negative_text(item.argument()); break;
                case type::bytes: out += "h'" + hex(item.byte_string()) + "'"; break;
                case type::text: out += json_text(item.text()); break;
                case type::array: {
                    out += '[';
                    bool first = true;
                    for (const auto &each: item.items()) {
                        if (!first) out += ", ";
                        first = false;
                        diagnose(out, each);
                    }
                    out += ']';
                    break;
                }
                case type::map: {
                    out += '{';
                    bool first = true;
                    for (const auto &[key, content]: item.entries()) {
                        if (!first) out += ", ";
                        first = false;
                        diagnose(out, key);
                        out += ": ";
                        diagnose(out, content);
                    }
                    out += '}';
                    break;
                }
                case type::tag: out += std::to_string(item.tag()) + "("; diagnose(out, item.content()); out += ')'; break;
                case type::floating: out += float_text(item.floating()); break;
                case type::simple: {
                    const auto number = item.simple_number();
                    out += number == simple::false_value  ? "false"
                           : number == simple::true_value ? "true"
                           : number == simple::null       ? "null"
                           : number == simple::undefined  ? "undefined"
                                                          : "simple(" + std::to_string(number) + ")";
                    break;
                }
            }
        }

        // --- JSON

        // A bignum's bytes as a number, when they fit.
        std::optional<std::uint64_t> bignum(const std::string &bytes) {
            std::size_t start = 0;
            while (start < bytes.size() && bytes[start] == '\0') ++start;
            if (bytes.size() - start > 8) return std::nullopt;
            std::uint64_t out = 0;
            for (auto i = start; i < bytes.size(); ++i) out = out << 8 | static_cast<std::uint8_t>(bytes[i]);
            return out;
        }

        // How byte strings in a tag 21, 22 or 23 are written (section 6.1: base64url, base64, base16).
        enum class bytes_as { base64url, base64, base16 };

        nlohmann::json json_of(const value &item, const bytes_as encoding) {
            switch (item.type()) {
                case type::unsigned_integer: return item.argument();
                case type::negative_integer:
                    if (item.argument() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return item.integer();
                    return -1.0 - static_cast<double>(item.argument());
                case type::bytes:
                    return encoding == bytes_as::base64 ? base64::encode(item.byte_string())
                           : encoding == bytes_as::base16 ? hex(item.byte_string())
                                                          : base64::encode_url(item.byte_string());
                case type::text: return item.text();
                case type::array: {
                    auto out = nlohmann::json::array();
                    for (const auto &each: item.items()) out.push_back(json_of(each, encoding));
                    return out;
                }
                case type::map: {
                    auto out = nlohmann::json::object();
                    for (const auto &[key, content]: item.entries()) out[key.is_text() ? key.text() : diagnostic(key)] = json_of(content, encoding);
                    return out;
                }
                case type::tag: {
                    const auto tag = item.tag();
                    const auto &content = item.content();
                    if ((tag == 2 || tag == 3) && content.is_bytes()) {
                        // A bignum: a number when it fits, else base64url ("~" for a negative one).
                        if (const auto n = bignum(content.byte_string())) {
                            if (tag == 2) return *n;
                            if (*n <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return -1 - static_cast<std::int64_t>(*n);
                        }
                        return (tag == 3 ? "~" : "") + base64::encode_url(content.byte_string());
                    }
                    if (tag == 21) return json_of(content, bytes_as::base64url);
                    if (tag == 22) return json_of(content, bytes_as::base64);
                    if (tag == 23) return json_of(content, bytes_as::base16);
                    return json_of(content, encoding);
                }
                case type::floating: {
                    const auto number = item.floating();
                    if (std::isnan(number) || std::isinf(number)) return nullptr;
                    return number;
                }
                case type::simple:
                    if (item.simple_number() == simple::false_value) return false;
                    if (item.simple_number() == simple::true_value) return true;
                    return nullptr;
            }
            return nullptr;
        }

        [[noreturn]] void not_a(const char *what) { throw std::logic_error(std::string{"sc::cbor::value: not "} + what); }
    }

    // --- value

    value::value() = default;
    value::value(const int number) : value(static_cast<long long>(number)) {}
    value::value(const long number) : value(static_cast<long long>(number)) {}
    value::value(const long long number) {
        type_ = number >= 0 ? type::unsigned_integer : type::negative_integer;
        number_ = number >= 0 ? static_cast<std::uint64_t>(number) : static_cast<std::uint64_t>(-(number + 1));
    }
    value::value(const unsigned int number) : value(static_cast<unsigned long long>(number)) {}
    value::value(const unsigned long number) : value(static_cast<unsigned long long>(number)) {}
    value::value(const unsigned long long number) : type_(type::unsigned_integer), number_(number) {}
    value::value(const double number) : type_(type::floating), number_(0), floating_(number) {}
    value::value(const bool truth) : type_(type::simple), number_(truth ? simple::true_value : simple::false_value) {}
    value::value(std::nullptr_t) {}
    value::value(std::string text) : type_(type::text), number_(0), string_(std::move(text)) {}
    value::value(const std::string_view text) : value(std::string{text}) {}
    value::value(const char *text) : value(std::string{text}) {}
    value::value(cbor::array items) : type_(type::array), number_(0), items_(std::move(items)) {}
    value::value(cbor::map entries) : type_(type::map), number_(0), entries_(std::move(entries)) {}

    value value::bytes(std::string data) {
        value out;
        out.type_ = type::bytes;
        out.number_ = 0;
        out.string_ = std::move(data);
        return out;
    }

    value value::negative(const std::uint64_t argument) {
        value out;
        out.type_ = type::negative_integer;
        out.number_ = argument;
        return out;
    }

    value value::tagged(const std::uint64_t tag, value content) {
        value out;
        out.type_ = type::tag;
        out.number_ = tag;
        out.items_.push_back(std::move(content));
        return out;
    }

    value value::undefined() { return simple_value(simple::undefined); }

    value value::simple_value(const std::uint8_t number) {
        if (number >= 24 && number < 32) throw std::invalid_argument("sc::cbor: simple values 24 to 31 are reserved");
        value out;
        out.type_ = type::simple;
        out.number_ = number;
        return out;
    }

    value value::array_of(const std::initializer_list<value> items) { return value{cbor::array(items)}; }

    value value::map_of(const std::initializer_list<std::pair<value, value>> entries) { return value{cbor::map(entries)}; }

    bool value::is_bool() const { return type_ == type::simple && (number_ == simple::false_value || number_ == simple::true_value); }
    bool value::is_null() const { return type_ == type::simple && number_ == simple::null; }
    bool value::is_undefined() const { return type_ == type::simple && number_ == simple::undefined; }

    std::int64_t value::integer() const {
        if (!is_integer()) not_a("an integer");
        if (number_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) throw std::logic_error("sc::cbor::value: an integer that doesn't fit in 64 bits");
        return type_ == type::unsigned_integer ? static_cast<std::int64_t>(number_) : -1 - static_cast<std::int64_t>(number_);
    }

    std::uint64_t value::unsigned_integer() const {
        if (type_ != type::unsigned_integer) not_a("an unsigned integer");
        return number_;
    }

    std::uint64_t value::argument() const {
        if (!is_integer()) not_a("an integer");
        return number_;
    }

    double value::floating() const {
        if (type_ == type::floating) return floating_;
        if (type_ == type::unsigned_integer) return static_cast<double>(number_);
        if (type_ == type::negative_integer) return -1.0 - static_cast<double>(number_);
        not_a("a number");
    }

    bool value::boolean() const {
        if (!is_bool()) not_a("a boolean");
        return number_ == simple::true_value;
    }

    const std::string &value::byte_string() const {
        if (type_ != type::bytes) not_a("bytes");
        return string_;
    }

    const std::string &value::text() const {
        if (type_ != type::text) not_a("text");
        return string_;
    }

    const cbor::array &value::items() const {
        if (type_ != type::array) not_a("an array");
        return items_;
    }

    cbor::array &value::items() {
        if (type_ != type::array) not_a("an array");
        return items_;
    }

    const cbor::map &value::entries() const {
        if (type_ != type::map) not_a("a map");
        return entries_;
    }

    cbor::map &value::entries() {
        if (type_ != type::map) not_a("a map");
        return entries_;
    }

    std::uint64_t value::tag() const {
        if (type_ != type::tag) not_a("a tag");
        return number_;
    }

    const value &value::content() const {
        if (type_ != type::tag) not_a("a tag");
        return items_.front();
    }

    std::uint8_t value::simple_number() const {
        if (type_ != type::simple) not_a("a simple value");
        return static_cast<std::uint8_t>(number_);
    }

    const value *value::find(const value &key) const {
        if (type_ != type::map) return nullptr;
        for (const auto &[k, v]: entries_)
            if (k == key) return &v;
        return nullptr;
    }

    const value &value::item(const std::size_t index) const {
        if (type_ != type::array) not_a("an array");
        if (index >= items_.size()) throw std::out_of_range("sc::cbor::value: no item " + std::to_string(index));
        return items_[index];
    }

    const value &value::at(const value &key) const {
        const auto *found = find(key);
        if (!found) throw std::out_of_range("sc::cbor::value: no entry " + diagnostic(key));
        return *found;
    }

    std::size_t value::size() const {
        switch (type_) {
            case type::array: return items_.size();
            case type::map: return entries_.size();
            case type::bytes:
            case type::text: return string_.size();
            default: return 0;
        }
    }

    bool operator==(const value &a, const value &b) {
        if (a.type_ != b.type_) return false;
        switch (a.type_) {
            case type::floating: return std::memcmp(&a.floating_, &b.floating_, sizeof a.floating_) == 0;
            case type::bytes:
            case type::text: return a.string_ == b.string_;
            case type::array: return a.items_ == b.items_;
            case type::map: return a.entries_ == b.entries_;
            case type::tag: return a.number_ == b.number_ && a.items_ == b.items_;
            default: return a.number_ == b.number_;
        }
    }

    decode_options decode_options::strict() {
        decode_options out;
        out.max_depth = 16;
        out.max_items = 4096;
        out.max_length = 65536;
        out.indefinite_lengths = false;
        out.tags = false;
        out.text_keys_and_integers_only = true;
        return out;
    }

    // --- decoding and encoding

    value decode_prefix(const std::string_view input, std::size_t &used, const decode_options &options) {
        reader read{input, options};
        auto out = read.item(0);
        used = read.at;
        return out;
    }

    value decode(const std::string_view input, const decode_options &options) {
        std::size_t used = 0;
        auto out = decode_prefix(input, used, options);
        if (used != input.size()) throw error("CBOR: " + std::to_string(input.size() - used) + " bytes after the data item", used);
        return out;
    }

    std::vector<value> decode_sequence(std::string_view input, const decode_options &options) {
        std::vector<value> out;
        std::size_t offset = 0;
        while (offset < input.size()) {
            std::size_t used = 0;
            try {
                out.push_back(decode_prefix(input.substr(offset), used, options));
            } catch (const error &problem) {
                throw error(problem.what(), offset + problem.offset());
            }
            offset += used;
        }
        return out;
    }

    std::string encode(const value &item, const encode_options &options) {
        std::string out;
        write(out, item, options);
        return out;
    }

    std::string diagnostic(const value &item) {
        std::string out;
        diagnose(out, item);
        return out;
    }

    nlohmann::json to_json(const value &item) { return json_of(item, bytes_as::base64url); }

    value from_json(const nlohmann::json &json) {
        switch (json.type()) {
            case nlohmann::json::value_t::null:
            case nlohmann::json::value_t::discarded: return value{nullptr};
            case nlohmann::json::value_t::boolean: return value{json.get<bool>()};
            case nlohmann::json::value_t::number_integer: return value{json.get<long long>()};
            case nlohmann::json::value_t::number_unsigned: return value{json.get<unsigned long long>()};
            case nlohmann::json::value_t::number_float: return value{json.get<double>()};
            case nlohmann::json::value_t::string: return value{json.get<std::string>()};
            case nlohmann::json::value_t::binary: return value::bytes(std::string{json.get_binary().begin(), json.get_binary().end()});
            case nlohmann::json::value_t::array: {
                array out;
                for (const auto &each: json) out.push_back(from_json(each));
                return value{std::move(out)};
            }
            case nlohmann::json::value_t::object: {
                map out;
                for (const auto &[key, content]: json.items()) out.emplace_back(value{key}, from_json(content));
                return value{std::move(out)};
            }
        }
        return value{nullptr};
    }
}
