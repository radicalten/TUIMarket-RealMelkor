#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pwd.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <curl/curl.h>
#include "termbox2.h" // You must have this header available

// --- START: Changes and Additions ---

// Since config.h was removed for this single-file version,
// define the update interval here (in seconds).
#define INTERVAL 60

// Portable implementation of strnstr, as it's not standard in C.
// This ensures the code compiles on systems like Linux (glibc).
char *strnstr(const char *haystack, const char *needle, size_t len) {
    size_t needle_len;
    if (*needle == '\0') {
        return (char *)haystack;
    }
    needle_len = strlen(needle);
    for (; len >= needle_len; len--, haystack++) {
        if (*haystack == *needle && strncmp(haystack, needle, needle_len) == 0) {
            return (char *)haystack;
        }
    }
    return NULL;
}

// --- END: Changes and Additions ---


#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define SIZEOF(X) sizeof(X) / sizeof(*X)

const char alloc_fail[] = "memory allocation failure\n";

struct symbol {
	char symbol[16];
	char name[256];
	float price;
	float previous_price;
};
struct symbol *symbols = NULL;
size_t symbols_length = 0;

const char query_price[] =
	"https://query2.finance.yahoo.com/v7/finance/options/%s";

const char *paths[] = {
	"symbols",
	"symbols.txt",
};

struct mem {
	char *memory;
	size_t size;
};
struct mem chunk;

static size_t writecb(void *contents, size_t size, size_t nmemb, void *userp) {

	size_t realsize = size * nmemb;
	struct mem *mem = (struct mem*)userp;

	mem->memory = realloc(mem->memory, mem->size + realsize + 1);
	if(mem->memory == NULL) {
		printf(alloc_fail);
		return 0;
	}

	memcpy(&(mem->memory[mem->size]), contents, realsize);
	mem->size += realsize;
	mem->memory[mem->size] = 0;
	return realsize;
}

char *handle_url(char *url, size_t *len) {

	CURL *curl_handle;
	CURLcode res;

	chunk.memory = malloc(1);
	if (!chunk.memory) {
		printf(alloc_fail);
		return NULL;
	}
	chunk.size = 0;

	curl_handle = curl_easy_init();
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, writecb);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void*)&chunk);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

	res = curl_easy_perform(curl_handle);
	curl_easy_cleanup(curl_handle);

	if(res != CURLE_OK) {
		printf("curl_easy_perform() failed: %s\n",
			curl_easy_strerror(res));
        // Important: free memory even on failure
        free(chunk.memory);
        chunk.memory = NULL;
		return NULL;
	}

	*len = chunk.size;

	return chunk.memory;
}

static int load_symbols() {
    FILE *f = NULL;
    size_t i;

    // Just try each candidate in `paths[]`
    for (i = 0; i < SIZEOF(paths); i++) {
        f = fopen(paths[i], "r");
        if (f) break;
    }
    if (!f) return -1;

    for (i = 0; 1; i++) {
        struct symbol s = {0};
        size_t len;

        if (!fgets(s.symbol, sizeof(s.symbol), f)) break;

        len = strnlen(s.symbol, sizeof(s.symbol));
        if (!len || len >= sizeof(s.symbol)) continue; // Use >= to be safe

        if (s.symbol[len - 1] == '\n') s.symbol[len - 1] = '\0';
        
        // Skip empty lines
        if (s.symbol[0] == '\0') continue;

        void* new_mem = realloc(symbols, (symbols_length + 1) * sizeof(struct symbol));
        if (!new_mem) {
            printf(alloc_fail);
            fclose(f);
            return -1;
        }
        symbols = new_mem;
        symbols[symbols_length] = s;
        symbols_length++;
    }
    fclose(f);

    return 0;
}

const char str_price[] = "\"regularMarketPrice\":";
const char str_old_price[] = "\"regularMarketPreviousClose\":";
const char str_name[] = "\"shortName\":\"";

static int find_copy(const char *haystack, const char *needle, size_t hay_len,
			size_t needle_len, char stop, char *buf, size_t len) {

	const char *start, *end; // Use const char* for pointers into haystack

	start = strnstr(haystack, needle, hay_len);
	if (!start) return -1;

	start += needle_len; // Adjusted from needle_len - 1
	end = start;
	while (*end && *end != stop) end++;

	// Ensure we don't copy too much
    size_t copy_len = end - start;
	if (!*end || copy_len >= len) return -1;

	memcpy(buf, start, copy_len);
	buf[copy_len] = '\0';

	return 0;
}

static int update_symbol(struct symbol *symbol) {

	char url[2048], buf[64], *data;
	size_t len;
	int ret = -1;

	snprintf(url, sizeof(url), query_price, symbol->symbol);
	data = handle_url(url, &len);
	if (!data) return -1;

	if (find_copy(data, str_price, len, strlen(str_price), ',', buf,
			sizeof(buf)))
		goto clean;
	symbol->price = atof(buf);
	
	if (find_copy(data, str_old_price, len, strlen(str_old_price), ',', buf,
			sizeof(buf)))
		goto clean;
	symbol->previous_price = atof(buf);

	if (find_copy(data, str_name, len, strlen(str_name), '"',
			symbol->name, sizeof(symbol->name)))
		goto clean;

	ret = 0;
clean:
	free(data);

	return ret;
}

void ansi_sleep(long micro) {
    struct timespec ts;
    ts.tv_sec = micro / 1000000;
    ts.tv_nsec = (micro % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

void *update_thread(void *ptr) {

	int *run = ptr, interval, counter;

	interval = 0;
	while (*run) {
		size_t i;
		for (i = 0; i < symbols_length; i++) {
			update_symbol(&symbols[i]);
			counter = 0;
			// A simpler sleep to avoid complex timing logic
            if (interval > 0) {
			    ansi_sleep( (long)interval * 1000000 / symbols_length );
            }
			if (!*run) break;
		}
        // If it's the first run, interval is 0, so sleep for a short time
        if (interval == 0) {
            ansi_sleep(100000); 
        }
		interval = INTERVAL;
	}
	return ptr;
}

#define COL_SYMBOL 2
#define COL_NAME (COL_SYMBOL + sizeof("Symbol |") + 1)
#define COL_VARIATION (-(signed)sizeof("Variation") - 8)
#define COL_PRICE (COL_VARIATION -(signed)sizeof("| Price") - 3)

int display(int *scroll) {

	struct symbol symbol;
	int i, w, h, bottom;

	w = tb_width();
	h = tb_height();

	if ((size_t)h > symbols_length) *scroll = 0;

	tb_clear();
	for (i = 0; i < w; i++) tb_set_cell(i, 0, ' ', TB_BLACK, TB_WHITE);

	tb_print(COL_SYMBOL - 2, 0, TB_BLACK, TB_WHITE, " Symbol");
	tb_print(COL_NAME - 2, 0, TB_BLACK, TB_WHITE, "| Name");
	tb_print(w + COL_PRICE - 2, 0, TB_BLACK, TB_WHITE, "| Price");
	tb_print(w + COL_VARIATION - 2, 0, TB_BLACK, TB_WHITE, "| Variation");

	bottom = 1;
	for (i = *scroll; i < (int)symbols_length; i++) {
		int gain, j, y = i - *scroll + 1;
		symbol = symbols[i];
		gain = (symbol.price >= symbol.previous_price);
		tb_print(COL_SYMBOL, y, TB_DEFAULT, TB_DEFAULT, symbol.symbol);
		tb_print(COL_NAME, y, TB_DEFAULT, TB_DEFAULT, symbol.name);

		j = w + COL_PRICE - 2;
		while (j++ < w)
			tb_set_cell(j, y, ' ', TB_DEFAULT, TB_DEFAULT);
		tb_printf(w + COL_PRICE, y, TB_DEFAULT, TB_DEFAULT, "%.2f", symbol.price);
		tb_printf(w + COL_VARIATION + gain, y,
			gain ? TB_GREEN : TB_RED, TB_DEFAULT, "%.2f (%.2f%%)",
			symbol.price - symbol.previous_price,
            // Avoid division by zero
			(symbol.previous_price != 0) ? (symbol.price / symbol.previous_price * 100 - 100) : 0);
		if (y >= h -1) {
			bottom = 0;
			break;
		}
	}

	tb_present();

    // --- KEYBOARD INPUT FIX ---
    // The original code had `if (!tb_peek_event(...))`, which is incorrect.
    // This new block correctly waits for an event and processes it.
    struct tb_event ev;
    // Wait up to 100ms for an event. This acts as our main loop's "sleep",
    // preventing 100% CPU usage.
    int event_type = tb_peek_event(&ev, 100);

    if (event_type == TB_EVENT_KEY) {
        // A key was pressed.
        if (ev.key == TB_KEY_ESC || ev.ch == 'q') {
            return -1; // Signal to exit the main loop
        }
        if ((ev.key == TB_KEY_ARROW_DOWN || ev.ch == 'j') && !bottom) {
            (*scroll)++;
        }
        if ((ev.key == TB_KEY_ARROW_UP || ev.ch == 'k') && *scroll > 0) {
            (*scroll)--;
        }
    } else if (event_type < 0) {
        // An error occurred
        return -1;
    }
    // If it was a timeout (event_type == 0) or another event type,
    // just continue looping.

	return 0; // Signal to continue the main loop
}

int main(int argc, char *argv[]) {

	int scroll = 0, run;
	pthread_t thread;

	if (argc > 1) {
        paths[0] = argv[1];
    }

	if (load_symbols()) {
		printf("Cannot find symbols file.\n");
        printf("Please create a file named 'symbols.txt' with one stock symbol per line (e.g., AAPL, GOOG, MSFT).\n");
		return -1;
	}

	curl_global_init(CURL_GLOBAL_ALL);

	if (tb_init()) {
		printf("tb_init failed: %s\n", strerror(errno));
		return -1;
	}

	run = 1;
	pthread_create(&thread, NULL, update_thread, &run);

	// The loop continues as long as display() returns 0
	while (!display(&scroll)) ;

	run = 0; // Signal the update thread to stop
	tb_shutdown();
	pthread_join(thread, NULL); // Wait for thread to finish
	curl_global_cleanup();
	free(symbols);

	return 0;
}
