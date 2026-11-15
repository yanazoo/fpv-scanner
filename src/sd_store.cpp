#include "sd_store.h"

bool sd_valid_name(const char *name) {
    size_t n = strlen(name);
    if (n < 5 || n > 60 || name[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
    }
    return !strcasecmp(name + n - 4, ".png") || !strcasecmp(name + n - 4, ".bmp");
}

#if SD_SLOT
#include <SD.h>
#include "lcd_board.h"

bool sd_available() { return true; }

namespace {
const uint32_t IDLE_MS = 3000;  /* release the card after this long unused */
const uint32_t WAIT_MS = 8000;  /* a request from another task gives up after this */

TaskHandle_t owner;             /* the LCD task (set by its first sd_service()) */
bool mounted;
uint32_t last_use;
File reader;                    /* kept open between reads of the same file */
char reader_name[64];

void close_reader() {
    if (reader) reader.close();
    reader_name[0] = '\0';
}

bool mount() {
    last_use = millis();
    if (mounted) return true;
    mounted = lcd_sd_begin();
    return mounted;
}

void path_of(const char *name, char *path) { snprintf(path, 70, "/%s", name); }

/* The library gives every open file a 4 KB buffer of its own, on top of the
 * card's sector buffer: 512 bytes are enough for the pieces read and
 * written here, and leave the memory to Wi-Fi. */
File open_file(const char *path, const char *mode) {
    File f = SD.open(path, mode);
    if (f) f.setBufferSize(512);
    return f;
}

/* ---- the operations themselves (LCD task only) ---- */
int do_list(SdEntry *out, int max, int *count) {
    *count = 0;
    if (!mount()) return SD_NO_CARD;
    close_reader();
    File root = SD.open("/");
    if (!root) return SD_FAIL;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        const char *base = f.name();
        if (*base == '/') base++;
        if (!f.isDirectory() && sd_valid_name(base) && *count < max) {
            strlcpy(out[*count].name, base, sizeof(out[*count].name));
            out[*count].size = f.size();
            ++*count;
        }
        f.close();
    }
    root.close();
    /* newest names last on the card are not guaranteed: sort by name */
    for (int i = 1; i < *count; i++)
        for (int j = i; j > 0 && strcasecmp(out[j - 1].name, out[j].name) > 0; j--) std::swap(out[j - 1], out[j]);
    return SD_OK;
}

int do_read(const char *name, uint32_t offset, uint8_t *buf, size_t size, size_t *got, uint32_t *total) {
    *got = 0;
    if (!sd_valid_name(name)) return SD_BAD_NAME;
    if (!mount()) return SD_NO_CARD;
    if (!reader || strcmp(reader_name, name)) {
        close_reader();
        char path[70];
        path_of(name, path);
        reader = open_file(path, FILE_READ);
        if (!reader) return SD_FAIL;
        strlcpy(reader_name, name, sizeof(reader_name));
    }
    *total = reader.size();
    if (offset >= *total) return SD_OK;
    if (!reader.seek(offset)) return SD_FAIL;
    int n = reader.read(buf, min(size, (size_t)(*total - offset)));
    if (n < 0) return SD_FAIL;
    *got = n;
    return SD_OK;
}

int do_write(const char *name, uint32_t offset, const uint8_t *data, size_t size) {
    if (!sd_valid_name(name)) return SD_BAD_NAME;
    if (!mount()) return SD_NO_CARD;
    close_reader();
    char path[70];
    path_of(name, path);
    File f = open_file(path, offset ? FILE_APPEND : FILE_WRITE);
    if (!f) {
        Serial.printf("SD: cannot open %s (heap %lu)\n", name, (unsigned long)ESP.getFreeHeap());
        return SD_FAIL;
    }
    /* a piece must follow the file as it is (the library's size of a file
     * it has just made is not to be trusted: a new file starts empty) */
    size_t was = offset ? f.size() : 0, wrote = 0;
    bool ok = was == offset && (wrote = f.write(data, size)) == size;
    f.close();
    if (!ok)
        Serial.printf("SD: write %s at %lu failed (file %lu, wrote %u of %u, heap %lu)\n", name,
                      (unsigned long)offset, (unsigned long)was, (unsigned)wrote, (unsigned)size,
                      (unsigned long)ESP.getFreeHeap());
    return ok ? SD_OK : SD_FAIL;
}

int do_rename(const char *from, const char *to) {
    if (!sd_valid_name(from) || !sd_valid_name(to)) return SD_BAD_NAME;
    if (!mount()) return SD_NO_CARD;
    close_reader();
    char a[70], b[70];
    path_of(from, a), path_of(to, b);
    if (SD.exists(b)) return SD_FAIL;
    return SD.rename(a, b) ? SD_OK : SD_FAIL;
}

int do_remove(const char *name) {
    if (!sd_valid_name(name)) return SD_BAD_NAME;
    if (!mount()) return SD_NO_CARD;
    close_reader();
    char path[70];
    path_of(name, path);
    return SD.remove(path) ? SD_OK : SD_FAIL;
}

/* ---- a request from another task ---- */
enum Op { NONE, LIST, READ, WRITE, RENAME, REMOVE };
struct Request {
    Op op;
    const char *a, *b;
    uint32_t offset;
    uint8_t *buf;
    const uint8_t *data;
    size_t size, *got;
    uint32_t *total;
    SdEntry *entries;
    int max, *count;
    int result;
};
SemaphoreHandle_t gate;  /* one request at a time */
SemaphoreHandle_t done;  /* the LCD task finished it */
portMUX_TYPE handoff = portMUX_INITIALIZER_UNLOCKED;
Request *pending;        /* waiting for the LCD task (under handoff) */
void (*later)();         /* sd_run_later() (under handoff) */

int run(Request &r) {
    switch (r.op) {
    case LIST: return do_list(r.entries, r.max, r.count);
    case READ: return do_read(r.a, r.offset, r.buf, r.size, r.got, r.total);
    case WRITE: return do_write(r.a, r.offset, r.data, r.size);
    case RENAME: return do_rename(r.a, r.b);
    case REMOVE: return do_remove(r.a);
    default: return SD_FAIL;
    }
}

/* Runs r here when called from the LCD task, else hands it over. */
int submit(Request &r) {
    if (!owner || xTaskGetCurrentTaskHandle() == owner) return owner ? run(r) : SD_BUSY;
    if (!gate) return SD_BUSY;
    if (xSemaphoreTake(gate, pdMS_TO_TICKS(WAIT_MS)) != pdTRUE) return SD_BUSY;
    portENTER_CRITICAL(&handoff);
    pending = &r;
    portEXIT_CRITICAL(&handoff);
    if (xSemaphoreTake(done, pdMS_TO_TICKS(WAIT_MS)) != pdTRUE) {
        /* withdraw it, unless the LCD task has taken it: then it is being
         * run on this stack and must be waited for */
        portENTER_CRITICAL(&handoff);
        bool taken = pending != &r;
        pending = nullptr;
        portEXIT_CRITICAL(&handoff);
        if (!taken) {
            xSemaphoreGive(gate);
            return SD_BUSY;
        }
        xSemaphoreTake(done, portMAX_DELAY);
    }
    xSemaphoreGive(gate);
    return r.result;
}
} // namespace

void sd_service() {
    if (!owner) {
        owner = xTaskGetCurrentTaskHandle();
        gate = xSemaphoreCreateMutex();
        done = xSemaphoreCreateBinary();
    }
    portENTER_CRITICAL(&handoff);
    Request *r = pending;
    pending = nullptr;
    void (*fn)() = later;
    later = nullptr;
    portEXIT_CRITICAL(&handoff);
    if (r) {
        r->result = run(*r);
        xSemaphoreGive(done);
    }
    if (fn) fn();
    if (mounted && millis() - last_use > IDLE_MS) {
        close_reader();
        lcd_sd_end();
        mounted = false;
    }
}

bool sd_run_later(void (*fn)()) {
    if (!owner) return false;
    portENTER_CRITICAL(&handoff);
    bool free = !later;
    if (free) later = fn;
    portEXIT_CRITICAL(&handoff);
    return free;
}

int sd_list(SdEntry *out, int max, int *count) {
    Request r{};
    r.op = LIST, r.entries = out, r.max = max, r.count = count;
    return submit(r);
}
int sd_read(const char *name, uint32_t offset, uint8_t *buf, size_t size, size_t *got, uint32_t *total) {
    Request r{};
    r.op = READ, r.a = name, r.offset = offset, r.buf = buf, r.size = size, r.got = got, r.total = total;
    return submit(r);
}
int sd_write(const char *name, uint32_t offset, const uint8_t *data, size_t size) {
    Request r{};
    r.op = WRITE, r.a = name, r.offset = offset, r.data = data, r.size = size;
    return submit(r);
}
int sd_rename(const char *from, const char *to) {
    Request r{};
    r.op = RENAME, r.a = from, r.b = to;
    return submit(r);
}
int sd_remove(const char *name) {
    Request r{};
    r.op = REMOVE, r.a = name;
    return submit(r);
}

/* streaming writes from the LCD task (screenshots) */
static File writer;
int sd_create(const char *name) {
    if (!sd_valid_name(name)) return SD_BAD_NAME;
    if (!mount()) return SD_NO_CARD;
    close_reader();
    char path[70];
    path_of(name, path);
    writer = open_file(path, FILE_WRITE);
    return writer ? SD_OK : SD_FAIL;
}
bool sd_append(const uint8_t *data, size_t size) {
    last_use = millis();
    return writer && writer.write(data, size) == size;
}
bool sd_finish() {
    if (!writer) return false;
    writer.close();
    last_use = millis();
    return true;
}

/* (LCD task: reads the folder itself, without a list of every file) */
int sd_next_scan_name(char *name, size_t size) {
    if (!mount()) return SD_NO_CARD;
    close_reader();
    File root = SD.open("/");
    if (!root) return SD_FAIL;
    unsigned next = 1;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        const char *base = f.name();
        if (*base == '/') base++;
        unsigned n;
        char tail;
        if (sscanf(base, "SCAN%4u.BM%c", &n, &tail) == 2 && n >= next) next = n + 1;
        f.close();
    }
    root.close();
    snprintf(name, size, "SCAN%04u.BMP", next % 10000);
    return SD_OK;
}

#else /* no card slot */
bool sd_available() { return false; }
int sd_list(SdEntry *, int, int *count) { return *count = 0, SD_NO_CARD; }
int sd_read(const char *, uint32_t, uint8_t *, size_t, size_t *got, uint32_t *) { return *got = 0, SD_NO_CARD; }
int sd_write(const char *, uint32_t, const uint8_t *, size_t) { return SD_NO_CARD; }
int sd_rename(const char *, const char *) { return SD_NO_CARD; }
int sd_remove(const char *) { return SD_NO_CARD; }
int sd_next_scan_name(char *, size_t) { return SD_NO_CARD; }
int sd_create(const char *) { return SD_NO_CARD; }
bool sd_append(const uint8_t *, size_t) { return false; }
bool sd_finish() { return false; }
void sd_service() {}
bool sd_run_later(void (*)()) { return false; }
#endif
