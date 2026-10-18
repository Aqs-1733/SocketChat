#include "../src/protocol.h"
#include <cassert>
#include <iostream>
#include <limits>

int main() {
    using namespace chat;
    assert(is_valid_name("Alice_01"));
    assert(!is_valid_name(""));
    assert(!is_valid_name("中文"));
    assert(!is_valid_name("name-with-dash"));
    assert(!is_valid_name("abcdefghijklmnopq"));
    assert(is_valid_utf8("中文 Hello 🐱"));
    assert(!is_valid_utf8(std::string("\xC0\xAF", 2)));
    assert(!is_valid_utf8(std::string("\xED\xA0\x80", 3)));
    assert(!is_valid_utf8(std::string("\xE4\xB8", 2)));
    assert(is_valid_body("中文|English"));
    assert(!is_valid_body(""));
    assert(!is_valid_body(std::string(4097, 'a')));
    assert(!is_valid_body("hello\nworld"));
    std::uint64_t seq = 0;
    assert(parse_sequence("18446744073709551615", seq) && seq == std::numeric_limits<std::uint64_t>::max());
    assert(!parse_sequence("18446744073709551616", seq));
    assert(!parse_sequence("0", seq));
    assert(!parse_sequence("+1", seq));
    const auto fields = split_fields("TEXT|1|你好|world", 2);
    assert(fields.size() == 3 && fields[2] == "你好|world");
    std::string pending;
    std::vector<std::string> frames;
    assert(chat::extract_frames(pending, "TEXT|1|", 7, frames) && frames.empty());
    assert(chat::extract_frames(pending, "abc\nWHO\r\n", 9, frames));
    assert(frames.size() == 2 && frames[0] == "TEXT|1|abc" && frames[1] == "WHO");
    frames.clear();
    assert(!chat::extract_frames(pending, std::string(8193, 'x').data(), 8193, frames));
    std::cout << "PASS: protocol unit tests" << std::endl;
}
