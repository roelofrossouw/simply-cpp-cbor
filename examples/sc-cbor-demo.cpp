// Shows CBOR as diagnostic notation (RFC 8949 section 8) or JSON: from hex on the command line, or
// a file's bytes; --deterministic prints its core deterministic encoding instead, in hex.
//
//     sc-cbor-demo a201020304              # {1: 2, 3: 4}
//     sc-cbor-demo --file attestation.cbor --json

#include <sc_cbor.h>
#include <params.h>

#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-cbor-demo"},
        {"description", "Shows a CBOR data item, given in hex or read from a file, as diagnostic notation, as JSON (--json), or "
                        "as its core deterministic encoding in hex (--deterministic)."},
        {"options", {
            {"file", {{"short", "f"}, {"value", "FILE"}, {"default", ""}, {"help", "read the bytes from FILE"}}},
            {"json", {{"short", "j"}, {"type", "boolean"}, {"help", "print JSON (section 6.1)"}}},
            {"deterministic", {{"short", "d"}, {"type", "boolean"}, {"help", "print its deterministic encoding in hex"}}}}},
        {"positional", {
            {"hex", {{"default", "a26161016162820203"}, {"help", "the bytes in hex"}}}}}}};
    try {
        // [readme]
        std::string input;
        if (const auto file = args.as<std::string>("file"); !file.empty()) {
            std::ifstream in{file, std::ios::binary};
            std::stringstream read;
            read << in.rdbuf();
            input = read.str();
        } else {
            const auto text = args.as<std::string>("hex");
            for (std::size_t i = 0; i + 1 < text.size(); i += 2) input += static_cast<char>(std::stoi(text.substr(i, 2), nullptr, 16));
        }
        const auto item = sc::cbor::decode(input);
        if (args.as<bool>("json")) {
            std::cout << sc::cbor::to_json(item).dump(2) << '\n';
        } else if (args.as<bool>("deterministic")) {
            for (const auto c: sc::cbor::encode(item, {.deterministic = true})) std::printf("%02x", static_cast<unsigned char>(c));
            std::cout << '\n';
        } else {
            std::cout << sc::cbor::diagnostic(item) << '\n';
        }
        // [readme]
        return 0;
    } catch (const sc::cbor::error &problem) {
        std::cerr << problem.what() << " (at byte " << problem.offset() << ")\n";
    } catch (const std::exception &problem) {
        std::cerr << problem.what() << '\n';
    }
    return 1;
}
