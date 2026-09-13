/*
 * Hand-written Aho-Corasick over the keyword table prefilter_init() is handed --
 * config.h's KEYWORDS[] unless the user's config file replaced it. Built once
 * into the permanent arena; scoring allocates nothing at all.
 *
 * Why not a token hash map: half the useful terms are phrases ("good first
 * issue", "race condition"), and useful hits land mid-token ("cudaMemcpy"
 * contains "cuda"). A trie also makes pattern bytes literal for free, which is
 * what makes "[bot]" work without any escaping.
 */

#include "pipeline/prefilter.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "core/util.h"

#define AC_NIL (-1)

/*
 * Node representation: a sorted singly-linked child list, not a 256-way
 * transition table. CONTEXT.md's "a few tens of KB" only holds for the sparse
 * form -- 256-way would be n_nodes * 256 * 4 bytes, i.e. ~156 KB for today's
 * table and megabytes if KEYWORDS[] grows to the ~200 terms the docs assume,
 * which would not fit comfortably in a 4 MB ARENA_SIZE.
 *
 * Real footprint: 4416 bytes of permanent arena for the current 15-term table,
 * measured, not estimated -- 151 pattern bytes give a 152-node capacity (150
 * actually used, two 1-byte prefixes are shared), so 152*24 B of nodes, 15*8 B
 * of output cells, 152*4 B of build-time BFS queue that stays resident because
 * arenas do not free, plus the handle and alignment padding. At 200 terms
 * averaging 12 chars that scales to ~2400 nodes: ~58 KB of nodes plus ~10 KB of
 * queue, which is the "few tens of KB" the doc promises.
 *
 * The cost of the sparse form is a linear scan per byte over a node's children.
 * Fan-out is bounded by the distinct next-bytes of the dictionary (root is the
 * widest at ~10 here), and the scan exits early because children are kept in
 * ascending byte order.
 */
typedef struct {
    int32_t child;       /* first child, AC_NIL when none */
    int32_t sibling;     /* next sibling; children sorted ascending by byte */
    int32_t fail;        /* failure link; the root's points at the root */
    int32_t out;         /* head of this node's output list, AC_NIL when empty */
    int32_t out_link;    /* nearest proper suffix node with a non-empty output */
    unsigned char byte;  /* the edge byte from this node's parent */
} ac_node_t;

/*
 * An output list rather than a single terminal index: two identical terms in
 * KEYWORDS[] must both count, and out_link chaining makes suffix-nested
 * patterns ("bot" inside "[bot]", were both present) report at the same step.
 */
typedef struct {
    int32_t kw;
    int32_t next;
} ac_out_t;

/*
 * `kws` is borrowed from the caller and outlives us. The two count arrays are
 * scoring scratch: they were stack arrays sized by N_KEYWORDS, which stopped
 * being a compile-time constant when the table became user-supplied. Holding
 * them here keeps prefilter_score() allocation-free, which is the rule that
 * mattered; single-threaded means one shared scratch is safe, and every entry
 * point zeroes them before use.
 */
struct ac_automaton {
    ac_node_t *nodes;
    ac_out_t *outs;
    int32_t n_nodes;
    int32_t n_outs;

    const kw_t *kws;
    size_t n_kws;
    int *text_counts;
    int *label_counts;
};

/*
 * ASCII-only fold. Bytes >= 0x80 are passed through as opaque bytes: UTF-8 has
 * no case mapping we could do here, and tolower() on a plain char is undefined
 * for negative values, so the cast to unsigned char is not optional.
 */
static unsigned char ac_fold(unsigned char b)
{
    return (b >= 'A' && b <= 'Z') ? (unsigned char)(b + ('a' - 'A')) : b;
}

static int32_t ac_child(const ac_t *ac, int32_t s, unsigned char b)
{
    int32_t k = ac->nodes[s].child;

    while (k != AC_NIL && ac->nodes[k].byte < b)
        k = ac->nodes[k].sibling;
    return (k != AC_NIL && ac->nodes[k].byte == b) ? k : AC_NIL;
}

/* Appends a fresh node under `parent`, keeping the child list byte-ordered. */
static int32_t ac_add_child(ac_t *ac, int32_t parent, unsigned char b, int32_t cap)
{
    int32_t *link = &ac->nodes[parent].child;
    int32_t n;

    while (*link != AC_NIL && ac->nodes[*link].byte < b)
        link = &ac->nodes[*link].sibling;

    if (ac->n_nodes >= cap)
        return AC_NIL;
    n = ac->n_nodes++;

    ac->nodes[n].child = AC_NIL;
    ac->nodes[n].sibling = *link;
    ac->nodes[n].fail = 0;
    ac->nodes[n].out = AC_NIL;
    ac->nodes[n].out_link = AC_NIL;
    ac->nodes[n].byte = b;

    *link = n;
    return n;
}

int prefilter_init(arena_t *perm, const kw_t *kws, size_t n_kws, ac_t **out)
{
    size_t total = 0;
    size_t i;
    int32_t cap;
    int32_t *queue;
    int32_t qh = 0, qt = 0;
    ac_t *ac;

    if (perm == NULL || out == NULL || kws == NULL || n_kws == 0)
        return -EINVAL;
    *out = NULL;

    for (i = 0; i < n_kws; i++)
        if (kws[i].term != NULL)
            total += strlen(kws[i].term);
    cap = (int32_t)(total + 1);  /* root, plus at most one node per pattern byte */

    ac = arena_alloc(perm, sizeof *ac, 0);
    if (ac == NULL)
        return -ENOMEM;
    ac->kws = kws;
    ac->n_kws = n_kws;
    ac->nodes = arena_alloc(perm, (size_t)cap * sizeof *ac->nodes, 0);
    if (ac->nodes == NULL)
        return -ENOMEM;
    ac->outs = arena_alloc(perm, n_kws * sizeof *ac->outs, 0);
    if (ac->outs == NULL)
        return -ENOMEM;
    ac->text_counts = arena_alloc(perm, n_kws * sizeof *ac->text_counts, 0);
    if (ac->text_counts == NULL)
        return -ENOMEM;
    ac->label_counts = arena_alloc(perm, n_kws * sizeof *ac->label_counts, 0);
    if (ac->label_counts == NULL)
        return -ENOMEM;
    queue = arena_alloc(perm, (size_t)cap * sizeof *queue, 0);
    if (queue == NULL)
        return -ENOMEM;

    ac->n_nodes = 1;
    ac->n_outs = 0;
    ac->nodes[0].child = AC_NIL;
    ac->nodes[0].sibling = AC_NIL;
    ac->nodes[0].fail = 0;
    ac->nodes[0].out = AC_NIL;
    ac->nodes[0].out_link = AC_NIL;
    ac->nodes[0].byte = 0;

    for (i = 0; i < n_kws; i++) {
        const char *t = kws[i].term;
        int32_t s = 0;
        size_t j;

        /* An empty term would make the root terminal and fire on every byte. */
        if (t == NULL || t[0] == '\0')
            continue;

        for (j = 0; t[j] != '\0'; j++) {
            unsigned char b = ac_fold((unsigned char)t[j]);
            int32_t nx = ac_child(ac, s, b);

            if (nx == AC_NIL) {
                nx = ac_add_child(ac, s, b, cap);
                if (nx == AC_NIL)
                    return -ENOMEM;
            }
            s = nx;
        }

        ac->outs[ac->n_outs].kw = (int32_t)i;
        ac->outs[ac->n_outs].next = ac->nodes[s].out;
        ac->nodes[s].out = ac->n_outs++;
    }

    /* Failure and output links by BFS over the trie. */
    {
        int32_t k;

        for (k = ac->nodes[0].child; k != AC_NIL; k = ac->nodes[k].sibling) {
            ac->nodes[k].fail = 0;
            ac->nodes[k].out_link = AC_NIL;
            queue[qt++] = k;
        }
    }
    while (qh < qt) {
        int32_t u = queue[qh++];
        int32_t v;

        for (v = ac->nodes[u].child; v != AC_NIL; v = ac->nodes[v].sibling) {
            unsigned char b = ac->nodes[v].byte;
            int32_t f = ac->nodes[u].fail;
            int32_t t;

            while (f != 0 && ac_child(ac, f, b) == AC_NIL)
                f = ac->nodes[f].fail;
            t = ac_child(ac, f, b);
            ac->nodes[v].fail = (t != AC_NIL && t != v) ? t : 0;

            t = ac->nodes[v].fail;
            ac->nodes[v].out_link = (ac->nodes[t].out != AC_NIL) ? t : ac->nodes[t].out_link;

            queue[qt++] = v;
        }
    }

    *out = ac;
    return 0;
}

static int32_t ac_step(const ac_t *ac, int32_t s, unsigned char b)
{
    for (;;) {
        int32_t nx = ac_child(ac, s, b);

        if (nx != AC_NIL)
            return nx;
        if (s == 0)
            return 0;
        s = ac->nodes[s].fail;
    }
}

/*
 * One pass over `text`, bumping per-term counts. Counts saturate at
 * KW_COUNT_CAP on the way in: the score only ever uses min(count, cap), so
 * clamping here is free and makes a 40 KB stack trace unable to overflow an int.
 */
static void ac_scan(const ac_t *ac, const char *text, int *counts)
{
    const unsigned char *p;
    int32_t s = 0;

    if (text == NULL)
        return;

    for (p = (const unsigned char *)text; *p != '\0'; p++) {
        int32_t o;

        s = ac_step(ac, s, ac_fold(*p));
        for (o = s; o != AC_NIL; o = ac->nodes[o].out_link) {
            int32_t e;

            for (e = ac->nodes[o].out; e != AC_NIL; e = ac->outs[e].next) {
                int32_t k = ac->outs[e].kw;

                if (counts[k] < KW_COUNT_CAP)
                    counts[k]++;
            }
        }
    }
}

/*
 * score = sum over terms of weight * (KW_LABEL_MULTIPLIER * label_hits + text_hits)
 *
 * The cap is per term and across the WHOLE issue, not per field: a word
 * repeated in the title, the body and a label is still one term worth at most
 * KW_COUNT_CAP hits. Label hits fill that budget first because labels are the
 * highest-signal field, so a term seen once in a label and five times in the
 * body scores weight * (2*1 + 2) with the default cap of 3, not weight * (2*1 + 3).
 *
 * label_only terms ignore the title/body counts entirely.
 */
static int ac_score_counts(const ac_t *ac)
{
    int score = 0;
    size_t i;

    for (i = 0; i < ac->n_kws; i++) {
        int nl = ac->label_counts[i] < KW_COUNT_CAP ? ac->label_counts[i]
                                                    : KW_COUNT_CAP;
        int nt = 0;

        if (!ac->kws[i].label_only) {
            int room = KW_COUNT_CAP - nl;

            nt = ac->text_counts[i] < room ? ac->text_counts[i] : room;
        }
        score += ac->kws[i].weight * (KW_LABEL_MULTIPLIER * nl + nt);
    }
    return score;
}

/* Both scoring entry points start here: the scratch is shared, so it is only
 * as good as the zeroing, and a missed reset would carry counts between two
 * unrelated issues and inflate the second. */
static void ac_reset_counts(const ac_t *ac)
{
    memset(ac->text_counts, 0, ac->n_kws * sizeof *ac->text_counts);
    memset(ac->label_counts, 0, ac->n_kws * sizeof *ac->label_counts);
}

int prefilter_score(const ac_t *ac, const issue_t *iss)
{
    int i, n;

    if (ac == NULL || iss == NULL)
        return 0;

    ac_reset_counts(ac);

    ac_scan(ac, iss->title, ac->text_counts);
    ac_scan(ac, iss->body, ac->text_counts);

    /* Clamp defensively: a malformed parse must not walk off the labels array. */
    n = iss->n_labels;
    if (n > GH_MAX_LABELS)
        n = GH_MAX_LABELS;
    for (i = 0; i < n; i++)
        ac_scan(ac, iss->labels[i], ac->label_counts);

    return ac_score_counts(ac);
}

int prefilter_score_text(const ac_t *ac, const char *text, int is_label)
{
    if (ac == NULL)
        return 0;

    ac_reset_counts(ac);
    ac_scan(ac, text, is_label ? ac->label_counts : ac->text_counts);

    return ac_score_counts(ac);
}

size_t prefilter_apply(const ac_t *ac, issue_t *issues, size_t n)
{
    size_t i, keep = 0, near = 0;
    int best_dropped = 0;

    if (ac == NULL || issues == NULL)
        return 0;

    for (i = 0; i < n; i++) {
        issues[i].kw_score = prefilter_score(ac, &issues[i]);
        if (issues[i].kw_score < KW_SCORE_MIN) {
            /*
             * KW_SCORE_MIN is the one number in config.h that has to be re-tuned
             * by hand after any KEYWORDS[] edit, and nothing reported whether it
             * sat in empty space or right on top of a cluster. A pile of issues
             * one point short is the signal that the gate, not the corpus, is
             * what is keeping the board empty.
             */
            if (issues[i].kw_score > best_dropped)
                best_dropped = issues[i].kw_score;
            if (issues[i].kw_score >= KW_SCORE_MIN * 3 / 4)
                near++;
            continue;
        }
        if (keep != i)
            issues[keep] = issues[i];  /* stable compaction, order preserved */
        keep++;
    }

    if (n > 0)
        LOGI("prefilter: %zu kept at >=%d, %zu near miss (>=%d), best dropped %d",
             keep, KW_SCORE_MIN, near, KW_SCORE_MIN * 3 / 4, best_dropped);
    return keep;
}
