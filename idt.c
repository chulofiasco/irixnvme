/*
 * idt.c
 *
 * C89-compatible interactive disk performance tester for IRIX 6.5,
 * with:
 *  - 3 workload modes (one-file-per-thread, shared file, raw device)
 *  - Interactive menu for options
 *  - Command-line options: --help, --version, --direct (O_DIRECT I/O)
 *  - Defaults: 2 GB per-file, 4 KB block
 *  - pthreads worker threads
 *  - CPU affinity per thread using sysmp(MP_MUSTRUN, cpu) (Option 1 mapping)
 *  - Per-thread live stats: one-shot 'l' and continuous 'L' toggle
 *  - Comprehensive logging into diskperf_logs/run_YYYY-MM-DD_HH-MM-SS.log
 *  - O_DIRECT support with aligned buffers (4096-byte alignment)
 *
 * Compile:
 *   cc -o idt idt.c -lpthread
 *
 * Usage:
 *   idt              # Interactive mode
 *   idt --direct     # Enable O_DIRECT for direct I/O
 *   idt --help       # Show help message
 *   idt --version    # Show version information
 *
 * Notes:
 *  - Run as root for best affinity behavior and raw device access.
 *  - This file is written to be C89-compatible.
 *  - O_DIRECT bypasses kernel buffer cache for more accurate performance testing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/sysmp.h>
#include <termios.h>
#include <sys/select.h>

#ifndef _SC_NPROCESSORS_CONF
#define _SC_NPROCESSORS_CONF _SC_NPROCESSORS_ONLN
#endif

/* Defaults */
#define DEFAULT_BLOCK 4096
#define DEFAULT_DURATION 10
#define DEFAULT_ALLOC_MB 2048  /* 2 GB */
#define MAX_THREADS 128
#define PATH_MAX_LEN 512
#define LOGDIR "diskperf_logs"
#define VERSION "0.1.0"
#define ALIGNMENT 4096  /* O_DIRECT requires 4096-byte alignment */

typedef enum { MODE_READ, MODE_WRITE } mode_t;
typedef enum { PATTERN_SEQ, PATTERN_RAND } pattern_t;
typedef enum { WORK_A_ONEFILE_PER_THREAD = 1,
               WORK_B_SHARED_FILE_REGIONS = 2,
               WORK_C_RAW_DEVICE_PER_THREAD = 3 } workload_t;

struct cfg {
    workload_t workload;
    char base_path[PATH_MAX_LEN];
    int threads;
    int duration;
    size_t block_size;
    mode_t mode;
    pattern_t pattern;
    off_t alloc_mb;
    int use_direct_io;  /* flag for O_DIRECT */
};

struct thread_info {
    int id;
    int fd;
    size_t block_size;
    mode_t mode;
    pattern_t pattern;
    off_t region_offset;
    off_t region_size;
    volatile int *stop_flag;
    unsigned long ops;               /* cumulative ops */
    unsigned long long bytes;        /* cumulative bytes */
    unsigned long long last_sample_bytes; /* last sample cumulative bytes */
    unsigned long last_sample_ops;        /* last sample cumulative ops */
    unsigned int seed;
    char path[PATH_MAX_LEN];
};

/* Global shared state */
static volatile int g_stop_flag = 0;
static volatile int g_live_continuous = 0; /* toggled by 'L' */
static volatile int g_live_snapshot_request = 0; /* set by 'l' once */
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;

/* Utility: time diff */
static double time_diff_sec(struct timeval *a, struct timeval *b) {
    return (double)(a->tv_sec - b->tv_sec) + (double)(a->tv_usec - b->tv_usec) / 1000000.0;
}

/* Utility: build per-thread path */
static void build_thread_path(char *out, size_t outlen, const char *base, int tid) {
    if (strchr(base, '%')) {
        snprintf(out, outlen, base, tid);
    } else {
        snprintf(out, outlen, "%s.%d", base, tid);
    }
}

/* Thread main worker */
static void *thread_main(void *arg) {
    struct thread_info *ti = (struct thread_info *)arg;
    char *buf;
    char *aligned_buf = NULL;
    off_t offset = 0;
    ssize_t r;
    size_t bs = ti->block_size;
    int rc;
    int ncpus;
    int cpu;

    /* Allocate buffer with extra space for alignment if needed */
    buf = (char *)malloc(bs + ALIGNMENT);
    if (!buf) {
        perror("malloc");
        return NULL;
    }
    
    /* Align buffer to ALIGNMENT boundary for O_DIRECT */
    aligned_buf = (char *)(((unsigned long)buf + ALIGNMENT - 1) & ~(ALIGNMENT - 1));

    /* CPU affinity Option 1: map thread id to cpu id modulo ncpus */
    ncpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (ncpus < 1) ncpus = 1;
    cpu = ti->id % ncpus;
    rc = sysmp(MP_MUSTRUN, cpu);
    if (rc == -1) {
        fprintf(stderr, "thread %d: sysmp(MP_MUSTRUN,%d) failed: %s\n", ti->id, cpu, strerror(errno));
    }

    if (ti->mode == MODE_WRITE) {
        int i;
        for (i = 0; i < (int)bs; ++i) aligned_buf[i] = (char)(0x5A + (ti->id & 0xFF));
    }

    ti->seed = (unsigned int)(time(NULL) ^ ti->id ^ getpid());
    ti->ops = 0UL;
    ti->bytes = 0ULL;
    ti->last_sample_bytes = 0ULL;
    ti->last_sample_ops = 0UL;

    while (!*(ti->stop_flag)) {
        if (ti->pattern == PATTERN_RAND) {
            off_t range = (ti->region_size > (off_t)bs) ? (ti->region_size - (off_t)bs) : (off_t)1;
            unsigned long rnum = (unsigned long)(rand_r(&ti->seed) & 0x7fffffffUL);
            offset = ti->region_offset + (off_t)(rnum % (unsigned long)range);
        } else {
            offset += bs;
            if (offset >= ti->region_offset + ti->region_size) offset = ti->region_offset;
        }

        if (lseek(ti->fd, offset, SEEK_SET) == (off_t)-1) {
            /* ignore */
        }

        if (ti->mode == MODE_READ) {
            r = read(ti->fd, aligned_buf, bs);
            if (r <= 0) {
                if (r == 0) {
                    if (lseek(ti->fd, ti->region_offset, SEEK_SET) == (off_t)-1) { }
                    continue;
                } else {
                    continue;
                }
            }
            pthread_mutex_lock(&g_stats_lock);
            ti->ops++;
            ti->bytes += (unsigned long long)r;
            pthread_mutex_unlock(&g_stats_lock);
        } else {
            r = write(ti->fd, aligned_buf, bs);
            if (r <= 0) {
                continue;
            }
            pthread_mutex_lock(&g_stats_lock);
            ti->ops++;
            ti->bytes += (unsigned long long)r;
            pthread_mutex_unlock(&g_stats_lock);
        }
    }

    /* optional unbind: sysmp(MP_EMPOWER, getpid()); */

    free(buf);
    return NULL;
}

/* Prompt helpers */
static int prompt_int(const char *prompt, int def, int minv, int maxv) {
    char line[128];
    int v;
    printf("%s [%d]: ", prompt, def);
    if (!fgets(line, sizeof(line), stdin)) return def;
    if (line[0] == '\n') return def;
    v = atoi(line);
    if (v < minv) v = minv;
    if (maxv > 0 && v > maxv) v = maxv;
    return v;
}

static off_t prompt_offt_mb(const char *prompt, off_t def_mb) {
    char line[128];
    off_t v;
    printf("%s [%ld MB]: ", prompt, (long)def_mb);
    if (!fgets(line, sizeof(line), stdin)) return def_mb;
    if (line[0] == '\n') return def_mb;
    v = (off_t)atol(line);
    if (v < 0) v = 0;
    return v;
}

static void prompt_str(const char *prompt, const char *def, char *out, size_t outlen) {
    char line[PATH_MAX_LEN];
    printf("%s [%s]: ", prompt, def);
    if (!fgets(line, sizeof(line), stdin)) {
        strncpy(out, def, outlen-1); out[outlen-1] = '\0';
        return;
    }
    if (line[0] == '\n') {
        strncpy(out, def, outlen-1); out[outlen-1] = '\0';
        return;
    }
    {
        char *p = strchr(line, '\n');
        if (p) *p = '\0';
    }
    strncpy(out, line, outlen-1);
    out[outlen-1] = '\0';
}

/* Logging helpers */
static int ensure_logdir(void) {
    struct stat st;
    if (stat(LOGDIR, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        return -1;
    }
    if (mkdir(LOGDIR, 0755) == -1) return -1;
    return 0;
}

static FILE *open_run_log(char *outpath, size_t outlen) {
    time_t now;
    struct tm *tm;
    char fname[256];
    FILE *f;
    if (ensure_logdir() != 0) return NULL;
    now = time(NULL);
    tm = localtime(&now);
    snprintf(fname, sizeof(fname), "run_%04d-%02d-%02d_%02d-%02d-%02d.log",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    snprintf(outpath, outlen, LOGDIR "/%s", fname);
    f = fopen(outpath, "w");
    return f;
}

/* Control thread: reads single-key input without blocking main run */
static void *control_thread(void *arg) {
    struct termios orig, raw;
    int rc;
    fd_set rfds;
    struct timeval tv;
    int fd = fileno(stdin);
    char c;

    /* set terminal to raw, non-canonical, no-echo */
    if (tcgetattr(fd, &orig) == -1) {
        return NULL;
    }
    raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw) == -1) {
        return NULL;
    }

    while (!g_stop_flag) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000; /* 200ms poll */
        rc = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rc > 0 && FD_ISSET(fd, &rfds)) {
            ssize_t n = read(fd, &c, 1);
            if (n == 1) {
                if (c == 'l') {
                    g_live_snapshot_request = 1;
                } else if (c == 'L') {
                    g_live_continuous = !g_live_continuous;
                } else if (c == 'q') {
                    g_stop_flag = 1;
                }
            }
        }
    }

    /* restore terminal */
    tcsetattr(fd, TCSANOW, &orig);
    return NULL;
}

/* Stats watcher thread: per-thread sampling, printing, logging */
struct stats_watcher_arg {
    struct thread_info *tis;
    int nthreads;
    int duration;
    FILE *log;
};

/* Helper: print a per-thread snapshot line to stdout and log */
static void print_and_log_snapshot(FILE *log, int second, int tid, struct thread_info *ti,
                                   unsigned long long delta_bytes, unsigned long delta_ops, double sec) {
    double mbps = (double)delta_bytes / (1024.0 * 1024.0) / (sec > 0.0 ? sec : 1.0);
    double iops = (double)delta_ops / (sec > 0.0 ? sec : 1.0);
    double pct = 0.0;
    if (ti->region_size > 0) {
        pct = ((double)ti->bytes / (double)ti->region_size) * 100.0;
        if (pct > 100.0) pct = 100.0;
    }
    {
        time_t now = time(NULL);
        char timestr[64];
        struct tm *tm = localtime(&now);
        strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", tm);
        printf("[%s] T=%d THREAD=%d MB/s=%.2f IOPS=%.2f DELTA_BYTES=%llu BYTES=%llu OPS=%lu PCT=%.2f PATH=%s\n",
               timestr, second, tid, mbps, iops,
               (unsigned long long)delta_bytes,
               (unsigned long long)ti->bytes,
               (unsigned long)ti->ops,
               pct, ti->path);
        if (log) {
            fprintf(log, "[%s] T=%d THREAD=%d MB/s=%.2f IOPS=%.2f DELTA_BYTES=%llu BYTES=%llu OPS=%lu PCT=%.2f PATH=%s\n",
                    timestr, second, tid, mbps, iops,
                    (unsigned long long)delta_bytes,
                    (unsigned long long)ti->bytes,
                    (unsigned long)ti->ops,
                    pct, ti->path);
            fflush(log);
        }
    }
}

static void *stats_watcher(void *varg) {
    struct stats_watcher_arg *arg = (struct stats_watcher_arg *)varg;
    int i;
    int n = arg->nthreads;
    int elapsed = 0;
    struct timeval tv1, tv2;
    double sec;

    gettimeofday(&tv1, NULL);
    while (!g_stop_flag && elapsed < arg->duration) {
        sleep(1);
        gettimeofday(&tv2, NULL);
        sec = time_diff_sec(&tv2, &tv1);
        tv1 = tv2;
        elapsed++;

        /* aggregate under lock */
        pthread_mutex_lock(&g_stats_lock);
        {
            unsigned long long total_delta_bytes = 0ULL;
            unsigned long total_delta_ops = 0UL;
            /* For each thread compute delta and print/log */
            for (i = 0; i < n; ++i) {
                struct thread_info *ti = &arg->tis[i];
                unsigned long long cur_bytes = ti->bytes;
                unsigned long cur_ops = ti->ops;
                unsigned long long delta_bytes;
                unsigned long delta_ops;

                if (cur_bytes >= ti->last_sample_bytes) delta_bytes = cur_bytes - ti->last_sample_bytes;
                else delta_bytes = cur_bytes;

                if (cur_ops >= ti->last_sample_ops) delta_ops = cur_ops - ti->last_sample_ops;
                else delta_ops = cur_ops;

                /* update last_sample */
                ti->last_sample_bytes = cur_bytes;
                ti->last_sample_ops = cur_ops;

                total_delta_bytes += delta_bytes;
                total_delta_ops += delta_ops;

                /* print per-thread if continuous mode or if snapshot requested */
                if (g_live_continuous || g_live_snapshot_request) {
                    print_and_log_snapshot(arg->log, elapsed, i, ti, delta_bytes, delta_ops, sec);
                }
            }

            /* print aggregate line as well if needed */
            if (g_live_continuous || g_live_snapshot_request) {
                double mbps = (double)total_delta_bytes / (1024.0 * 1024.0) / (sec > 0.0 ? sec : 1.0);
                double iops = (double)total_delta_ops / (sec > 0.0 ? sec : 1.0);
                time_t now = time(NULL);
                char timestr[64];
                struct tm *tm = localtime(&now);
                strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", tm);
                printf("[%s] T=%d AGGREGATE MB/s=%.2f IOPS=%.2f DELTA_BYTES=%llu DELTA_OPS=%lu\n",
                       timestr, elapsed, mbps, iops, (unsigned long long)total_delta_bytes, (unsigned long)total_delta_ops);
                if (arg->log) {
                    fprintf(arg->log, "[%s] T=%d AGGREGATE MB/s=%.2f IOPS=%.2f DELTA_BYTES=%llu DELTA_OPS=%lu\n",
                            timestr, elapsed, mbps, iops, (unsigned long long)total_delta_bytes, (unsigned long)total_delta_ops);
                    fflush(arg->log);
                }
            }

            /* clear snapshot request after handling */
            if (g_live_snapshot_request) g_live_snapshot_request = 0;
        }
        pthread_mutex_unlock(&g_stats_lock);
    }

    return NULL;
}

/* Print version information */
static void print_version(void) {
    printf("IRIX Disk Performance Tester - Version %s\n", VERSION);
    printf("A C89-compatible interactive disk performance testing tool for IRIX 6.5\n");
}

/* Print help/usage information */
static void print_help(const char *prog_name) {
    printf("IRIX Disk Performance Tester - Version %s\n", VERSION);
    printf("\n");
    printf("USAGE:\n");
    printf("  %s [OPTIONS]\n", prog_name);
    printf("\n");
    printf("OPTIONS:\n");
    printf("  -h, --help          Display this help message and exit\n");
    printf("  -v, --version       Display version information and exit\n");
    printf("  -d, --direct        Enable O_DIRECT flag for direct I/O (bypasses kernel cache)\n");
    printf("                      Requires buffer alignment to %d bytes\n", ALIGNMENT);
    printf("\n");
    printf("DESCRIPTION:\n");
    printf("  This tool performs disk performance testing with various workload patterns.\n");
    printf("  When run without options, it starts in interactive mode where you can\n");
    printf("  configure all test parameters.\n");
    printf("\n");
    printf("WORKLOAD MODES:\n");
    printf("  Option A - One file per thread\n");
    printf("    Each thread operates on its own separate file. Best for testing parallel\n");
    printf("    write performance with minimal contention.\n");
    printf("\n");
    printf("  Option B - Single shared file with per-thread regions\n");
    printf("    All threads share a single file but operate on different regions. Tests\n");
    printf("    concurrent access to the same file.\n");
    printf("\n");
    printf("  Option C - Raw device per thread\n");
    printf("    Each thread accesses a raw device directly. For testing raw device\n");
    printf("    performance or using the same device with multiple threads.\n");
    printf("\n");
    printf("PARAMETERS (configured interactively):\n");
    printf("  Threads:    Number of parallel worker threads (1-%d)\n", MAX_THREADS);
    printf("  Duration:   Test duration in seconds\n");
    printf("  Block size: I/O block size in bytes (default: %d)\n", DEFAULT_BLOCK);
    printf("  Mode:       read or write operations\n");
    printf("  Pattern:    sequential or random I/O access pattern\n");
    printf("\n");
    printf("RUNTIME CONTROLS:\n");
    printf("  l - Print one-shot per-thread snapshot (single display)\n");
    printf("  L - Toggle continuous per-second per-thread live stats on/off\n");
    printf("  q - Request early stop of the test\n");
    printf("\n");
    printf("DIRECT I/O (--direct option):\n");
    printf("  When enabled, uses O_DIRECT flag to bypass kernel buffer cache.\n");
    printf("  Benefits:\n");
    printf("    - More accurate disk performance measurement\n");
    printf("    - Avoids cache pollution\n");
    printf("    - Tests actual disk speeds, not memory cache speeds\n");
    printf("  Requirements:\n");
    printf("    - Buffers must be aligned to %d bytes\n", ALIGNMENT);
    printf("    - File offsets should be aligned to filesystem block size\n");
    printf("  Note: Not all filesystems support O_DIRECT on all operations.\n");
    printf("\n");
    printf("EXAMPLES:\n");
    printf("  # Start in interactive mode\n");
    printf("  %s\n", prog_name);
    printf("\n");
    printf("  # Start with direct I/O enabled\n");
    printf("  %s --direct\n", prog_name);
    printf("\n");
    printf("  # Show version\n");
    printf("  %s --version\n", prog_name);
    printf("\n");
    printf("OUTPUT:\n");
    printf("  Results are logged to: %s/run_YYYY-MM-DD_HH-MM-SS.log\n", LOGDIR);
    printf("  Live statistics show MB/s, IOPS, and completion percentage per thread.\n");
    printf("\n");
    printf("NOTES:\n");
    printf("  - Run as root for best CPU affinity behavior and raw device access\n");
    printf("  - This tool is C89-compatible and designed for IRIX 6.5\n");
    printf("  - Compile: cc -o idt idt.c -lpthread\n");
    printf("\n");
}

/* Main program */
int main(int argc, char **argv) {
    struct cfg cfg;
    int i;
    pthread_t th[MAX_THREADS];
    struct thread_info tis[MAX_THREADS];
    int fds[MAX_THREADS];
    struct timeval tstart, tend;
    double elapsed;
    unsigned long long total_bytes = 0ULL;
    unsigned long total_ops = 0UL;
    char line[256];
    int choice;
    pthread_t ctrl_thread = 0;
    pthread_t watcher_thread = 0;
    FILE *logf = NULL;
    char logfile_path[PATH_MAX_LEN];

    /* defaults */
    cfg.workload = WORK_A_ONEFILE_PER_THREAD;
    strcpy(cfg.base_path, "irix_diskperf_testfile.%d");
    cfg.threads = 1;
    cfg.duration = DEFAULT_DURATION;
    cfg.block_size = DEFAULT_BLOCK;
    cfg.mode = MODE_READ;
    cfg.pattern = PATTERN_SEQ;
    cfg.alloc_mb = DEFAULT_ALLOC_MB;
    cfg.use_direct_io = 0;  /* disabled by default */

    /* Parse command-line arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--direct") == 0) {
            cfg.use_direct_io = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Use -h or --help for usage information.\n");
            return 1;
        }
    }

    printf("IRIX Disk Performance Tester (interactive)\n");
    printf("Defaults: block=%d bytes, alloc=%ld MB per-file (if applicable)\n",
           DEFAULT_BLOCK, (long)DEFAULT_ALLOC_MB);
    
    if (cfg.use_direct_io) {
        printf("\nDirect I/O mode ENABLED (O_DIRECT flag will be used)\n");
        printf("  - Kernel buffer cache will be bypassed\n");
        printf("  - Buffers aligned to %d bytes\n", ALIGNMENT);
    }

    /* workload selection */
    printf("\nSelect workload option:\n");
    printf("  1) Option A - One file per thread (each thread gets its own file)\n");
    printf("  2) Option B - Single shared file with per-thread regions\n");
    printf("  3) Option C - Raw device per thread (use devices or same device)\n");
    choice = prompt_int("Choose 1/2/3", (int)cfg.workload, 1, 3);
    cfg.workload = (workload_t)choice;

    if (cfg.workload == WORK_A_ONEFILE_PER_THREAD) {
        prompt_str("Base file path (use %d to include thread id, e.g. /tmp/testfile.%d)", "irix_diskperf_testfile.%d", cfg.base_path, sizeof(cfg.base_path));
    } else if (cfg.workload == WORK_B_SHARED_FILE_REGIONS) {
        prompt_str("Shared file path (single file used by all threads)", "irix_diskperf_shared_testfile", cfg.base_path, sizeof(cfg.base_path));
    } else {
        prompt_str("Device path template (use %d to include thread id, e.g. /dev/rdsk/dks0d1.%d)", "/dev/rdsk/dks0d1", cfg.base_path, sizeof(cfg.base_path));
    }

    cfg.threads = prompt_int("Number of threads", cfg.threads, 1, MAX_THREADS);
    cfg.duration = prompt_int("Duration (seconds)", cfg.duration, 1, 36000);
    cfg.block_size = (size_t)prompt_int("Block size (bytes)", (int)cfg.block_size, 512, 16 * 1024 * 1024);
    
    /* Validate block size alignment if using O_DIRECT */
    if (cfg.use_direct_io && cfg.block_size % ALIGNMENT != 0) {
        fprintf(stderr, "\nWARNING: Block size (%zu) is not aligned to %d bytes.\n", 
                cfg.block_size, ALIGNMENT);
        fprintf(stderr, "         This may cause I/O errors with O_DIRECT.\n");
        fprintf(stderr, "         Recommended block sizes: 4096, 8192, 16384, etc.\n\n");
    }
    
    printf("Mode: 1) read  2) write\n");
    choice = prompt_int("Choose mode", (cfg.mode == MODE_READ) ? 1 : 2, 1, 2);
    cfg.mode = (choice == 2) ? MODE_WRITE : MODE_READ;
    printf("Pattern: 1) sequential  2) random\n");
    choice = prompt_int("Choose pattern", (cfg.pattern == PATTERN_SEQ) ? 1 : 2, 1, 2);
    cfg.pattern = (choice == 2) ? PATTERN_RAND : PATTERN_SEQ;

    if (cfg.workload == WORK_A_ONEFILE_PER_THREAD) {
        cfg.alloc_mb = prompt_offt_mb("Pre-allocate per-file size (MB, 0 to disable)", cfg.alloc_mb);
    } else if (cfg.workload == WORK_B_SHARED_FILE_REGIONS) {
        cfg.alloc_mb = prompt_offt_mb("Total shared file size (MB, >= threads*4 recommended, 0 to disable)", cfg.alloc_mb);
    } else {
        cfg.alloc_mb = 0;
    }

    /* create/open log file */
    logf = open_run_log(logfile_path, sizeof(logfile_path));
    if (logf) {
        time_t now = time(NULL);
        char timestr[64];
        struct tm *tm = localtime(&now);
        strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", tm);
        fprintf(logf, "Run start: %s\n", timestr);
        fprintf(logf, "Workload: %d  base: %s\n", (int)cfg.workload, cfg.base_path);
        fprintf(logf, "threads=%d duration=%d block=%zu mode=%s pattern=%s alloc_mb=%ld direct_io=%s\n",
                cfg.threads, cfg.duration, cfg.block_size,
                (cfg.mode == MODE_READ) ? "read" : "write",
                (cfg.pattern == PATTERN_SEQ) ? "sequential" : "random",
                (long)cfg.alloc_mb,
                cfg.use_direct_io ? "enabled" : "disabled");
        fprintf(logf, "Log file: %s\n", logfile_path);
        fflush(logf);
        printf("Writing run log to: %s\n", logfile_path);
    } else {
        fprintf(stderr, "WARNING: cannot open run log in %s, logging disabled\n", LOGDIR);
    }

    printf("\nSummary:\n");
    if (cfg.workload == WORK_A_ONEFILE_PER_THREAD) {
        printf("  Workload: One file per thread\n  Base path template: %s\n", cfg.base_path);
    } else if (cfg.workload == WORK_B_SHARED_FILE_REGIONS) {
        printf("  Workload: Shared file with per-thread regions\n  Shared file: %s\n", cfg.base_path);
    } else {
        printf("  Workload: Raw device per thread\n  Device template: %s\n", cfg.base_path);
    }
    printf("  threads=%d  duration=%d  block=%zu  mode=%s  pattern=%s  alloc_mb=%ld  direct_io=%s\n\n",
           cfg.threads, cfg.duration, cfg.block_size,
           (cfg.mode == MODE_READ) ? "read" : "write",
           (cfg.pattern == PATTERN_SEQ) ? "sequential" : "random",
           (long)cfg.alloc_mb,
           cfg.use_direct_io ? "enabled" : "disabled");

    printf("Controls during run:\n");
    printf("  l = one-shot per-thread snapshot (prints once and logs)\n");
    printf("  L = toggle continuous per-second per-thread live stats on/off\n");
    printf("  q = request early stop\n");
    printf("Press ENTER to start the test, or Ctrl-C to abort.\n");
    fgets(line, sizeof(line), stdin);

    /* Open files/devices and set up threads */
    for (i = 0; i < cfg.threads; ++i) {
        int flags;
        char path[PATH_MAX_LEN];
        off_t region_offset = 0;
        off_t region_size = 0;
        int fd;

        if (cfg.workload == WORK_A_ONEFILE_PER_THREAD) {
            build_thread_path(path, sizeof(path), cfg.base_path, i);
            if (cfg.mode == MODE_WRITE) flags = O_RDWR | O_CREAT | O_TRUNC;
            else flags = O_RDONLY;
            if (cfg.mode == MODE_WRITE) flags |= O_SYNC;
            if (cfg.use_direct_io) {
#ifdef O_DIRECT
                flags |= O_DIRECT;
#else
                fprintf(stderr, "WARNING: O_DIRECT not supported on this platform, ignoring --direct flag\n");
#endif
            }
            fd = open(path, flags, 0644);
            if (fd < 0) {
                fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
                if (cfg.use_direct_io && errno == EINVAL) {
                    fprintf(stderr, "  Note: O_DIRECT may not be supported on this filesystem or device\n");
                }
                int j;
                for (j = 0; j < i; ++j) close(fds[j]);
                if (logf) fprintf(logf, "open(%s) failed: %s\n", path, strerror(errno));
                return 1;
            }
            if (cfg.alloc_mb > 0) {
                off_t size = cfg.alloc_mb * 1024LL * 1024LL;
                if (ftruncate(fd, size) == -1) {
                    fprintf(stderr, "ftruncate(%s) failed (ignored): %s\n", path, strerror(errno));
                    if (logf) fprintf(logf, "ftruncate(%s) failed (ignored): %s\n", path, strerror(errno));
                }
                region_offset = 0;
                region_size = (cfg.alloc_mb > 0) ? (cfg.alloc_mb * 1024LL * 1024LL) : 0;
            } else {
                region_offset = 0;
                region_size = 1024LL * 1024LL; /* default 1MB window */
            }
            strncpy(tis[i].path, path, sizeof(tis[i].path)-1);
            tis[i].path[sizeof(tis[i].path)-1] = '\0';
        } else if (cfg.workload == WORK_B_SHARED_FILE_REGIONS) {
            snprintf(path, sizeof(path), "%s", cfg.base_path);
            if (cfg.mode == MODE_WRITE) flags = O_RDWR | O_CREAT;
            else flags = O_RDONLY;
            if (cfg.mode == MODE_WRITE) flags |= O_SYNC;
            if (cfg.use_direct_io) {
#ifdef O_DIRECT
                flags |= O_DIRECT;
#else
                fprintf(stderr, "WARNING: O_DIRECT not supported on this platform, ignoring --direct flag\n");
#endif
            }
            fd = open(path, flags, 0644);
            if (fd < 0) {
                fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
                if (cfg.use_direct_io && errno == EINVAL) {
                    fprintf(stderr, "  Note: O_DIRECT may not be supported on this filesystem or device\n");
                }
                int j;
                for (j = 0; j < i; ++j) close(fds[j]);
                if (logf) fprintf(logf, "open(%s) failed: %s\n", path, strerror(errno));
                return 1;
            }
            if (cfg.alloc_mb > 0 && i == 0) {
                off_t size = cfg.alloc_mb * 1024LL * 1024LL;
                if (ftruncate(fd, size) == -1) {
                    fprintf(stderr, "ftruncate(%s) failed (ignored): %s\n", path, strerror(errno));
                    if (logf) fprintf(logf, "ftruncate(%s) failed (ignored): %s\n", path, strerror(errno));
                }
            }
            if (cfg.alloc_mb > 0) {
                off_t total_bytes = cfg.alloc_mb * 1024LL * 1024LL;
                off_t per = total_bytes / cfg.threads;
                region_offset = (off_t)i * per;
                region_size = per;
            } else {
                region_offset = (off_t)i * (1024LL * 1024LL);
                region_size = 1024LL * 1024LL;
            }
            strncpy(tis[i].path, path, sizeof(tis[i].path)-1);
            tis[i].path[sizeof(tis[i].path)-1] = '\0';
        } else {
            build_thread_path(path, sizeof(path), cfg.base_path, i);
            if (cfg.mode == MODE_WRITE) flags = O_RDWR;
            else flags = O_RDONLY;
            if (cfg.use_direct_io) {
#ifdef O_DIRECT
                flags |= O_DIRECT;
#else
                fprintf(stderr, "WARNING: O_DIRECT not supported on this platform, ignoring --direct flag\n");
#endif
            }
            fd = open(path, flags);
            if (fd < 0) {
                fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
                if (cfg.use_direct_io && errno == EINVAL) {
                    fprintf(stderr, "  Note: O_DIRECT may not be supported on this filesystem or device\n");
                }
                int j;
                for (j = 0; j < i; ++j) close(fds[j]);
                if (logf) fprintf(logf, "open(%s) failed: %s\n", path, strerror(errno));
                return 1;
            }
            region_offset = 0;
            region_size = 1024LL * 1024LL * 1024LL; /* default 1GB window */
            strncpy(tis[i].path, path, sizeof(tis[i].path)-1);
            tis[i].path[sizeof(tis[i].path)-1] = '\0';
        }

        fds[i] = fd;
        tis[i].id = i;
        tis[i].fd = fd;
        tis[i].block_size = cfg.block_size;
        tis[i].mode = cfg.mode;
        tis[i].pattern = cfg.pattern;
        tis[i].region_offset = region_offset;
        tis[i].region_size = region_size;
        tis[i].stop_flag = &g_stop_flag;
        tis[i].ops = 0;
        tis[i].bytes = 0ULL;
        tis[i].last_sample_bytes = 0ULL;
        tis[i].last_sample_ops = 0UL;
        tis[i].seed = (unsigned int)(time(NULL) ^ i ^ getpid());
    }

    /* spawn control thread (key handling) */
    if (pthread_create(&ctrl_thread, NULL, control_thread, NULL) != 0) {
        fprintf(stderr, "failed to create control thread\n");
        ctrl_thread = 0;
    }

    /* spawn stats watcher thread */
    {
        struct stats_watcher_arg *arg;
        arg = (struct stats_watcher_arg *)malloc(sizeof(struct stats_watcher_arg));
        if (arg) {
            arg->tis = tis;
            arg->nthreads = cfg.threads;
            arg->duration = cfg.duration;
            arg->log = logf;
            if (pthread_create(&watcher_thread, NULL, stats_watcher, arg) != 0) {
                fprintf(stderr, "failed to create stats watcher thread\n");
                free(arg);
                watcher_thread = 0;
            }
        }
    }

    /* start worker threads */
    gettimeofday(&tstart, NULL);
    for (i = 0; i < cfg.threads; ++i) {
        if (pthread_create(&th[i], NULL, thread_main, &tis[i]) != 0) {
            fprintf(stderr, "pthread_create for thread %d failed: %s\n", i, strerror(errno));
            g_stop_flag = 1;
            break;
        }
    }

    /* main: sleep until duration or early stop requested */
    {
        int sec = 0;
        while (!g_stop_flag && sec < cfg.duration) {
            sleep(1);
            sec++;
        }
        g_stop_flag = 1;
    }

    /* join worker threads */
    for (i = 0; i < cfg.threads; ++i) {
        pthread_join(th[i], NULL);
    }

    /* wait for watcher */
    if (watcher_thread) pthread_join(watcher_thread, NULL);

    gettimeofday(&tend, NULL);
    elapsed = time_diff_sec(&tend, &tstart);
    if (elapsed <= 0.0) elapsed = 1e-6;

    /* summarize and write to log */
    total_bytes = 0ULL;
    total_ops = 0UL;
    pthread_mutex_lock(&g_stats_lock);
    for (i = 0; i < cfg.threads; ++i) {
        total_bytes += tis[i].bytes;
        total_ops += tis[i].ops;
    }
    pthread_mutex_unlock(&g_stats_lock);

    printf("\nDisk performance test results\n");
    printf("  workload: %d  base: %s\n", (int)cfg.workload, cfg.base_path);
    printf("  threads: %d  duration=%d sec  block=%zu bytes  mode=%s  pattern=%s\n",
           cfg.threads, cfg.duration, cfg.block_size,
           (cfg.mode == MODE_READ) ? "read" : "write",
           (cfg.pattern == PATTERN_SEQ) ? "sequential" : "random");

    if (logf) {
        fprintf(logf, "\nPer-thread results:\n");
    }

    for (i = 0; i < cfg.threads; ++i) {
        double mb = (double)tis[i].bytes / (1024.0 * 1024.0);
        double mbps = mb / elapsed;
        double iops = (double)tis[i].ops / elapsed;
        printf("  thread %2d: file=%s bytes=%llu ops=%lu IOPS=%.2f MB/s=%.2f\n",
               tis[i].id, tis[i].path,
               (unsigned long long)tis[i].bytes, (unsigned long)tis[i].ops,
               iops, mbps);
        if (logf) {
            fprintf(logf, "thread %d: file=%s bytes=%llu ops=%lu IOPS=%.2f MB/s=%.2f\n",
                    tis[i].id, tis[i].path,
                    (unsigned long long)tis[i].bytes, (unsigned long)tis[i].ops,
                    iops, mbps);
        }
    }

    printf("\n  TOTAL: bytes=%llu  ops=%lu  elapsed=%.3f sec\n",
           (unsigned long long)total_bytes, (unsigned long)total_ops, elapsed);
    {
        double total_mb = (double)total_bytes / (1024.0 * 1024.0);
        printf("  AGGREGATE: MB/s = %.2f   IOPS = %.2f\n",
               total_mb / elapsed, (double)total_ops / elapsed);
        if (logf) {
            fprintf(logf, "\nTOTAL bytes=%llu ops=%lu elapsed=%.3f sec\n",
                    (unsigned long long)total_bytes, (unsigned long)total_ops, elapsed);
            fprintf(logf, "AGGREGATE MB/s = %.2f IOPS = %.2f\n",
                    total_mb / elapsed, (double)total_ops / elapsed);
            fflush(logf);
        }
    }

    /* close log */
    if (logf) fclose(logf);

    /* close fds */
    for (i = 0; i < cfg.threads; ++i) close(fds[i]);

    /* stop/join control thread */
    if (ctrl_thread) pthread_join(ctrl_thread, NULL);

    return 0;
}