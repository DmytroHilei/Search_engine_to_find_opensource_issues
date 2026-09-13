#include "test_util.h"

#include "core/util.h"

/*
 * text_trunc_clean() is the one piece of core/ with real judgement in it: every
 * other truncation in the tree either fits or cuts flat. It decides where a
 * notification body stops reading like a sentence, so the cases below are the
 * ones that came off a real board -- dotted identifiers, version numbers, and
 * reasons that carry no terminator at all.
 */

static void test_short_text_is_untouched(void)
{
    const char *s = "unclaimed bounty, CUDA kernel.";

    CHECK_EQ(text_trunc_clean(s, strlen(s), 96), strlen(s));
    CHECK_EQ(text_trunc_clean(s, strlen(s), strlen(s)), strlen(s));
    CHECK_EQ(text_trunc_clean(NULL, 0, 96), 0u);
}

static void test_stops_at_the_last_sentence(void)
{
    /* 44 bytes to the first stop, 84 to the second, then an unfinished third. */
    const char *s = "Unclaimed bounty on the fp16 reduction path. "
                    "Reproducer is in the issue body. "
                    "The maintainer suggested reworking the";

    CHECK_EQ(text_trunc_clean(s, strlen(s), 96), 77u);
    CHECK_EQ(memcmp(s + 74, "dy.", 3), 0);
}

/* A stop below max/2 costs more text than the ragged edge, so it is refused. */
static void test_an_early_stop_does_not_collapse_the_text(void)
{
    const char *s = "Yes. Then a long unbroken clause about the sharded tile "
                    "layout that runs well past the cap";
    size_t n = text_trunc_clean(s, strlen(s), 96);

    CHECK(n > 48u);
    CHECK(n <= 96u);
    CHECK(s[n - 1] != ' ');
}

/*
 * The two false positives that actually occur in this corpus. Neither dot ends
 * a sentence, and cutting at one produces "ttnn.conv1d" -> "ttnn." on the board.
 */
static void test_dotted_identifiers_are_not_sentence_ends(void)
{
    const char *ident = "conv_config.activation is ignored on the 1D path and the "
                        "kernel silently falls back to the unfused variant here";
    const char *ver   = "Regression against CUDA 12.4 only; the same kernel is "
                        "correct under 12.3 on every architecture we tested on";
    size_t n;

    n = text_trunc_clean(ident, strlen(ident), 96);
    CHECK(n > 12u);                     /* not cut back to "conv_config." */
    CHECK(ident[n - 1] != ' ');

    n = text_trunc_clean(ver, strlen(ver), 96);
    CHECK(n > 30u);                     /* not cut back to "CUDA 12." */
    CHECK(ver[n - 1] != ' ');
}

/* No terminator and no space: nothing to back up to, so it still cuts flat. */
static void test_unbroken_run_cuts_flat(void)
{
    char s[200];

    memset(s, 'x', sizeof s);
    CHECK_EQ(text_trunc_clean(s, sizeof s, 96), 96u);
}

static void test_word_boundary_leaves_no_trailing_space(void)
{
    const char *s = "silent instant-EOS beyond the fourth layer on the hybrid "
                    "path with speculative decoding enabled and no warning";
    size_t n = text_trunc_clean(s, strlen(s), 96);

    CHECK(n <= 96u);
    CHECK(n > 48u);
    CHECK(s[n - 1] != ' ');
    CHECK(s[n] == ' ');                 /* cut fell on a word boundary */
}

/* text_trunc_clean() must never split a codepoint, whatever else it decides. */
static void test_never_splits_a_codepoint(void)
{
    char s[300];
    size_t w = 0, max, n;

    while (w + 3 <= sizeof s) {         /* U+20AC EURO SIGN, 3 bytes */
        s[w++] = (char)0xe2;
        s[w++] = (char)0x82;
        s[w++] = (char)0xac;
    }

    for (max = 1; max < w; max++) {
        n = text_trunc_clean(s, w, max);
        CHECK_EQ(n % 3u, 0u);
        CHECK(n <= max);
    }
}

static void test_utf8_trunc_len_keeps_whole_characters(void)
{
    const char *euro = "\xe2\x82\xac\xe2\x82\xac";   /* two 3-byte codepoints */

    CHECK_EQ(utf8_trunc_len(euro, 6, 6), 6u);
    CHECK_EQ(utf8_trunc_len(euro, 6, 5), 3u);
    CHECK_EQ(utf8_trunc_len(euro, 6, 4), 3u);
    CHECK_EQ(utf8_trunc_len(euro, 6, 3), 3u);
    CHECK_EQ(utf8_trunc_len(euro, 6, 2), 0u);
}

int main(void)
{
    TEST_RUN(test_short_text_is_untouched);
    TEST_RUN(test_stops_at_the_last_sentence);
    TEST_RUN(test_an_early_stop_does_not_collapse_the_text);
    TEST_RUN(test_dotted_identifiers_are_not_sentence_ends);
    TEST_RUN(test_unbroken_run_cuts_flat);
    TEST_RUN(test_word_boundary_leaves_no_trailing_space);
    TEST_RUN(test_never_splits_a_codepoint);
    TEST_RUN(test_utf8_trunc_len_keeps_whole_characters);
    TEST_REPORT();
}
