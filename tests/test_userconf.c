#include "test_util.h"

#include "core/arena.h"
#include "core/userconf.h"

/*
 * The config file is the one input a user edits by hand, so the tests care
 * about two things above all: that a typo is refused loudly rather than
 * silently dropped, and that a partial or failed parse never leaves a config
 * half-applied. A daemon that quietly polls the author's repositories because
 * line 12 said "repos" instead of "repo" is the failure this guards.
 */

static arena_t g_arena;

static int parse(const char *text, userconf_t *out)
{
    return userconf_parse(&g_arena, text, "test.conf", out);
}

static void test_empty_file_keeps_the_defaults(void)
{
    userconf_t def, got;

    userconf_defaults(&def);
    CHECK_EQ(parse("", &got), 0);

    CHECK_EQ(got.n_repos, def.n_repos);
    CHECK_EQ(got.n_keywords, def.n_keywords);
    CHECK_STREQ(got.profile, def.profile);
    CHECK(got.repos == def.repos);          /* borrowed, not copied */
}

static void test_comments_and_blank_lines_are_skipped(void)
{
    userconf_t got;

    CHECK_EQ(parse("# a comment\n"
                   "\n"
                   "   \t \n"
                   "   # indented comment\n"
                   "repo tinygrad/tinygrad\n", &got), 0);
    CHECK_EQ(got.n_repos, 1u);
    CHECK_STREQ(got.repos[0], "tinygrad/tinygrad");
}

/*
 * Replace, not append. Without this a user cannot drop a repo from the shipped
 * list -- they could only ever add to it, and the list is the main reason the
 * file exists.
 */
static void test_repos_replace_the_defaults(void)
{
    userconf_t def, got;

    userconf_defaults(&def);
    CHECK(def.n_repos > 2);

    CHECK_EQ(parse("repo a/b\nrepo c/d\n", &got), 0);
    CHECK_EQ(got.n_repos, 2u);
    CHECK_STREQ(got.repos[0], "a/b");
    CHECK_STREQ(got.repos[1], "c/d");
    /* Untouched sections still come from the defaults. */
    CHECK_EQ(got.n_keywords, def.n_keywords);
}

static void test_a_malformed_repo_is_refused(void)
{
    userconf_t got;

    CHECK(parse("repo tinygrad\n", &got) != 0);            /* no slash */
    CHECK(parse("repo /tinygrad\n", &got) != 0);           /* no owner */
    CHECK(parse("repo tinygrad/\n", &got) != 0);           /* no name */
    CHECK(parse("repo a/b/c\n", &got) != 0);               /* two slashes */
    CHECK(parse("repo\n", &got) != 0);                     /* no value */
}

/*
 * Phrases are the point: "good first issue" and "race condition" are ordinary
 * entries in the shipped table, and a format that could only take single words
 * would quietly turn them into "good" and "race".
 */
static void test_keywords_take_phrases(void)
{
    userconf_t got;

    CHECK_EQ(parse("keyword 24 bounty\n"
                   "keyword 5 good first issue\n"
                   "label-keyword 7 help wanted\n", &got), 0);
    CHECK_EQ(got.n_keywords, 3u);

    CHECK_STREQ(got.keywords[0].term, "bounty");
    CHECK_EQ(got.keywords[0].weight, 24);
    CHECK_EQ(got.keywords[0].label_only, 0);

    CHECK_STREQ(got.keywords[1].term, "good first issue");
    CHECK_EQ(got.keywords[1].weight, 5);

    CHECK_STREQ(got.keywords[2].term, "help wanted");
    CHECK_EQ(got.keywords[2].label_only, 1);
}

static void test_a_bad_keyword_weight_is_refused(void)
{
    userconf_t got;

    CHECK(parse("keyword bounty\n", &got) != 0);        /* weight comes first */
    CHECK(parse("keyword 24\n", &got) != 0);            /* no term */
    CHECK(parse("keyword abc bounty\n", &got) != 0);    /* not a number */
    CHECK(parse("keyword 0 bounty\n", &got) != 0);      /* a no-op, so a typo */
    CHECK(parse("keyword 9999 bounty\n", &got) != 0);   /* above the range */
}

/*
 * Negative weights are how the shipped table pushes bot noise BELOW
 * KW_SCORE_MIN instead of merely not lifting it above -- see `-10 dependabot`.
 * A parser that took only positive weights could not express the default it
 * ships with, which is the first thing a user copies.
 */
static void test_negative_weights_are_accepted(void)
{
    userconf_t got;

    CHECK_EQ(parse("keyword -10 dependabot\nkeyword -10 [bot]\n", &got), 0);
    CHECK_EQ(got.n_keywords, 2u);
    CHECK_EQ(got.keywords[0].weight, -10);
    CHECK_STREQ(got.keywords[1].term, "[bot]");
}

/* Each line is one prompt line: the default profile is a ranked list whose
 * structure the model reads, and joining it into one paragraph loses that. */
static void test_profile_lines_keep_their_structure(void)
{
    userconf_t got;

    CHECK_EQ(parse("profile Rust and WASM developer.\n"
                   "profile (1) PAID work first.\n"
                   "profile (2) Then reputation.\n", &got), 0);
    CHECK_STREQ(got.profile,
                "Rust and WASM developer.\n(1) PAID work first.\n(2) Then reputation.");
}

static void test_credentials_are_read(void)
{
    userconf_t got;

    CHECK_EQ(parse("ntfy-topic abc123\ngist-id deadbeef\n", &got), 0);
    CHECK_STREQ(got.ntfy_topic, "abc123");
    CHECK_STREQ(got.gist_id, "deadbeef");
}

/*
 * The central rule. An unknown key is a typo, and a typo that parses to
 * "nothing happened" is the worst outcome: the daemon runs, reports success,
 * and watches the wrong repositories.
 */
static void test_an_unknown_setting_is_an_error(void)
{
    userconf_t got;

    CHECK(parse("repos tinygrad/tinygrad\n", &got) != 0);   /* plural */
    CHECK(parse("Repo tinygrad/tinygrad\n", &got) != 0);    /* capitalised */
    CHECK(parse("ntfy_topic abc\n", &got) != 0);            /* underscore */
}

/*
 * A file that fails on a later line must not apply its earlier lines. The
 * caller gets a failure and keeps the built-in config; a half-applied one --
 * two of five repos, say -- would look like it worked.
 */
static void test_a_late_error_applies_nothing(void)
{
    userconf_t got;

    userconf_defaults(&got);
    CHECK(parse("repo a/b\nrepo c/d\nnonsense here\n", &got) != 0);
    /* `got` is whatever the caller had; the parse wrote nothing into it. */
    CHECK(got.n_repos > 2);
}

/* An empty section is a config that cannot work: no repo means nothing to poll,
 * and no keyword means the prefilter drops every issue before the judge. */
static void test_an_empty_section_is_refused(void)
{
    userconf_t got;

    CHECK(parse("keyword 0 bounty\n", &got) != 0);
    CHECK_EQ(parse("keyword 1 bounty\n", &got), 0);
    CHECK_EQ(got.n_keywords, 1u);
}

static void test_whitespace_is_tolerated(void)
{
    userconf_t got;

    CHECK_EQ(parse("   repo   a/b   \n"
                   "\tkeyword\t24\tbounty\t\n"
                   "repo c/d", &got), 0);      /* no trailing newline either */
    CHECK_EQ(got.n_repos, 2u);
    CHECK_STREQ(got.repos[0], "a/b");
    CHECK_STREQ(got.repos[1], "c/d");
    CHECK_STREQ(got.keywords[0].term, "bounty");
    CHECK_EQ(got.keywords[0].weight, 24);
}

/*
 * src/config.example is what a user copies, so it must be a faithful dump of
 * the compiled-in defaults rather than prose about them. Checked field by
 * field: a config.h edit that does not reach the example ships a file whose
 * first line is already wrong, and nothing else would catch it.
 *
 * This is also the end-to-end proof that every default is EXPRESSIBLE in the
 * format -- it already caught negative keyword weights, which the first parser
 * rejected outright.
 */
static void test_the_example_round_trips_to_the_defaults(void)
{
    userconf_t def, got;
    char *text;
    size_t len, i;

    userconf_defaults(&def);
    text = fixture_read("src/config.example", &len);
    CHECK_EQ(userconf_parse(&g_arena, text, "src/config.example", &got), 0);
    free(text);

    CHECK_EQ(got.n_repos, def.n_repos);
    if (got.n_repos == def.n_repos)
        for (i = 0; i < def.n_repos; i++)
            CHECK_STREQ(got.repos[i], def.repos[i]);

    CHECK_EQ(got.n_keywords, def.n_keywords);
    if (got.n_keywords == def.n_keywords) {
        for (i = 0; i < def.n_keywords; i++) {
            CHECK_STREQ(got.keywords[i].term, def.keywords[i].term);
            CHECK_EQ(got.keywords[i].weight, def.keywords[i].weight);
            CHECK_EQ(got.keywords[i].label_only, def.keywords[i].label_only);
        }
    }

    CHECK_STREQ(got.profile, def.profile);

    /* The credentials are the one thing that must NOT match a real value: the
     * example is tracked, and a real topic in it is a capability in public. */
    CHECK_STREQ(got.ntfy_topic, "REPLACE_ME_WITH_RANDOM_HEX");
    CHECK_STREQ(got.gist_id, "REPLACE_ME_WITH_GIST_ID");
}

/*
 * A missing default location is the normal first run and falls back. A missing
 * --config is a typo, and falling back there would run the built-in repo list
 * while appearing to have honoured the flag -- the same silent-wrong-config
 * failure that makes an unknown key an error.
 */
static void test_a_missing_explicit_config_is_fatal(void)
{
    userconf_t got;

    CHECK(userconf_load(&g_arena, "/nonexistent/issuewatch.conf", &got) != 0);
}

static void test_a_missing_default_config_falls_back(void)
{
    userconf_t def, got;

    userconf_defaults(&def);
    /* No HOME and no XDG_CONFIG_HOME: there is no path to try, so the built-in
     * config stands rather than the process refusing to start. */
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("HOME");
    CHECK_EQ(userconf_load(&g_arena, NULL, &got), 0);
    CHECK_EQ(got.n_repos, def.n_repos);
}

static void test_bad_arguments_are_rejected(void)
{
    userconf_t got;

    CHECK(userconf_parse(NULL, "", "t", &got) != 0);
    CHECK(userconf_parse(&g_arena, NULL, "t", &got) != 0);
    CHECK(userconf_parse(&g_arena, "", "t", NULL) != 0);
}

int main(void)
{
    if (arena_init(&g_arena, 1u << 20) != 0) {
        fprintf(stderr, "arena_init failed\n");
        return 2;
    }

    TEST_RUN(test_empty_file_keeps_the_defaults);
    TEST_RUN(test_comments_and_blank_lines_are_skipped);
    TEST_RUN(test_repos_replace_the_defaults);
    TEST_RUN(test_a_malformed_repo_is_refused);
    TEST_RUN(test_keywords_take_phrases);
    TEST_RUN(test_a_bad_keyword_weight_is_refused);
    TEST_RUN(test_negative_weights_are_accepted);
    TEST_RUN(test_profile_lines_keep_their_structure);
    TEST_RUN(test_credentials_are_read);
    TEST_RUN(test_an_unknown_setting_is_an_error);
    TEST_RUN(test_a_late_error_applies_nothing);
    TEST_RUN(test_an_empty_section_is_refused);
    TEST_RUN(test_whitespace_is_tolerated);
    TEST_RUN(test_the_example_round_trips_to_the_defaults);
    TEST_RUN(test_a_missing_explicit_config_is_fatal);
    TEST_RUN(test_a_missing_default_config_falls_back);
    TEST_RUN(test_bad_arguments_are_rejected);

    arena_destroy(&g_arena);
    TEST_REPORT();
}
