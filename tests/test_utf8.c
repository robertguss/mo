#include "base/utf8.h"
#include "test.h"

static void expect_scalar(const uint8_t *bytes, size_t length, uint32_t value) {
    size_t pos = 0;
    uint32_t out = 0xffffffffu;
    CHECK(mo_utf8_decode(bytes, length, &pos, &out));
    CHECK(pos == length);
    CHECK(out == value);
    CHECK(mo_utf8_valid(bytes, length));
}

static void expect_invalid(const uint8_t *bytes, size_t length) {
    size_t pos = 0;
    uint32_t out = 0x123456u;
    CHECK(!mo_utf8_decode(bytes, length, &pos, &out));
    CHECK(pos == 0);
    CHECK(out == 0x123456u);
    CHECK(!mo_utf8_valid(bytes, length));
}

int main(void) {
    const uint8_t empty[] = {0};
    CHECK(mo_utf8_valid(NULL, 0));
    CHECK(mo_utf8_valid(empty, 0));
    size_t pos = 0;
    uint32_t out = 7;
    CHECK(!mo_utf8_decode(NULL, 0, &pos, &out));
    CHECK(pos == 0 && out == 7);

    const uint8_t nul[] = {0};
    expect_scalar(nul, 1, 0);
    const uint8_t ascii[] = {0x7f};
    expect_scalar(ascii, 1, 0x7f);
    const uint8_t two[] = {0xc2, 0x80};
    expect_scalar(two, 2, 0x80);
    const uint8_t max_two[] = {0xdf, 0xbf};
    expect_scalar(max_two, 2, 0x7ff);
    const uint8_t three[] = {0xe0, 0xa0, 0x80};
    expect_scalar(three, 3, 0x800);
    const uint8_t before_surrogate[] = {0xed, 0x9f, 0xbf};
    expect_scalar(before_surrogate, 3, 0xd7ff);
    const uint8_t after_surrogate[] = {0xee, 0x80, 0x80};
    expect_scalar(after_surrogate, 3, 0xe000);
    const uint8_t max_bmp[] = {0xef, 0xbf, 0xbf};
    expect_scalar(max_bmp, 3, 0xffff);
    const uint8_t four[] = {0xf0, 0x90, 0x80, 0x80};
    expect_scalar(four, 4, 0x10000);
    const uint8_t max_scalar[] = {0xf4, 0x8f, 0xbf, 0xbf};
    expect_scalar(max_scalar, 4, 0x10ffff);
    const uint8_t e_acute[] = {0xc3, 0xa9};
    expect_scalar(e_acute, 2, 0xe9);
    const uint8_t grin[] = {0xf0, 0x9f, 0x98, 0x80};
    expect_scalar(grin, 4, 0x1f600);

    const uint8_t cafe[] = {0x63, 0x61, 0x66, 0xc3, 0xa9};
    CHECK(mo_utf8_valid(cafe, sizeof cafe));
    pos = 0;
    CHECK(mo_utf8_decode(cafe, sizeof cafe, &pos, &out) && out == 'c' && pos == 1);
    CHECK(mo_utf8_decode(cafe, sizeof cafe, &pos, &out) && out == 'a' && pos == 2);
    CHECK(mo_utf8_decode(cafe, sizeof cafe, &pos, &out) && out == 'f' && pos == 3);
    CHECK(mo_utf8_decode(cafe, sizeof cafe, &pos, &out) && out == 0xe9 && pos == 5);
    CHECK(!mo_utf8_decode(cafe, sizeof cafe, &pos, &out) && pos == 5);

    const uint8_t continuation[] = {0x80};
    expect_invalid(continuation, 1);
    const uint8_t overlong_nul[] = {0xc0, 0x80};
    expect_invalid(overlong_nul, 2);
    const uint8_t overlong_c1[] = {0xc1, 0xbf};
    expect_invalid(overlong_c1, 2);
    const uint8_t truncated_two[] = {0xc2};
    expect_invalid(truncated_two, 1);
    const uint8_t overlong_three[] = {0xe0, 0x9f, 0xbf};
    expect_invalid(overlong_three, 3);
    const uint8_t surrogate[] = {0xed, 0xa0, 0x80};
    expect_invalid(surrogate, 3);
    const uint8_t high_surrogate[] = {0xed, 0xbf, 0xbf};
    expect_invalid(high_surrogate, 3);
    const uint8_t overlong_four[] = {0xf0, 0x8f, 0xbf, 0xbf};
    expect_invalid(overlong_four, 4);
    const uint8_t too_large[] = {0xf4, 0x90, 0x80, 0x80};
    expect_invalid(too_large, 4);
    const uint8_t lead_f5[] = {0xf5, 0x80, 0x80, 0x80};
    expect_invalid(lead_f5, 4);
    const uint8_t ff[] = {0xff};
    expect_invalid(ff, 1);
    const uint8_t truncated_three[] = {0xe1, 0x80};
    expect_invalid(truncated_three, 2);
    const uint8_t truncated_four[] = {0xf1, 0x80, 0x80};
    expect_invalid(truncated_four, 3);
    const uint8_t bad_cont[] = {0xe1, 0x41, 0x80};
    expect_invalid(bad_cont, 3);
    const uint8_t mixed[] = {'a', 0xc0, 0x80};
    CHECK(!mo_utf8_valid(mixed, sizeof mixed));

    return mo_test_report("utf-8");
}
