#include "mrb.h"

#include <err.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/mman.h>


#ifndef MIN
#define MIN(a, b) ((a) < (b)) ? (a) : (b)
#endif


struct mrb {
    unsigned char *buff;
    size_t size;
    int writer;
    int reader;
};


int
mrb_validatesize(size_t size) {
    int pagesize = getpagesize();

    /* Calculate the real size (multiple of pagesize). */
    if (size % pagesize) {
        warn(
            "Invalid size: %lu, size should be multiple of pagesize (%d), "
            "see getpagesize(2).",
            size,
            pagesize
        );
        return -1;
    }

    return 0;
}


size_t
mrb_calcsize(unsigned int pages) {
    int pagesize = getpagesize();

    return pagesize * pages;
}


int
mrb_init(struct mrb *b, size_t size) {
    if (mrb_validatesize(size)) {
        return -1;
    }

    b->size = size;
    b->writer = 0;
    b->reader = 0;

    /* Create a temporary file with requested size as the backend for mmap. */
    FILE *file = tmpfile();
    const int fd = fileno(file);
    ftruncate(fd, b->size);

    /* Allocate the underlying backed buffer. */
    b->buff = mmap(
            NULL,
            b->size * 2,
            PROT_NONE,
            MAP_ANONYMOUS | MAP_PRIVATE,
            -1,
            0
        );
    if (b->buff == MAP_FAILED) {
        close(fd);
        fclose(file);
        return -1;
    }

    unsigned char *first = mmap(
            b->buff,
            b->size,
            PROT_READ | PROT_WRITE,
            MAP_FIXED | MAP_SHARED,
            fd,
            0
        );

    if (first == MAP_FAILED) {
        munmap(b->buff, b->size * 2);
        close(fd);
        fclose(file);
        return -1;
    }

    unsigned char *second = mmap(
            b->buff + b->size,
            b->size,
            PROT_READ | PROT_WRITE,
            MAP_FIXED | MAP_SHARED,
            fd,
            0
        );

    if (second == MAP_FAILED) {
        munmap(b->buff, b->size * 2);
        close(fd);
        fclose(file);
        return -1;
    }

    fclose(file);
    return 0;
}


struct mrb *
mrb_create(size_t size) {
    struct mrb *b;

    /* Allocate memory for mrb structure. */
    b = malloc(sizeof(struct mrb));
    if (b == NULL) {
        return NULL;
    }

    if (mrb_init(b, size)) {
        free(b);
        return NULL;
    }

    return b;
}


int
mrb_deinit(struct mrb *b) {
    /* unmap second part */
    if (munmap(b->buff + b->size, b->size)) {
        return -1;
    }

    /* unmap first part */
    if (munmap(b->buff, b->size)) {
        return -1;
    }

    /* unmap backed buffer */
    if (munmap(b->buff, b->size * 2)) {
        return -1;
    }
    return 0;
}


int
mrb_destroy(struct mrb *b) {
    if (mrb_deinit(b)) {
        return -1;
    }
    free(b);
    return 0;
}


/** Obtain the total buffer capacity of a VRB.
 */
size_t
mrb_size(struct mrb *b) {
    return b->size;
}


/** Obtain the length of empty space in the buffer.
 */
size_t
mrb_available(struct mrb *b) {
    // 11000111
    //   w  r
    if (b->writer < b->reader) {
        return b->reader - b->writer - 1;
    }

    // 00111100
    //   r   w
    return b->size - (b->writer - b->reader) - 1;
}


/** Obtain the length of data in the buffer.
 */
size_t
mrb_used(struct mrb *b) {
    // 00111000
    //   r  w
    if (b->writer >= b->reader) {
        return b->writer - b->reader;
    }

    // 11000111
    //   w  r
    return b->size - (b->reader - b->writer);
}


/** Determine if the buffer is currently empty.
 */
bool
mrb_isempty(struct mrb *b) {
    return b->reader == b->writer;
}


/** Determine if the buffer is currently full.
 */
bool
mrb_isfull(struct mrb *b) {
    return b->size == (mrb_used(b) + 1);
}


/** Copy data from a caller location to the magic ring buffer.
 */
size_t
mrb_put(struct mrb *b, const char *restrict source, size_t size) {
    size_t amount = MIN(size, mrb_available(b));
    memcpy(b->buff + b->writer, source, amount);
    b->writer = (b->writer + amount) % b->size;
    return amount;
}


/** Copy data to the magic ring buffer only if all of it will fit.
 */
int
mrb_putall(struct mrb *b, const char *restrict source, size_t size) {
    if (size > mrb_available(b)) {
        return -1;
    }
    memcpy(b->buff + b->writer, source, size);
    b->writer = (b->writer + size) % b->size;
    return 0;
}


/** Copy data from the magic ring buffer to a caller location.
 */
size_t
mrb_get(struct mrb *b, char *dest, size_t size) {
    size_t amount = MIN(size, mrb_used(b));
    memcpy(dest, b->buff + b->reader, amount);
    b->reader = (b->reader + amount) % b->size;
    return amount;
}


/** Copy data from the magic ring buffer to a caller location without
  modifying the buffer state.
 */
size_t
mrb_softget(struct mrb *b, char *dest, size_t size, size_t offset) {
    size_t amount = MIN(size + offset, mrb_used(b));
    memcpy(dest, b->buff + b->reader + offset, amount - offset);
    return amount - offset;
}


/** Tail drop
  */
int
mrb_skip(struct mrb *b, size_t size) {
    if (mrb_used(b) < size) {
        return -1;
    }
    b->reader = (b->reader + size) % b->size;
    return 0;
}


/** Rollback reader
  */
int
mrb_rollback(struct mrb *b, size_t size) {
    if (mrb_used(b) > size) {
        return -1;
    }
    b->reader = (b->reader - size) % b->size;
    return 0;
}


/** Get data from a magic ring buffer and copy it to the space provider by
  the caller only if the minimum specified amount can be copied. If less data
  than the minimum is available, then no data is copied.
 */
ssize_t
mrb_getmin(struct mrb *b, char *dest, size_t minsize, size_t maxsize) {
    size_t used = mrb_used(b);
    if (minsize > used) {
        return -1;
    }
    size_t amount = MIN(maxsize, used);
    memcpy(dest, b->buff + b->reader, amount);
    b->reader = (b->reader + amount) % b->size;
    return amount;
}


/** read(2) data into a magic ring buffer until EOF or full, or I/O would
  block.
 */
ssize_t
mrb_readin(struct mrb *b, int fd, size_t size) {
    size_t amount = MIN(size, mrb_available(b));
    ssize_t res = read(fd, b->buff + b->writer, amount);
    if (res > 0) {
        b->writer = (b->writer + res) % b->size;
    }
    return res;
}


/** Print formatted string into buffer
  */
int
mrb_print(struct mrb *b, const char *format, ...) {
    va_list args;

    if (format) {
        va_start(args, format);
    }

    int written = mrb_vprint(b, format, args);

    if (format) {
        va_end(args);
    }

    return written;
}


/** Print formatted string into buffer
  */
int
mrb_vprint(struct mrb *b, const char *format, va_list args) {
    int written = vsnprintf(b->buff + b->writer, mrb_available(b), format,
            args);

    if (written > 0) {
        b->writer = (b->writer + written) % b->size;
    }

    return written;
}


/** write(2) data from a magic ring buffer until empty, or I/O would block.
 */
ssize_t
mrb_writeout(struct mrb *b, int fd, size_t size) {
    size_t amount = MIN(size, mrb_used(b));
    ssize_t res = write(fd, b->buff + b->reader, amount);
    if (res > 0) {
        b->reader = (b->reader + res) % b->size;
    }
    return res;
}


/** Search for the specified string within buffer


  limit argument will be ignored if it's value is 0 or greater than data available
  in the buffer.

  if start is greater than data available in the buffer, then -1 is returned,
  and errno set to indicate the error. <- TODO

  Return: Index of the first occurance or -1 if not found.
  */
ssize_t
mrb_search(struct mrb *b, const char *needle, size_t needlelen, size_t start,
        ssize_t limit) {

    size_t used;
    unsigned char *s;
    unsigned char *found;
    if ((needle == NULL) || (needlelen == 0)) {
        return -1;
    }

    used = mrb_used(b);
    if ((start < 0) || (start >= used)) {
        errno = EINVAL;
        return -1;
    }

    if (limit <= 0) {
        limit = used;
    }
    else {
        limit = MIN(limit, used);
    }

    s = b->buff + b->reader;
    s += start;

    found = memmem(s, limit, needle, needlelen);
    if (found == NULL) {
        return -1;
    }

    return found - (b->buff + b->reader);
}
