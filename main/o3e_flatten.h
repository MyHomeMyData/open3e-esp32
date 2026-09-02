/* Flatten a decoded datapoint into (topic, value) pairs.
 *
 * This is open3e's mqttdump() (Open3Eclient.py:347): dict keys and list
 * indices become path segments, and every scalar leaf is published as its
 * plain string form -- "27.2", not "\"27.2\"".
 *
 * Kept separate from mqtt_pub.c so it carries no ESP-IDF dependency and can be
 * diffed against the Python original on a workstation (test/test_flatten.c).
 */
#ifndef O3E_FLATTEN_H
#define O3E_FLATTEN_H

#include <stddef.h>
#include <stdint.h>

#include "o3e_codec.h"

/* Calls `emit` once per scalar leaf with the full topic and the plain value.
 * Returns the number of leaves emitted. */
uint32_t o3e_flatten(const o3e_node_t *node, const uint8_t *payload, size_t len,
                     const char *base,
                     void (*emit)(void *user, const char *topic, const char *value),
                     void *user);

#endif /* O3E_FLATTEN_H */
