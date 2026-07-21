#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <curl/curl.h>
#include <git2.h>
#include "c_progress_bar.h"

#define DB_DIR "/var/lib/scpm"
#define MANIFEST_DIR "/var/lib/scpm/installed"
#define INDEX_URL "https://raw.githubusercontent.com/AuriFeen/scpm-repo/main/packages.json"

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

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "\n[-] Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

int run_cmd(const char *cmd) {
    int ret = system(cmd);
    if (ret != 0) {
        return -1;
    }
    return 0;
}

void init_db() {
    mkdir("/var/lib", 0755);
    mkdir(DB_DIR, 0755);
    mkdir(MANIFEST_DIR, 0755);
}

int is_package_installed(const char *pkg_name) {
    char db_manifest[1150];
    snprintf(db_manifest, sizeof(db_manifest), "%s/%s.list", MANIFEST_DIR, pkg_name);
    struct stat st;
    return (stat(db_manifest, &st) == 0);
}

int fetch_package_index(struct MemoryStruct *chunk) {
    CURL *curl_handle;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();
    if (!curl_handle) return -1;

    curl_easy_setopt(curl_handle, CURLOPT_URL, INDEX_URL);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "scpm-client/1.0");
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);

    res = curl_easy_perform(curl_handle);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl_handle);
        curl_global_cleanup();
        return -1;
    }

    long response_code = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();
    return (response_code == 200) ? 0 : -1;
}

int package_exists_in_index(const char *pkg_name, const char *json_data) {
    char search_pattern[256];
    snprintf(search_pattern, sizeof(search_pattern), "\"name\": \"%s\"", pkg_name);
    return (strstr(json_data, search_pattern) != NULL);
}

int resolve_package_info(const char *target_pkg, const char *json_data, char *url_dest, size_t max_url_len, char *flags_dest, size_t max_flags_len, char deps_dest[][64], int *dep_count, int max_deps) {
    char search_pattern[256];
    snprintf(search_pattern, sizeof(search_pattern), "\"name\": \"%s\"", target_pkg);

    char *p = (char *)strstr(json_data, search_pattern);
    if (!p) return -1;

    // Locate URL
    char *url_ptr = strstr(p, "\"url\": \"");
    if (!url_ptr) return -1;
    url_ptr += 8;
    char *end_quote = strchr(url_ptr, '"');
    if (!end_quote) return -1;

    size_t len = end_quote - url_ptr;
    if (len >= max_url_len) return -1;
    strncpy(url_dest, url_ptr, len);
    url_dest[len] = '\0';

    // Locate optional configure_flags
    flags_dest[0] = '\0';
    char *flags_ptr = strstr(p, "\"configure_flags\": \"");
    if (flags_ptr) {
        flags_ptr += 20;
        char *flags_end = strchr(flags_ptr, '"');
        if (flags_end) {
            size_t flen = flags_end - flags_ptr;
            if (flen < max_flags_len) {
                strncpy(flags_dest, flags_ptr, flen);
                flags_dest[flen] = '\0';
            }
        }
    }

    // Locate Dependencies array
    *dep_count = 0;
    char *dep_array_ptr = strstr(p, "\"dependencies\":");
    if (dep_array_ptr) {
        char *bracket_start = strchr(dep_array_ptr, '[');
        char *bracket_end = strchr(dep_array_ptr, ']');
        if (bracket_start && bracket_end && bracket_start < bracket_end) {
            char *curr = bracket_start + 1;
            while (curr < bracket_end) {
                char *q1 = strchr(curr, '"');
                if (!q1 || q1 > bracket_end) break;
                char *q2 = strchr(q1 + 1, '"');
                if (!q2 || q2 > bracket_end) break;

                size_t dlen = q2 - (q1 + 1);
                if (dlen < 64 && *dep_count < max_deps) {
                    strncpy(deps_dest[*dep_count], q1 + 1, dlen);
                    deps_dest[*dep_count][dlen] = '\0';
                    (*dep_count)++;
                }
                curr = q2 + 1;
            }
        }
    }

    return 0;
}

int clone_package_repo(const char *repo_url, const char *dest_dir) {
    git_repository *repo = NULL;
    int error = 0;

    git_libgit2_init();
    error = git_clone(&repo, repo_url, dest_dir, NULL);
    if (error < 0) {
        git_libgit2_shutdown();
        return -1;
    }

    if (repo) git_repository_free(repo);
    git_libgit2_shutdown();
    return 0;
}

// Forward declaration for recursion
int pkg_install_internal(const char *pkg_name, const char *json_data);

int pkg_install(const char *pkg_name) {
    init_db();

    if (is_package_installed(pkg_name)) {
        printf("[+] Package '%s' is already installed.\n", pkg_name);
        return 0;
    }

    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    printf("[+] Fetching repository index for package '%s'...\n", pkg_name);
    if (fetch_package_index(&chunk) != 0) {
        free(chunk.memory);
        fprintf(stderr, "[-] Failed to fetch package index.\n");
        return -1;
    }

    int ret = pkg_install_internal(pkg_name, chunk.memory);
    free(chunk.memory);
    return ret;
}

int pkg_install_internal(const char *pkg_name, const char *json_data) {
    if (is_package_installed(pkg_name)) {
        printf("[+] Dependency '%s' is already installed, skipping.\n", pkg_name);
        return 0;
    }

    char repo_url[512];
    char configure_flags[256] = "";
    char dependencies[32][64];
    int dep_count = 0;

    if (resolve_package_info(pkg_name, json_data, repo_url, sizeof(repo_url), configure_flags, sizeof(configure_flags), dependencies, &dep_count, 32) != 0) {
        fprintf(stderr, "[-] Package/Dependency '%s' not found in index.\n", pkg_name);
        return -1;
    }

    // Recursively resolve and install dependencies first
    for (int i = 0; i < dep_count; i++) {
        printf("[+] Resolving dependency: %s (needed by %s)\n", dependencies[i], pkg_name);
        if (pkg_install_internal(dependencies[i], json_data) != 0) {
            fprintf(stderr, "[-] Failed to install dependency '%s'\n", dependencies[i]);
            return -1;
        }
    }

    CPB_Config config = cpb_get_default_config();
    config.description = "Installing";
    config.min_refresh_time = 0.05;

    CPB_ProgressBar bar;
    cpb_init(&bar, 0, 7, config);
    cpb_start(&bar);
    int current_step = 0;
    int last_cols = get_terminal_cols();

    #define CHECK_RESIZE(step_val) \
        current_step = step_val; \
        if (winch_received) { \
            winch_received = 0; \
            int cols = get_terminal_cols(); \
            if (cols != last_cols) { \
                last_cols = cols; \
                cpb_finish(&bar); \
                cpb_init(&bar, 0, 7, config); \
                cpb_start(&bar); \
                cpb_update(&bar, current_step); \
            } \
        }

    // Step 1 & 2: URL & Dependencies already resolved
    CHECK_RESIZE(2);
    cpb_update(&bar, 2);
    cpb_finish(&bar);
    printf("[+] Installing %s (Resolved URL: %s)\n", pkg_name, repo_url);
    fflush(stdout);
    cpb_init(&bar, 0, 7, config);
    cpb_start(&bar);
    cpb_update(&bar, 2);

    char clone_dir[512], stage_dir[512], db_manifest[1150], cmd[2048];
    snprintf(clone_dir, sizeof(clone_dir), "/tmp/scpm_build_%s", pkg_name);
    snprintf(stage_dir, sizeof(stage_dir), "/tmp/scpm_stage_%s", pkg_name);
    snprintf(db_manifest, sizeof(db_manifest), "%s/%s.list", MANIFEST_DIR, pkg_name);

    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" \"%s\"", clone_dir, stage_dir);
    run_cmd(cmd);

    // Step 3: Clone Repository
    CHECK_RESIZE(3);
    cpb_update(&bar, 3);
    cpb_finish(&bar);
    printf("[+] Cloning repository via libgit2...\n");
    fflush(stdout);
    cpb_init(&bar, 0, 7, config);
    cpb_start(&bar);
    cpb_update(&bar, 3);

    if (clone_package_repo(repo_url, clone_dir) != 0) {
        cpb_finish(&bar);
        fprintf(stderr, "[-] Repository clone failed for %s.\n", pkg_name);
        return -1;
    }

    // Step 4: Build package (with automatic error log fallback check)
    CHECK_RESIZE(4);
    cpb_update(&bar, 4);
    cpb_finish(&bar);
    printf("[+] Building package (configure/make)...\n");
    fflush(stdout);
    cpb_init(&bar, 0, 7, config);
    cpb_start(&bar);
    cpb_update(&bar, 4);

    snprintf(cmd, sizeof(cmd), "cd \"%s\" && if [ -f autogen.sh ]; then ./autogen.sh; fi && if [ -f configure ]; then ./configure %s; fi && make > /tmp/scpm_build_err.log 2>&1", clone_dir, configure_flags);
    if (run_cmd(cmd) != 0) {
        // Automatically check build error log for missing dependencies/headers
        FILE *log_file = fopen("/tmp/scpm_build_err.log", "r");
        int auto_installed = 0;
        if (log_file) {
            char log_line[512];
            char missing_pkg[64] = "";
            while (fgets(log_line, sizeof(log_line), log_file)) {
                if (strstr(log_line, "curses.h") || strstr(log_line, "-lncurses")) {
                    strcpy(missing_pkg, "ncurses");
                    break;
                } else if (strstr(log_line, "glib-2.0") || strstr(log_line, "glib.h")) {
                    strcpy(missing_pkg, "glib");
                    break;
                }
            }
            fclose(log_file);

            if (strlen(missing_pkg) > 0 && strcmp(missing_pkg, pkg_name) != 0) {
                if (package_exists_in_index(missing_pkg, json_data)) {
                    cpb_finish(&bar);
                    printf("[+] Auto-detected missing dependency '%s' from build logs. Installing dynamically...\n", missing_pkg);
                    if (pkg_install_internal(missing_pkg, json_data) == 0) {
                        cpb_init(&bar, 0, 7, config);
                        cpb_start(&bar);
                        cpb_update(&bar, 4);
                        
                        snprintf(cmd, sizeof(cmd), "cd \"%s\" && make > /tmp/scpm_build_err.log 2>&1", clone_dir);
                        if (run_cmd(cmd) == 0) {
                            auto_installed = 1;
                        }
                    }
                }
            }
        }

        if (!auto_installed) {
            cpb_finish(&bar);
            system("cat /tmp/scpm_build_err.log");
            fprintf(stderr, "[-] Build process failed for %s.\n", pkg_name);
            return -1;
        }
    }

    // Step 5: Stage files
    CHECK_RESIZE(5);
    cpb_update(&bar, 5);
    cpb_finish(&bar);
    printf("[+] Staging files...\n");
    fflush(stdout);
    cpb_init(&bar, 0, 7, config);
    cpb_start(&bar);
    cpb_update(&bar, 5);

    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\" && cd \"%s\" && make DESTDIR=\"%s\" install > /dev/null 2>&1", stage_dir, clone_dir, stage_dir);
    if (run_cmd(cmd) != 0) {
        cpb_finish(&bar);
        fprintf(stderr, "[-] Staging installation failed for %s.\n", pkg_name);
        return -1;
    }

    // Step 6: Generate manifest & Deploy root
    CHECK_RESIZE(6);
    cpb_update(&bar, 6);
    cpb_finish(&bar);
    printf("[+] Generating manifest & deploying files...\n");
    fflush(stdout);
    cpb_init(&bar, 0, 7, config);
    cpb_start(&bar);
    cpb_update(&bar, 6);

    snprintf(cmd, sizeof(cmd), "find \"%s\" -mindepth 1 -printf '%%P\n' | sort -r > \"%s\"", stage_dir, db_manifest);
    run_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "rsync -a \"%s/\" /", stage_dir);
    if (run_cmd(cmd) != 0) {
        cpb_finish(&bar);
        fprintf(stderr, "[-] Deployment to system root failed for %s.\n", pkg_name);
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" \"%s\"", clone_dir, stage_dir);
    run_cmd(cmd);

    // Step 7: Complete
    CHECK_RESIZE(7);
    cpb_update(&bar, 7);
    cpb_finish(&bar);

    printf("[+] [SCPM] Successfully installed %s!\n", pkg_name);
    return 0;
}

int pkg_remove(const char *pkg_name) {
    char db_manifest[1150];
    snprintf(db_manifest, sizeof(db_manifest), "%s/%s.list", MANIFEST_DIR, pkg_name);

    FILE *f = fopen(db_manifest, "r");
    if (!f) {
        fprintf(stderr, "[-] Package '%s' is not installed.\n", pkg_name);
        return -1;
    }

    printf("[+] [SCPM] Removing package: %s\n", pkg_name);
    char lines[1024][512];
    int count = 0;
    while (fgets(lines[count], sizeof(lines[count]), f) && count < 1024) {
        lines[count][strcspn(lines[count], "\n")] = 0;
        count++;
    }
    fclose(f);

    for (int i = 0; i < count; i++) {
        if (strlen(lines[i]) == 0) continue;
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "/%s", lines[i]);
        struct stat st;
        if (lstat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) rmdir(full_path);
            else unlink(full_path);
        }
    }

    unlink(db_manifest);
    printf("[+] [SCPM] Successfully removed %s!\n", pkg_name);
    return 0;
}

int pkg_list() {
    init_db();
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ls -1 %s/*.list 2>/dev/null", MANIFEST_DIR);

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    printf("SCPM Installed Packages:\n");
    printf("------------------------\n");
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
    if (!found) printf("(No packages installed via SCPM)\n");
    return 0;
}

int print_usage(const char *prog) {
    printf("SCPM (Simple Custom Package Manager)\n");
    printf("Usage: %s <command> [arguments]\n", prog);
    return 1;
}

int main(int argc, char *argv[]) {
    if (getuid() != 0) {
        fprintf(stderr, "[-] Error: SCPM must be run as root.\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_winch;
    sigaction(SIGWINCH, &sa, NULL);

    if (argc < 2) return print_usage(argv[0]);

    if (strcmp(argv[1], "install") == 0) {
        if (argc < 3) return print_usage(argv[0]);
        return pkg_install(argv[2]);
    } else if (strcmp(argv[1], "remove") == 0) {
        if (argc < 3) return print_usage(argv[0]);
        return pkg_remove(argv[2]);
    } else if (strcmp(argv[1], "list") == 0) {
        return pkg_list();
    } else {
        return print_usage(argv[0]);
    }
}
