/* libSceJson2: sce::Json, the SDK's C++ JSON library, in the layouts the game builds them in.
 * Deception parses PSN replies with it (friends' onlineStatus, ...): one Initializer with its own
 * MemAllocator subclass, documents parsed with Parser::parse and read with operator[], count,
 * the getters and toString.
 *
 * Value (0x20 bytes, the game reserves that much per Value): type, payload, and the null access
 * callback with its argument. A missing key or index, or a getter on a Value of another type,
 * returns what that callback returns for the expected type (the game hands back a default Value
 * of that type), or a null Value without one. Parsed children inherit the root's callback.
 * String (8 bytes): a NUL-terminated copy in low memory (guest-visible), or NULL for "".
 * Arrays and objects live in low memory, owned by their Value. The game's MemAllocator is not
 * used: everything comes from runtime_low_alloc. */
#include "runtime.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_NULL, T_BOOL, T_INT, T_UINT, T_REAL, T_STRING, T_ARRAY, T_OBJECT };
#define ERR_PARSE ((int32_t)0x80920001) /* any failure: the game only tests for 0 */

typedef struct { char *text; } String;
typedef const void *(ABI *NullCallback)(int32_t type, const void *parent, void *context);
typedef struct Value {
    int32_t type, pad;
    union { bool b; int64_t i; uint64_t u; double d; String s; struct Array *a; struct Object *o; };
    NullCallback null_access;
    void *context;
} Value;
_Static_assert(sizeof(Value)==0x20,"sce::Json::Value is 0x20 bytes in the game");
_Static_assert(sizeof(String)==8,"sce::Json::String is 8 bytes in the game");
typedef struct Array { size_t count, cap; Value *items; } Array;
typedef struct { String key; Value value; } Pair;
typedef struct Object { size_t count, cap; Pair *items; } Object;

static const Value null_value;
static const String empty_string;

static char *copy_text(const char *text, size_t length) {
    char *copy=runtime_low_alloc(length+1);
    if (!copy) return NULL;
    memcpy(copy,text,length);
    copy[length]=0;
    return copy;
}
static void *grow(void *items, size_t *cap, size_t count, size_t item) {
    if (count<*cap) return items;
    size_t next=*cap ? *cap*2 : 4;
    void *bigger=runtime_low_alloc(next*item);
    if (!bigger) return NULL;
    if (items) { memcpy(bigger,items,count*item); runtime_low_free(items); }
    *cap=next;
    return bigger;
}

/* Value payload release; the type becomes null, the callback stays. */
static void release(Value *v) {
    if (v->type==T_STRING && v->s.text) runtime_low_free(v->s.text);
    if (v->type==T_ARRAY && v->a) {
        for (size_t i=0;i<v->a->count;++i) release(&v->a->items[i]);
        if (v->a->items) runtime_low_free(v->a->items);
        runtime_low_free(v->a);
    }
    if (v->type==T_OBJECT && v->o) {
        for (size_t i=0;i<v->o->count;++i) {
            if (v->o->items[i].key.text) runtime_low_free(v->o->items[i].key.text);
            release(&v->o->items[i].value);
        }
        if (v->o->items) runtime_low_free(v->o->items);
        runtime_low_free(v->o);
    }
    v->type=T_NULL;
    v->u=0;
}
/* What a read of `type` from `v` gives when `v` holds no such thing: the callback's Value. */
static const Value *missing(const Value *v, int32_t type) {
    if (v && v->null_access) {
        const Value *r=v->null_access(type,v,v->context);
        if (r) return r;
    }
    return &null_value;
}

/* String::String(), String(const char*), ~String() */
static ABI void string_ctor(String *s) { s->text=NULL; }
static ABI void string_ctor_text(String *s, const char *text) { s->text=text && *text ? copy_text(text,strlen(text)) : NULL; }
static ABI void string_dtor(String *s) { if (s->text) runtime_low_free(s->text); s->text=NULL; }
/* const char *String::c_str() const; size()/length(): bytes before the NUL. */
static ABI const char *string_c_str(const String *s) { return s->text ? s->text : ""; }
static ABI size_t string_size(const String *s) { return s->text ? strlen(s->text) : 0; }
/* const char &String::at(size_t) const: the NUL past the end. */
static ABI const char *string_at(const String *s, size_t i) { return i<string_size(s) ? s->text+i : ""; }
/* bool String::operator==(const char*) const */
static ABI bool string_equals(const String *s, const char *text) { return !strcmp(string_c_str(s),text ? text : ""); }
static void string_set(String *s, const char *text, size_t length) {
    char *copy=length ? copy_text(text,length) : NULL;
    if (s->text) runtime_low_free(s->text);
    s->text=copy;
}

/* Value::Value(), ~Value(), clear(): a null Value; clear keeps the callback. */
static ABI void value_ctor(Value *v) { memset(v,0,sizeof(*v)); }
static ABI void value_dtor(Value *v) { release(v); }
static ABI void value_clear(Value *v) { release(v); }
/* Value::set(...): replaces the payload. set(ValueType) gives that type's empty value. */
static ABI void value_set_type(Value *v, int32_t type) { release(v); v->type=type>=T_NULL && type<=T_OBJECT ? type : T_NULL; }
static ABI void value_set_bool(Value *v, bool b) { release(v); v->type=T_BOOL; v->b=b; }
static ABI void value_set_int(Value *v, int64_t i) { release(v); v->type=T_INT; v->i=i; }
static ABI void value_set_uint(Value *v, uint64_t u) { release(v); v->type=T_UINT; v->u=u; }
static ABI void value_set_real(Value *v, double d) { release(v); v->type=T_REAL; v->d=d; }
static ABI void value_set_string(Value *v, const String *s) {
    char *copy=s && s->text ? copy_text(s->text,strlen(s->text)) : NULL;
    release(v); v->type=T_STRING; v->s.text=copy;
}
/* Value::setNullAccessCallBack(callback, context) */
static ABI void value_set_null_access(Value *v, NullCallback callback, void *context) { v->null_access=callback; v->context=context; }

/* Getters: the payload, numbers converted between the integer kinds, else the callback's. */
static ABI bool value_get_bool(const Value *v) { return v->type==T_BOOL ? v->b : missing(v,T_BOOL)->b; }
static ABI int64_t value_get_int(const Value *v) {
    if (v->type==T_INT || v->type==T_UINT) return v->i;
    const Value *r=missing(v,T_INT);
    return r->type==T_INT || r->type==T_UINT ? r->i : 0;
}
static ABI const String *value_get_string(const Value *v) {
    if (v->type==T_STRING) return &v->s;
    const Value *r=missing(v,T_STRING);
    return r->type==T_STRING ? &r->s : &empty_string;
}
/* operator bool: the Value holds something (not null). */
static ABI bool value_valid(const Value *v) { return v->type!=T_NULL; }
/* size_t count() const: elements of an array or object, else 0. */
static ABI size_t value_count(const Value *v) {
    return v->type==T_ARRAY && v->a ? v->a->count : v->type==T_OBJECT && v->o ? v->o->count : 0;
}
/* const Value &operator[](const char*) const / operator[](size_t) const */
static ABI const Value *value_key(const Value *v, const char *key) {
    if (v->type==T_OBJECT && v->o && key)
        for (size_t i=0;i<v->o->count;++i)
            if (!strcmp(string_c_str(&v->o->items[i].key),key)) return &v->o->items[i].value;
    return missing(v,T_NULL);
}
static ABI const Value *value_index(const Value *v, size_t i) {
    if (v->type==T_ARRAY && v->a && i<v->a->count) return &v->a->items[i];
    return missing(v,T_NULL);
}

/* Serializer for toString: a growable buffer in host memory. */
typedef struct { char *data; size_t size, cap; int failed; } Buffer;
static void put(Buffer *b, const char *text, size_t length) {
    if (b->failed) return;
    if (b->size+length+1>b->cap) {
        size_t cap=b->cap ? b->cap : 64;
        while (cap<b->size+length+1) cap*=2;
        char *bigger=realloc(b->data,cap);
        if (!bigger) { b->failed=1; return; }
        b->data=bigger; b->cap=cap;
    }
    memcpy(b->data+b->size,text,length);
    b->size+=length;
    b->data[b->size]=0;
}
static void put_text(Buffer *b, const char *text) { put(b,text,strlen(text)); }
static void put_quoted(Buffer *b, const char *text) {
    put(b,"\"",1);
    for (const unsigned char *p=(const unsigned char *)text;*p;++p) {
        char escape[8];
        if (*p=='"' || *p=='\\') { escape[0]='\\'; escape[1]=(char)*p; put(b,escape,2); }
        else if (*p<0x20) { snprintf(escape,sizeof(escape),"\\u%04x",*p); put_text(b,escape); }
        else put(b,(const char *)p,1);
    }
    put(b,"\"",1);
}
static void serialize(Buffer *b, const Value *v, int quote) {
    char number[40];
    switch (v->type) {
    case T_BOOL: put_text(b,v->b ? "true" : "false"); break;
    case T_INT: snprintf(number,sizeof(number),"%" PRId64,v->i); put_text(b,number); break;
    case T_UINT: snprintf(number,sizeof(number),"%" PRIu64,v->u); put_text(b,number); break;
    case T_REAL: snprintf(number,sizeof(number),"%.17g",v->d); put_text(b,number); break;
    case T_STRING: if (quote) put_quoted(b,string_c_str(&v->s)); else put_text(b,string_c_str(&v->s)); break;
    case T_ARRAY:
        put(b,"[",1);
        for (size_t i=0;v->a && i<v->a->count;++i) { if (i) put(b,",",1); serialize(b,&v->a->items[i],1); }
        put(b,"]",1);
        break;
    case T_OBJECT:
        put(b,"{",1);
        for (size_t i=0;v->o && i<v->o->count;++i) {
            if (i) put(b,",",1);
            put_quoted(b,string_c_str(&v->o->items[i].key));
            put(b,":",1);
            serialize(b,&v->o->items[i].value,1);
        }
        put(b,"}",1);
        break;
    default: put_text(b,"null");
    }
}
/* int Value::toString(String&) const: a string's own text, anything else as JSON. 0. */
static ABI int32_t value_to_string(const Value *v, String *out) {
    Buffer b={0};
    serialize(&b,v,0);
    string_set(out,b.data ? b.data : "",b.failed ? 0 : b.size);
    free(b.data);
    return 0;
}

/* Parser: RFC 8259, the whole text one value (surrounding whitespace allowed). */
typedef struct { const char *p, *end; NullCallback callback; void *context; int depth; } Parse;
static void skip_space(Parse *s) { while (s->p<s->end && (*s->p==' ' || *s->p=='\t' || *s->p=='\n' || *s->p=='\r')) ++s->p; }
static int literal(Parse *s, const char *word) {
    size_t n=strlen(word);
    if ((size_t)(s->end-s->p)<n || memcmp(s->p,word,n)) return 0;
    s->p+=n;
    return 1;
}
static int hex4(Parse *s, uint32_t *out) {
    if (s->end-s->p<4) return 0;
    uint32_t v=0;
    for (int i=0;i<4;++i) {
        char c=*s->p++;
        v=v*16+(c>='0' && c<='9' ? (uint32_t)(c-'0') : c>='a' && c<='f' ? (uint32_t)(c-'a'+10) : c>='A' && c<='F' ? (uint32_t)(c-'A'+10) : 99);
        if (v>0xffff) return 0;
    }
    *out=v;
    return 1;
}
static void put_utf8(Buffer *b, uint32_t c) {
    char u[4];
    if (c<0x80) { u[0]=(char)c; put(b,u,1); }
    else if (c<0x800) { u[0]=(char)(0xc0|c>>6); u[1]=(char)(0x80|(c&63)); put(b,u,2); }
    else if (c<0x10000) { u[0]=(char)(0xe0|c>>12); u[1]=(char)(0x80|((c>>6)&63)); u[2]=(char)(0x80|(c&63)); put(b,u,3); }
    else { u[0]=(char)(0xf0|c>>18); u[1]=(char)(0x80|((c>>12)&63)); u[2]=(char)(0x80|((c>>6)&63)); u[3]=(char)(0x80|(c&63)); put(b,u,4); }
}
/* A string after its opening quote, into `out` (host memory, freed by the caller). */
static int parse_string(Parse *s, Buffer *out) {
    put(out,"",0);
    while (s->p<s->end && *s->p!='"') {
        unsigned char c=(unsigned char)*s->p++;
        if (c<0x20) return 0;
        if (c!='\\') { put(out,(const char *)&c,1); continue; }
        if (s->p>=s->end) return 0;
        char e=*s->p++;
        uint32_t code, low;
        switch (e) {
        case '"': case '\\': case '/': put(out,&e,1); break;
        case 'b': put(out,"\b",1); break;
        case 'f': put(out,"\f",1); break;
        case 'n': put(out,"\n",1); break;
        case 'r': put(out,"\r",1); break;
        case 't': put(out,"\t",1); break;
        case 'u':
            if (!hex4(s,&code)) return 0;
            if (code>=0xd800 && code<0xdc00 && s->end-s->p>=6 && s->p[0]=='\\' && s->p[1]=='u') {
                s->p+=2;
                if (!hex4(s,&low) || low<0xdc00 || low>0xdfff) return 0;
                code=0x10000+((code-0xd800)<<10)+(low-0xdc00);
            }
            put_utf8(out,code);
            break;
        default: return 0;
        }
    }
    if (s->p>=s->end) return 0;
    ++s->p;
    return !out->failed;
}
static int parse_value(Parse *s, Value *v);
static int parse_number(Parse *s, Value *v) {
    const char *start=s->p;
    int real=0;
    if (s->p<s->end && *s->p=='-') ++s->p;
    if (s->p>=s->end || *s->p<'0' || *s->p>'9') return 0;
    while (s->p<s->end && ((*s->p>='0' && *s->p<='9') || *s->p=='.' || *s->p=='e' || *s->p=='E' || *s->p=='+' || *s->p=='-')) {
        if (*s->p=='.' || *s->p=='e' || *s->p=='E') real=1;
        ++s->p;
    }
    char text[64];
    size_t n=(size_t)(s->p-start);
    if (n>=sizeof(text)) { if (!real) return 0; n=sizeof(text)-1; }
    memcpy(text,start,n); text[n]=0;
    char *end;
    if (!real) {
        if (text[0]=='-') { v->i=strtoll(text,&end,10); v->type=T_INT; }
        else {
            uint64_t u=strtoull(text,&end,10);
            v->u=u; v->type=u>INT64_MAX ? T_UINT : T_INT;
        }
    } else { v->d=strtod(text,&end); v->type=T_REAL; }
    return *end==0;
}
static int parse_value(Parse *s, Value *v) {
    memset(v,0,sizeof(*v));
    v->null_access=s->callback; v->context=s->context;
    skip_space(s);
    if (s->p>=s->end || ++s->depth>512) return 0;
    int ok=1;
    if (*s->p=='{') {
        ++s->p;
        v->type=T_OBJECT;
        if (!(v->o=runtime_low_alloc(sizeof(Object)))) return 0;
        memset(v->o,0,sizeof(Object));
        skip_space(s);
        if (s->p<s->end && *s->p=='}') ++s->p;
        else for (;;) {
            skip_space(s);
            Buffer key={0};
            if (s->p>=s->end || *s->p!='"') { ok=0; break; }
            ++s->p;
            if (!parse_string(s,&key)) { free(key.data); ok=0; break; }
            skip_space(s);
            if (s->p>=s->end || *s->p!=':') { free(key.data); ok=0; break; }
            ++s->p;
            Pair *items=grow(v->o->items,&v->o->cap,v->o->count,sizeof(Pair));
            if (!items) { free(key.data); ok=0; break; }
            v->o->items=items;
            Pair *pair=&items[v->o->count++];
            pair->key.text=key.size ? copy_text(key.data,key.size) : NULL;
            free(key.data);
            if (!parse_value(s,&pair->value)) { ok=0; break; }
            skip_space(s);
            if (s->p<s->end && *s->p==',') { ++s->p; continue; }
            if (s->p<s->end && *s->p=='}') { ++s->p; break; }
            ok=0; break;
        }
    } else if (*s->p=='[') {
        ++s->p;
        v->type=T_ARRAY;
        if (!(v->a=runtime_low_alloc(sizeof(Array)))) return 0;
        memset(v->a,0,sizeof(Array));
        skip_space(s);
        if (s->p<s->end && *s->p==']') ++s->p;
        else for (;;) {
            Value *items=grow(v->a->items,&v->a->cap,v->a->count,sizeof(Value));
            if (!items) { ok=0; break; }
            v->a->items=items;
            if (!parse_value(s,&items[v->a->count++])) { ok=0; break; }
            skip_space(s);
            if (s->p<s->end && *s->p==',') { ++s->p; continue; }
            if (s->p<s->end && *s->p==']') { ++s->p; break; }
            ok=0; break;
        }
    } else if (*s->p=='"') {
        ++s->p;
        Buffer text={0};
        ok=parse_string(s,&text);
        v->type=T_STRING;
        v->s.text=ok && text.size ? copy_text(text.data,text.size) : NULL;
        free(text.data);
    } else if (literal(s,"true")) { v->type=T_BOOL; v->b=true; }
    else if (literal(s,"false")) { v->type=T_BOOL; v->b=false; }
    else if (literal(s,"null")) v->type=T_NULL;
    else ok=parse_number(s,v);
    --s->depth;
    return ok;
}
/* static int Parser::parse(Value&, const char *text, size_t size): 0, or an error with `value`
 * null. The Value keeps its callback and gives it to every element. */
static ABI int32_t parser_parse(Value *value, const char *text, size_t size) {
    NullCallback callback=value->null_access;
    void *context=value->context;
    release(value);
    Parse s={text,text ? text+size : NULL,callback,context,0};
    int ok=text && parse_value(&s,value);
    skip_space(&s);
    if (ok && s.p==s.end) return 0;
    release(value);
    value->null_access=callback; value->context=context;
    if (runtime_verbose()) printf("Runtime: Json parse failed at byte %zu of %zu\n",text ? (size_t)(s.p-text) : 0,size);
    return ERR_PARSE;
}

/* Initializer(), ~Initializer(), initialize(const InitParameter*): nothing to set up, 0. */
static ABI void initializer_ctor(uint64_t *self) { *self=0; }
static ABI void initializer_dtor(uint64_t *self) { (void)self; }
static ABI int32_t initializer_initialize(uint64_t *self, const void *param) { (void)self; (void)param; return 0; }
/* MemAllocator base constructor/destructor (the game's subclass sets its own vtable after) and
 * the default notifyError(int error, size_t size, void *user): nothing. */
static ABI void allocator_dtor(void *self) { (void)self; }
static ABI void allocator_notify_error(void *self, int32_t error, size_t size, void *user) { (void)self; (void)error; (void)size; (void)user; }
static ABI void *allocator_allocate(void *self, size_t size, void *user) { (void)self; (void)user; return runtime_low_alloc(size); }
static ABI void allocator_deallocate(void *self, void *pointer, void *user) { (void)self; (void)user; runtime_low_free(pointer); }
static void *const allocator_vtable[]={(void *)allocator_dtor,(void *)allocator_dtor,(void *)allocator_allocate,
                                       (void *)allocator_deallocate,(void *)allocator_notify_error};
static ABI void allocator_ctor(void **self) { *self=(void *)allocator_vtable; }

static const RuntimeExport exports[]={
    {"_ZN3sce4Json11InitializerC1Ev",initializer_ctor}, {"_ZN3sce4Json11InitializerD1Ev",initializer_dtor},
    {"_ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE",initializer_initialize},
    {"_ZN3sce4Json12MemAllocatorC2Ev",allocator_ctor}, {"_ZN3sce4Json12MemAllocatorD2Ev",allocator_dtor},
    {"_ZN3sce4Json12MemAllocator11notifyErrorEimPv",allocator_notify_error},
    {"_ZN3sce4Json6StringC1Ev",string_ctor}, {"_ZN3sce4Json6StringC1EPKc",string_ctor_text},
    {"_ZN3sce4Json6StringD1Ev",string_dtor}, {"_ZNK3sce4Json6String5c_strEv",string_c_str},
    {"_ZNK3sce4Json6String4sizeEv",string_size}, {"_ZNK3sce4Json6String6lengthEv",string_size},
    {"_ZNK3sce4Json6String2atEm",string_at}, {"_ZNK3sce4Json6StringeqEPKc",string_equals},
    {"_ZN3sce4Json5ValueC1Ev",value_ctor}, {"_ZN3sce4Json5ValueD1Ev",value_dtor},
    {"_ZN3sce4Json5Value5clearEv",value_clear}, {"_ZN3sce4Json5Value3setENS0_9ValueTypeE",value_set_type},
    {"_ZN3sce4Json5Value3setEb",value_set_bool}, {"_ZN3sce4Json5Value3setEl",value_set_int},
    {"_ZN3sce4Json5Value3setEm",value_set_uint}, {"_ZN3sce4Json5Value3setEd",value_set_real},
    {"_ZN3sce4Json5Value3setERKNS0_6StringE",value_set_string},
    {"_ZN3sce4Json5Value21setNullAccessCallBackEPFRKS1_NS0_9ValueTypeEPS2_PvES6_",value_set_null_access},
    {"_ZNK3sce4Json5Value10getBooleanEv",value_get_bool}, {"_ZNK3sce4Json5Value10getIntegerEv",value_get_int},
    {"_ZNK3sce4Json5Value9getStringEv",value_get_string}, {"_ZNK3sce4Json5ValuecvbEv",value_valid},
    {"_ZNK3sce4Json5Value5countEv",value_count}, {"_ZNK3sce4Json5ValueixEPKc",value_key},
    {"_ZNK3sce4Json5ValueixEm",value_index}, {"_ZNK3sce4Json5Value8toStringERNS0_6StringE",value_to_string},
    {"_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm",parser_parse},
};
uintptr_t runtime_json_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
