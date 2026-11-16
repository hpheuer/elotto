/* Wire format for the node link — see include/elotto_link.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "elotto_link.h"

int elotto_link_pack(char *buf, int cap, uint32_t seq, const char *payload)
{
    return snprintf(buf, cap, ELOTTO_LINK_MAGIC " %lu %s",
                    (unsigned long)seq, payload);
}

void el_slots_fmt(char *buf, int cap, const float *z)
{
    char t[8 + EL_SLOTS * 5];
    int  n = snprintf(t, sizeof(t), ",sl="), any = 0;
    for (int s = 0; s < EL_SLOTS; s++) {
        if (s) t[n++] = '/';
        if (z[s] == z[s]) {                       /* not NAN */
            float v = z[s] > 9.99f ? 9.99f : (z[s] < -9.99f ? -9.99f : z[s]);
            n += snprintf(t + n, sizeof(t) - n, "%d",
                          (int)(v * 100.0f + (v < 0 ? -0.5f : 0.5f)));
            any = 1;
        }
    }
    t[n] = '\0';
    size_t l = strlen(buf);
    if (any && l + (size_t)n < (size_t)cap) memcpy(buf + l, t, (size_t)n + 1);
}

bool el_slots_parse(const char *reply, float *z)
{
    for (int s = 0; s < EL_SLOTS; s++) z[s] = 0.0f / 0.0f;
    const char *p = reply ? strstr(reply, ",sl=") : NULL;
    if (!p) return false;
    p += 4;
    for (int s = 0; s < EL_SLOTS; s++) {
        char *end = NULL;
        long  v = strtol(p, &end, 10);
        if (end != p) z[s] = (float)v / 100.0f;
        p = end;
        if (*p != '/') break;
        p++;
    }
    return true;
}

bool elotto_link_parse(char *msg, uint32_t *seq, char **payload)
{
    const size_t ml = sizeof(ELOTTO_LINK_MAGIC) - 1;
    if (strncmp(msg, ELOTTO_LINK_MAGIC, ml) != 0 || msg[ml] != ' ') return false;

    char *p = msg + ml + 1;
    char *end = NULL;
    unsigned long s = strtoul(p, &end, 10);
    if (end == p || *end != ' ') return false;

    *seq     = (uint32_t)s;
    *payload = end + 1;
    return **payload != '\0';
}
