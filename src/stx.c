/*
Stricks v0.2.0
Copyright (C) 2021 - Francois Alcover <francois@alcover.fr>
NO WARRANTY EXPRESSED OR IMPLIED.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <assert.h>
#include <errno.h>

#include "stx.h"
#include "log.h"
#include "util.c"

typedef unsigned char uchar;

typedef struct Head1 {   
    uint8_t     cap;  
    uint8_t     len; 
} Head1;

typedef struct Head4 {   
    uint32_t    cap;  
    uint32_t    len; 
} Head4;

typedef struct HeadS {   
    size_t    cap;  
    size_t    len; 
} HeadS;

typedef struct Attr {   
    uchar   cookie; 
    uchar   flags;
    uchar    data[]; 
} Attr;

typedef enum Type {
    TYPE1 = (int)log2(sizeof(Head1)), 
    TYPE4 = (int)log2(sizeof(Head4))
} Type;

#define ISPOW2(n) (n==2||n==4||n==8||n==16||n==32)
static_assert (ISPOW2(sizeof(Head1)), "bad Head1");
static_assert (ISPOW2(sizeof(Head4)), "bad Head4");
static_assert ((1<<TYPE1) == sizeof(Head1), "bad TYPE1");
static_assert ((1<<TYPE4) == sizeof(Head4), "bad TYPE4");

#define MAGIC 170 // 0xaa 10101010
#define TYPE_BITS 2
#define TYPE_MASK ((1<<TYPE_BITS)-1)

#define COOKIE(s) (((uchar*)(s))[-2])
#define FLAGS(s)  (((uchar*)(s))[-1])
#define TYPE(s) (FLAGS(s) & TYPE_MASK)
#define HEADSZ(type) (1<<type)
#define MEMSZ(type,cap) (HEADSZ(type) + sizeof(Attr) + cap + 1)
#define CHECK(s) ((s) && COOKIE(s) == MAGIC)
#define HEAD(s) ((char*)(s) - sizeof(Attr) - HEADSZ(TYPE(s)))
#define ATTR(head,type) ((Attr*)((char*)(head) + HEADSZ(type)))
#define DATA(head,type) ((char*)head + HEADSZ(type) + sizeof(Attr))

#define SETPROP(head, type, prop, val) \
switch(type) { \
    case TYPE1: ((Head1*)head)->prop = val; break; \
    case TYPE4: ((Head4*)head)->prop = val; break; \
} 

#define GETPROP(head, type, prop) \
((type == TYPE4) ? ((Head4*)head)->prop : ((Head1*)head)->prop)

#define GETCAP(head, type) \
((type == TYPE4) ? (((Head4*)head)->cap - ((Head4*)head)->len) \
                 : (((Head1*)head)->cap - ((Head1*)head)->len))

static intmax_t 
append (void* dst, const char* src, const size_t n, bool alloc/*, bool strict*/);
static bool 
resize (stx_t *ps, const size_t newcap);


//==== PUBLIC =======================================================================

stx_t 
stx_new (const size_t cap)
{
    const Type type = (cap >= 256) ? TYPE4 : TYPE1;

    void* head = STX_MALLOC (MEMSZ(type, cap));
    if (!head) return NULL;

    SETPROP(head, type, cap, cap);
    SETPROP(head, type, len, 0);

    Attr* attr = ATTR(head, type);
    attr->cookie = MAGIC;
    attr->flags = type;
    attr->data[0] = 0; 
    attr->data[cap] = 0; 
    
    return (char*)(attr->data);
}

const stx_t
stx_from (const char* src)
{
    const size_t len = strlen(src);
    const stx_t ret = stx_new(len);

    stx_append_count (ret, src, len);

    return ret;
}

static stx_t
dup (const stx_t s)
{
    const void* head = HEAD(s);
    const Type type = TYPE(s);
    const size_t len = GETPROP(head, type, len);
    const size_t sz = MEMSZ(type,len);
    void* new_head = malloc(sz);

    if (!new_head) return NULL;

    memcpy (new_head, head, sz);
    SETPROP(new_head, type, cap, len);
    stx_t ret = DATA(new_head, type);
    ret[len] = 0; // to be sure

    return ret;
}


stx_t
stx_dup (const stx_t s)
{
    return CHECK(s) ? dup(s) : NULL;
}

void 
stx_reset (const stx_t s)
{
    if (!CHECK(s)) return;

    const void* head = HEAD(s);
    const Type type = TYPE(s);

    SETPROP(head, type, len, 0);
    *s = 0;
}

void 
stx_free (const stx_t s)
{
    if (!CHECK(s)) return;

    char* head = HEAD(s);
    
    switch(TYPE(s)) {
        case TYPE4: bzero(head, sizeof(Head4) + sizeof(Attr)); break;
        case TYPE1: bzero(head, sizeof(Head1) + sizeof(Attr)); break;
    }

    STX_FREE(head);
}

#define ACCESS(s, prop) \
if (!CHECK(s)) return 0; \
void* head = HEAD(s); \
switch(TYPE(s)){ \
    case TYPE1: return ((Head1*)head)->prop; \
    case TYPE4: return ((Head4*)head)->prop; \
    default: return 0; \
}

size_t 
stx_cap (const stx_t s) {ACCESS(s,cap);}

size_t 
stx_len (const stx_t s) {ACCESS(s,len);}

size_t 
stx_spc (const stx_t s)
{
    if (!CHECK(s)) return 0;

    void* head = HEAD(s);
    
    switch(TYPE(s)) {
        case TYPE4: return ((Head4*)head)->cap - ((Head4*)head)->len;
        case TYPE1: return ((Head1*)head)->cap - ((Head1*)head)->len;
        default: return 0;
    }
}

intmax_t 
stx_append (stx_t dst, const char* src) 
{
    return append((void*)dst, src, 0, false);       
}

intmax_t 
stx_append_count (stx_t dst, const char* src, const size_t n) 
{
    return append((void*)dst, src, n, false);       
}

size_t 
stx_append_alloc (stx_t* dst, const char* src)
{
    return append((void*)dst, src, 0, true);        
}

size_t 
stx_append_count_alloc (stx_t* dst, const char* src, const size_t n)
{
    return append((void*)dst, src, n, true);        
}



static int 
append_format (stx_t dst, const char* fmt, va_list args)
{
    const void* head = HEAD(dst);
    const Type type = TYPE(dst);
    const size_t len = GETPROP(head, type, len);
    const size_t spc = GETCAP(head, type);

    if (!spc) return STX_FAIL;

    char* end = dst + len;

    errno = 0;
    const size_t src_len = vsnprintf(end, spc+1, fmt, args);
     
    // Error
    if (src_len < 0) {
        perror("stx_append_format");
        *end = 0; // undo
        return STX_FAIL;
    }

    // Truncation
    if (src_len > spc) {
        *end = 0; // undo
        return -(len + src_len);
    } 

    // Update length
    SETPROP(head, type, len, src_len);

    return src_len;
}


int 
stx_append_format (const stx_t dst, const char* fmt, ...) 
{
    if (!CHECK(dst)) return 0;

    va_list args;
    va_start(args, fmt);
    int rc = append_format (dst, fmt, args);            
    va_end(args);

    return rc;
}

bool
stx_resize (stx_t *ps, const size_t newcap)
{
    return resize(ps, newcap);
}

bool 
stx_equal (const stx_t a, const stx_t b) 
{
    if (!CHECK(a) || !CHECK(b)) return false;
    const void* heada = HEAD(a);
    const void* headb = HEAD(b);
    const Type typea = TYPE(a);
    const Type typeb = TYPE(b);
    const size_t lena = GETPROP(heada, typea, len);

    return (lena == GETPROP(headb, typeb, len)) && !memcmp (a, b, lena);
}

bool 
stx_check (const stx_t s)
{
    return CHECK(s);
}

void 
stx_show (const stx_t s)
{
    if (!CHECK(s)) return;

    void* head = HEAD(s);
    Type type = TYPE(s);

    #define SHOW_FMT "cap:%zu len:%zu cookie:%d flags:%d data:'%s'\n"
    #define SHOW_ARGS (size_t)(h->cap), (size_t)(h->len), (uchar)s[-2], (uchar)s[-1], s

    switch(type){
        case TYPE4: {
            Head4* h = (Head4*)head;
            printf (SHOW_FMT, SHOW_ARGS);
            break;
        }
        case TYPE1: {
            Head1* h = (Head1*)head;
            printf (SHOW_FMT, SHOW_ARGS);
            break;
        }
        default: ERR("stx_show: unknown type\n");
    }

    fflush(stdout);
}

//==== PRIVATE =======================================================================

static intmax_t 
append (void* dst, const char* src, const size_t n, bool alloc/*, bool strict*/) 
{
    stx_t s = alloc ? *((stx_t**)(dst)) : dst;
    
    if (!CHECK(s)||!src) return STX_FAIL;

    const void* head = HEAD(s);
    const Type type = TYPE(s);
    const size_t cap = GETPROP(head, type, cap);
    const size_t len = GETPROP(head, type, len);

    const size_t inc = n ? strnlen(src,n) : strlen(src);
    const size_t totlen = len + inc;

    if (totlen > cap) {  
        // Would truncate, return needed capacity
        if (!alloc) return -totlen;

        if (!resize(&s, totlen*2)) {
            ERR("resize failed");
            return STX_FAIL;
        }
        head = HEAD(s);
        *((stx_t*)(dst)) = s;
    }

    char* end = s + len;
    memcpy (end, src, inc);
    end[inc] = 0;
    SETPROP(head, type, len, totlen);

    return inc;        
}


static bool 
resize (stx_t *ps, const size_t newcap)
{    
    stx_t s = *ps;

    if (!CHECK(s)) return false;

    const void* head = HEAD(s);
    const Type type = TYPE(s);
    const size_t cap = GETPROP(head, type, cap);
    const size_t len = GETPROP(head, type, len);

    if (newcap == cap) return true;
    
    const Type newtype = (newcap >= 256) ? TYPE4 : TYPE1;
    const bool sametype = (newtype == type);
   
    void* newhead;

    if (sametype)
        newhead = realloc ((void*)head, MEMSZ(type, newcap));
    else {
        newhead = STX_MALLOC (MEMSZ(newtype, newcap));
    }

    if (!newhead) {
        ERR ("stx_resize: realloc failed\n");
        return false;
    }
    
    stx_t news = DATA(newhead, newtype);
    
    if (!sametype) {
        memcpy(news, s, len+1); //?
        // reput len
        SETPROP(newhead, newtype, len, len);
        // reput cookie
        COOKIE(news) = MAGIC;
        // update flags
        FLAGS(news) = (FLAGS(news) & ~TYPE_MASK) | newtype;
    }
    
    // truncated
    if (newcap < len) {
        #ifdef STX_WARNINGS
            LOG("stx_resize: truncated");
        #endif
        SETPROP(newhead, newtype, len, newcap);
    }

    // update cap
    SETPROP(newhead, newtype, cap, newcap); 
    // update cap sentinel
    news[newcap] = 0;
    
    *ps = news;
    return true;
}
