#include <stdio.h>
#include <stdlib.hh>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pwd.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <curl/curl.h>
#include "termbox2.h"

#define INTERVAL 30 /* update stock ticker every x seconds */
#define REFRESH_MS 200 /* refresh screen at least every x milliseconds */

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define SIZEOF(X) (sizeof(X) / sizeof(*(X)))

const char alloc_fail[] = "memory allocation failure\n";

// --- Data Structures ---
struct symbol {
	char symbol[16];
	char name[256];
	float price;
	float previous_price;
};
struct symbol *symbols = NULL;
size_t symbols_length = 0;

struct mem {
	char *memory;
	size_t size;
};

// --- Global state for the update thread ---
static volatile int run_thread = 1;

// --- Default file paths ---
const char *default_paths[] = {
	"symbols.txt",
	"symbols",
};

// --- cURL Memory Handling ---
static size_t writecb(void *contents, size_t size, size_t nmemb, void *userp) {
	size_t realsize = size * nmemb;
	struct mem *mem = (struct mem*)userp;

	char *ptr = realloc(mem->memory, mem->size + realsize + 1);
	if(ptr == NULL) {
		fprintf(stderr, "%s", alloc_fail);
		return 0; // Returning 0 will cause cURL to abort
	}

	mem->memory = ptr;
	memcpy(&(mem->memory[mem->size]), contents, realsize);
	mem->size += realsize;
	mem->memory[mem->size] = 0;
	return realsize;
}

char *handle_url(char *url) {
	CURL *curl_handle;
	CURLcode res;
	struct mem chunk = { .memory = NULL, .size = 0 };

	chunk.memory = malloc(1);
	if (!chunk.memory) {
		fprintf(stderr, "%s", alloc_fail);
		return NULL;
	}

	curl_handle = curl_easy_init();
	if (!curl_handle) {
        free(chunk.memory);
        return NULL;
    }

	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, writecb);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void*)&chunk);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L); // Follow redirects
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L); // 10 second timeout

	res = curl_easy_perform(curl_handle);
	curl_easy_cleanup(curl_handle);

	if(res != CURLE_OK) {
		fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(chunk.memory);
		return NULL;
	}

	return chunk.memory;
}

// --- Symbol Data Loading & Updating ---

static int load_symbols_from_file(FILE *f) {
    char line[sizeof(((struct symbol*)0)->symbol)];
    size_t count = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strnlen(line, sizeof(line));
        if (len == 0) continue;

        // Remove trailing newline
        if (line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        // Resize symbols array
        struct symbol *new_symbols = realloc(symbols, (count + 1) * sizeof(struct symbol));
        if (!new_symbols) {
            fprintf(stderr, "%s", alloc_fail);
            return -1;
        }
        symbols = new_symbols;

        // Initialize and copy new symbol
        memset(&symbols[count], 0, sizeof(struct symbol));
        strncpy(symbols[count].symbol, line, sizeof(symbols[count].symbol) - 1);
        count++;
    }
    symbols_length = count;
    return 0;
}


static int load_symbols(const char *user_file) {
    FILE *f = NULL;

    // Try user-provided file first
    if (user_file) {
        f = fopen(user_file, "r");
        if (!f) {
            perror(user_file);
            return -1;
        }
    } else {
        // Try default paths
        for (size_t i = 0; i < SIZEOF(default_paths); i++) {
            f = fopen(default_paths[i], "r");
            if (f) break;
        }
    }

    if (!f) {
        fprintf(stderr, "Error: Could not find a symbols file.\n");
        fprintf(stderr, "Create 'symbols.txt' or provide a path as an argument.\n");
        return -1;
    }

    int result = load_symbols_from_file(f);
    fclose(f);
    return result;
}


const char str_price[] = "\"regularMarketPrice\":";
const char str_old_price[] = "\"regularMarketPreviousClose\":";
const char str_name[] = "\"shortName\":\"";
const char query_price[] = "https://query2.finance.yahoo.com/v7/finance/options/%s";

static int find_copy(const char *haystack, const char *needle,
			char stop, char *buf, size_t len) {
	const char *start = strstr(haystack, needle);
	if (!start) return -1;

	start += strlen(needle); // Move pointer to after the needle

	const char *end = strchr(start, stop);
	if (!end) return -1;

	size_t copy_len = end - start;
	if (copy_len >= len) return -1; // Buffer too small

	memcpy(buf, start, copy_len);
	buf[copy_len] = '\0';

	return 0;
}

static int update_symbol(struct symbol *symbol) {
	char url[256], buf[64];
	int ret = -1;

	snprintf(url, sizeof(url), query_price, symbol->symbol);
	char *data = handle_url(url);
	if (!data) return -1;

	if (find_copy(data, str_price, ',', buf, sizeof(buf)) == 0) {
		symbol->price = atof(buf);
	}
	if (find_copy(data, str_old_price, ',', buf, sizeof(buf)) == 0) {
		symbol->previous_price = atof(buf);
	}
	if (find_copy(data, str_name, '"', symbol->name, sizeof(symbol->name)) == 0) {
		ret = 0; // Success only if we get the name
	}

	free(data);
	return ret;
}

// --- Background Update Thread ---
void *update_thread(void *ptr) {
	(void)ptr; // Unused parameter
	while (run_thread) {
		for (size_t i = 0; i < symbols_length && run_thread; i++) {
			update_symbol(&symbols[i]);
			// Sleep for a short duration between each symbol to not hammer the API
			usleep(200 * 1000); // 200ms
		}

		// After updating all, wait for the main interval
		if (run_thread) {
            for (int i = 0; i < INTERVAL && run_thread; ++i) {
                sleep(1);
            }
        }
	}
	return NULL;
}

// --- UI Display & Input ---

#define COL_SYMBOL 2
#define COL_NAME (COL_SYMBOL + sizeof("Symbol |") + 1)
#define COL_VARIATION (-(signed)sizeof("Variation") - 8)
#define COL_PRICE (COL_VARIATION -(signed)sizeof("| Price") - 3)

static void draw_ui(int scroll) {
	tb_clear();
	int w = tb_width();
	int h = tb_height();

	// Header
	for (int i = 0; i < w; i++) tb_set_cell(i, 0, ' ', TB_BLACK, TB_WHITE);
	tb_print(COL_SYMBOL - 2, 0, TB_BLACK, TB_WHITE, " Symbol");
	tb_print(COL_NAME - 2, 0, TB_BLACK, TB_WHITE, "| Name");
	tb_print(w + COL_PRICE - 2, 0, TB_BLACK, TB_WHITE, "| Price");
	tb_print(w + COL_VARIATION - 2, 0, TB_BLACK, TB_WHITE, "| Variation");

	// Ticker list
	int screen_y = 1;
	for (size_t i = scroll; i < symbols_length && screen_y < h; i++, screen_y++) {
		struct symbol *s = &symbols[i];
		int gain = (s->price >= s->previous_price);

		tb_print(COL_SYMBOL, screen_y, TB_DEFAULT, TB_DEFAULT, s->symbol);
		tb_print(COL_NAME, screen_y, TB_DEFAULT, TB_DEFAULT, s->name);

		// Clear right-aligned part to prevent artifacts
		for (int j = w + COL_PRICE - 2; j < w; j++) {
			tb_set_cell(j, screen_y, ' ', TB_DEFAULT, TB_DEFAULT);
        }

		tb_printf(w + COL_PRICE, screen_y, TB_DEFAULT, TB_DEFAULT, "%.2f", s->price);
		tb_printf(w + COL_VARIATION + gain, screen_y,
			gain ? TB_GREEN : TB_RED, TB_DEFAULT, "%.2f (%.2f%%)",
			s->price - s->previous_price,
			s->previous_price != 0 ? (s->price / s->previous_price * 100 - 100) : 0.0);
	}
	tb_present();
}

static void handle_input(int *running, int *scroll) {
    struct tb_event ev;
    // Wait for an event or timeout
    int type = tb_peek_event(&ev, REFRESH_MS);

    if (type < 0) { // Error
        *running = 0;
        return;
    }

    if (type == 0) { // Timeout, no event
        return;
    }

    // Process key press event
    if (ev.type == TB_EVENT_KEY) {
        int h = tb_height();
        int max_scroll = (int)symbols_length - h + 1;
        if (max_scroll < 0) max_scroll = 0;

        switch (ev.key) {
        case TB_KEY_ESC:
        case TB_KEY_CTRL_C:
            *running = 0;
            break;
        case TB_KEY_ARROW_DOWN:
            if (*scroll < max_scroll) (*scroll)++;
            break;
        case TB_KEY_ARROW_UP:
            if (*scroll > 0) (*scroll)--;
            break;
        case TB_KEY_PGDN:
            *scroll += h / 2;
            if (*scroll > max_scroll) *scroll = max_scroll;
            break;
        case TB_KEY_PGUP:
            *scroll -= h / 2;
            if (*scroll < 0) *scroll = 0;
            break;
        case TB_KEY_HOME:
            *scroll = 0;
            break;
        case TB_KEY_END:
            *scroll = max_scroll;
            break;
        default:
            // Handle character keys
            if (ev.ch == 'q') {
                *running = 0;
            } else if (ev.ch == 'j') {
                if (*scroll < max_scroll) (*scroll)++;
            } else if (ev.ch == 'k') {
                if (*scroll > 0) (*scroll)--;
            }
            break;
        }
    }
}

void print_usage(const char *prog_name) {
    printf("Usage: %s [options] [symbols_file]\n", prog_name);
    printf("A simple terminal stock ticker.\n\n");
    printf("Options:\n");
    printf("  -h, --help    Show this help message and exit.\n\n");
    printf("If [symbols_file] is not provided, it will look for 'symbols.txt' or 'symbols' in the current directory.\n");
}


int main(int argc, char *argv[]) {
    const char *symbols_file = NULL;

    // --- Command-line argument parsing ---
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        symbols_file = argv[1];
    }

	if (load_symbols(symbols_file) != 0) {
		return 1;
	}
    if (symbols_length == 0) {
        fprintf(stderr, "No symbols loaded from file. Exiting.\n");
        return 1;
    }

	curl_global_init(CURL_GLOBAL_ALL);

	if (tb_init() != 0) {
		fprintf(stderr, "termbox initialization failed: %s\n", strerror(errno));
		return 1;
	}

    int scroll = 0;
	int running = 1;
	pthread_t thread;

	pthread_create(&thread, NULL, update_thread, NULL);

	while (running) {
        draw_ui(scroll);
        handle_input(&running, &scroll);
    }

	// --- Cleanup ---
	run_thread = 0; // Signal the thread to stop
	tb_shutdown();
	pthread_join(thread, NULL);
	curl_global_cleanup();
	free(symbols);

	return 0;
}
