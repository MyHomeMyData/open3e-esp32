/* The pure half of the contact inputs: debouncing and naming.
 *
 * No FreeRTOS, no driver, no allocation -- so test/test_contact.c can run the
 * state machine against a synthesised bell signal on a workstation, which is
 * the only practical way to check the AC case without standing at a door.
 */
#include "contact.h"

#include <string.h>

/* Connector order on the SH1.0 header: GND, 3V3, GPIO2, GPIO1. Indexed here
 * as 1 then 2, because "input 1 is GPIO1" is the only mapping nobody has to
 * look up. */
const int CONTACT_PINS[CONTACT_COUNT] = { 1, 2 };

void contact_deb_reset(contact_deb_t *st, uint32_t now_ms)
{
    memset(st, 0, sizeof(*st));
    st->since_ms = now_ms;
    st->last_active_ms = now_ms;
}

bool contact_deb_step(contact_deb_t *st, bool raw_active, uint32_t now_ms,
                      uint16_t release_ms)
{
    bool was = st->active;

    if (raw_active) {
        st->last_active_ms = now_ms;
        if (st->run < CONTACT_ATTACK) {
            st->run++;
        }
        if (st->run >= CONTACT_ATTACK) {
            st->active = true;
        }
    } else {
        /* A single quiet sample only breaks the attack run; it does not
         * release. That is what lets a chopped signal hold. */
        st->run = 0;
        if (st->active && now_ms - st->last_active_ms >= release_ms) {
            st->active = false;
        }
    }

    if (st->active == was) {
        return false;
    }
    st->since_ms = now_ms;
    if (st->active) {
        st->edges++;
    }
    return true;
}

/* ------------------------------------------------------------------ */

static void put(char *out, size_t out_sz, size_t *n, char c)
{
    if (*n + 1 < out_sz) {
        out[(*n)++] = c;
    }
}

void contact_slug(const contact_cfg_t *cfg, int idx, char *out, size_t out_sz)
{
    size_t n = 0;
    const unsigned char *s = (const unsigned char *)cfg->name;

    for (size_t i = 0; s[i] && n + 1 < out_sz; i++) {
        unsigned char c = s[i];
        /* The umlauts, spelled out. A topic is read by people, and
         * "klingel_haust_r" helps nobody. Both cases of each, as UTF-8. */
        if (c == 0xC3 && s[i + 1]) {
            unsigned char d = s[++i];
            switch (d) {
            case 0xA4: case 0x84: put(out, out_sz, &n, 'a');
                                  put(out, out_sz, &n, 'e'); continue;
            case 0xB6: case 0x96: put(out, out_sz, &n, 'o');
                                  put(out, out_sz, &n, 'e'); continue;
            case 0xBC: case 0x9C: put(out, out_sz, &n, 'u');
                                  put(out, out_sz, &n, 'e'); continue;
            case 0x9F:            put(out, out_sz, &n, 's');
                                  put(out, out_sz, &n, 's'); continue;
            default:              c = '_'; break;
            }
        } else if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            c = '_';
        }
        /* No runs of separators, and none at the front. */
        if (c == '_' && (n == 0 || out[n - 1] == '_')) {
            continue;
        }
        put(out, out_sz, &n, (char)c);
    }
    while (n && out[n - 1] == '_') {
        n--;
    }
    out[n] = '\0';

    if (!n && out_sz >= 4) {
        out[0] = 'i';
        out[1] = 'n';
        out[2] = (char)('1' + idx);
        out[3] = '\0';
    }
}
