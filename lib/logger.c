#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <errno.h>
#include "recorder.h"
#include "zlib.h"


// Global file handler (per rank) for the local trace log file
FILE *__datafh;
FILE *__metafh;

// Starting timestamp of each rank
double START_TIMESTAMP;
double TIME_RESOLUTION = 0.000001;

// Filename to integer map
hashmap_map *__filename2id_map;

// set to true after initialization (log_init) and before exit()
bool __recording;


/* A sliding window for peephole compression */
#define RECORD_WINDOW_SIZE 3
Record __record_window[RECORD_WINDOW_SIZE];

/* For zlib compression */
#define ZLIB_BUF_SIZE 4096
z_stream __zlib_stream;

/* compression mode */
CompressionMode __compression_mode = COMP_ZLIB;


RecorderLocalDef __local_def;


// Buffer the tracing records. Dump when its full
struct MemBuf {
    void *buffer;
    int size;
    int pos;
    void (*release) (struct MemBuf*);
    void (*append)(struct MemBuf*, const void* ptr, int length);
    void (*dump) (struct MemBuf*);
};
void membufRelease(struct MemBuf *membuf) {
    free(membuf->buffer);
    membuf->pos = 0;
}
void membufAppend(struct MemBuf* membuf, const void *ptr, int length) {
    printf("append\n");
    if (length >= membuf->size) {
        RECORDER_REAL_CALL(fwrite) (ptr, 1, length, __datafh);
        return;
    }
    if (membuf->pos + length >= membuf->size) {
        membuf->dump(membuf);
    }
    memcpy(membuf->buffer+membuf->pos, ptr, length);
    membuf->pos += length;
}
void membufDump(struct MemBuf *membuf) {
    printf("dump\n");
    RECORDER_REAL_CALL(fwrite) (membuf->buffer, 1, membuf->pos, __datafh);
    membuf->pos = 0;
}
void membufInit(struct MemBuf* membuf) {
    membuf->size = 12*1024*1024;            // 12M
    membuf->buffer = malloc(membuf->size);
    membuf->pos = 0;
    membuf->release = membufRelease;
    membuf->append = membufAppend;
    membuf->dump = membufDump;
}
struct MemBuf __membuf;


static inline int startsWith(const char *pre, const char *str) {
    size_t lenpre = strlen(pre),
           lenstr = strlen(str);
    return lenstr < lenpre ? 0 : memcmp(pre, str, lenpre) == 0;
}


static inline Record get_diff_record(Record old_record, Record new_record) {
    Record diff_record;
    diff_record.status = 0b10000000;
    diff_record.arg_count = 999;    // initialize an impossible large value at first

    // Same function should normally have the same number of arguments
    if (old_record.arg_count != new_record.arg_count)
        return diff_record;

    // Get the number of different arguments
    int count = 0;
    for(int i = 0; i < old_record.arg_count; i++)
        if(strcmp(old_record.args[i], new_record.args[i]) !=0)
            count++;

    // record.args store only the different arguments
    // record.status keeps track the position of different arguments
    diff_record.arg_count = count;
    int idx = 0;
    diff_record.args = malloc(sizeof(char *) * count);
    static char diff_bits[] = {0b10000001, 0b10000010, 0b10000100, 0b10001000,
                                0b10010000, 0b10100000, 0b11000000};
    for(int i = 0; i < old_record.arg_count; i++) {
        if(strcmp(old_record.args[i], new_record.args[i]) !=0) {
            diff_record.args[idx++] = new_record.args[i];
            if(i < 7) {
                diff_record.status = diff_record.status | diff_bits[i];
            }
        }
    }
    return diff_record;
}

// 0. Helper function, write all function arguments
static inline void writeArguments(FILE* f, int arg_count, char** args) {
    char invalid_str[] = "???";
    for(int i = 0; i < arg_count; i++) {
        __membuf.append(&__membuf, " ", 1);
        if(args[i])
            __membuf.append(&__membuf, args[i], strlen(args[i]));
        else
            __membuf.append(&__membuf, invalid_str, strlen(invalid_str));
    }
    __membuf.append(&__membuf, "\n", 1);
}

/* Mode 1. Write record in plan text format */
// tstart tend function args...
static inline void writeInText(FILE *f, Record record) {
    const char* func = get_function_name_by_id(record.func_id);
    char* tstart = ftoa(record.tstart);
    char* tend = ftoa(record.tend);
    __membuf.append(&__membuf, tstart, strlen(tstart));
    __membuf.append(&__membuf, " ", 1);
    __membuf.append(&__membuf, tend, strlen(tend));
    __membuf.append(&__membuf, " ", 1);
    __membuf.append(&__membuf, func, strlen(func));
    writeArguments(f, record.arg_count, record.args);
}

// Mode 2. Write in binary format, no compression
static inline void writeInBinary(FILE *f, Record record) {
    int tstart = (record.tstart - START_TIMESTAMP) / TIME_RESOLUTION;
    int tend   = (record.tend - START_TIMESTAMP) / TIME_RESOLUTION;
    __membuf.append(&__membuf, &(record.status), sizeof(char));
    __membuf.append(&__membuf, &tstart, sizeof(int));
    __membuf.append(&__membuf, &tend, sizeof(int));
    __membuf.append(&__membuf, &(record.func_id), sizeof(unsigned char));
    writeArguments(f, record.arg_count, record.args);
}


// Mode 3. Write in Recorder format (binary + peephole compression)
static inline void writeInRecorder(FILE* f, Record new_record) {

    bool compress = false;
    Record diff_record;
    int min_diff_count = 999;
    char ref_window_id;
    for(int i = 0; i < RECORD_WINDOW_SIZE; i++) {
        Record record = __record_window[i];
        // Only meets the following conditions that we consider to compress it:
        // 1. same function as the one in sliding window
        // 2. has at least 1 arguments
        // 3. has less than 8 arguments
        // 4. the number of different arguments is less the number of total arguments
        if ((record.func_id == new_record.func_id) && (new_record.arg_count < 8) &&
             (new_record.arg_count > 0) && (record.arg_count > 0)) {
            Record tmp_record = get_diff_record(record, new_record);

            // Cond.4
            if(tmp_record.arg_count >= new_record.arg_count)
                continue;

            // Currently has the minimum number of different arguments
            if(tmp_record.arg_count < min_diff_count) {
                min_diff_count = tmp_record.arg_count;
                ref_window_id = i;
                compress = true;
                diff_record = tmp_record;
            }
        }
    }

    if (compress) {
        diff_record.tstart = new_record.tstart;
        diff_record.tend = new_record.tend;
        diff_record.func_id = ref_window_id;
        writeInBinary(__datafh, diff_record);
    } else {
        new_record.status = 0b00000000;
        writeInBinary(__datafh, new_record);
    }

    __record_window[2] = __record_window[1];
    __record_window[1] = __record_window[0];
    __record_window[0] = new_record;

}

/* Mode 4. Compress the plain text with zlib and write it out */
static inline void writeInZlib(FILE *f, Record record) {
    static char in_buf[ZLIB_BUF_SIZE];
    static char out_buf[ZLIB_BUF_SIZE];
    sprintf(in_buf, "%f %f %s", record.tstart, record.tend, get_function_name_by_id(record.func_id));
    for(int i = 0; i < record.arg_count; i++) {
        strcat(in_buf, " ");
        if(record.args[i])
            strcat(in_buf, record.args[i]);
        else                    // some null argument ?
            strcat(in_buf, "???");
    }
    strcat(in_buf, "\n");

    __zlib_stream.avail_in = strlen(in_buf);
    __zlib_stream.next_in = in_buf;
    do {
        __zlib_stream.avail_out = ZLIB_BUF_SIZE;
        __zlib_stream.next_out = out_buf;
        int ret = deflate(&__zlib_stream, Z_NO_FLUSH);    /* no bad return value */
        unsigned have = ZLIB_BUF_SIZE - __zlib_stream.avail_out;
        RECORDER_REAL_CALL(fwrite) (out_buf, 1, have, f);
    } while (__zlib_stream.avail_out == 0);
}

void zlib_init() {
    /* allocate deflate state */
    __zlib_stream.zalloc = Z_NULL;
    __zlib_stream.zfree = Z_NULL;
    __zlib_stream.opaque = Z_NULL;
    deflateInit(&__zlib_stream, Z_DEFAULT_COMPRESSION);
}
void zlib_exit() {
    // Write out everythin zlib's buffer
    char out_buf[ZLIB_BUF_SIZE];
    do {

        __zlib_stream.avail_out = ZLIB_BUF_SIZE;
        __zlib_stream.next_out = out_buf;
        int ret = deflate(&__zlib_stream, Z_FINISH);    /* no bad return value */
        unsigned have = ZLIB_BUF_SIZE - __zlib_stream.avail_out;
        RECORDER_REAL_CALL(fwrite) (out_buf, 1, have, __datafh);
    } while (__zlib_stream.avail_out == 0);
    // Clean up and end the Zlib
    (void)deflateEnd(&__zlib_stream);
}



void write_record(Record new_record) {
    if (!__recording) return;       // have not initialized yet

    __local_def.total_records++;
    __local_def.function_count[new_record.func_id]++;

    switch(__compression_mode) {
        case COMP_TEXT:
            writeInText(__datafh, new_record);
            break;
        case COMP_BINARY:
            writeInBinary(__datafh, new_record);
            break;
        case COMP_ZLIB:
            writeInZlib(__datafh, new_record);
            break;
        default:
            writeInRecorder(__datafh, new_record);
            break;
    }
}

void logger_init(int rank, int nprocs) {

    // Map the functions we will use later
    // We did not intercept fprintf
    MAP_OR_FAIL(fopen)
    MAP_OR_FAIL(fclose)
    MAP_OR_FAIL(fwrite)
    MAP_OR_FAIL(ftell)
    MAP_OR_FAIL(fseek)
    MAP_OR_FAIL(mkdir)

    // Initialize the global values
    __filename2id_map = hashmap_new();

    START_TIMESTAMP = recorder_wtime();

    RECORDER_REAL_CALL(mkdir) ("logs", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    char logfile_name[256];
    char metafile_name[256];
    sprintf(logfile_name, "logs/%d.itf", rank);
    sprintf(metafile_name, "logs/%d.mt", rank);
    __datafh = RECORDER_REAL_CALL(fopen) (logfile_name, "wb");
    __metafh = RECORDER_REAL_CALL(fopen) (metafile_name, "wb");

    // Global metadata, include compression mode, time resolution
    const char* comp_mode = getenv("RECORDER_COMPRESSION_MODE");
    if (comp_mode) __compression_mode = atoi(comp_mode);
    if (__compression_mode == COMP_ZLIB)        // Initialize zlib if compression mode is COMP_ZLIB
        zlib_init();
    if (rank == 0) {
        FILE* global_metafh = RECORDER_REAL_CALL(fopen) ("logs/recorder.mt", "wb");
        RecorderGlobalDef global_def = {
            .time_resolution = TIME_RESOLUTION,
            .total_ranks = nprocs,
            .compression_mode = __compression_mode,
            .peephole_window_size = RECORD_WINDOW_SIZE
        };
        RECORDER_REAL_CALL(fwrite)(&global_def, sizeof(RecorderGlobalDef), 1, global_metafh);
        RECORDER_REAL_CALL(fclose)(global_metafh);
    }

    membufInit(&__membuf);
    __recording = true;
}


void logger_exit() {
    __recording = false;

    /* Call this before close file since we still could have data in zlib's buffer waiting to write out*/
    if (__compression_mode == COMP_ZLIB)
        zlib_exit();


    /* Write out local metadata information */
    __local_def.num_files = hashmap_length(__filename2id_map),
    __local_def.start_timestamp = START_TIMESTAMP,
    __local_def.end_timestamp = recorder_wtime(),
    RECORDER_REAL_CALL(fwrite) (&__local_def, sizeof(__local_def), 1, __metafh);

    /* Write out filename mappings, we call stat() to get file size
     * since __datafh is already closed (null), the stat() function
     * won't be intercepted. */
    if (hashmap_length(__filename2id_map) > 0 ) {
        for(int i = 0; i< __filename2id_map->table_size; i++) {
            if(__filename2id_map->data[i].in_use != 0) {
                char *filename = __filename2id_map->data[i].key;
                int id = __filename2id_map->data[i].data;
                size_t file_size = get_file_size(filename);
                int filename_len = strlen(filename);
                RECORDER_REAL_CALL(fwrite) (&id, sizeof(id), 1, __metafh);
                RECORDER_REAL_CALL(fwrite) (&file_size, sizeof(file_size), 1, __metafh);
                RECORDER_REAL_CALL(fwrite) (&filename_len, sizeof(filename_len), 1, __metafh);
                RECORDER_REAL_CALL(fwrite) (filename, sizeof(char), filename_len, __metafh);
            }
        }
    }

    hashmap_free(__filename2id_map);
    __filename2id_map = NULL;
    if ( __metafh) {
        RECORDER_REAL_CALL(fclose) (__metafh);
        __metafh = NULL;
    }


    __membuf.dump(&__membuf);
    __membuf.release(&__membuf);
    /* Close the log file */
    if ( __datafh ) {
        RECORDER_REAL_CALL(fclose) (__datafh);
        __datafh = NULL;
    }
}
