#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "myassert.h"
#include "rowindex.h"
#include "omp.h"
#include "types.h"
#include "utils.h"

dt_static_assert(offsetof(RowIndex, ind32) == offsetof(RowIndex, ind64),
                 "Addresses of RowIndex->ind32 and RowIndex->ind64 must "
                 "coincide");



/**
 * Internal macro to help iterate over a rowindex. Assumes that macro `CODE`
 * is defined in scope, and substitutes it into the body of each loop. Within
 * the macro, variable `int64_t j` can be used to refer to the source row that
 * was mapped, and `int64_t i` is the "destination" index.
 */
#define ITER_ALL {                                                             \
    RowIndexType ritype = rowindex->type;                                      \
    int64_t nrows = rowindex->length;                                          \
    if (ritype == RI_SLICE) {                                                  \
        int64_t start = rowindex->slice.start;                                 \
        int64_t step = rowindex->slice.step;                                   \
        for (int64_t i = 0, j = start; i < nrows; i++, j+= step) {             \
            CODE                                                               \
        }                                                                      \
    }                                                                          \
    else if (ritype == RI_ARR32) {                                             \
        int32_t *indices = rowindex->ind32;                                    \
        for (int64_t i = 0; i < nrows; i++) {                                  \
            int64_t j = (int64_t) indices[i];                                  \
            CODE                                                               \
        }                                                                      \
    }                                                                          \
    else if (ritype == RI_ARR64) {                                             \
        int64_t *indices = rowindex->ind64;                                    \
        for (int64_t i = 0; i < nrows; i++) {                                  \
            int64_t j = indices[i];                                            \
            CODE                                                               \
        }                                                                      \
    }                                                                          \
    else assert(0);                                                            \
}



/**
 * Given an ARR64 RowIndex `self` attempt to compactify it (i.e. convert into
 * an ARR32 rowindex), and return self if successful (the RowIndex object
 * is modified in-place).
 */
RowIndex* rowindex_compactify(RowIndex *self)
{
    if (self->type != RI_ARR64 || self->max > INT32_MAX ||
        self->length > INT32_MAX) return 0;

    int64_t *src = self->ind64;
    int32_t *res = (int32_t*) src;  // Note: res writes on top of src!
    int32_t len = (int32_t) self->length;
    for (int32_t i = 0; i < len; i++) {
        res[i] = (int32_t) src[i];
    }
    dtrealloc(res, int32_t, len);
    self->type = RI_ARR32;
    self->ind32 = res;
    return self;
}



/**
 * Construct a RowIndex object from triple `(start, count, step)`. The new
 * object will have type `RI_SLICE`.
 *
 * Note that we depart from Python's standard of using `(start, end, step)` to
 * denote a slice -- having a `count` gives several advantages:
 *   - computing the "end" is easy and unambiguous: `start + count * step`;
 *     whereas computing "count" from `end` is harder: `(end - start) / step`.
 *   - with explicit `count` the `step` may safely be 0.
 *   - there is no difference in handling positive/negative steps.
 *
 * Returns a new `RowIndex` object, or NULL if such object cannot be created.
 */
RowIndex* rowindex_from_slice(int64_t start, int64_t count, int64_t step)
{
    // check that 0 <= start, count, start + (count-1)*step <= INT64_MAX
    if (start < 0 || count < 0 ||
        (count > 1 && step < -(start/(count - 1))) ||
        (count > 1 && step > (INT64_MAX - start)/(count - 1))) return NULL;

    RowIndex *res = NULL;
    dtmalloc(res, RowIndex, 1);
    res->type = RI_SLICE;
    res->length = count;
    res->min = !count? 0 : step >= 0? start : start + step * (count - 1);
    res->max = !count? 0 : step >= 0? start + step * (count - 1) : start;
    res->slice.start = start;
    res->slice.step = step;
    return res;
}



/**
 * Construct an "array" `RowIndex` object from a series of triples
 * `(start, count, step)`. The triples are given as 3 separate arrays of starts,
 * of counts and of steps.
 *
 * This will create either an RI_ARR32 or RI_ARR64 object, depending on which
 * one is sufficient to hold all the indices.
 */
RowIndex* rowindex_from_slicelist(
    int64_t *starts, int64_t *counts, int64_t *steps, int64_t n)
{
    if (n < 0) return NULL;

    // Compute the total number of elements, and the largest index that needs
    // to be stored. Also check for potential overflows / invalid values.
    int64_t count = 0;
    int64_t minidx = INT64_MAX, maxidx = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t start = starts[i];
        int64_t step = steps[i];
        int64_t len = counts[i];
        if (len == 0) continue;
        if (len < 0 || start < 0 || count + len > INT64_MAX ||
            (len > 1 && step < -(start/(len - 1))) ||
            (len > 1 && step > (INT64_MAX - start)/(len - 1))) return NULL;
        int64_t end = start + step * (len - 1);
        if (start < minidx) minidx = start;
        if (start > maxidx) maxidx = start;
        if (end < minidx) minidx = end;
        if (end > maxidx) maxidx = end;
        count += len;
    }
    if (maxidx == 0) minidx = 0;
    assert(minidx >= 0 && minidx <= maxidx);

    RowIndex *res = NULL;
    dtmalloc(res, RowIndex, 1);
    res->length = count;
    res->min = minidx;
    res->max = maxidx;

    if (count <= INT32_MAX && maxidx <= INT32_MAX) {
        int32_t *rows = NULL;
        dtmalloc(rows, int32_t, count);
        int32_t *rowsptr = rows;
        for (int64_t i = 0; i < n; i++) {
            int32_t j = (int32_t) starts[i];
            int32_t l = (int32_t) counts[i];
            int32_t s = (int32_t) steps[i];
            for (int32_t k = 0; k < l; k++, j += s)
                *rowsptr++ = j;
        }
        assert(rowsptr - rows == count);
        res->ind32 = rows;
        res->type = RI_ARR32;
    } else {
        int64_t *rows = NULL;
        dtmalloc(rows, int64_t, count);
        int64_t *rowsptr = rows;
        for (int64_t i = 0; i < n; i++) {
            int64_t j = starts[i];
            int64_t l = counts[i];
            int64_t s = steps[i];
            for (int64_t k = 0; k < l; k++, j += s)
                *rowsptr++ = j;
        }
        assert(rowsptr - rows == count);
        res->ind64 = rows;
        res->type = RI_ARR64;
    }

    return res;
}



/**
 * Construct a `RowIndex` object from a plain list of int64_t/int32_t indices.
 * These functions steal ownership of the `array` -- the caller should not
 * attempt to free it afterwards.
 * The RowIndex constructed is always of the type corresponding to the array
 * passed, in particular we do not attempt to compactify an int64_t[] array into
 * int32_t[] even if it were possible.
 */
RowIndex*
rowindex_from_i32_array(int32_t *array, int64_t n)
{
    if (n < 0 || n > INT32_MAX) return NULL;
    RowIndex *res = NULL;
    dtmalloc(res, RowIndex, 1);
    res->type = RI_ARR32;
    res->length = n;
    res->ind32 = array;
    if (n == 0) {
        res->min = 0;
        res->max = 0;
    } else {
        int32_t min = array[0], max = array[0], nn = (int32_t) n;
        for (int32_t i = 1; i < nn; i++) {
            int32_t x = array[i];
            if (x < min) min = x;
            if (x > max) max = x;
        }
        res->min = (int64_t) min;
        res->max = (int64_t) max;
    }
    return res;
}

RowIndex*
rowindex_from_i64_array(int64_t *array, int64_t n)
{
    if (n < 0) return NULL;
    RowIndex *res = NULL;
    dtmalloc(res, RowIndex, 1);
    res->type = RI_ARR64;
    res->length = n;
    res->ind64 = array;
    if (n == 0) {
        res->min = 0;
        res->max = 0;
    } else {
        int64_t min = array[0], max = array[0];
        for (int64_t i = 1; i < n; i++) {
            int64_t x = array[i];
            if (x < min) min = x;
            if (x > max) max = x;
        }
        res->min = min;
        res->max = max;
    }
    return res;
}



/**
 * Construct a `RowIndex` object using a boolean "data" column `col`. The
 * index will contain only those rows where the `filter` contains true
 * values.
 * This function will create an RI_ARR32/64 RowIndex, depending on what is
 * minimally required.
 */
RowIndex* rowindex_from_datacolumn(Column *col, int64_t nrows)
{
    if (col->stype != ST_BOOLEAN_I1) return NULL;

    int8_t *data = col->data;
    int64_t nout = 0;
    int64_t maxrow = 0;
    for (int64_t i = 0; i < nrows; i++)
        if (data[i] == 1) {
            nout++;
            maxrow = i;
        }

    RowIndex *res = NULL;
    dtmalloc(res, RowIndex, 1);
    res->length = nout;
    res->max = maxrow;

    if (nout == 0) {
        res->min = 0;
        res->type = RI_ARR32;
        res->ind32 = NULL;
    } else
    if (nout <= INT32_MAX && maxrow <= INT32_MAX) {
        int32_t *out = NULL;
        dtmalloc(out, int32_t, nout);
        int32_t nn = (int32_t) maxrow;
        for (int32_t i = 0, j = 0; i <= nn; i++) {
            if (data[i] == 1)
                out[j++] = i;
        }
        res->min = out[0];
        res->ind32 = out;
        res->type = RI_ARR32;
    } else {
        int64_t *out = NULL;
        dtmalloc(out, int64_t, nout);
        for (int64_t i = 0, j = 0; i <= maxrow; i++) {
            if (data[i] == 1)
                out[j++] = i;
        }
        res->min = out[0];
        res->ind64 = out;
        res->type = RI_ARR64;
    }

    return res;
}



/**
 * Construct a `RowIndex` object using a boolean data column `col` with
 * another RowIndex applied to it.
 * This function is complimentary to `rowindex_from_datacolumn()`: if you
 * need to construct a RowIndex from a "view" column, then this column can
 * be mapped to a pair of source data column and a rowindex object.
 */
RowIndex*
rowindex_from_column_with_rowindex(Column *col, RowIndex *rowindex)
{
    if (col->stype != ST_BOOLEAN_I1) return NULL;

    int8_t *data = col->data;
    int64_t nouts = 0;
    int64_t maxrow = 0;
    #define CODE                                                               \
        if (data[j] == 1) {                                                    \
            nouts++;                                                           \
            maxrow = j;                                                        \
        }
    ITER_ALL
    #undef CODE

    RowIndex *res = NULL;
    dtmalloc(res, RowIndex, 1);
    res->length = nouts;
    res->max = maxrow;

    if (nouts == 0) {
        res->min = 0;
        res->type = RI_ARR32;
        res->ind32 = NULL;
    } else
    if (nouts <= INT32_MAX && maxrow <= INT32_MAX) {
        int32_t *out; dtmalloc(out, int32_t, nouts);
        int32_t k = 0;
        #define CODE                                                           \
            if (data[j] == 1) {                                                \
                out[k++] = (int32_t) i;                                        \
            }
        ITER_ALL
        #undef CODE
        res->min = out[0];
        res->ind32 = out;
        res->type = RI_ARR32;
    } else {
        int64_t *out; dtmalloc(out, int64_t, nouts);
        int64_t k = 0;
        #define CODE                                                           \
            if (data[j] == 1) {                                                \
                out[k++] = (int64_t) i;                                        \
            }
        ITER_ALL
        #undef CODE
        res->min = out[0];
        res->ind64 = out;
        res->type = RI_ARR64;
    }

    return res;
}



/**
 * Merge two `RowIndex`es, and return the combined `RowIndex`. Specifically,
 * suppose there are objects A, B, C such that the map from A rows onto B is
 * described by the index `ri_ab`, and the map from rows of B onto C is given
 * by `ri_bc`. Then the "merged" RowIndex will describe how rows of A are
 * mapped onto rows of C.
 * Rowindex `ri_ab` may also be NULL, in which case a clone of `ri_bc` is
 * returned.
 */
RowIndex* rowindex_merge(RowIndex *ri_ab, RowIndex *ri_bc)
{
    if (ri_bc == NULL) return NULL;

    int64_t n = ri_bc->length;
    RowIndexType type_bc = ri_bc->type;
    RowIndexType type_ab = ri_ab == NULL? 0 : ri_ab->type;

    RowIndex *res = NULL;
    dtmalloc(res, RowIndex, 1);
    res->length = n;
    if (n == 0) {
        res->type = RI_SLICE;
        res->min = 0;
        res->max = 0;
        res->slice.start = 0;
        res->slice.step = 1;
        return res;
    }

    switch (type_bc) {
        case RI_SLICE: {
            int64_t start_bc = ri_bc->slice.start;
            int64_t step_bc = ri_bc->slice.step;
            if (ri_ab == NULL) {
                res->type = RI_SLICE;
                res->min = ri_bc->min;
                res->max = ri_bc->max;
                res->slice.start = start_bc;
                res->slice.step = step_bc;
            }
            else if (type_ab == RI_SLICE) {
                // Product of 2 slices is again a slice.
                int64_t start_ab = ri_ab->slice.start;
                int64_t step_ab = ri_ab->slice.step;
                int64_t start = start_ab + step_ab * start_bc;
                int64_t step = step_ab * step_bc;
                res->type = RI_SLICE;
                res->slice.start = start;
                res->slice.step = step;
                res->min = step >= 0 ? start : start + step * (n - 1);
                res->max = step >= 0 ? start + step * (n - 1) : start;
            }
            else if (step_bc == 0) {
                // Special case: if `step_bc` is 0, then C just contains the
                // same value repeated `n` times, and hence can be created as
                // a slice even if `ri_ab` is an "array" rowindex.
                res->type = RI_SLICE;
                res->slice.step = 0;
                res->slice.start = (type_ab == RI_ARR32)
                    ? (int64_t) ri_ab->ind32[start_bc]
                    : (int64_t) ri_ab->ind64[start_bc];
                res->min = res->max = res->slice.start;
            }
            else if (type_ab == RI_ARR32) {
                // if A->B is ARR32, then all indices in B are int32, and thus
                // any valid slice over B will also be ARR32 (except possibly
                // a slice with step_bc = 0 and n > INT32_MAX).
                int32_t *rowsres; dtmalloc(rowsres, int32_t, n);
                int32_t *rowssrc = ri_ab->ind32;
                int32_t min = INT32_MAX, max = 0;
                for (int64_t i = 0, ic = start_bc; i < n; i++, ic += step_bc) {
                    int32_t x = rowssrc[ic];
                    rowsres[i] = x;
                    if (x < min) min = x;
                    if (x > max) max = x;
                }
                res->type = RI_ARR32;
                res->ind32 = rowsres;
                res->min = (int64_t) min;
                res->max = (int64_t) max;
            }
            else if (type_ab == RI_ARR64) {
                // if A->B is ARR64, then a slice of B may be either ARR64 or
                // ARR32. We'll create the result as ARR64 first, and then
                // attempt to compactify later.
                int64_t *rowsres; dtmalloc(rowsres, int64_t, n);
                int64_t *rowssrc = ri_ab->ind64;
                int64_t min = INT64_MAX, max = 0;
                for (int64_t i = 0, ic = start_bc; i < n; i++, ic += step_bc) {
                    int64_t x = rowssrc[ic];
                    rowsres[i] = x;
                    if (x < min) min = x;
                    if (x > max) max = x;
                }
                res->type = RI_ARR64;
                res->ind64 = rowsres;
                res->min = min;
                res->max = max;
                rowindex_compactify(res);
            }
            else assert(0);
        } break;  // case RI_SLICE

        case RI_ARR32:
        case RI_ARR64: {
            size_t elemsize = (type_bc == RI_ARR32)? 4 : 8;
            if (ri_ab == NULL) {
                res->type = type_bc;
                res->min = ri_bc->min;
                res->max = ri_bc->max;
                memcpy(res->ind32, ri_bc->ind32, elemsize * (size_t)n);
            }
            else if (type_ab == RI_SLICE) {
                int64_t start_ab = ri_ab->slice.start;
                int64_t step_ab = ri_ab->slice.step;
                int64_t *rowsres; dtmalloc(rowsres, int64_t, n);
                if (type_bc == RI_ARR32) {
                    int32_t *rows_bc = ri_bc->ind32;
                    for (int64_t i = 0; i < n; i++) {
                        rowsres[i] = start_ab + rows_bc[i] * step_ab;
                    }
                } else {
                    int64_t *rows_bc = ri_bc->ind64;
                    for (int64_t i = 0; i < n; i++) {
                        rowsres[i] = start_ab + rows_bc[i] * step_ab;
                    }
                }
                res->type = RI_ARR64;
                res->ind64 = rowsres;
                res->min = start_ab + step_ab * (step_ab >= 0? ri_bc->min : ri_bc->max);
                res->max = start_ab + step_ab * (step_ab >= 0? ri_bc->max : ri_bc->min);
                rowindex_compactify(res);
            }
            else if (type_ab == RI_ARR32 && type_bc == RI_ARR32) {
                int32_t *rows_ac; dtmalloc(rows_ac, int32_t, n);
                int32_t *rows_ab = ri_ab->ind32;
                int32_t *rows_bc = ri_bc->ind32;
                int32_t min = INT32_MAX, max = 0;
                for (int64_t i = 0; i < n; i++) {
                    int32_t x = rows_ab[rows_bc[i]];
                    rows_ac[i] = x;
                    if (x < min) min = x;
                    if (x > max) max = x;
                }
                res->type = RI_ARR32;
                res->ind32 = rows_ac;
                res->min = min;
                res->max = max;
            }
            else {
                int64_t *rows_ac; dtmalloc(rows_ac, int64_t, n);
                int64_t min = INT64_MAX, max = 0;
                if (type_ab == RI_ARR32 && type_bc == RI_ARR64) {
                    int32_t *rows_ab = ri_ab->ind32;
                    int64_t *rows_bc = ri_bc->ind64;
                    for (int64_t i = 0; i < n; i++) {
                        int64_t x = rows_ab[rows_bc[i]];
                        rows_ac[i] = x;
                        if (x < min) min = x;
                        if (x > max) max = x;
                    }
                } else
                if (type_ab == RI_ARR64 && type_bc == RI_ARR32) {
                    int64_t *rows_ab = ri_ab->ind64;
                    int32_t *rows_bc = ri_bc->ind32;
                    for (int64_t i = 0; i < n; i++) {
                        int64_t x = rows_ab[rows_bc[i]];
                        rows_ac[i] = x;
                        if (x < min) min = x;
                        if (x > max) max = x;
                    }
                } else
                if (type_ab == RI_ARR64 && type_bc == RI_ARR64) {
                    int64_t *rows_ab = ri_ab->ind64;
                    int64_t *rows_bc = ri_bc->ind64;
                    for (int64_t i = 0; i < n; i++) {
                        int64_t x = rows_ab[rows_bc[i]];
                        rows_ac[i] = x;
                        if (x < min) min = x;
                        if (x > max) max = x;
                    }
                }
                res->type = RI_ARR64;
                res->ind64 = rows_ac;
                res->min = min;
                res->max = max;
                rowindex_compactify(res);
            }
        } break;  // case RM_ARRXX

        default: assert(0);
    }
    return res;
}



/**
 * Construct a `RowIndex` object using an external filter function. The
 * provided filter function is expected to take a range of rows `row0:row1` and
 * an output buffer, and writes the indices of the selected rows into that
 * buffer. This function then handles assembling that output into a
 * final RowIndex structure, as well as distributing the work load among
 * multiple threads.
 *
 * @param filterfn
 *     Pointer to the filter function with the signature `(row0, row1, out,
 *     &nouts) -> int`. The filter function has to determine which rows in the
 *     range `row0:row1` are to be included, and write their indices into the
 *     array `out`. It should also store in the variable `nouts` the number of
 *     rows selected.
 *
 * @param nrows
 *     Number of rows in the datatable that is being filtered.
 */
RowIndex*
rowindex_from_filterfn32(rowindex_filterfn32 *filterfn, int64_t nrows)
{
    if (nrows > INT32_MAX) return NULL;

    // Output buffer, where we will write the indices of selected rows. This
    // buffer is preallocated to the length of the original dataset, and it will
    // be re-alloced to the proper length in the end. The reason we don't want
    // to scale this array dynamically is because it reduces performance (at
    // least some of the reallocs will have to memmove the data, and moreover
    // the realloc has to occur within a critical section, slowing down the
    // team of threads).
    int32_t *out;
    dtmalloc(out, int32_t, nrows);
    // Number of elements that were written (or tentatively written) so far
    // into the array `out`.
    size_t out_length = 0;
    // We divide the range of rows [0:nrows] into `num_chunks` pieces, each
    // (except the very last one) having `rows_per_chunk` rows. Each such piece
    // is a fundamental unit of work for this function: every thread in the team
    // works on a single chunk at a time, and then moves on to the next chunk
    // in the queue.
    int64_t rows_per_chunk = 65536;
    int64_t num_chunks = (nrows + rows_per_chunk - 1) / rows_per_chunk;

    #pragma omp parallel
    {
        // Intermediate buffer where each thread stores the row numbers it found
        // before they are consolidated into the final output buffer.
        int32_t *buf = malloc((size_t)rows_per_chunk * sizeof(int32_t));
        // Number of elements that are currently being held in `buf`.
        int32_t buf_length = 0;
        // Offset (within the output buffer) where this thread needs to save the
        // contents of its temporary buffer `buf`.
        // The algorithm works as follows: first, the thread calls `filterfn` to
        // fill up its buffer `buf`. After `filterfn` finishes, the variable
        // `buf_length` will contain the number of rows that were selected from
        // the current (`i`th) chunk. Those row numbers are stored in `buf`.
        // Then the thread enters the "ordered" section, where it stores the
        // current length of the output buffer into the `out_offset` variable,
        // and increases the `out_offset` as if it already copied the result
        // there. However the actual copying is done outside the "ordered"
        // section so as to block all other threads as little as possible.
        size_t out_offset = 0;

        #pragma omp for ordered schedule(dynamic, 1)
        for (int64_t i = 0; i < num_chunks; i++) {
            if (buf_length) {
                // This clause is conceptually located after the "ordered"
                // section -- however due to a bug in libgOMP the "ordered"
                // section must come last in the loop. So in order to circumvent
                // the bug, this block had to be moved to the front of the loop.
                size_t bufsize = (size_t)buf_length * sizeof(int32_t);
                memcpy(out + out_offset, buf, bufsize);
                buf_length = 0;
            }

            int64_t row0 = i * rows_per_chunk;
            int64_t row1 = min(row0 + rows_per_chunk, nrows);
            (*filterfn)(row0, row1, buf, &buf_length);

            #pragma omp ordered
            {
                out_offset = out_length;
                out_length += (size_t) buf_length;
            }
        }
        // Note: if the underlying array is small, then some threads may have
        // done nothing at all, and their buffers would be empty.
        if (buf_length) {
            size_t bufsize = (size_t)buf_length * sizeof(int32_t);
            memcpy(out + out_offset, buf, bufsize);
        }
        // End of #pragma omp parallel: clean up any temporary variables.
        free(buf);
    }

    // In the end we shrink the output buffer to the size corresponding to the
    // actual number of elements written.
    dtrealloc(out, int32_t, out_length);

    // Create and return the final rowindex object from the array of int32
    // indices `out`.
    RowIndex *res = NULL;
    dtmalloc(res, RowIndex, 1);
    res->type = RI_ARR32;
    res->length = (int64_t) out_length;
    res->ind32 = out;
    res->min = out_length? out[0] : 0;
    res->max = out_length? out[out_length - 1] : 0;
    return res;
}



RowIndex*
rowindex_from_filterfn64(rowindex_filterfn64 *filterfn, int64_t nrows)
{
    (void)filterfn;
    (void)nrows;
    return NULL;
}



/**
 * RowIndex's destructor.
 */
void rowindex_dealloc(RowIndex *rowindex) {
    if (rowindex == NULL) return;
    switch (rowindex->type) {
        case RI_ARR32: dtfree(rowindex->ind32); break;
        case RI_ARR64: dtfree(rowindex->ind64); break;
        default: /* do nothing */ break;
    }
    dtfree(rowindex);
}