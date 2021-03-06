/* This file is part of msolve.
 *
 * msolve is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * msolve is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with msolve.  If not, see <https://www.gnu.org/licenses/>
 *
 * Authors:
 * Jérémy Berthomieu
 * Christian Eder
 * Mohab Safey El Din */


#include "tools.h"

/* cpu time */
double cputime(void)
{
	double t;
	t =   CLOCKS_PER_SEC / 100000.;
	t +=  (double)clock();
	return t / CLOCKS_PER_SEC;
}

/* wall time */
double realtime(void)
{
	struct timeval t;
	gettimeofday(&t, NULL);
	t.tv_sec -= (2017 - 1970)*3600*24*365;
	return (1. + (double)t.tv_usec + ((double)t.tv_sec*1000000.)) / 1000000.;
}

static void construct_trace(
        trace_t *trace,
        mat_t *mat
        )
{
    len_t i, j;
    len_t ctr = 0;

    const len_t ld  = trace->ld;
    const len_t nru = mat->nru;
    const len_t nrl = mat->nrl;
    rba_t **rba     = mat->rba;

    /* check if there are any non zero new elements, otherwise we
     * do not need to do this matrix at all in the application steps. */
    i = 0;
    while (i < nrl && mat->tr[i] == NULL) {
        ++i;
    }
    if (i == nrl) {
        return;
    }

    /* non zero new elements exist */
    if (trace->ld == trace->sz) {
        trace->sz *=  2;
        trace->td =   realloc(trace->td,
                (unsigned long)trace->sz * sizeof(td_t));
        memset(trace->td+trace->sz/2, 0,
                (unsigned long)trace->sz/2 * sizeof(td_t));
    }

    const unsigned long lrba = nru / 32 + ((nru % 32) != 0);

    rba_t *reds = (rba_t *)calloc(lrba, sizeof(rba_t));

    for (i = 0; i < nrl; ++i) {
        if (mat->tr[i] != NULL) {
            rba[ctr]  = rba[i];
            ctr++;
        } else {
            free(rba[i]);
            rba[i]  = NULL;
        }
    }
    mat->rbal = ctr;
    mat->rba  = rba = realloc(rba, (unsigned long)mat->rbal * sizeof(rba_t *));

    const len_t ntr = ctr;

    /* construct rows to be reduced */
    trace->td[ld].tri  = realloc(trace->td[ld].tri,
            (unsigned long)ntr * 2 * sizeof(len_t));
    trace->td[ld].tld = 2 * ntr;

    ctr = 0;
    for (i = 0; i < nrl; ++i) {
        if (mat->tr[i] != NULL) {
            trace->td[ld].tri[ctr++]  = mat->tr[i][BINDEX];
            trace->td[ld].tri[ctr++]  = mat->tr[i][MULT];
        }
    }
    /* get all needed reducers */
    for (i = 0; i < ntr; ++i) {
        for (j = 0; j < lrba; ++j) {
            reds[j] |= rba[i][j];
        }
    }

    /* construct rows to reduce with */
    trace->td[ld].rri  = realloc(trace->td[ld].rri,
            (unsigned long)nru * 2 * sizeof(len_t));
    trace->td[ld].rld = 2 * nru;

    ctr = 0;
    for (i = 0; i < nru; ++i) {
        if (reds[i/32] >> (i%32) & 1U) {
            trace->td[ld].rri[ctr++]  = mat->rr[i][BINDEX];
            trace->td[ld].rri[ctr++]  = mat->rr[i][MULT];
        }
    }
    trace->td[ld].rri = realloc(trace->td[ld].rri,
            (unsigned long)ctr * sizeof(len_t));
    trace->td[ld].rld = ctr;
    const len_t nrr   = ctr;

    /* new length of rbas, useless reducers removed,
     * we only need nrr/2 since we do not store the
     * multipliers in rba */
    const unsigned long nlrba = nrr / 2 / 32 + (((nrr / 2) % 32) != 0);

    /* construct rba information */
    trace->td[ld].rba = realloc(trace->td[ld].rba,
            (unsigned long)ntr * sizeof(rba_t *));
    /* allocate memory     */
    for (i = 0; i < ntr; ++i) {
        trace->td[ld].rba[i]  = calloc(nlrba, sizeof(rba_t));
    }

    ctr = 0;
    /* write new rbas for tracer with useless reducers removed */
    for (i = 0; i < nru; ++i) {
        if (reds[i/32] >> (i%32) & 1U) {
            for (j = 0; j < ntr; ++j) {
                trace->td[ld].rba[j][ctr/32] |=
                    ((rba[j][i/32] >> i%32) & 1U) << ctr%32;
            }
            ctr++;
        }
    }
    free(reds);
}

static void add_lms_to_trace(
        trace_t *trace,
        const bs_t * const bs,
        const len_t np
        )
{
    len_t i;

    const len_t ld    = trace->ld;
    trace->td[ld].lms = realloc(trace->td[ld].lms,
            (unsigned long)np * sizeof(hm_t));

    for (i = 0; i < np; ++i) { trace->td[ld].lms[i]  = bs->hm[bs->ld + i][OFFSET];
    }
    trace->td[ld].nlm = np;
}

/* 
 * static inline val_t compare_and_swap(
 *         long *ptr,
 *         long old,
 *         long new
 *         )
 * {
 *     val_t prev;
 *
 * #if 0
 *     __asm__ __volatile__(
 *             "lock; cmpxchgl %2, %1" : "=a"(prev),
 *             "+m"(*ptr) : "r"(new), "0"(old) : "memory");
 *     [> on which systems do we need "cmpxchgq" instead of "cmpxchgl" ? <]
 * #else
 *     __asm__ __volatile__(
 *             "lock; cmpxchgq %2, %1" : "=a"(prev),
 *             "+m"(*ptr) : "r"(new), "0"(old) : "memory");
 * #endif
 *     return prev;
 * } */
