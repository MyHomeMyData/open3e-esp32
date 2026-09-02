/* HTTP server: REST API plus the static web UI from LittleFS. */
#ifndef O3E_HTTPD_API_H
#define O3E_HTTPD_API_H

#include <stdbool.h>

bool httpd_api_start(void);
void httpd_api_stop(void);

#endif /* O3E_HTTPD_API_H */
