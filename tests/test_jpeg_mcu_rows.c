#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    NJ_OK = 0,
    NJ_NO_JPEG = 1,
    NJ_UNSUPPORTED = 2,
    NJ_OUT_OF_MEM = 3,
    NJ_INTERNAL_ERR = 4,
    NJ_SYNTAX_ERROR = 5,
    NJ_CALLBACK_ABORT = 6
};

typedef int (*nj_mcu_row_callback_t)(int mcu_y, void *user);

void njInit(void);
int njDecodeMcuRows(const void *jpeg, const int size, nj_mcu_row_callback_t callback, void *user);
int njGetWidth(void);
int njGetHeight(void);
int njGetComponentCount(void);
const unsigned char *njGetComponentPixels(int index);
int njGetComponentWidth(int index);
int njGetComponentHeight(int index);
int njGetComponentStride(int index);
int njGetComponentSsx(int index);
int njGetComponentSsy(int index);
int njGetMcuRowCount(void);
void njDone(void);

typedef struct row_callback_state_t {
    int rows[8];
    int row_count;
    int fail_after_rows;
} row_callback_state_t;

static const char k_color_jpeg_hex[] =
    "ffd8ffe000104a46494600010200000100010000fffe00104c61766336322e31312e31303000"
    "ffdb0043000806060706070808080808080909090a0a0a090909090a0a0a0a0a0a0c0c0c0a"
    "0a0a0a0a0a0a0c0c0c0c0d0e0d0d0d0c0d0e0e0f0f0f1212111115151519191fffc4006b"
    "0001010100000000000000000000000000050307010101000000000000000000000000000000"
    "0610000202010304030101010000000000000201030412110513002241211424154223161100"
    "0300010402030100000000000000000301020413051100123106212351ffc00011080010001003"
    "012200021100031100ffda000c03010002110311003f00cba42dcecdf9abd3f7c71f296bc22"
    "11c611a23924965c4001792324b569795d5b61ffa2da37028ebff0094ce01b39fd492175d"
    "c66d58539675dc5a135c88f1cbb75cbd74cecf24652ef9558f34b37e7c815b2625602b3e4"
    "980097f58781ef6b5629e9d23527ac3b12d9fe3e1756b6ff1f94dcc358afbb235dcba65cb"
    "f1bde0fec63dcc35e99df3bdd724b92868b012944b475c968623cb23357e824c216fc1f23"
    "98fe927d768373da84cd9a121acba63cf13f7c5aa440d6329f02a54517db94edb97c69d2"
    "eff00ffd9";

static const char k_gray_jpeg_hex[] =
    "ffd8ffe000104a46494600010100000100010000ffdb00430005030404040305040404050505"
    "06070c08070707070f0b0b090c110f1212110f111113161c1713141a1511111821181a1d1d"
    "1f1f1f13172224221e241c1e1f1effc0000b080010001001011100ffc4001f000001050101"
    "0101010100000000000000000102030405060708090a0bffc400b510000201030302040305"
    "0504040000017d01020300041105122131410613516107227114328191a1082342b1c11552"
    "d1f02433627282090a161718191a25262728292a3435363738393a434445464748494a5354"
    "55565758595a636465666768696a737475767778797a838485868788898a92939495969798"
    "999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6"
    "d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffda0008010100003f00"
    "f17f879e1fff0057f27a76afa37e1e787ffd5fc9e9dab8cf879e1fff0057f27a76af73"
    "b59b4af06f83f51f14eb6de5586996cd712e19433e0711a6e201766c2a82465980ef5f"
    "ffd9";

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t capacity, size_t *out_size)
{
    size_t i;
    size_t len;

    if (hex == NULL || out == NULL || out_size == NULL) {
        return 0;
    }
    len = strlen(hex);
    if ((len & 1u) != 0u || len / 2u > capacity) {
        return 0;
    }
    for (i = 0u; i < len / 2u; i++) {
        const int hi = hex_value(hex[i * 2u]);
        const int lo = hex_value(hex[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_size = len / 2u;
    return 1;
}

static int collect_mcu_row(int mcu_y, void *user)
{
    row_callback_state_t *state = (row_callback_state_t *)user;

    if (state == NULL || state->row_count >= (int)(sizeof(state->rows) / sizeof(state->rows[0]))) {
        return 1;
    }
    if (state->fail_after_rows >= 0 && state->row_count >= state->fail_after_rows) {
        return 1;
    }
    state->rows[state->row_count++] = mcu_y;
    return 0;
}

static int expect_status(const char *name, int got, int expected)
{
    if (got != expected) {
        fprintf(stderr, "%s: got status %d, expected %d\n", name, got, expected);
        return 0;
    }
    return 1;
}

static int expect_int(const char *name, int got, int expected)
{
    if (got != expected) {
        fprintf(stderr, "%s: got %d, expected %d\n", name, got, expected);
        return 0;
    }
    return 1;
}

static int expect_ptr(const char *name, const void *ptr)
{
    if (ptr == NULL) {
        fprintf(stderr, "%s: unexpected NULL pointer\n", name);
        return 0;
    }
    return 1;
}

static int decode_and_check_color(void)
{
    uint8_t jpeg[512];
    size_t jpeg_size = 0u;
    row_callback_state_t state;
    int ok = 1;
    int status;

    if (!hex_to_bytes(k_color_jpeg_hex, jpeg, sizeof(jpeg), &jpeg_size)) {
        fprintf(stderr, "failed to decode color fixture hex\n");
        return 0;
    }

    memset(&state, 0, sizeof(state));
    state.fail_after_rows = -1;
    njInit();
    status = njDecodeMcuRows(jpeg, (int)jpeg_size, collect_mcu_row, &state);
    ok &= expect_status("color decode", status, NJ_OK);
    ok &= expect_int("color width", njGetWidth(), 16);
    ok &= expect_int("color height", njGetHeight(), 16);
    ok &= expect_int("color component count", njGetComponentCount(), 3);
    ok &= expect_int("color mcu rows", njGetMcuRowCount(), 1);
    ok &= expect_int("color callback count", state.row_count, 1);
    ok &= expect_int("color callback row 0", state.rows[0], 0);
    ok &= expect_ptr("color component 0 pixels", njGetComponentPixels(0));
    ok &= expect_ptr("color component 1 pixels", njGetComponentPixels(1));
    ok &= expect_ptr("color component 2 pixels", njGetComponentPixels(2));
    ok &= expect_int("color y width", njGetComponentWidth(0), 16);
    ok &= expect_int("color y height", njGetComponentHeight(0), 16);
    ok &= expect_int("color y stride", njGetComponentStride(0), 16);
    ok &= expect_int("color y ssx", njGetComponentSsx(0), 2);
    ok &= expect_int("color y ssy", njGetComponentSsy(0), 2);
    ok &= expect_int("color cb width", njGetComponentWidth(1), 8);
    ok &= expect_int("color cb height", njGetComponentHeight(1), 8);
    ok &= expect_int("color cb stride", njGetComponentStride(1), 8);
    ok &= expect_int("color cb ssx", njGetComponentSsx(1), 1);
    ok &= expect_int("color cb ssy", njGetComponentSsy(1), 1);
    njDone();
    return ok;
}

static int decode_and_check_gray(void)
{
    uint8_t jpeg[512];
    size_t jpeg_size = 0u;
    row_callback_state_t state;
    int ok = 1;
    int status;

    if (!hex_to_bytes(k_gray_jpeg_hex, jpeg, sizeof(jpeg), &jpeg_size)) {
        fprintf(stderr, "failed to decode grayscale fixture hex\n");
        return 0;
    }

    memset(&state, 0, sizeof(state));
    state.fail_after_rows = -1;
    njInit();
    status = njDecodeMcuRows(jpeg, (int)jpeg_size, collect_mcu_row, &state);
    ok &= expect_status("gray decode", status, NJ_OK);
    ok &= expect_int("gray width", njGetWidth(), 16);
    ok &= expect_int("gray height", njGetHeight(), 16);
    ok &= expect_int("gray component count", njGetComponentCount(), 1);
    ok &= expect_int("gray mcu rows", njGetMcuRowCount(), 2);
    ok &= expect_int("gray callback count", state.row_count, 2);
    ok &= expect_int("gray callback row 0", state.rows[0], 0);
    ok &= expect_int("gray callback row 1", state.rows[1], 1);
    ok &= expect_ptr("gray component pixels", njGetComponentPixels(0));
    ok &= expect_int("gray component width", njGetComponentWidth(0), 16);
    ok &= expect_int("gray component height", njGetComponentHeight(0), 16);
    ok &= expect_int("gray component stride", njGetComponentStride(0), 16);
    ok &= expect_int("gray component ssx", njGetComponentSsx(0), 1);
    ok &= expect_int("gray component ssy", njGetComponentSsy(0), 1);
    njDone();
    return ok;
}

static int callback_abort_is_deterministic(void)
{
    uint8_t jpeg[512];
    size_t jpeg_size = 0u;
    row_callback_state_t state;
    int status;

    if (!hex_to_bytes(k_color_jpeg_hex, jpeg, sizeof(jpeg), &jpeg_size)) {
        fprintf(stderr, "failed to decode callback fixture hex\n");
        return 0;
    }

    memset(&state, 0, sizeof(state));
    state.fail_after_rows = 0;
    njInit();
    status = njDecodeMcuRows(jpeg, (int)jpeg_size, collect_mcu_row, &state);
    njDone();
    return expect_status("callback abort", status, NJ_CALLBACK_ABORT) &&
           expect_int("callback abort count", state.row_count, 0);
}

static int unsupported_sof_is_deterministic(void)
{
    uint8_t jpeg[512];
    size_t jpeg_size = 0u;
    row_callback_state_t state;
    size_t i;
    int found = 0;
    int status;

    if (!hex_to_bytes(k_color_jpeg_hex, jpeg, sizeof(jpeg), &jpeg_size)) {
        fprintf(stderr, "failed to decode unsupported fixture hex\n");
        return 0;
    }
    for (i = 0u; i + 1u < jpeg_size; i++) {
        if (jpeg[i] == 0xffu && jpeg[i + 1u] == 0xc0u) {
            jpeg[i + 1u] = 0xc2u;
            found = 1;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "failed to find SOF0 marker in fixture\n");
        return 0;
    }

    memset(&state, 0, sizeof(state));
    state.fail_after_rows = -1;
    njInit();
    status = njDecodeMcuRows(jpeg, (int)jpeg_size, collect_mcu_row, &state);
    njDone();
    return expect_status("unsupported SOF2", status, NJ_UNSUPPORTED) &&
           expect_int("unsupported callback count", state.row_count, 0);
}

int main(void)
{
    int ok = 1;

    ok &= decode_and_check_color();
    ok &= decode_and_check_gray();
    ok &= callback_abort_is_deterministic();
    ok &= unsupported_sof_is_deterministic();

    return ok ? 0 : 1;
}
