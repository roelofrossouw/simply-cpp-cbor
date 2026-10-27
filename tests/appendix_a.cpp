// RFC 8949 Appendix A: every example decodes to its value (as diagnostic notation), and encoding
// it again gives the same bytes (they're all in preferred serialization); the indefinite length
// ones decode to what their definite forms encode.
#include <sc_cbor.h>

#include "sc_test.h"

#include <cmath>

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

    // Decodes to diagnostic and encodes back to the same bytes.
    void example(const std::string_view diagnostic, const std::string_view encoded) {
        const auto item = sc::cbor::decode(bytes(encoded));
        CHECK_MSG(sc::cbor::diagnostic(item) == diagnostic, std::string{encoded} + " is " + sc::cbor::diagnostic(item) + ", not " + std::string{diagnostic});
        CHECK_MSG(hex(sc::cbor::encode(item)) == encoded, std::string{diagnostic} + " encodes as " + hex(sc::cbor::encode(item)));
    }

    // A float: its value, and the same bytes back.
    void float_example(const double number, const std::string_view encoded) {
        const auto item = sc::cbor::decode(bytes(encoded));
        CHECK(item.is_float());
        if (std::isnan(number)) CHECK(std::isnan(item.floating()));
        else CHECK_MSG(item.floating() == number && std::signbit(item.floating()) == std::signbit(number), std::string{encoded});
        CHECK_MSG(hex(sc::cbor::encode(sc::cbor::value{number})) == encoded, std::string{encoded} + " from the double encodes as " + hex(sc::cbor::encode(sc::cbor::value{number})));
    }

    // An indefinite length item: what it decodes to, encoded definitely.
    void indefinite(const std::string_view encoded, const std::string_view definite, const std::string_view diagnostic) {
        const auto item = sc::cbor::decode(bytes(encoded));
        CHECK_MSG(hex(sc::cbor::encode(item)) == definite, std::string{encoded} + " encodes as " + hex(sc::cbor::encode(item)));
        CHECK_MSG(sc::cbor::diagnostic(item) == diagnostic, std::string{encoded} + " is " + sc::cbor::diagnostic(item));
    }
}

int main() {
    SECTION("Integers, and bignums as tags 2 and 3");
    {
        example("0", "00");
        example("1", "01");
        example("10", "0a");
        example("23", "17");
        example("24", "1818");
        example("25", "1819");
        example("100", "1864");
        example("1000", "1903e8");
        example("1000000", "1a000f4240");
        example("1000000000000", "1b000000e8d4a51000");
        example("18446744073709551615", "1bffffffffffffffff");
        example("2(h'010000000000000000')", "c249010000000000000000");
        example("-18446744073709551616", "3bffffffffffffffff");
        example("3(h'010000000000000000')", "c349010000000000000000");
        example("-1", "20");
        example("-10", "29");
        example("-100", "3863");
        example("-1000", "3903e7");
        CHECK_EQ(sc::cbor::decode(bytes("3903e7")).integer(), std::int64_t{-1000});
        CHECK_EQ(sc::cbor::decode(bytes("1bffffffffffffffff")).unsigned_integer(), std::uint64_t{18446744073709551615ull});
        CHECK_EQ(sc::cbor::to_json(sc::cbor::decode(bytes("c249010000000000000000"))).get<std::string>(), std::string{"AQAAAAAAAAAA"});
        CHECK_EQ(sc::cbor::to_json(sc::cbor::decode(bytes("c2420100"))).get<std::uint64_t>(), std::uint64_t{256});
    }

    SECTION("Floats, in the shortest exact width");
    {
        float_example(0.0, "f90000");
        float_example(-0.0, "f98000");
        float_example(1.0, "f93c00");
        float_example(1.1, "fb3ff199999999999a");
        float_example(1.5, "f93e00");
        float_example(65504.0, "f97bff");
        float_example(100000.0, "fa47c35000");
        float_example(3.4028234663852886e+38, "fa7f7fffff");
        float_example(1.0e+300, "fb7e37e43c8800759c");
        float_example(5.960464477539063e-8, "f90001");
        float_example(0.00006103515625, "f90400");
        float_example(-4.0, "f9c400");
        float_example(-4.1, "fbc010666666666666");
        float_example(INFINITY, "f97c00");
        float_example(-INFINITY, "f9fc00");
        CHECK(std::isnan(sc::cbor::decode(bytes("f97e00")).floating()));
        CHECK_EQ(hex(sc::cbor::encode(sc::cbor::value{NAN})), std::string{"f97e00"});
        // A payload is kept, in the narrowest width that holds it.
        CHECK_EQ(hex(sc::cbor::encode(sc::cbor::decode(bytes("fa7fc00001")))), std::string{"fa7fc00001"});
        CHECK_EQ(hex(sc::cbor::encode(sc::cbor::decode(bytes("fb7ff8000000000001")))), std::string{"fb7ff8000000000001"});
        // The wider forms of the same values decode too.
        CHECK(std::isinf(sc::cbor::decode(bytes("fa7f800000")).floating()));
        CHECK(std::isnan(sc::cbor::decode(bytes("fa7fc00000")).floating()));
        CHECK(std::isinf(sc::cbor::decode(bytes("fb7ff0000000000000")).floating()));
        CHECK(std::isnan(sc::cbor::decode(bytes("fb7ff8000000000000")).floating()));
        CHECK_EQ(sc::cbor::diagnostic(sc::cbor::decode(bytes("f93e00"))), std::string{"1.5"});
        CHECK_EQ(sc::cbor::diagnostic(sc::cbor::decode(bytes("f93c00"))), std::string{"1.0"});
        CHECK_EQ(sc::cbor::diagnostic(sc::cbor::decode(bytes("fb7e37e43c8800759c"))), std::string{"1.0e+300"});
        CHECK_EQ(sc::cbor::diagnostic(sc::cbor::decode(bytes("f90001"))), std::string{"5.960464477539063e-8"});
        CHECK_EQ(sc::cbor::diagnostic(sc::cbor::decode(bytes("f97e00"))), std::string{"NaN"});
        CHECK_EQ(sc::cbor::diagnostic(sc::cbor::decode(bytes("f9fc00"))), std::string{"-Infinity"});
    }

    SECTION("Simple values");
    {
        example("false", "f4");
        example("true", "f5");
        example("null", "f6");
        example("undefined", "f7");
        example("simple(16)", "f0");
        example("simple(255)", "f8ff");
    }

    SECTION("Tags");
    {
        example("0(\"2013-03-21T20:04:00Z\")", "c074323031332d30332d32315432303a30343a30305a");
        example("1(1363896240)", "c11a514b67b0");
        CHECK_EQ(hex(sc::cbor::encode(sc::cbor::decode(bytes("c1fb41d452d9ec200000")))), std::string{"c1fb41d452d9ec200000"});
        CHECK_EQ(sc::cbor::decode(bytes("c1fb41d452d9ec200000")).content().floating(), 1363896240.5);
        example("23(h'01020304')", "d74401020304");
        example("24(h'6449455446')", "d818456449455446");
        example("32(\"http://www.example.com\")", "d82076687474703a2f2f7777772e6578616d706c652e636f6d");
        // Section 6.1's expected conversions: 23 writes its bytes in base16.
        CHECK_EQ(sc::cbor::to_json(sc::cbor::decode(bytes("d74401020304"))).get<std::string>(), std::string{"01020304"});
    }

    SECTION("Byte strings and text");
    {
        example("h''", "40");
        example("h'01020304'", "4401020304");
        example("\"\"", "60");
        example("\"a\"", "6161");
        example("\"IETF\"", "6449455446");
        example("\"\\\"\\\\\"", "62225c");
        example("\"ü\"", "62c3bc");
        example("\"水\"", "63e6b0b4");
        example("\"\U00010151\"", "64f0908591");
    }

    SECTION("Arrays and maps");
    {
        example("[]", "80");
        example("[1, 2, 3]", "83010203");
        example("[1, [2, 3], [4, 5]]", "8301820203820405");
        example("[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25]",
                "98190102030405060708090a0b0c0d0e0f101112131415161718181819");
        example("{}", "a0");
        example("{1: 2, 3: 4}", "a201020304");
        example("{\"a\": 1, \"b\": [2, 3]}", "a26161016162820203");
        example("[\"a\", {\"b\": \"c\"}]", "826161a161626163");
        example("{\"a\": \"A\", \"b\": \"B\", \"c\": \"C\", \"d\": \"D\", \"e\": \"E\"}", "a56161614161626142616361436164614461656145");
    }

    SECTION("Indefinite lengths, decoded to their definite forms");
    {
        indefinite("5f42010243030405ff", "450102030405", "h'0102030405'");
        indefinite("7f657374726561646d696e67ff", "6973747265616d696e67", "\"streaming\"");
        indefinite("9fff", "80", "[]");
        indefinite("9f018202039f0405ffff", "8301820203820405", "[1, [2, 3], [4, 5]]");
        indefinite("9f01820203820405ff", "8301820203820405", "[1, [2, 3], [4, 5]]");
        indefinite("83018202039f0405ff", "8301820203820405", "[1, [2, 3], [4, 5]]");
        indefinite("83019f0203ff820405", "8301820203820405", "[1, [2, 3], [4, 5]]");
        indefinite("9f0102030405060708090a0b0c0d0e0f101112131415161718181819ff", "98190102030405060708090a0b0c0d0e0f101112131415161718181819",
                   "[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25]");
        indefinite("bf61610161629f0203ffff", "a26161016162820203", "{\"a\": 1, \"b\": [2, 3]}");
        indefinite("826161bf61626163ff", "826161a161626163", "[\"a\", {\"b\": \"c\"}]");
        indefinite("bf6346756ef563416d7421ff", "a26346756ef563416d7421", "{\"Fun\": true, \"Amt\": -2}");
    }

    TEST_SUMMARY();
}
