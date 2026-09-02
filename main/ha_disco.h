/* Home Assistant MQTT discovery.
 *
 * Publishes retained config topics so selected datapoints appear as entities
 * without any YAML.  Units come straight from the open3e codec, so the entity
 * carries the same unit the heat pump reports.
 */
#ifndef O3E_HA_DISCO_H
#define O3E_HA_DISCO_H

#include <stdbool.h>

/* Publish (or clear) discovery for the current datapoint selection. */
void ha_disco_publish_all(void);

/* Remove every discovery topic this device published. Called when the feature
 * is switched off, so entities disappear instead of going stale. */
void ha_disco_clear_all(void);

#endif /* O3E_HA_DISCO_H */
