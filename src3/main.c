#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <curl/curl.h>
#include <sys/select.h>
#include <stdarg.h>
#include "termbox2.h"

#define INTERVAL 30   /* update stock ticker every x seconds */
#define REFRESH 1000  /* refresh screen every x milliseconds */

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define SIZEOF(X) (sizeof(X) / sizeof(*(X)))

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
        printf("%s", alloc_fail);
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
        printf("%s", alloc_fail);
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
        return NULL;
    }

    *len = chunk.size;
    return chunk.memory;
}

static int load_symbols() {
    FILE *f = NULL;
    size_t i;

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
        if (!len || len > sizeof(s.symbol)) break;

        if (s.symbol[len - 1] == '\n') s.symbol[len - 1] = '\0';

        symbols = realloc(symbols, (i + 1) * sizeof(struct symbol));
        if (!symbols) {
            printf("%s", alloc_fail);
            return -1;
        }
        symbols[i] = s;
    }
    symbols_length = i;
    fclose(f);
    return 0;
}

const char str_price[] = "\"regularMarketPrice\":";
const char str_old_price[] = "\"regularMarketPreviousClose\":";
const char str_name[] = "\"shortName\":\"";

static char *strnstr_local(const char *haystack, const char *needle, size_t len) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return (char*)haystack;
    for (size_t i = 0; i + needle_len <= len; i++) {
        if (haystack[i] == needle[0] && !strncmp(haystack + i, needle, needle_len))
            return (char*)(haystack + i);
    }
    return NULL;
}

static int find_copy(const char *haystack, const char *needle, size_t hay_len,
            size_t needle_len, char stop, char *buf, size_t len) {

    char *start, *end;

    start = strnstr_local(haystack, needle, hay_len);
    if (!start) return -1;

    start += needle_len - 1;
    end = start;
    while (*end && *end != stop) end++;

    if (!*end || (size_t)(end - start) > len) return -1;

    memcpy(buf, start, end - start);
    buf[end - start] = '\0';

    return 0;
}

static int update_symbol(struct symbol *symbol) {
    char url[2048], buf[64], *data;
    size_t len;
    int ret = -1;

    snprintf(url, sizeof(url), query_price, symbol->symbol);
    data = handle_url(url, &len);
    if (!data) return -1;

    if (find_copy(data, str_price, len, sizeof(str_price), ',', buf,
            sizeof(buf)))
        goto clean;
    symbol->price = atof(buf);
    
    if (find_copy(data, str_old_price, len, sizeof(str_old_price), ',', buf,
            sizeof(buf)))
        goto clean;
    symbol->previous_price = atof(buf);

    if (find_copy(data, str_name, len, sizeof(str_name), '"',
            symbol->name, sizeof(symbol->name)))
        goto clean;

    ret = 0;
clean:
    free(data);

    return ret;
}

void ansi_sleep(long micro) {
    struct timeval tv;
    tv.tv_sec = micro / 1000000;
    tv.tv_usec = micro % 1000000;
    select(0, NULL, NULL, NULL, &tv);
}

void *update_thread(void *ptr) {
    int *run = ptr, interval, counter;

    interval = 0;
    while (*run) {
        size_t i;
        for (i = 0; i < symbols_length; i++) {
            update_symbol(&symbols[i]);
            counter = 0;
            while (counter++ < interval * 10 && *run)
                ansi_sleep(100000 / symbols_length);
            if (!*run) break;
        }
        interval = INTERVAL;
    }
    return ptr;
}

#define COL_SYMBOL 2
#define COL_NAME (COL_SYMBOL + sizeof("Symbol |") + 1)
#define COL_VARIATION (-(signed)sizeof("Variation") - 8)
#define COL_PRICE (COL_VARIATION -(signed)sizeof("| Price") - 3)

/* --- tiny text printing helpers for termbox2 --- */
void tb_print(int x, int y, uint32_t fg, uint32_t bg, const char *str) {
    while (*str) {
        tb_set_cell(x++, y, *str++, fg, bg);
    }
}
void tb_printf(int x, int y, uint32_t fg, uint32_t bg, const char *fmt, ...) {
    char buf[256];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    tb_print(x, y, fg, bg, buf);
}

int display(int *scroll) {
    struct tb_event ev;
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
        int gain, j, y = i + 1;
        symbol = symbols[i];
        gain = (symbol.price >= symbol.previous_price);
        tb_print(COL_SYMBOL, y - *scroll, TB_DEFAULT, TB_DEFAULT,
                 symbol.symbol);
        tb_print(COL_NAME, y - *scroll, TB_DEFAULT, TB_DEFAULT,
                 symbol.name);

        j = w + COL_PRICE - 2;
        while (j++ < w)
            tb_set_cell(j, y, ' ', TB_DEFAULT, TB_DEFAULT);
        tb_printf(w + COL_PRICE, y - *scroll, TB_DEFAULT, TB_DEFAULT,
                  "%.2f", symbol.price);
        tb_printf(w + COL_VARIATION + gain, y - *scroll,
                  gain ? TB_GREEN : TB_RED, TB_DEFAULT, "%.2f (%.2f%%)",
                  symbol.price - symbol.previous_price,
                  symbol.price / symbol.previous_price * 100 - 100);
        if (y - *scroll >= h) {
            bottom = 0;
            break;
        }
    }

    tb_present();

    int evres = tb_peek_event(&ev, REFRESH);
    if (evres > 0) {
        if (ev.type == TB_EVENT_KEY) {
            if (ev.key == TB_KEY_ESC || ev.ch == 'q') return -1;
            if ((ev.key == TB_KEY_ARROW_DOWN || ev.ch == 'j') && !bottom)
                (*scroll)++;
            if ((ev.key == TB_KEY_ARROW_UP || ev.ch == 'k') && *scroll)
                (*scroll)--;
        }
    } else if (evres < 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    int scroll = 0, run;
    pthread_t thread;

    if (argc > 1) {
        paths[0] = argv[1];
    }

    if (load_symbols()) {
        printf("cannot find symbols file\n");
        return -1;
    }

    curl_global_init(CURL_GLOBAL_ALL);

    if (tb_init()) {
        printf("tb_init: %s\n", strerror(errno));
        return -1;
    }

    run = 1;
    pthread_create(&thread, NULL, update_thread, &run);

    while (!display(&scroll));

    run = 0;
    tb_shutdown();
    pthread_join(thread, NULL);
    curl_global_cleanup();
    free(symbols);

    return 0;
}
