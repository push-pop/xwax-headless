/*
 * Copyright (C) 2011 Mark Hills <mark@pogo.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */

#define _BSD_SOURCE /* vfork() */
#define _GNU_SOURCE /* getdelim(), strdupa() */
#include <assert.h>
#include <libgen.h> /*  basename() */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "library.h"

#define CRATE_ALL "All records"


/* Initialise a crate; note the deep copy of the crate name */

static int crate_init(struct crate_t *c, const char *name, bool is_fixed)
{
    c->name = strdup(name);
    if (c->name == NULL) {
        perror("strdup");
        return -1;
    }

    c->is_fixed = is_fixed;
    listing_init(&c->listing);

    return 0;
}


static void crate_clear(struct crate_t *c)
{
    listing_clear(&c->listing);
    free(c->name);
}


static int crate_cmp(const struct crate_t *a, const struct crate_t *b)
{
    if (a->is_fixed && !b->is_fixed)
        return -1;
    if (!a->is_fixed && b->is_fixed)
        return 1;

    return strcmp(a->name, b->name);
}


/* Comparison function fo ruse with qsort() */

static int qcompar(const void *a, const void *b)
{
    return crate_cmp(*(struct crate_t**)a, *(struct crate_t**)b);
}


static void sort_crates(struct library_t *lib)
{
    qsort(lib->crate, lib->crates, sizeof(struct crate_t*), qcompar);
}


/* Add a crate to the list of all crates */

static int add_crate(struct library_t *lib, struct crate_t *c)
{
    struct crate_t **cn;

    cn = realloc(lib->crate, sizeof(struct crate_t*) * (lib->crates + 1));
    if (cn == NULL) {
        perror("realloc");
        return -1;
    }

    lib->crate = cn;
    lib->crate[lib->crates++] = c;

    sort_crates(lib);

    return 0;
}


struct crate_t* get_crate(struct library_t *lib, const char *name)
{
    int n;

    for (n = 0; n < lib->crates; n++) {
        if (strcmp(lib->crate[n]->name, name) == 0)
            return lib->crate[n];
    }

    return NULL;
}


/* Get an existing crate, or create a new one if necessary */

struct crate_t* use_crate(struct library_t *lib, char *name, bool is_fixed)
{
    struct crate_t *new_crate;

    /* does this crate already exist? then return existing crate */
    new_crate = get_crate(lib, name);
    if (new_crate != NULL) {
        fprintf(stderr, "Crate '%s' already exists...\n", name);
        return new_crate;
    }

    /* allocate and fill space for new crate */
    new_crate = malloc(sizeof(struct crate_t));
    if (new_crate == NULL) {
        perror("malloc");
        return NULL;
    }

    if (crate_init(new_crate, name, is_fixed) == -1)
        goto fail;

    if (add_crate(lib, new_crate) == -1)
        goto fail;

    return new_crate;

 fail:
    free(new_crate);
    return NULL;
}


int library_init(struct library_t *li)
{
    li->crate = NULL;
    li->crates = 0;

    if (crate_init(&li->all, CRATE_ALL, true) == -1)
        return -1;

    if (add_crate(li, &li->all) == -1) {
        crate_clear(&li->all);
        return -1;
    }

    return 0;
}


static void record_clear(struct record_t *re)
{
    free(re->pathname);
    free(re->artist);
    free(re->title);
}


void library_clear(struct library_t *li)
{
    int n;

    /* This object is responsible for all the record pointers */

    for (n = 0; n < li->all.listing.entries; n++) {
        struct record_t *re;

        re = li->all.listing.record[n];
        record_clear(re);
        free(re);
    }

    /* Clear crates */

    for (n = 1; n < li->crates; n++) { /* skip the 'all' crate */
        struct crate_t *crate;

        crate = li->crate[n];
        crate_clear(crate);
        free(crate);
    }
    free(li->crate);

    crate_clear(&li->all);
}


static int crate_add(struct crate_t *crate, struct record_t *lr)
{
    return listing_add(&crate->listing, lr);
}


/* Read and allocate a null-terminated field from the given file handle.
 * If the empty string is read, *s is set to NULL. Return 0 on success,
 * -1 on error or EOF */

static int get_field(FILE *fp, char delim, char **f)
{
    char *s = NULL;
    size_t n, z;

    z = getdelim(&s, &n, delim, fp);
    if (z == -1) {
        free(s);
        return -1;
    }

    s[z - 1] = '\0'; /* don't include delimiter */
    *f = s;
    return 0;
}


/* Scan a record library at the given path, using the given scan
 * script. Returns -1 on fatal error which may leak resources */

int library_import(struct library_t *li, bool sort,
                   const char *scan, const char *path)
{
    int pstdout[2], status;
    char *cratename, *pathname;
    pid_t pid;
    FILE *fp;
    struct crate_t *crate, *all_crate;

    fprintf(stderr, "Scanning '%s'...\n", path);

    all_crate = get_crate(li, CRATE_ALL);
    assert(all_crate != NULL);

    pathname = strdupa(path);
    cratename = basename(pathname); /* POSIX version, see basename(3) */
    assert(cratename != NULL);
    crate = use_crate(li, cratename, false);
    if (crate == NULL)
        return -1;

    if (pipe(pstdout) == -1) {
        perror("pipe");
        return -1;
    }

    pid = vfork();

    if (pid == -1) {
        perror("vfork");
        return -1;

    } else if (pid == 0) { /* child */

        if (close(pstdout[0]) == -1) {
            perror("close");
            abort();
        }

        if (dup2(pstdout[1], STDOUT_FILENO) == -1) {
            perror("dup2");
            _exit(EXIT_FAILURE); /* vfork() was used */
        }

        if (close(pstdout[1]) == -1) {
            perror("close");
            abort();
        }

        if (execl(scan, "scan", path, NULL) == -1) {
            perror("execl");
            fprintf(stderr, "Failed to launch library scanner %s\n", scan);
            _exit(EXIT_FAILURE); /* vfork() was used */
        }

        abort(); /* execl() does not return */
    }

    if (close(pstdout[1]) == -1) {
        perror("close");
        abort();
    }

    fp = fdopen(pstdout[0], "r");
    if (fp == NULL) {
        perror("fdopen");
        return -1;
    }

    for (;;) {
        struct record_t *d;
        char *pathname;

        if (get_field(fp, '\t', &pathname) != 0)
            break;

        d = malloc(sizeof(struct record_t));
        if (d == NULL) {
            perror("malloc");
            return -1;
        }

        d->pathname = pathname;

        if (get_field(fp, '\t', &d->artist) != 0) {
            fprintf(stderr, "EOF when reading artist for '%s'.\n", d->pathname);
            return -1;
        }

        if (get_field(fp, '\n', &d->title) != 0) {
            fprintf(stderr, "EOF when reading title for '%s'.\n", d->pathname);
            return -1;
        }

        /* Add to crates */

        if (crate_add(all_crate, d) != 0)
            return -1;
        if (crate_add(crate, d) != 0)
            return -1;
    }

    if (fclose(fp) == -1) {
        perror("close");
        abort(); /* assumption fclose() can't on read-only descriptor */
    }

    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        return -1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
        fputs("Library scan exited reporting failure.\n", stderr);
        return -1;
    }

    /* sort the listings */

    listing_sort(&all_crate->listing);
    if (sort)
        listing_sort(&crate->listing);

    return 0;
}
