/* The contact debouncer, on a workstation.
 *
 * The signal this has to survive cannot be produced at a desk: a German
 * doorbell runs on 8-12 V AC, and an optocoupler across the coil delivers a
 * 50 Hz chopped square wave for as long as the button is held -- not a level.
 * Getting that wrong gives a doorbell that reports one press as a dozen, and
 * the only way to notice on hardware is to stand outside and ring. So the
 * shapes are synthesised here instead: a clean press, a bouncing one, a
 * half-wave chopped one, a single induced spike, and a mains dropout in the
 * middle of a ring.
 */
#include <stdio.h>
#include <string.h>

#include "../main/contact.h"

static int fail;

static void check(const char *what, int ok, const char *detail)
{
    if (!ok) {
        fail++;
        printf("  FAIL %-30s %s\n", what, detail ? detail : "");
    }
}

/* Run `ms` milliseconds of a raw signal produced by `shape`, from `t0`. */
typedef bool (*shape_fn)(uint32_t t);

static uint32_t run(contact_deb_t *st, shape_fn shape, uint32_t t0, uint32_t ms,
                    uint16_t release_ms)
{
    uint32_t t = t0;
    for (; t < t0 + ms; t += CONTACT_POLL_MS) {
        contact_deb_step(st, shape(t), t, release_ms);
    }
    return t;
}

static bool low(uint32_t t)   { (void)t; return false; }
static bool high(uint32_t t)  { (void)t; return true; }

/* A relay or an optocoupler fed from a half-wave rectified 50 Hz supply:
 * conducting for 10 ms, dark for 10 ms. */
static bool chopped(uint32_t t) { return (t % 20) < 10; }

/* Contact bounce: the first 8 ms are anybody's guess. */
static bool bouncing(uint32_t t) { return t < 8 ? (t % 4 == 0) : true; }

int main(void)
{
    contact_deb_t st;
    char detail[128];

    /* ---- a clean press ---- */
    contact_deb_reset(&st, 0);
    uint32_t t = run(&st, low, 0, 1000, CONTACT_RELEASE_MS);
    check("idle stays idle", !st.active && st.edges == 0, NULL);

    t = run(&st, high, t, 500, CONTACT_RELEASE_MS);
    check("press detected", st.active, NULL);
    snprintf(detail, sizeof(detail), "edges=%u", (unsigned)st.edges);
    check("press counted once", st.edges == 1, detail);

    t = run(&st, low, t, 1000, CONTACT_RELEASE_MS);
    check("release detected", !st.active, NULL);
    check("release counts no edge", st.edges == 1, detail);

    /* ---- the AC case: one held button must be one edge ---- */
    contact_deb_reset(&st, 0);
    t = run(&st, chopped, 0, 2000, CONTACT_RELEASE_MS);
    check("chopped signal reads active", st.active, NULL);
    snprintf(detail, sizeof(detail), "edges=%u after 2 s of 50 Hz chop",
             (unsigned)st.edges);
    check("chopped signal is one press", st.edges == 1, detail);

    t = run(&st, low, t, 1000, CONTACT_RELEASE_MS);
    check("chopped signal releases", !st.active, NULL);
    check("still one press", st.edges == 1, detail);

    /* ---- a single induced spike must not ring the bell ---- */
    contact_deb_reset(&st, 0);
    contact_deb_step(&st, true, 0, CONTACT_RELEASE_MS);
    run(&st, low, CONTACT_POLL_MS, 1000, CONTACT_RELEASE_MS);
    snprintf(detail, sizeof(detail), "edges=%u", (unsigned)st.edges);
    check("single spike ignored", !st.active && st.edges == 0, detail);

    /* ---- contact bounce is one press, not four ---- */
    contact_deb_reset(&st, 0);
    t = run(&st, bouncing, 0, 400, CONTACT_RELEASE_MS);
    snprintf(detail, sizeof(detail), "edges=%u", (unsigned)st.edges);
    check("bounce is one press", st.active && st.edges == 1, detail);

    /* ---- a gap longer than the release really is two presses ---- */
    contact_deb_reset(&st, 0);
    t = run(&st, high, 0, 200, CONTACT_RELEASE_MS);
    t = run(&st, low,  t, CONTACT_RELEASE_MS + 100, CONTACT_RELEASE_MS);
    t = run(&st, high, t, 200, CONTACT_RELEASE_MS);
    snprintf(detail, sizeof(detail), "edges=%u", (unsigned)st.edges);
    check("two presses counted twice", st.edges == 2, detail);

    /* ---- the millisecond counter wraps after 49 days ---- */
    contact_deb_reset(&st, 0xFFFFFE00u);
    t = run(&st, high, 0xFFFFFE00u, 200, CONTACT_RELEASE_MS);
    check("press across the wrap", st.active && st.edges == 1, NULL);
    /* `t` has wrapped by now; the release must still land. */
    for (uint32_t i = 0; i < 400 / CONTACT_POLL_MS; i++) {
        contact_deb_step(&st, false, t + i * CONTACT_POLL_MS, CONTACT_RELEASE_MS);
    }
    check("release across the wrap", !st.active, NULL);

    /* ---- slugs ---- */
    struct { const char *name; const char *want; int idx; } slugs[] = {
        { "Klingel",           "klingel",           0 },
        { "Klingel Haustür",   "klingel_haustuer",  0 },
        { "Öl / Wasser",       "oel_wasser",        1 },
        { "  ",                "in1",               0 },
        { "",                  "in2",               1 },
        { "Störung!!",         "stoerung",          0 },
        { "ÄÖÜß",              "aeoeuess",          0 },
    };
    for (size_t i = 0; i < sizeof(slugs) / sizeof(slugs[0]); i++) {
        contact_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.name, sizeof(cfg.name), "%s", slugs[i].name);
        char out[CONTACT_NAME_MAX * 2];
        contact_slug(&cfg, slugs[i].idx, out, sizeof(out));
        snprintf(detail, sizeof(detail), "\"%s\" -> \"%s\", want \"%s\"",
                 slugs[i].name, out, slugs[i].want);
        check("slug", strcmp(out, slugs[i].want) == 0, detail);
    }

    /* A name that is all umlauts doubles in length; it must not run off the
     * end of a buffer that only fits the original. */
    contact_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    memset(cfg.name, 0, sizeof(cfg.name));
    for (int i = 0; i < 15; i++) {
        cfg.name[i * 2] = (char)0xC3;
        cfg.name[i * 2 + 1] = (char)0xBC;   /* "ü" fifteen times */
    }
    char tight[8];
    memset(tight, 0x7F, sizeof(tight));
    contact_slug(&cfg, 0, tight, sizeof(tight));
    snprintf(detail, sizeof(detail), "\"%s\"", tight);
    check("slug stays in its buffer", strlen(tight) < sizeof(tight), detail);

    printf("%s: contact debounce and naming\n", fail ? "FAILED" : "contact");
    return fail ? 1 : 0;
}
