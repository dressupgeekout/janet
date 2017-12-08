/*
* Copyright (c) 2017 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#include <dst/dst.h>
#include "strtod.h"

/* Checks if a string slice is equal to a string constant */
static int check_str_const(const char *ref, const uint8_t *start, const uint8_t *end) {
    while (*ref && start < end) {
        if (*ref != *(char *)start) return 0;
        ++ref;
        ++start;
    }
    return !*ref && start == end;
}

/* Quote a value */
static DstValue quote(DstValue x) {
    DstValue *t = dst_tuple_begin(2);
    t[0] = dst_csymbolv("quote");
    t[1] = x;
    return dst_wrap_tuple(dst_tuple_end(t));
}

/* Check if a character is whitespace */
static int is_whitespace(uint8_t c) {
    return c == ' ' 
        || c == '\t'
        || c == '\n'
        || c == '\r'
        || c == '\0'
        || c == ',';
}

/* Check if a character is a valid symbol character */
/* TODO - allow utf8 - shouldn't be difficult, err on side
 * of inclusivity */
static int is_symbol_char(uint8_t c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= ':') return 1;
    if (c >= '<' && c <= '@') return 1;
    if (c >= '*' && c <= '/') return 1;
    if (c >= '$' && c <= '&') return 1;
    if (c == '_') return 1;
    if (c == '^') return 1;
    if (c == '!') return 1;
    return 0;
}

/* Get hex digit from a letter */
static int to_hex(uint8_t c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    } else if (c >= 'A' && c <= 'F') {
        return 10 + c - 'A';
    } else if (c >= 'a' && c <= 'f') {
        return 10 + c - 'a';
    } else {
        return -1;
    }
}

/* Make source mapping for atom (non recursive structure) */
static DstValue atom_map(int32_t start, int32_t end) {
    DstValue *t = dst_tuple_begin(2);
    t[0] = dst_wrap_integer(start);
    t[1] = dst_wrap_integer(end);
    return dst_wrap_tuple(dst_tuple_end(t));
}

/* Create mappingd for recursive data structure */
static DstValue ds_map(int32_t start, int32_t end, DstValue submap) {
    DstValue *t = dst_tuple_begin(3);
    t[0] = dst_wrap_integer(start);
    t[1] = dst_wrap_integer(end);
    t[2] = submap;
    return dst_wrap_tuple(dst_tuple_end(t));
}

/* Create a sourcemapping for a key value pair */
static DstValue kv_map(DstValue k, DstValue v) {
    DstValue *t = dst_tuple_begin(2);
    t[0] = k;
    t[1] = v;
    return dst_wrap_tuple(dst_tuple_end(t));
}

typedef struct {
    DstArray stack;
    DstArray mapstack;
    const uint8_t *srcstart;
    const uint8_t *end;
    const char *errmsg;
    DstParseStatus status;
} ParseArgs;

/* Entry point of the recursive descent parser */
static const uint8_t *parse_recur(
    ParseArgs *args,
    const uint8_t *src,
    int32_t recur) {

    const uint8_t *end = args->end;
    const uint8_t *mapstart;
    int32_t qcount = 0;
    DstValue ret;
    DstValue submapping;

    /* Prevent stack overflow */
    if (recur == 0) goto too_much_recur;

    /* try parsing again */
    begin:

    /* Trim leading whitespace and count quotes */
    while (src < end && (is_whitespace(*src) || *src == '\'')) {
        if (*src == '\'') {
            ++qcount;
        }
        ++src;
    }

    /* Check for end of source */
    if (src >= end) goto unexpected_eos;
    
    /* Open mapping */
    mapstart = src;
    submapping = dst_wrap_nil();

    /* Detect token type based on first character */
    switch (*src) {

        /* Numbers, symbols, simple literals */
        default: 
        atom:
        {
            DstValue numcheck;
            const uint8_t *tokenend = src;
            if (!is_symbol_char(*src)) goto unexpected_character;
            while (tokenend < end && is_symbol_char(*tokenend))
                tokenend++;
            numcheck = dst_scan_number(src, tokenend - src);
            if (!dst_checktype(numcheck, DST_NIL)) {
                ret = numcheck;
            } else if (check_str_const("nil", src, tokenend)) {
                ret = dst_wrap_nil();
            } else if (check_str_const("false", src, tokenend)) {
                ret = dst_wrap_boolean(0);
            } else if (check_str_const("true", src, tokenend)) {
                ret = dst_wrap_boolean(1);
            } else {
                if (*src >= '0' && *src <= '9') {
                    goto sym_nodigits;
                } else {
                    ret = dst_symbolv(src, tokenend - src);
                }
            }
            src = tokenend;
            break;
        }

        case '#':
        {
            /* Jump to next newline */
            while (src < end && *src != '\n')
                ++src;
            goto begin;
        }

        /* String literals */
        case '"':
        {
            const uint8_t *strend = ++src;
            const uint8_t *strstart = strend;
            int32_t len = 0;
            int containsEscape = 0;
            /* Preprocess string to check for escapes and string end */
            while (strend < end && *strend != '"') {
                len++;
                if (*strend++ == '\\') {
                    containsEscape = 1;
                    if (strend >= end) goto unexpected_eos;
                    if (*strend == 'h') {
                        strend += 3;
                        if (strend >= end) goto unexpected_eos;
                    } else {
                        strend++;
                        if (strend >= end) goto unexpected_eos;
                    }
                }
            }
            if (containsEscape) {
                uint8_t *buf = dst_string_begin(len);
                uint8_t *write = buf;
                while (src < strend) {
                    if (*src == '\\') {
                        src++;
                        switch (*src++) {
                            case 'n': *write++ = '\n'; break;
                            case 'r': *write++ = '\r'; break;
                            case 't': *write++ = '\t'; break;
                            case 'f': *write++ = '\f'; break;
                            case '0': *write++ = '\0'; break;
                            case '"': *write++ = '"'; break;
                            case '\'': *write++ = '\''; break;
                            case 'z': *write++ = '\0'; break;
                            case 'e': *write++ = 27; break;
                            case 'h': {
                                int d1 = to_hex(*src++);
                                int d2 = to_hex(*src++);
                                if (d1 < 0 || d2 < 0) goto invalid_hex;
                                *write++ = 16 * d1 + d2;
                                break;
                            }
                            default:
                                goto unknown_strescape;
                        }
                    } else {
                        *write++ = *src++;
                    }
                }
                ret = dst_wrap_string(dst_string_end(buf));
            } else {
                ret = dst_wrap_string(dst_string(strstart, strend - strstart));
            }
            src = strend + 1;
            break;
        }

        /* Data Structure literals */
        case '@':
            if (src[1] != '{')
                goto atom;
        case '(':
        case '[':
        case '{': 
        {
            int32_t n = 0, i = 0;
            int32_t istable = 0;
            uint8_t close;
            switch (*src++) {
                case '[': close = ']'; break;
                case '{': close = '}'; break;
                case '@': close = '}'; src++; istable = 1; break;
                default: close = ')'; break;
            }
            /* Trim trailing whitespace */
            while (src < end && (is_whitespace(*src)))
                ++src;
            /* Recursively parse inside literal */
            while (*src != close) {
                src = parse_recur(args, src, recur - 1);
                if (args->errmsg || !src) return src;
                n++;
                /* Trim trailing whitespace */
                while (src < end && (is_whitespace(*src)))
                    ++src;
            }
            src++;
            switch (close) {
                case ')':
                {
                    DstValue *tup = dst_tuple_begin(n);
                    DstValue *subtup = dst_tuple_begin(n);
                    for (i = n; i > 0; i--) {
                        tup[i - 1] = dst_array_pop(&args->stack);
                        subtup[i - 1] = dst_array_pop(&args->mapstack);
                    }
                    ret = dst_wrap_tuple(dst_tuple_end(tup));
                    submapping = dst_wrap_tuple(dst_tuple_end(subtup));
                    break;
                }
                case ']':
                {
                    DstArray *arr = dst_array(n);
                    DstArray *subarr = dst_array(n);
                    for (i = n; i > 0; i--) {
                        arr->data[i - 1] = dst_array_pop(&args->stack);
                        subarr->data[i - 1] = dst_array_pop(&args->mapstack);
                    }
                    arr->count = n;
                    subarr->count = n;
                    ret = dst_wrap_array(arr);
                    submapping = dst_wrap_array(subarr);
                    break;
                }
                case '}':
                {
                    if (n & 1) goto struct_oddargs;
                    if (istable) {
                        DstTable *t = dst_table(n);
                        DstTable *subt = dst_table(n);
                        for (i = n; i > 0; i -= 2) {
                            DstValue val = dst_array_pop(&args->stack);
                            DstValue key = dst_array_pop(&args->stack);
                            DstValue subval = dst_array_pop(&args->mapstack);
                            DstValue subkey = dst_array_pop(&args->mapstack);

                            dst_table_put(t, key, val);
                            dst_table_put(subt, key, kv_map(subkey, subval));
                        }
                        ret = dst_wrap_table(t);
                        submapping = dst_wrap_table(subt);
                    } else {
                        DstValue *st = dst_struct_begin(n >> 1);
                        DstValue *subst = dst_struct_begin(n >> 1);
                        for (i = n; i > 0; i -= 2) {
                            DstValue val = dst_array_pop(&args->stack);
                            DstValue key = dst_array_pop(&args->stack);
                            DstValue subval = dst_array_pop(&args->mapstack);
                            DstValue subkey = dst_array_pop(&args->mapstack);

                            dst_struct_put(st, key, val);
                            dst_struct_put(subst, key, kv_map(subkey, subval));
                        }
                        ret = dst_wrap_struct(dst_struct_end(st));
                        submapping = dst_wrap_struct(dst_struct_end(subst));
                    }
                    break;
                }
            }
            break;
        }
    }

    /* Quote the returned value qcount times */
    while (qcount--) ret = quote(ret);

    /* Push the result to the stack */
    dst_array_push(&args->stack, ret);
    
    /* Push source mapping */
    if (dst_checktype(submapping, DST_NIL)) {
        /* We just parsed an atom */
        dst_array_push(&args->mapstack, atom_map(
                    mapstart - args->srcstart,
                    src - args->srcstart));
    } else {
        /* We just parsed a recursive data structure */
        dst_array_push(&args->mapstack, ds_map(
                    mapstart - args->srcstart,
                    src - args->srcstart,
                    submapping));
    }

    /* Return the new source position for further calls */
    return src;

    /* Errors below */

    unexpected_eos:
    args->errmsg = "unexpected end of source";
    args->status = DST_PARSE_UNEXPECTED_EOS;
    return NULL;

    unexpected_character:
    args->errmsg = "unexpected character";
    args->status = DST_PARSE_ERROR;
    return src;

    sym_nodigits:
    args->errmsg = "symbols cannot start with digits";
    args->status = DST_PARSE_ERROR;
    return src;

    struct_oddargs:
    args->errmsg = "struct literal needs an even number of arguments";
    args->status = DST_PARSE_ERROR;
    return src;

    unknown_strescape:
    args->errmsg = "unknown string escape sequence";
    args->status = DST_PARSE_ERROR;
    return src;

    invalid_hex:
    args->errmsg = "invalid hex escape in string";
    args->status = DST_PARSE_ERROR;
    return src;

    too_much_recur:
    args->errmsg = "recursed too deeply in parsing";
    args->status = DST_PARSE_ERROR;
    return src;
}

/* Parse an array of bytes. Return value in the fiber return value. */
DstParseResult dst_parse(const uint8_t *src, int32_t len) {
    DstParseResult res;
    ParseArgs args;
    const uint8_t *newsrc;

    dst_array_init(&args.stack, 10);
    args.status = DST_PARSE_OK;
    args.srcstart = src;
    args.end = src + len;
    args.errmsg = NULL;

    dst_array_init(&args.mapstack, 10);

    newsrc = parse_recur(&args, src, DST_RECURSION_GUARD);
    res.status = args.status;
    res.bytes_read = (int32_t) (newsrc - src);

    if (args.errmsg) {
        res.result.error = dst_cstring(args.errmsg);
        res.map = NULL;
    } else {
        res.result.value = dst_array_pop(&args.stack);
        res.map = dst_unwrap_tuple(dst_array_pop(&args.mapstack));
    }

    dst_array_deinit(&args.stack);
    dst_array_deinit(&args.mapstack);

    return res;
}

/* Parse a c string */
DstParseResult dst_parsec(const char *src) {
    int32_t len = 0;
    while (src[len]) ++len;
    return dst_parse((const uint8_t *)src, len);
}

/* Get the sub source map by indexing a value. Used to traverse
 * into arrays and tuples */
const DstValue *dst_parse_submap_index(const DstValue *map, int32_t index) {
    if (NULL != map && dst_tuple_length(map) >= 3) {
        const DstValue *seq;
        int32_t len;
        if (dst_seq_view(map[2], &seq, &len)) {
            if (index >= 0 && index < len) {
                if (dst_checktype(seq[index], DST_TUPLE)) {
                    const DstValue *ret = dst_unwrap_tuple(seq[index]);
                    if (dst_tuple_length(ret) >= 2 &&
                        dst_checktype(ret[0], DST_INTEGER) &&
                        dst_checktype(ret[1], DST_INTEGER)) {
                        return ret;
                    }
                }
            }
        }
    }
    return NULL;
}

/* Traverse into tables and structs */
static const DstValue *dst_parse_submap_kv(const DstValue *map, DstValue key, int kv) {
    if (NULL != map && dst_tuple_length(map) >= 3) {
        DstValue kvpair = dst_get(map[2], key);
        if (dst_checktype(kvpair, DST_TUPLE)) {
            const DstValue *kvtup = dst_unwrap_tuple(kvpair);
            if (dst_tuple_length(kvtup) >= 2) {
                if (dst_checktype(kvtup[kv], DST_TUPLE)) {
                    const DstValue *ret = dst_unwrap_tuple(kvtup[kv]);
                    if (dst_tuple_length(ret) >= 2 &&
                        dst_checktype(ret[0], DST_INTEGER) &&
                        dst_checktype(ret[1], DST_INTEGER)) {
                        return ret;
                    }
                }
            }
        }
    }
    return NULL;
}

/* Traverse into a key of a table or struct */
const DstValue *dst_parse_submap_key(const DstValue *map, DstValue key) {
    return dst_parse_submap_kv(map, key, 0);
}

/* Traverse into a value of a table or struct */
const DstValue *dst_parse_submap_value(const DstValue *map, DstValue key) {
    return dst_parse_submap_kv(map, key, 1);
}
