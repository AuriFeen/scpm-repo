#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <errno.h>
#include <curl/curl.h>
#include <time.h>

#define DB_DIR "/var/lib/scpm"
#define MANIFEST_DIR "/var/lib/scpm/installed"
#define LOCAL_INDEX_PATH "/var/lib/scpm/packages.json"
#define CONFIG_PATH "/etc/scpm/scpm.conf"

/* Default fallback repository URLs */
#define DEFAULT_STABLE_URL "https://aurifeen.github.io/scpm-repo/packages-stable.json"
#define DEFAULT_BLEEDING_URL "https://aurifeen.github.io/scpm-repo/packages-bleeding.json"

static volatile sig_atomic_t winch_received = 0;

static void handle_winch(int sig) {
    (void)sig;
    winch_received = 1;
}

static int get_terminal_cols(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80;
}

/* ------------------------------------------------------------------------- */
/* Configuration & Stream Management                                         */
/* ------------------------------------------------------------------------- */

static void write_default_config(void) {
    mkdir("/etc/scpm", 0755);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;

    fprintf(f, "# =========================================================================\n");
    fprintf(f, "# SCPM (Simple C Package Manager) Configuration File\n");
    fprintf(f, "# =========================================================================\n");
    fprintf(f, "# Choose your active package stream channel:\n");
    fprintf(f, "# source %s\n", DEFAULT_STABLE_URL);
    fprintf(f, "source %s\n\n", DEFAULT_BLEEDING_URL);
    fprintf(f, "# [overrides]\n");
    fprintf(f, "# Format: pkg_name=custom_archive_url|custom_configure_flags\n");
    fprintf(f, "# Example override for bleeding edge htop:\n");
    fprintf(f, "# htop=https://github.com/htop-dev/htop/archive/refs/heads/main.zip|--enable-unicode\n");
    fclose(f);
}

static int get_active_source_url(char *url_dest, size_t max_len) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        write_default_config();
        f = fopen(CONFIG_PATH, "r");
        if (!f) {
            snprintf(url_dest, max_len, "%s", DEFAULT_STABLE_URL);
            return 0;
        }
    }

    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (strncmp(p, "source ", 7) == 0) {
            snprintf(url_dest, max_len, "%s", p + 7);
            found = 1;
            break;
        }
    }
    fclose(f);

    if (!found) {
        snprintf(url_dest, max_len, "%s", DEFAULT_STABLE_URL);
    }
    return 0;
}

static int get_package_override(const char *pkg_name, char *url_dest, size_t max_url_len, char *flags_dest, size_t max_flags_len) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return -1;

    char line[1024];
    int in_overrides = 0;
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (strncmp(p, "[overrides]", 11) == 0) {
            in_overrides = 1;
            continue;
        }

        if (in_overrides && p[0] != '#' && p[0] != '\0') {
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = '\0';
                char *name = p;
                char *val = eq + 1;

                if (strcmp(name, pkg_name) == 0) {
                    char *pipe = strchr(val, '|');
                    if (pipe) {
                        *pipe = '\0';
                        snprintf(url_dest, max_url_len, "%s", val);
                        snprintf(flags_dest, max_flags_len, "%s", pipe + 1);
                    } else {
                        snprintf(url_dest, max_url_len, "%s", val);
                        flags_dest[0] = '\0';
                    }
                    found = 1;
                    break;
                }
            }
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* Self-contained terminal progress bar                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *description;
    double min_refresh_time;
} CPB_Config;

typedef struct {
    int current;
    int total;
    CPB_Config config;
    struct timespec last_update;
    int active;
} CPB_ProgressBar;

static CPB_Config cpb_get_default_config(void) {
    CPB_Config cfg = {0};
    cfg.description = "Progress";
    cfg.min_refresh_time = 0.05;
    return cfg;
}

static double cpb_timespec_diff(const struct timespec *a, const struct timespec *b) {
    return (a->tv_sec - b->tv_sec) + (a->tv_nsec - b->tv_nsec) / 1e9;
}

static void cpb_draw(CPB_ProgressBar *bar) {
    int cols = get_terminal_cols();
    if (cols < 20) cols = 20;

    const char *desc = bar->config.description ? bar->config.description : "";
    int desc_len = (int)strlen(desc);

    int pct = (bar->total > 0) ? (bar->current * 100 / bar->total) : 0;
    int bar_width = cols - desc_len - 20;
    if (bar_width < 10) bar_width = 10;

    int filled = (bar->total > 0) ? (bar->current * bar_width / bar->total) : 0;
    if (filled > bar_width) filled = bar_width;

    printf("\r\033[K");
    printf("%s [", desc);
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) printf("=");
        else if (i == filled) printf(">");
        else printf(" ");
    }
    printf("] %3d%% (%d/%d)", pct, bar->current, bar->total);
    fflush(stdout);
}

static void cpb_init(CPB_ProgressBar *bar, int current, int total, CPB_Config config) {
    memset(bar, 0, sizeof(*bar));
    bar->current = current;
    bar->total = total;
    bar->config = config;
    clock_gettime(CLOCK_MONOTONIC, &bar->last_update);
    bar->active = 0;
}

static void cpb_start(CPB_ProgressBar *bar) {
    bar->active = 1;
    cpb_draw(bar);
}

static void cpb_update(CPB_ProgressBar *bar, int current) {
    if (!bar->active) return;
    bar->current = current;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (cpb_timespec_diff(&now, &bar->last_update) >= bar->config.min_refresh_time) {
        cpb_draw(bar);
        bar->last_update = now;
    }
}

static void cpb_finish(CPB_ProgressBar *bar) {
    if (!bar->active) return;
    bar->current = bar->total;
    cpb_draw(bar);
    printf("\n");
    fflush(stdout);
    bar->active = 0;
}

/* ------------------------------------------------------------------------- */
/* SCPM core networking & execution                                          */
/* ------------------------------------------------------------------------- */

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

int run_cmd(const char *cmd) {
    int ret = system(cmd);
    if (ret == -1 || (WIFEXITED(ret) && WEXITSTATUS(ret) != 0)) {
        return -1;
    }
    return 0;
}

int init_db() {
    mkdir("/var/lib", 0755);
    mkdir(DB_DIR, 0755);
    mkdir(MANIFEST_DIR, 0755);
    return 0;
}

int is_package_installed(const char *pkg_name) {
    char db_manifest[4096];
    snprintf(db_manifest, sizeof(db_manifest), "%s/%s.list", MANIFEST_DIR, pkg_name);
    struct stat st;
    return (stat(db_manifest, &st) == 0) ? 1 : 0;
}

int fetch_remote_index(struct MemoryStruct *chunk) {
    char index_url[512];
    get_active_source_url(index_url, sizeof(index_url));

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, index_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "scpm-client/2.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || response_code != 200) {
        fprintf(stderr, "[-] Error: Failed to fetch package index from %s\n", index_url);
        return -1;
    }
    return 0;
}

int pkg_update() {
    init_db();
    printf("[+] Updating package index from active stream...\n");
    
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.size = 0;

    if (fetch_remote_index(&chunk) != 0) {
        free(chunk.memory);
        return -1;
    }

    FILE *f = fopen(LOCAL_INDEX_PATH, "w");
    if (!f) {
        free(chunk.memory);
        return -1;
    }

    fwrite(chunk.memory, 1, chunk.size, f);
    fclose(f);
    free(chunk.memory);

    printf("[+] Success! Database updated successfully.\n");
    return 0;
}

int load_package_index(struct MemoryStruct *chunk) {
    FILE *f = fopen(LOCAL_INDEX_PATH, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long length = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (length > 0) {
            chunk->memory = malloc(length + 1);
            if (chunk->memory) {
                size_t read_bytes = fread(chunk->memory, 1, length, f);
                chunk->memory[read_bytes] = '\0';
                chunk->size = read_bytes;
                fclose(f);
                return 0;
            }
        }
        fclose(f);
    }

    chunk->memory = malloc(1);
    chunk->size = 0;
    return fetch_remote_index(chunk);
}

int resolve_package_info(const char *target_pkg, const char *json_data, char *url_dest, size_t max_url_len, char *flags_dest, size_t max_flags_len, char deps_dest[][64], int *dep_count, int max_deps) {
    if (get_package_override(target_pkg, url_dest, max_url_len, flags_dest, max_flags_len) == 0) {
        printf("[*] Applied config override for package: %s\n", target_pkg);
        *dep_count = 0;
        return 0;
    }

    char search_pattern[256];
    snprintf(search_pattern, sizeof(search_pattern), "\"name\": \"%s\"", target_pkg);

    const char *p = strstr(json_data, search_pattern);
    if (!p) {
        fprintf(stderr, "[-] Error: Package '%s' not found in index.\n", target_pkg);
        return -1;
    }

    const char *url_ptr = strstr(p, "\"url\": \"");
    if (!url_ptr) return -1;
    url_ptr += 8;
    const char *end_quote = strchr(url_ptr, '"');
    size_t len = end_quote - url_ptr;
    if (len >= max_url_len) len = max_url_len - 1;
    memcpy(url_dest, url_ptr, len);
    url_dest[len] = '\0';

    flags_dest[0] = '\0';
    const char *flags_ptr = strstr(p, "\"configure_flags\": \"");
    if (flags_ptr) {
        flags_ptr += 20;
        const char *flags_end = strchr(flags_ptr, '"');
        size_t flen = flags_end - flags_ptr;
        if (flen < max_flags_len) {
            memcpy(flags_dest, flags_ptr, flen);
            flags_dest[flen] = '\0';
        }
    }

    *dep_count = 0;
    const char *dep_array_ptr = strstr(p, "\"dependencies\":");
    if (dep_array_ptr) {
        const char *bracket_start = strchr(dep_array_ptr, '[');
        const char *bracket_end = strchr(dep_array_ptr, ']');
        if (bracket_start && bracket_end && bracket_start < bracket_end) {
            const char *curr = bracket_start + 1;
            while (curr < bracket_end) {
                const char *q1 = strchr(curr, '"');
                if (!q1 || q1 > bracket_end) break;
                const char *q2 = strchr(q1 + 1, '"');
                if (!q2 || q2 > bracket_end) break;

                size_t dlen = q2 - (q1 + 1);
                if (*dep_count < max_deps && dlen < 64) {
                    memcpy(deps_dest[*dep_count], q1 + 1, dlen);
                    deps_dest[*dep_count][dlen] = '\0';
                    (*dep_count)++;
                }
                curr = q2 + 1;
            }
        }
    }
    return 0;
}

int fetch_and_extract_archive(const char *archive_url, const char *dest_dir) {
    char archive_path[512];
    snprintf(archive_path, sizeof(archive_path), "%s/source.archive", dest_dir);

    char prep_cmd[1024];
    snprintf(prep_cmd, sizeof(prep_cmd), "rm -rf \"%s\" && mkdir -p \"%s\"", dest_dir, dest_dir);
    if (run_cmd(prep_cmd) != 0) return -1;

    printf("[+] Downloading archive from %s...\n", archive_url);
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    FILE *fp = fopen(archive_path, "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, archive_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "scpm-client/2.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[-] Error: Archive download failed.\n");
        return -1;
    }

    printf("[+] Extracting archive...\n");
    char extract_cmd[2048];
    snprintf(extract_cmd, sizeof(extract_cmd), "unzip -q \"%s\" -d \"%s\" && rm -f \"%s\"", archive_path, dest_dir, archive_path);
    if (run_cmd(extract_cmd) != 0) {
        fprintf(stderr, "[-] Error: Extraction failed (ensure 'unzip' is installed).\n");
        return -1;
    }

    return 0;
}

int build_and_install_repo(const char *repo_dir, const char *pkg_name, const char *configure_flags) {
    CPB_Config config = cpb_get_default_config();
    config.description = "Installing";
    CPB_ProgressBar bar;
    cpb_init(&bar, 0, 5, config);
    cpb_start(&bar);

    char find_cmd[1024];
    snprintf(find_cmd, sizeof(find_cmd), "cd \"%s\" && echo */", repo_dir);
    FILE *fp = popen(find_cmd, "r");
    char subfolder[256] = "";
    if (fp) {
        if (fgets(subfolder, sizeof(subfolder), fp) != NULL) {
            subfolder[strcspn(subfolder, "\r\n/")] = 0;
        }
        pclose(fp);
    }

    char src_path[2048];
    if (strlen(subfolder) > 0) {
        snprintf(src_path, sizeof(src_path), "%s/%s", repo_dir, subfolder);
    } else {
        snprintf(src_path, sizeof(src_path), "%s", repo_dir);
    }

    cpb_update(&bar, 2);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "cd \"%s\" && if [ -f autogen.sh ]; then ./autogen.sh; fi && if [ -f configure ]; then ./configure %s; fi", src_path, configure_flags);
    if (run_cmd(cmd) != 0) {
        cpb_finish(&bar);
        fprintf(stderr, "[-] Error: Configure failed for '%s'.\n", pkg_name);
        return -1;
    }

    cpb_update(&bar, 3);
    snprintf(cmd, sizeof(cmd), "cd \"%s\" && make", src_path);
    if (run_cmd(cmd) != 0) {
        cpb_finish(&bar);
        fprintf(stderr, "[-] Error: Build failed for '%s'.\n", pkg_name);
        return -1;
    }

    cpb_update(&bar, 4);
    snprintf(cmd, sizeof(cmd), "cd \"%s\" && make install", src_path);
    if (run_cmd(cmd) != 0) {
        cpb_finish(&bar);
        fprintf(stderr, "[-] Error: Install failed for '%s'.\n", pkg_name);
        return -1;
    }

    cpb_finish(&bar);

    char manifest_path[2150];
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s.list", MANIFEST_DIR, pkg_name);
    FILE *mf = fopen(manifest_path, "w");
    if (mf) {
        fprintf(mf, "usr/local/bin/%s\n", pkg_name);
        fclose(mf);
    }

    printf("[+] Successfully compiled and installed %s!\n", pkg_name);
    return 0;
}

int pkg_install_internal(const char *target_pkg, const char *json_data) {
    if (is_package_installed(target_pkg)) {
        printf("[+] Package '%s' is already installed, skipping.\n", target_pkg);
        return 0;
    }

    char archive_url[512];
    char configure_flags[256] = "";
    char dependencies[32][64];
    int dep_count = 0;

    if (resolve_package_info(target_pkg, json_data, archive_url, sizeof(archive_url), configure_flags, sizeof(configure_flags), dependencies, &dep_count, 32) != 0) {
        return -1;
    }

    for (int i = 0; i < dep_count; i++) {
        printf("[+] Resolving dependency: %s\n", dependencies[i]);
        if (pkg_install_internal(dependencies[i], json_data) != 0) return -1;
    }

    if (fetch_and_extract_archive(archive_url, "/tmp/scpm_build") != 0) return -1;

    return build_and_install_repo("/tmp/scpm_build", target_pkg, configure_flags);
}

int pkg_install(const char *pkg_name) {
    init_db();
    if (is_package_installed(pkg_name)) {
        printf("[+] Package '%s' is already installed.\n", pkg_name);
        return 0;
    }

    struct MemoryStruct chunk;
    if (load_package_index(&chunk) != 0) {
        fprintf(stderr, "[-] Error: Failed to load package database. Run 'scpm update' first.\n");
        return -1;
    }

    int ret = pkg_install_internal(pkg_name, chunk.memory);
    free(chunk.memory);
    return ret;
}

int pkg_remove(const char *pkg_name) {
    char db_manifest[4096];
    snprintf(db_manifest, sizeof(db_manifest), "%s/%s.list", MANIFEST_DIR, pkg_name);
    
    FILE *f = fopen(db_manifest, "r");
    if (!f) {
        fprintf(stderr, "[-] Error: Package '%s' is not installed.\n", pkg_name);
        return -1;
    }

    printf("[+] Removing package: %s\n", pkg_name);
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) == 0) continue;
        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "/%s", line);
        unlink(full_path);
    }
    fclose(f);
    unlink(db_manifest);
    printf("[+] Successfully removed %s!\n", pkg_name);
    return 0;
}

int pkg_list() {
    init_db();
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ls -1 %s/*.list 2>/dev/null", MANIFEST_DIR);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    printf("Linux Nexus Installed Packages:\n-------------------------------\n");
    char path[512];
    int found = 0;
    while (fgets(path, sizeof(path), fp) != NULL) {
        char *filename = strrchr(path, '/');
        if (filename) {
            filename++;
            char *ext = strstr(filename, ".list");
            if (ext) *ext = '\0';
            printf(" - %s\n", filename);
            found = 1;
        }
    }
    pclose(fp);
    if (!found) printf("(No packages installed)\n");
    return 0;
}

int main(int argc, char *argv[]) {
    if (getuid() != 0) {
        fprintf(stderr, "[-] Error: SCPM must be run as root.\n");
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_winch;
    sigaction(SIGWINCH, &sa, NULL);

    curl_global_init(CURL_GLOBAL_ALL);

    if (argc < 2) {
        printf("SCPM (Simple C Package Manager) - Linux Nexus\nUsage: %s <update|install|remove|list> [pkg]\n", argv[0]);
        curl_global_cleanup();
        return 1;
    }

    int ret = 0;
    if (strcmp(argv[1], "update") == 0) {
        ret = pkg_update();
    } else if (strcmp(argv[1], "install") == 0 && argc >= 3) {
        ret = pkg_install(argv[2]);
    } else if (strcmp(argv[1], "remove") == 0 && argc >= 3) {
        ret = pkg_remove(argv[2]);
    } else if (strcmp(argv[1], "list") == 0) {
        ret = pkg_list();
    } else {
        printf("Invalid command or missing arguments.\n");
        ret = 1;
    }

    curl_global_cleanup();
    return ret;
}
