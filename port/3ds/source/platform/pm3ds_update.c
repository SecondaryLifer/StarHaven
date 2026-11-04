/*
 * pm3ds_update.c — tell the player when a newer release is out.
 *
 * Once per launch a background thread asks GitHub where the repository's
 * latest-release page leads: it answers with a redirect to
 * .../releases/tag/vX.Y.Z, so one request and the Location header are all it
 * takes (no JSON, no API rate limit). The request goes through libcurl with
 * mbedtls, not the system's httpc: the console's own SSL module can't finish a
 * handshake with github.com any more (GitHub #51, rc D8A0A03C on hardware;
 * emulators do TLS on the PC, so they never showed it). When that version is newer than this
 * build, the bottom screen shows a notice (bottom_screen.c). Nothing is
 * downloaded or installed. No Wi-Fi, no answer or a refused connection: no
 * notice. Options -> Check at launch turns it off (pm3ds_settings.c), and
 * Options -> Check for updates asks again at any time (GitHub #82). That one
 * also shows a release the player said to skip: they did ask.
 */
#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pm3ds_brand.h"
#include "pm3ds_log.h"
#include "pm3ds_settings.h"
#include "pm3ds_update.h"

#define LATEST_URL   "https://github.com/" PM3DS_GITHUB_REPO "/releases/latest"
#ifdef PM3DS_UPDATE_TEST_VERSION   /* test builds: pretend to be an older version, so the notice shows */
#define THIS_VERSION PM3DS_UPDATE_TEST_VERSION
#else
#define THIS_VERSION PM3DS_VERSION_CODE(PM3DS_VERSION_MAJOR, PM3DS_VERSION_MINOR, PM3DS_VERSION_MICRO)
#endif

static volatile int s_latest;   /* newer release to show a notice for, 0 none (or dismissed) */
static volatile int s_found;    /* newer release the last check found */
static volatile int s_state;    /* PM3DS_UPD_* */

/* "https://github.com/<repo>/releases/tag/v1.2.3" -> 10203; 0 if it isn't one */
static int parse_tag(const char *loc) {
    const char *t = strstr(loc, "/releases/tag/");
    if (!t) return 0;
    t += 14;
    if (*t == 'v' || *t == 'V') t++;
    int a = -1, b = -1, c = 0;
    if (sscanf(t, "%d.%d.%d", &a, &b, &c) < 2) return 0;
    if (a < 0 || a > 99 || b < 0 || b > 99 || c < 0 || c > 99) return 0;
    return PM3DS_VERSION_CODE(a, b, c);
}

/* the console may still be joining its network right after launch */
static bool wait_for_wifi(int tries) {
    if (R_FAILED(acInit())) return false;
    bool up = false;
    for (int i = 0; i < tries && !up; i++) {
        u32 status = 0;
        up = R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status != 0;
        if (!up) svcSleepThread(2000 * 1000 * 1000LL);
    }
    acExit();
    return up;
}

/* curl hands over the body; only the redirect matters */
static size_t discard(char *p, size_t size, size_t n, void *u) {
    (void)p; (void)u;
    return size * n;
}

#define SOC_BUF_SIZE 0x80000

static void check_thread(void *arg) {
    bool manual = arg != NULL;
    if (!wait_for_wifi(manual ? 2 : 15)) {
        pm3ds_log(PM3DS_LOG_WARN, "update check: no Wi-Fi");
        s_state = PM3DS_UPD_NO_WIFI;
        return;
    }
    /* dev builds may have sockets up already (pm3ds_netlog.c); then they stay */
    u32 *soc = (u32 *)memalign(0x1000, SOC_BUF_SIZE);
    bool own_soc = soc && R_SUCCEEDED(socInit(soc, SOC_BUF_SIZE));
    if (!own_soc) { free(soc); soc = NULL; }

    char loc[256] = "";
    long status = 0;
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *c = rc == CURLE_OK ? curl_easy_init() : NULL;
    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, LATEST_URL);
        curl_easy_setopt(c, CURLOPT_USERAGENT, PM3DS_APP_NAME "/" PM3DS_VERSION_STR);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        /* no CA bundle on the console; only a version number is read from the
         * answer, and the notice's text is fixed */
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        rc = curl_easy_perform(c);
        if (rc == CURLE_OK) {
            char *url = NULL;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
            if (curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &url) == CURLE_OK && url) {
                strncpy(loc, url, sizeof loc - 1);
            }
        }
        curl_easy_cleanup(c);
    }
    curl_global_cleanup();
    if (own_soc) { socExit(); free(soc); }

    if (rc != CURLE_OK) pm3ds_log(PM3DS_LOG_WARN, "update check: curl %d (%s)", (int)rc, curl_easy_strerror(rc));
    int latest = parse_tag(loc);
    /* a failed check is a warning, so it reaches log.txt (GitHub #51: "doesn't work for me") */
    pm3ds_log(latest ? PM3DS_LOG_INFO : PM3DS_LOG_WARN, "update check: HTTP %ld, latest %d.%d.%d", status,
              latest / 10000, latest / 100 % 100, latest % 100);
    s_found = latest > THIS_VERSION ? latest : 0;
    if (s_found && (manual || latest != pm3ds_cfg.update_skip)) s_latest = latest;
    s_state = s_found ? PM3DS_UPD_NEWER : latest ? PM3DS_UPD_CURRENT : PM3DS_UPD_FAILED;
}

void pm3ds_update_check_start(void) {
    if (!pm3ds_cfg.update_check) return;
    /* below the main thread, above the game's (port_main.c); it mostly waits.
     * The stack is for the TLS handshake (mbedtls keeps big buffers on it) */
    s_state = PM3DS_UPD_RUNNING;
    if (!threadCreate(check_thread, NULL, 0x20000, 0x38, -2, true)) {
        pm3ds_log(PM3DS_LOG_WARN, "update check: no thread");
        s_state = PM3DS_UPD_FAILED;
    }
}

void pm3ds_update_check_now(void) {
    if (s_state == PM3DS_UPD_RUNNING) return;
    s_state = PM3DS_UPD_RUNNING;
    /* the game is running now: on a New 3DS the handshake takes the spare core */
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    Thread t = n3ds ? threadCreate(check_thread, (void *)1, 0x20000, 0x38, 2, true) : NULL;
    if (!t) t = threadCreate(check_thread, (void *)1, 0x20000, 0x38, -2, true);
    if (!t) {
        pm3ds_log(PM3DS_LOG_WARN, "update check: no thread");
        s_state = PM3DS_UPD_FAILED;
    }
}

int pm3ds_update_state(void) { return s_state; }
int pm3ds_update_found(void) { return s_found; }

int pm3ds_update_available(void) { return s_latest; }

void pm3ds_update_dismiss(bool skip) {
    if (skip && s_latest) {
        pm3ds_cfg.update_skip = s_latest;
        pm3ds_settings_save();
    }
    s_latest = 0;
}
