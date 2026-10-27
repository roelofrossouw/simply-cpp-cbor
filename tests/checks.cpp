// What decoding refuses (RFC 8949 Appendix F's not-well-formed examples, section 5.3's validity,
// the limits and strict mode), deterministic encoding (4.2.1), the JSON conversions (section 6),
// CBOR sequences (RFC 8742), and building values in code.
#include <sc_cbor.h>

#include "sc_test.h"

#include <random>

namespace {
    std::string bytes(const std::string_view hex) {
        std::string out;
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out += static_cast<char>(std::stoi(std::string{hex.substr(i, 2)}, nullptr, 16));
        return out;
    }

    std::string hex(const std::string_view data) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        for (const auto c: data) {
            out += digits[static_cast<std::uint8_t>(c) >> 4];
            out += digits[static_cast<std::uint8_t>(c) & 0xf];
        }
        return out;
    }

    bool refused(const std::string &input, const sc::cbor::decode_options &options = {}) {
        try {
            (void) sc::cbor::decode(input, options);
        } catch (const sc::cbor::error &) {
            return true;
        }
        return false;
    }
}

int main() {
    using sc::cbor::value;

    SECTION("Not well-formed (Appendix F)");
    {
        // End of input in a head, or in a string, array or map's content.
        for (const auto *each: {"18", "19", "1a", "1b", "1901", "1a0102", "1b01020304050607", "38", "58", "78", "98", "9a01ff00", "b8", "d8", "f8", "f900",
                                "fa0000", "fb000000", "41", "61", "5affffffff00", "5bffffffffffffffff010203", "7affffffff00", "7b7fffffffffffffff010203",
                                "81", "818181818181818181", "8200", "a1", "a20102", "a100", "a2000000", "c0", "5f4100", "7f6100", "9f", "9f0102", "bf",
                                "bf01020102", "819f", "9f8000", "9f9f9f9f9fffffffff", "9f819f819f9fffffff"})
            CHECK_MSG(refused(bytes(each)), std::string{"refused: "} + each);
        // Reserved additional information.
        for (const auto *each: {"1c", "1d", "1e", "3c", "3d", "3e", "5c", "5d", "5e", "7c", "7d", "7e", "9c", "9d", "9e", "bc", "bd", "be", "dc", "dd", "de",
                                "fc", "fd", "fe"})
            CHECK_MSG(refused(bytes(each)), std::string{"refused: "} + each);
        // Two-byte simple values below 32.
        for (const auto *each: {"f800", "f801", "f818", "f81f"}) CHECK_MSG(refused(bytes(each)), std::string{"refused: "} + each);
        // Indefinite strings with chunks of another type, or indefinite chunks.
        for (const auto *each: {"5f00ff", "5f21ff", "5f6100ff", "5f80ff", "5fa0ff", "5fc000ff", "5fe0ff", "7f4100ff", "5f5f4100ffff", "7f7f6100ffff"})
            CHECK_MSG(refused(bytes(each)), std::string{"refused: "} + each);
        // Breaks out of place, and indefinite lengths where there are none.
        for (const auto *each: {"ff", "81ff", "8200ff", "a1ff", "a1ff00", "a100ff", "a20000ff", "9f81ff", "9f829f819f9fffffffff", "bf00ff", "bf000000ff",
                                "1f", "3f", "df"})
            CHECK_MSG(refused(bytes(each)), std::string{"refused: "} + each);
        // Where it went wrong.
        try {
            (void) sc::cbor::decode(bytes("8301021c"));
        } catch (const sc::cbor::error &problem) {
            CHECK_EQ(problem.offset(), std::size_t{3});
        }
    }

    SECTION("Validity (section 5.3) and the options");
    {
        CHECK(refused(bytes("62c328")));               // not UTF-8
        CHECK(refused(bytes("7f61c361a9ff")));         // a character split over chunks
        CHECK(!refused(bytes("62c328"), {.check_utf8 = false}));
        CHECK(refused(bytes("a201020103")));           // a key twice
        CHECK(refused(bytes("a2f93c00f5f93c00f4")));   // 1.0 twice
        CHECK(!refused(bytes("a2f93c0001f9bc0002")));  // 1.0 and -1.0 differ
        const auto last = sc::cbor::decode(bytes("a201020103"), {.duplicate_keys = true});
        CHECK_EQ(last.size(), std::size_t{1});
        CHECK_EQ(last.at(1).integer(), std::int64_t{3});
        CHECK(refused(bytes("c001")));                 // tag 0 holds text
        CHECK(refused(bytes("c26161")));               // tag 2 holds bytes
        CHECK(refused(bytes("c48102")));               // tag 4 holds [exponent, mantissa]
        CHECK(!refused(bytes("c482211903e8")));        // 4([-2, 1000])
        CHECK(!refused(bytes("c001"), {.check_standard_tags = false}));
        CHECK(!refused(bytes("d9d9f700")));            // an unknown tag is any content
        CHECK(refused(bytes("0001")));                 // something after the item
        std::size_t used = 0;
        CHECK_EQ(sc::cbor::decode_prefix(bytes("0001"), used).integer(), std::int64_t{0});
        CHECK_EQ(used, std::size_t{1});
    }

    SECTION("Limits and strict mode");
    {
        std::string deep;
        for (int i = 0; i < 300; ++i) deep += '\x81';
        CHECK(refused(deep + bytes("00")));
        CHECK(!refused(std::string(255, '\x81') + bytes("00")));
        CHECK(refused(bytes("9b00000000ffffffff"))); // more items than allowed (and than there are bytes for)
        CHECK(refused(bytes("5bffffffffffffffff")));
        CHECK(refused(bytes("43010203"), {.max_length = 2}));
        CHECK(refused(bytes("83010203"), {.max_items = 2}));
        const auto strict = sc::cbor::decode_options::strict();
        CHECK(refused(bytes("9fff"), strict));        // indefinite
        CHECK(refused(bytes("c11a514b67b0"), strict)); // a tag
        CHECK(refused(bytes("a14100f6"), strict));     // a bytes key
        CHECK(!refused(bytes("a3010203262001"), strict)); // COSE-like: {1: 2, 3: -7, -1: 1}
        CHECK(refused(std::string(17, '\x81') + bytes("00"), strict));
    }

    SECTION("Deterministic encoding (4.2.1)");
    {
        const auto item = value::map_of({{"aa", 1}, {"b", 2}, {10, 3}, {-1, 4}, {100, 5}, {false, 6}, {value::array_of({100}), 7}});
        // Keys sorted by their encodings' bytes: 10 (0a), 100 (1864), -1 (20), "b" (6162), "aa" (626161), [100] (811864), false (f4).
        CHECK_EQ(hex(sc::cbor::encode(item, {.deterministic = true})), std::string{"a70a031864052004616202626161018118640" "7f406"});
        CHECK_EQ(hex(sc::cbor::encode(item)).substr(0, 10), std::string{"a762616101"}); // the given order without it
        CHECK_EQ(hex(sc::cbor::encode(value{NAN}, {.deterministic = true})), std::string{"f97e00"});
        const auto tiny = hex(sc::cbor::encode(value{1.0e-40})); // no float holds it exactly: a double
        CHECK_EQ(tiny.substr(0, 2), std::string{"fb"});
        CHECK_EQ(tiny.size(), std::size_t{18});
    }

    SECTION("JSON (section 6)");
    {
        const auto item = sc::cbor::decode(bytes("a4616101616282f5f6616343010203a10102f4"));
        const auto json = sc::cbor::to_json(item);
        CHECK_EQ(json["a"].get<int>(), 1);
        CHECK(json["b"][0].get<bool>());
        CHECK(json["b"][1].is_null());
        CHECK_EQ(json["c"].get<std::string>(), std::string{"AQID"});
        CHECK(!json.at("{1: 2}").get<bool>()); // a key that isn't text, as its diagnostic notation
        const auto back = sc::cbor::from_json(nlohmann::json::parse(R"({"x": [1, -2, 1.5, "t", true, null], "y": {}})"));
        CHECK_EQ(sc::cbor::diagnostic(back), std::string{"{\"x\": [1, -2, 1.5, \"t\", true, null], \"y\": {}}"});
        CHECK(sc::cbor::to_json(value{NAN}).is_null());
        CHECK(sc::cbor::to_json(value::undefined()).is_null());
    }

    SECTION("Sequences (RFC 8742)");
    {
        const auto items = sc::cbor::decode_sequence(bytes("0161618100"));
        CHECK_EQ(items.size(), std::size_t{3});
        CHECK_EQ(items[1].text(), std::string{"a"});
        CHECK(sc::cbor::decode_sequence("").empty());
        try {
            (void) sc::cbor::decode_sequence(bytes("010218"));
            CHECK(false);
        } catch (const sc::cbor::error &problem) {
            CHECK_EQ(problem.offset(), std::size_t{3});
        }
    }

    SECTION("Values in code");
    {
        const auto cose = value::map_of({{1, 2}, {3, -7}, {-1, 1}, {-2, value::bytes(std::string(32, 'x'))}});
        CHECK_EQ(cose.at(3).integer(), std::int64_t{-7});
        CHECK(!cose.find(4));
        CHECK_EQ(sc::cbor::decode(sc::cbor::encode(cose)), cose);
        CHECK(value{1} != value{1.0});
        CHECK(value::bytes("a") != value{"a"});
        CHECK_EQ(value::negative(0).integer(), std::int64_t{-1});
        CHECK_EQ(sc::cbor::diagnostic(value::negative(UINT64_MAX)), std::string{"-18446744073709551616"});
        CHECK_THROWS_AS((void) value::negative(UINT64_MAX).integer(), std::logic_error);
        CHECK_THROWS_AS((void) value{"x"}.integer(), std::logic_error);
        CHECK_THROWS_AS((void) cose.at(9), std::out_of_range);
        CHECK_THROWS_AS((void) value::simple_value(25), std::invalid_argument);
        CHECK_EQ(sc::cbor::diagnostic(value::tagged(1, 1363896240)), std::string{"1(1363896240)"});
    }

    SECTION("Fuzz: random bytes decode or throw sc::cbor::error, and what decodes encodes and decodes the same");
    {
        std::mt19937 random{8949};
        int decoded = 0, unexpected = 0, unstable = 0;
        for (int round = 0; round < 50000; ++round) {
            std::string input(random() % 48, '\0');
            for (auto &c: input) c = static_cast<char>(random() % 4 == 0 ? 0xff : random());
            try {
                const auto item = sc::cbor::decode(input);
                ++decoded;
                if (!(sc::cbor::decode(sc::cbor::encode(item)) == item)) ++unstable;
            } catch (const sc::cbor::error &) {
            } catch (const std::exception &problem) {
                if (++unexpected <= 5) std::cerr << "    unexpected: " << problem.what() << '\n';
            }
        }
        CHECK_EQ(unexpected, 0);
        CHECK_EQ(unstable, 0);
        std::cout << "    " << decoded << " of 50000 random inputs decoded\n";
    }

    TEST_SUMMARY();
}
