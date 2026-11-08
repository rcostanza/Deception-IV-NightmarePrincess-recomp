/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceJson2 (src/runtime_json.c): parsing, reads, the null access callback and toString, as the
 * game uses them on PSN replies. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/runtime_json.c"

void *runtime_low_alloc(size_t size) { return malloc(size); }
void runtime_low_free(void *pointer) { free(pointer); }
int runtime_verbose(void) { return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* The game's callback: a default Value per expected type (bool, int, uint, real, string), else null. */
static Value defaults[6];
static ABI const void *defaults_for(int32_t type, const void *parent, void *context) {
    (void)parent;
    Value *d=context;
    return type>=T_BOOL && type<=T_STRING ? &d[type-1] : &d[5];
}

static int parse(Value *v, const char *text) { return parser_parse(v,text,strlen(text)); }
static const char *text_of(const Value *v, String *s) { value_to_string(v,s); return string_c_str(s); }

static void test_reads(void) {
    Value root; value_ctor(&root);
    assert(!parse(&root," {\"onlineStatus\":\"online\",\"n\":-42,\"big\":18446744073709551615,"
                        "\"ok\":true,\"list\":[1,2.5,\"x\\u00e9\\ud83d\\ude00\",null],\"o\":{}} "));
    assert(value_valid(&root) && value_count(&root)==6);
    String s; string_ctor(&s);
    assert(!strcmp(text_of(value_key(&root,"onlineStatus"),&s),"online"));
    assert(string_equals(&s,"online") && string_size(&s)==6 && *string_at(&s,1)=='n' && !*string_at(&s,6));
    assert(value_get_int(value_key(&root,"n"))==-42);
    assert(value_key(&root,"big")->type==T_UINT);
    assert(value_get_bool(value_key(&root,"ok")));
    const Value *list=value_key(&root,"list");
    assert(value_count(list)==4 && value_get_int(value_index(list,0))==1);
    assert(!strcmp(string_c_str(value_get_string(value_index(list,2))),"x\xc3\xa9\xf0\x9f\x98\x80"));
    assert(!value_valid(value_index(list,3)) && !value_valid(value_index(list,9)));
    assert(!strcmp(text_of(list,&s),"[1,2.5,\"x\xc3\xa9\xf0\x9f\x98\x80\",null]"));
    assert(!strcmp(text_of(value_key(&root,"o"),&s),"{}"));
    string_dtor(&s);
    value_dtor(&root);
}

static void test_null_access(void) {
    for (int i=0;i<6;++i) value_ctor(&defaults[i]);
    value_set_bool(&defaults[0],false);
    value_set_int(&defaults[1],0);
    String invalid; string_ctor_text(&invalid,"invalid string");
    value_set_string(&defaults[4],&invalid);
    string_dtor(&invalid);
    Value root; value_ctor(&root);
    value_set_null_access(&root,defaults_for,defaults);
    value_clear(&root);
    assert(!parse(&root,"{\"a\":{\"b\":7}}"));
    assert(value_key(&root,"missing")==&defaults[5]);
    assert(value_key(value_key(&root,"a"),"nope")==&defaults[5]); /* children inherit the callback */
    assert(value_get_int(value_key(value_key(&root,"a"),"b"))==7);
    assert(!strcmp(string_c_str(value_get_string(value_key(&root,"a"))),"invalid string"));
    assert(!value_get_bool(value_key(&root,"missing")));
    value_dtor(&root);
    for (int i=0;i<6;++i) value_dtor(&defaults[i]);
}

static void test_errors(void) {
    static const char *const bad[]={"","{","{\"a\"}","[1,]","tru","\"\\x\"","{} x","01x","\"a\x01\""};
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        Value v; value_ctor(&v);
        assert(parse(&v,bad[i])==ERR_PARSE && !value_valid(&v));
        value_dtor(&v);
    }
    Value v; value_ctor(&v);
    assert(!parse(&v,"null") && !value_valid(&v));
    value_dtor(&v);
}

int main(void) {
    test_reads();
    test_null_access();
    test_errors();
    puts("json test: ok");
    return 0;
}
