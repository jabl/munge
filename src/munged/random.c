/*****************************************************************************
 *  Written by Chris Dunlap <cdunlap@llnl.gov>.
 *  Copyright (C) 2007-2018 Lawrence Livermore National Security, LLC.
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  UCRL-CODE-155910.
 *
 *  This file is part of the MUNGE Uid 'N' Gid Emporium (MUNGE).
 *  For details, see <https://dun.github.io/munge/>.
 *
 *  MUNGE is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.  Additionally for the MUNGE library (libmunge), you
 *  can redistribute it and/or modify it under the terms of the GNU Lesser
 *  General Public License as published by the Free Software Foundation,
 *  either version 3 of the License, or (at your option) any later version.
 *
 *  MUNGE is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  and GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  and GNU Lesser General Public License along with MUNGE.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *****************************************************************************/


#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <munge.h>
#include <stdint.h>
#include <string.h>
#if HAVE_SYS_RANDOM_H
#include <sys/random.h>
#endif /* HAVE_SYS_RANDOM_H */
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "common.h"
#include "conf.h"
#include "crypto.h"
#include "log.h"
#include "munge_defs.h"
#include "path.h"
#include "random.h"
#include "timer.h"


/*****************************************************************************
 *  Constants
 *****************************************************************************/

/*  String specifying the pathname of the random number source device.
 */
#define RANDOM_SOURCE_PATH              "/dev/urandom"

/*  Integer for the number of bytes to read from the random number source
 *    device when seeding the PRNG entropy pool.
 *  Note that an upper limit of 256 bytes is imposed when using either
 *    getentropy() or getrandom().
 */
#define RANDOM_SOURCE_BYTES             128

/*  Integer for the number of bytes to read from (and write to) the seed file.
 */
#define RANDOM_SEED_BYTES               1024

/*  Integer for the minimum number of bytes needed to adequately seed the
 *    PRNG entropy pool.
 */
#define RANDOM_BYTES_MIN                128

/*  Integer for the minimum number of bytes wanted to seed the PRNG entropy
 *    pool.  This is set such that "enhanced stirring" (i.e., starting the PRNG
 *    stir timer's exponential backoff interval at 1) will be enabled unless
 *    there is entropy from both the kernel source and the seed file.
 */
#define RANDOM_BYTES_WANTED             1152

/*  Integer for the maximum number of seconds between stirrings of the PRNG
 *    entropy pool.  If set to 0, entropy pool stirrings will be disabled.
 */
#define RANDOM_STIR_MAX_SECS            32768


/*****************************************************************************
 *  Private Data
 *****************************************************************************/

static long _random_timer_id = 0;       /* timer ID for entropy pool stir    */

static int  _random_stir_secs;          /* secs between entropy pool stirs   */


/*****************************************************************************
 *  Private Prototypes
 *****************************************************************************/

static int  _random_read_entropy_from_kernel (void);
static int  _random_read_entropy_from_file (const char *path);
static int  _random_read_entropy_from_process (void);
static int  _random_read_seed (const char *path, int num_bytes);
static int  _random_write_seed (const char *path, int num_bytes);
static int  _random_check_entropy (unsigned char *buf, int n);
static void _random_stir_entropy (void *_arg_not_used_);

static void _random_cleanup (void);
static void _random_add (const void *buf, int n);
static void _random_bytes (void *buf, int n);
static void _random_pseudo_bytes (void *buf, int n);


/*****************************************************************************
 *  Public Functions
 *****************************************************************************/

int
random_init (const char *seed_path)
{
    int num_bytes_entropy = 0;
    int got_bad_seed = 0;
    int n;

    /*  Fill the entropy pool.
     */
    n = _random_read_entropy_from_kernel ();
    if (n > 0) {
        num_bytes_entropy += n;
    }
    if (seed_path) {
        n = _random_read_entropy_from_file (seed_path);
        if (n > 0) {
            num_bytes_entropy += n;
        }
        else if (n < 0) {
            got_bad_seed = 1;
        }
    }
    n = _random_read_entropy_from_process ();
    if (n > 0) {
        num_bytes_entropy += n;
    }
    if (num_bytes_entropy < RANDOM_BYTES_MIN) {
        if (!conf->got_force) {
            log_err (EMUNGE_SNAFU, LOG_ERR,
                    "Failed to seed PRNG with sufficient entropy");
        }
        else {
            log_msg (LOG_WARNING,
                    "Failed to seed PRNG with sufficient entropy");
        }
    }
    /*  Compute the initial time interval for stirring the entropy pool.
     *  If the desired amount of entropy is not available, increase the
     *    initial rate of stirring to mix stuff up.  Otherwise, just stir
     *    at the max interval.
     */
    if (conf->got_benchmark || (RANDOM_STIR_MAX_SECS <= 0)) {
        _random_stir_secs = 0;
        log_msg (LOG_INFO, "Disabled PRNG entropy pool stirring");
    }
    else if (num_bytes_entropy < RANDOM_BYTES_WANTED) {
        _random_stir_secs = 1;
        log_msg (LOG_INFO, "Enabled PRNG entropy pool enhanced stirring");
    }
    else {
        _random_stir_secs = RANDOM_STIR_MAX_SECS;
    }

    /*  Schedule repeated stirring of the entropy pool.
     */
    if (_random_stir_secs > 0) {
        _random_stir_entropy (NULL);
    }

    if (got_bad_seed) {
        return (-1);
    }
    if (num_bytes_entropy < RANDOM_BYTES_WANTED) {
        return (0);
    }
    return (1);
}


void
random_fini (const char *seed_path)
{
    if (_random_timer_id > 0) {
        timer_cancel (_random_timer_id);
    }
    if (seed_path != NULL) {
        (void) _random_write_seed (seed_path, RANDOM_SEED_BYTES);
    }
    _random_cleanup ();
    return;
}


void
random_add (const void *buf, int n)
{
    if (!buf || (n <= 0)) {
        return;
    }
    _random_add (buf, n);
    return;
}


void
random_bytes (void *buf, int n)
{
    if (!buf || (n <= 0)) {
        return;
    }
    _random_bytes (buf, n);
    return;
}


void
random_pseudo_bytes (void *buf, int n)
{
    if (!buf || (n <= 0)) {
        return;
    }
    _random_pseudo_bytes (buf, n);
    return;
}


/*****************************************************************************
 *  Private Functions (Common)
 *****************************************************************************/

static int
_random_read_entropy_from_kernel (void)
{
/*  Reads entropy from the kernel's CSPRNG.
 *  Returns the number of bytes of entropy added, or -1 on error.
 */
    int            n = -1;
    int            fd;
    size_t         len;
    const char    *src;
    struct stat    st;
    unsigned char  buf [RANDOM_SOURCE_BYTES];

#if HAVE_GETRANDOM
    /*
     * If the urandom source has been initialized, reads of up to 256 bytes
     *   will always return as many bytes as requested and not be interrupted
     *   by signals.  No such guarantees apply for larger buffer sizes.
     */
    src = "getrandom()";
    len = MIN(256, sizeof (buf));
retry_getrandom:
    n = getrandom (buf, len, 0);
    if (n < 0) {
        if (errno == EINTR) {
            goto retry_getrandom;
        }
        log_msg (LOG_WARNING, "Failed to fill buffer via getrandom(): %s",
                strerror (errno));
    }
#elif HAVE_GETENTROPY
    /*
     *  The maximum buffer size permitted is 256 bytes.
     */
    src = "getentropy()";
    len = MIN(256, sizeof (buf));
    n = getentropy (buf, len);
    if (n < 0) {
        log_msg (LOG_WARNING, "Failed to fill buffer via getentropy(): %s",
                strerror (errno));
    }
    else if (n == 0) {
        n = len;
    }
#endif /* HAVE_GETENTROPY */

    if (n < 0) {
retry_open:
        fd = open (RANDOM_SOURCE_PATH, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            if (errno == EINTR) {
                goto retry_open;
            }
            log_msg (LOG_WARNING, "Failed to open \"%s\": %s",
                    RANDOM_SOURCE_PATH, strerror (errno));
        }
        else {
            if (fstat (fd, &st) < 0) {
                log_msg (LOG_WARNING, "Failed to stat \"%s\": %s",
                        RANDOM_SOURCE_PATH, strerror (errno));
            }
            else if (!S_ISCHR (st.st_mode)) {
                log_msg (LOG_WARNING, "Failed to validate \"%s\": "
                        "not a character device (mode=0x%x)",
                        RANDOM_SOURCE_PATH, (st.st_mode & S_IFMT));
            }
            else {
                src = "\"" RANDOM_SOURCE_PATH "\"";
                len = sizeof (buf);
                n = fd_read_n (fd, buf, len);
                if (n < 0) {
                    log_msg (LOG_WARNING, "Failed to read from \"%s\": %s",
                            RANDOM_SOURCE_PATH, strerror (errno));
                }
            }
            if (close (fd) < 0) {
                log_msg (LOG_WARNING, "Failed to close \"%s\": %s",
                        RANDOM_SOURCE_PATH, strerror (errno));
            }
        }
    }

    if (n > 0) {
        if (n > sizeof (buf)) {
            log_msg (LOG_WARNING, "Ignoring entropy from \"%s\":"
                    " read %d byte%s into a %d-byte buffer",
                    RANDOM_SOURCE_PATH, n, (n == 1 ? "" : "s"), sizeof (buf));
            n = 0;
        }
        else if (_random_check_entropy (buf, n) < 0) {
            log_msg (LOG_WARNING, "Ignoring entropy from \"%s\":"
                    " does not appear random", RANDOM_SOURCE_PATH);
            n = 0;
        }
        else {
            _random_add (buf, n);
            log_msg (LOG_INFO, "PRNG seeded with %d byte%s from %s",
                    n, (n == 1 ? "" : "s"), src);
        }
    }
    return (n);
}


static int
_random_read_entropy_from_file (const char *path)
{
/*  Reads entropy from the seed file specified by 'path'.
 *  Returns the number of bytes of entropy added, or -1 on error.
 */
    int  is_path_secure = 0;
    int  n;
    char dir [PATH_MAX];
    char ebuf [1024];

    if ((path == NULL) || (path[0] == '\0')) {
        errno = EINVAL;
        return (-1);
    }

    if (path_dirname (path, dir, sizeof (dir)) < 0) {
        log_err (EMUNGE_SNAFU, LOG_ERR,
                "Failed to determine dirname of PRNG seed \"%s\"", path);
    }

    n = path_is_secure (dir, ebuf, sizeof (ebuf), PATH_SECURITY_NO_FLAGS);
    if (n < 0) {
        log_err (EMUNGE_SNAFU, LOG_ERR,
                "Failed to check PRNG seed dir \"%s\": %s", dir, ebuf);
    }
    else if ((n == 0) && (!conf->got_force)) {
        log_err (EMUNGE_SNAFU, LOG_ERR, "PRNG seed dir is insecure: %s", ebuf);
    }
    else if (n == 0) {
        log_msg (LOG_WARNING, "PRNG seed dir is insecure: %s", ebuf);
    }
    else {
        is_path_secure = 1;
    }

    n = _random_read_seed (path, RANDOM_SEED_BYTES);
    if (n < 0) {
        if (unlink (path) < 0) {
            if (errno != ENOENT) {
                log_msg (LOG_WARNING,
                        "Failed to remove insecure PRNG seed \"%s\"", path);
            }
        }
        else {
            log_msg (LOG_INFO, "Removed insecure PRNG seed \"%s\"", path);
        }
        n = 0;
    }
    return (!is_path_secure ? -1 : n);
}


static int
_random_read_entropy_from_process (void)
{
/*  Reads entropy from sources related to the process.
 *  Returns the number of bytes of entropy added, or -1 on error.
 */
    pid_t  pid;
    time_t now;

    pid = getpid ();
    _random_add (&pid, sizeof (pid));

    if (time (&now) != (time_t) -1) {
        _random_add (&now, sizeof (now));
    }
    /*  Since these sources do not provide much entropy, return 0.
     */
    return (0);
}


static int
_random_read_seed (const char *path, int num_bytes)
{
/*  Reads up to 'num_bytes' from the seed file specified by 'path',
 *    and adds them to the PRNG entropy pool.
 *  Returns the number of bytes read, or -1 on error.
 */
    int           fd;
    int           is_symlink;
    int           is_valid = 0;
    int           num_left = num_bytes;
    int           num_want;
    int           n;
    struct stat   st;
    unsigned char buf [RANDOM_SEED_BYTES];

    assert (path != NULL);
    assert (num_bytes > 0);

    /*  Do not allow symbolic links in 'path' since the parent directories in
     *    the path of the actual file have not been checked to ensure they are
     *    secure.
     */
    is_symlink = (lstat (path, &st) == 0) ? S_ISLNK (st.st_mode) : 0;
    if (is_symlink) {
        log_msg (LOG_WARNING,
                "Ignoring PRNG seed \"%s\": symbolic link not allowed", path);
        return (-1);
    }

retry_open:
    fd = open (path, O_RDONLY);
    if (fd < 0) {
        if (errno == EINTR) {
            goto retry_open;
        }
        if (errno == ENOENT) {
            return (0);
        }
        log_msg (LOG_WARNING, "Failed to open PRNG seed \"%s\": %s",
                path, strerror (errno));
        return (-1);
    }
    /*  File is now open.  Do not prematurely return until it has been closed.
     */
    if (fstat (fd, &st) < 0) {
        log_msg (LOG_WARNING, "Failed to stat PRNG seed \"%s\": %s",
                path, strerror (errno));
    }
    else if (!S_ISREG (st.st_mode)) {
        log_msg (LOG_WARNING,
                "Ignoring PRNG seed \"%s\": not a regular file (mode=0x%x)",
                path, (st.st_mode & S_IFMT));
    }
    else if (st.st_uid != geteuid ()) {
        log_msg (LOG_WARNING, "Ignoring PRNG seed \"%s\": not owned by UID %u",
                path, (unsigned) geteuid ());
    }
    else if (st.st_mode & (S_IRGRP | S_IROTH)) {
        log_msg (LOG_WARNING,
                "Ignoring PRNG seed \"%s\": readable by group or other", path);
    }
    else if (st.st_mode & (S_IWGRP | S_IWOTH)) {
        log_msg (LOG_WARNING,
                "Ignoring PRNG seed \"%s\": writable by group or other", path);
    }
    else {
        is_valid = 1;
        while (num_left > 0) {
            num_want = (num_left < sizeof (buf)) ? num_left : sizeof (buf);
            n = fd_read_n (fd, buf, num_want);
            if (n < 0) {
                log_msg (LOG_WARNING,
                        "Failed to read from PRNG seed \"%s\": %s",
                        path, strerror (errno));
                break;
            }
            if (n == 0) {
                break;
            }
            _random_add (buf, n);
            num_left -= n;
        }
    }
    if (close (fd) < 0) {
        log_msg (LOG_WARNING, "Failed to close PRNG seed \"%s\": %s",
                path, strerror (errno));
    }
    n = num_bytes - num_left;
    if (n > 0) {
        log_msg (LOG_INFO, "PRNG seeded with %d byte%s from \"%s\"",
                n, (n == 1 ? "" : "s"), path);
    }
    return (!is_valid ? -1 : n);
}


static int
_random_write_seed (const char *path, int num_bytes)
{
/*  Writes 'num_bytes' of random bytes to the seed file specified by 'path'.
 *  Returns the number of bytes written, or -1 on error.
 */
    int            fd;
    int            num_left;
    int            num_want;
    int            n;
    unsigned char  buf [RANDOM_SEED_BYTES];

    assert (path != NULL);
    assert (num_bytes > 0);

    if ((unlink (path) < 0) && (errno != ENOENT)) {
        log_msg (LOG_WARNING, "Failed to unlink old PRNG seed \"%s\": %s",
                path, strerror (errno));
    }
retry_open:
    fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        if (errno == EINTR) {
            goto retry_open;
        }
        log_msg (LOG_WARNING, "Failed to create PRNG seed \"%s\": %s",
                path, strerror (errno));
        return (-1);
    }
    num_left = num_bytes;
    while (num_left > 0) {
        num_want = (num_left < sizeof (buf)) ? num_left : sizeof (buf);
        _random_bytes (buf, num_want);
        n = fd_write_n (fd, buf, num_want);
        if (n < 0) {
            log_msg (LOG_WARNING, "Failed to write to PRNG seed \"%s\": %s",
                    path, strerror (errno));
            break;
        }
        num_left -= n;
    }
    if (close (fd) < 0) {
        log_msg (LOG_WARNING, "Failed to close PRNG seed \"%s\": %s",
                path, strerror (errno));
    }
    n = num_bytes - num_left;
    if (n > 0) {
        log_msg (LOG_INFO, "Wrote %d byte%s to PRNG seed \"%s\"",
                n, (n == 1 ? "" : "s"), path);
    }
    return (n);
}


static int
_random_check_entropy (unsigned char *buf, int n)
{
/*  Checks if the buffer 'buf' of length 'n' contains sufficient entropy.
 *    This is just a simple approximation to guard against an egregiously
 *    broken or fraudulent entropy source.
 *  Returns 0 if entropy appears sufficient; o/w returns -1.
 */
    unsigned char c;
    int           i;
    int           cnt;
    int           lim;

    assert (buf != NULL);
    assert (n > 0);

    c = buf[0];
    for (i = 0, cnt = 0; i < n; i++) {
        if (buf[i] == c) {
            cnt++;
        }
    }
    /*  This just checks if 'buf' is entirely filled with the same byte.
     */
    lim = n;
    if (cnt >= lim) {
        return (-1);
    }
    return (0);
}


static void
_random_stir_entropy (void *_arg_not_used_)
{
/*  Periodically stirs the entropy pool by mixing in new entropy.
 */
    static uint32_t cnt;
    clock_t         cpu;
    struct timeval  tv;
    int             msecs;

    assert (RANDOM_STIR_MAX_SECS > 0);

    if (_random_stir_secs <= 0) {
        return;
    }
    if (cnt == 0) {
        _random_bytes (&cnt, sizeof (cnt));
    }
    _random_timer_id = 0;

    log_msg (LOG_DEBUG, "Stirring PRNG entropy pool");

    cpu = clock ();
    if (cpu != (clock_t) -1) {
        cnt += (uint32_t) cpu;
    }
    if (gettimeofday (&tv, NULL) == 0) {
        cnt += (uint32_t) (tv.tv_sec + tv.tv_usec);
    }
    _random_add (&cnt, sizeof (cnt));

    /*  Perform an exponential backoff up to the maximum timeout.  This allows
     *    for vigorous stirring of the entropy pool when the daemon is started.
     */
    if (_random_stir_secs < RANDOM_STIR_MAX_SECS) {
        _random_stir_secs = MIN(_random_stir_secs * 2, RANDOM_STIR_MAX_SECS);
    }
    /*  The 10 low-order bits of the current time are used to mix things up and
     *    stagger subsequent timer callbacks by up to 1023ms.
     */
    msecs = (_random_stir_secs * 1000) + (tv.tv_usec & 0x3FF);

    _random_timer_id = timer_set_relative (
            (callback_f) _random_stir_entropy, NULL, msecs);

    if (_random_timer_id < 0) {
        log_errno (EMUNGE_SNAFU, LOG_ERR, "Failed to set PRNG stir timer");
    }
    return;
}


/*****************************************************************************
 *  Private Functions (Libgcrypt)
 *****************************************************************************/

#if HAVE_LIBGCRYPT

#include <gcrypt.h>
#include "fd.h"

static void
_random_cleanup (void)
{
    return;
}


static void
_random_add (const void *buf, int n)
{
    gcry_error_t e;

    assert (buf != NULL);
    assert (n > 0);

    e = gcry_random_add_bytes (buf, n, -1);
    if (e) {
        log_msg (LOG_WARNING, "Failed to add %d byte%s to entropy pool: %s",
                n, (n == 1 ? "" : "s"), gcry_strerror (e));
    }
    gcry_fast_random_poll ();
    return;
}


static void
_random_bytes (void *buf, int n)
{
    assert (buf != NULL);
    assert (n > 0);

    gcry_randomize (buf, n, GCRY_STRONG_RANDOM);
    return;
}


static void
_random_pseudo_bytes (void *buf, int n)
{
    assert (buf != NULL);
    assert (n > 0);

    gcry_create_nonce (buf, n);
    return;
}

#endif /* HAVE_LIBGCRYPT */


/*****************************************************************************
 *  Private Functions (OpenSSL)
 *****************************************************************************/

#if HAVE_OPENSSL

#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/rand.h>

static void
_random_cleanup (void)
{
#if HAVE_RAND_CLEANUP
    /*  OpenSSL < 1.1.0  */
    RAND_cleanup ();
#endif /* HAVE_RAND_CLEANUP */
    return;
}


static void
_random_add (const void *buf, int n)
{
    assert (buf != NULL);
    assert (n > 0);

    RAND_seed (buf, n);
    return;
}


static void
_random_bytes (void *buf, int n)
{
    int rc;

    assert (buf != NULL);
    assert (n > 0);

    rc = RAND_bytes (buf, n);
    if (rc == -1) {
        log_msg (LOG_ERR,
                "RAND_bytes failed: not supported by OpenSSL RAND method");
    }
    else if (rc == 0) {
        unsigned long e = ERR_get_error ();
        log_msg (LOG_WARNING,
                "RAND_bytes failed: %s", ERR_reason_error_string (e));
    }
    return;
}


static void
_random_pseudo_bytes (void *buf, int n)
{
    int rc;

    assert (buf != NULL);
    assert (n > 0);

    /*  RAND_pseudo_bytes() was deprecated in OpenSSL 1.1.0.  Unfortunately,
     *    AC_CHECK_FUNCS(RAND_pseudo_bytes) still sets HAVE_RAND_PSEUDO_BYTES
     *    since the function exists (albeit with the "deprecated" attribute).
     *    This results in a deprecated-declarations warning when compiling
     *    against OpenSSL >1.1.0.  And that warning will break the build if
     *    "-Werror" is specified (as is the case for the Travis CI build).
     *  The check for OPENSSL_VERSION_NUMBER from <openssl/opensslv.h>
     *    handles this case.
     */
#if HAVE_RAND_PSEUDO_BYTES && (OPENSSL_VERSION_NUMBER < 0x10100000L)
    /*  OpenSSL >= 0.9.5, < 1.1.0  */
    rc = RAND_pseudo_bytes (buf, n);
    if (rc == -1) {
        log_msg (LOG_ERR, "RAND_pseudo_bytes failed: "
                "not supported by OpenSSL RAND method");
    }
    else if (rc == 0) {
        unsigned long e = ERR_get_error ();
        log_msg (LOG_WARNING, "RAND_pseudo_bytes failed: %s",
                ERR_reason_error_string (e));
    }
#else  /* !HAVE_RAND_PSEUDO_BYTES */
    _random_bytes (buf, n);
    (void) rc;                          /* suppress unused-variable warning */
#endif /* !HAVE_RAND_PSEUDO_BYTES */
    return;
}

#endif /* HAVE_OPENSSL */
