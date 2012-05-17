
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "debugging.h"
#include "defaults.h"

static char *log_level_str[5] = {
    "LOG_FATAL",
    "LOG_ERR",
    "LOG_WARN",
    "LOG_DEBUG",
    "LOG_TRACE"
};

static int log_msg(FILE *fp, const char *location, const char *format, va_list ap);

static int
log_msg(FILE *fp, const char *location, const char *format, va_list ap)
{
    char buffer[MAX_ERR_BUF_SIZE], *bptr = buffer;
    int bsize = sizeof(buffer);
    //int ret = -EINVAL;
    
    if (vsnprintf(bptr, bsize, format, ap) < 0)
        return -errno;

#ifdef TIMESTAMP_ENABLED
    const char *TIMESTAMP_FMT = "%F %X";        /* = YYYY-MM-DD HH:MM:SS */
#define TS_BUF_SIZE sizeof("YYYY-MM-DD HH:MM:SS")       /* Includes '\0' */
    char timestamp[TS_BUF_SIZE];
    time_t t;
    struct tm *loc;

    t = time(NULL);
    loc = localtime(&t);
    if ((loc == NULL) || 
        (strftime(timestamp, TS_BUF_SIZE, TIMESTAMP_FMT, loc) == 0)) {
        if (fprintf(fp, "???Unknown time????: ") < 0)
            return -errno;
    } else {
        if (fprintf(fp, "[%s] ", timestamp) < 0)
            return -errno;
    }
#endif

    if (location != NULL) {
        if (fprintf(fp, "{%s} ", location) < 0)
            return -errno;
    }

    strcat(buffer, "\n");
    if (fprintf(fp, "%s", buffer) < 0)
        return -errno;
    
    fflush(fp);
    
    return 0;
}

void logMessage(log_level_t level, const char *location, const char *format, ...)
{
    if (level <= sys_log_level) {
        va_list ap;

        va_start(ap, format);
        if (log_msg(log_fp, location, format, ap) < 0) {
            fprintf(stdout, "ERROR: debugging error.\n");
            //exit(1);
        }

        va_end(ap);
    }

}

/* Open the log file 'logFilename' */

void logOpen(const char *logFilename, log_level_t level)
{
    mode_t m;

    m = umask(077);
    log_fp = fopen(logFilename, "w+");
    if (log_fp == NULL) {
        fprintf(stdout, 
                "ERROR: creating log file(%s): %s\n", logFilename, strerror(errno));
        exit(1);
    }
    umask(m);

    /* If opening the log fails we can't display a message... */

    if (log_fp == NULL)
        exit(EXIT_FAILURE);

    sys_log_level = level;
    setbuf(log_fp, NULL);                    /* Disable stdio buffering */

    fprintf(log_fp, "[mode=%s] Opened log file(%s). ##### \n.", 
            log_level_str[(int)sys_log_level], logFilename);
}

/* Close the log file */

void logClose(void)
{
    fprintf(log_fp, "[mode=%s] Closed log file.######### \n", 
            log_level_str[(int)sys_log_level]);
    fclose(log_fp);
}

