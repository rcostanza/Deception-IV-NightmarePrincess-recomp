/* System services contracts: dialogs (progress bars run until closed), the save list, trophies. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_services.c"
#include <assert.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define test_mkdir(d) _mkdir(d)
#define test_rmdir(d) _rmdir(d)
#define test_getpid() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define test_mkdir(d) mkdir(d, 0755)
#define test_rmdir(d) rmdir(d)
#define test_getpid() getpid()
#endif

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
int deception_gpu_text_input_begin(const char *text, const char *prompt) { (void)text; (void)prompt; return 0; }
int deception_gpu_text_input_poll(char *text, uint64_t size) { (void)text; (void)size; return 0; }
void deception_gpu_common_dialog_initialized(void) {}
/* Not the repository's user/ (run.py's save folder: a real trophies.dat there failed the checks). */
#define MOCK_USER "out/test_services_no_user"
static char mock_user_dir[512]=MOCK_USER;
const char *runtime_file_user_dir(void) { return mock_user_dir; }
static char last_toast_title[128], last_toast_detail[256];
void deception_gpu_toast_trophy(int id, const char *title, const char *detail, const unsigned char *png, uint64_t png_size) {
    (void)id; (void)png; (void)png_size;
    if (title) snprintf(last_toast_title, sizeof(last_toast_title), "%s", title);
    else last_toast_title[0] = 0;
    if (detail) snprintf(last_toast_detail, sizeof(last_toast_detail), "%s", detail);
    else last_toast_detail[0] = 0;
}
/* No trophy00.trp in the package. */
int64_t runtime_file_open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; return -1; }
int64_t runtime_file_read(int fd, void *buffer, uint64_t size) { (void)fd; (void)buffer; (void)size; return -1; }
int64_t runtime_file_close(int fd) { (void)fd; return 0; }
static int choice_next=-1;
static int choice_count, choice_focus, choice_available=1;
static int preview_calls;
static uint64_t preview_size;
static unsigned char preview_bytes[8];
static char shown_title[3000], shown_items[33][3000];
static unsigned char save_param[RUNTIME_SAVE_PARAM_SIZE];
static int save_param_available;
int deception_gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus) {
    (void)images;
    choice_count=count; choice_focus=focus; choice_next=-1;
    snprintf(shown_title,sizeof(shown_title),"%s",title);
    for (int i=0;i<count && i<33;++i) snprintf(shown_items[i],sizeof(shown_items[i]),"%s",items[i]);
    return choice_available && count>0;
}
int deception_gpu_choice_poll(void) { return choice_next; }
int deception_gpu_choice_confirm(const char *title, const char *const *items, int count, int focus,
                                 const unsigned char *png, uint64_t size, const char *caption, const char *unavailable) {
    assert(caption && *caption && unavailable && *unavailable);
    ++preview_calls; preview_size=size;
    memset(preview_bytes,0,sizeof(preview_bytes));
    if (png && size) memcpy(preview_bytes,png,size<sizeof(preview_bytes) ? (size_t)size : sizeof(preview_bytes));
    return deception_gpu_choice_begin(title,items,NULL,count,focus);
}
int runtime_savedata_param(int32_t user, const char *dir, void *param) {
    (void)user; (void)dir;
    if (!save_param_available) return -1;
    memcpy(param,save_param,sizeof(save_param)); return 0;
}
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size) { (void)user; (void)dir; (void)out; (void)size; return -1; }
static struct {
    char dir[32];
    int64_t mtime;
} test_save_mtimes[16];
static int test_save_mtimes_count;
int64_t runtime_savedata_mtime(int32_t user, const char *dir) {
    (void)user;
    for (int i=0;i<test_save_mtimes_count;++i) {
        if (!strcmp(test_save_mtimes[i].dir,dir)) return test_save_mtimes[i].mtime;
    }
    return 0;
}

int main(void) {
    unsigned char param[128]={0}, result[64]={0}; /* the game zeroes the pointers it does not want */
    uint32_t value;

    /* NpManager requests: an async one completes signed out and is polled. */
    int32_t np_result=0;
    const uint64_t async_param[3]={24,0,0}, bad_param[3]={16,0,0};
    assert(np_create_async(bad_param)==NP_INVALID_SIZE);
    int32_t async=np_create_async(async_param);
    int8_t age=0; unsigned char info[8];
    assert(np_poll_request(async,&np_result)==NP_INVALID_ID);
    assert(np_parental(async,1,&age,info)==0 && np_poll_request(async,&np_result)==0 && np_result==NP_SIGNED_OUT);
    assert(np_abort_request(async)==0 && np_delete_request(async)==0 && np_delete_request(async)==NP_REQUEST_NOT_FOUND);
    uint64_t account=1;
    assert(np_account_id(1,&account)==0 && account==NP_ACCOUNT_ID && np_account_id(-1,&account)==NP_INVALID_ARGUMENT);
    assert(np_account_id(2,&account)==NP_USER_NOT_FOUND && account==0);
    /* IME keyboard: one open, never connected. */
    ImeKeyboardParam kb={0};
    assert(ime_update(NULL)==IME_NOT_OPENED && keyboard_open_call(1,&kb)==IME_INVALID_HANDLER);
    kb.handler=(void *)1; kb.option=0x40;
    assert(keyboard_open_call(1,&kb)==IME_INVALID_OPTION);
    kb.option=1; kb.reserved2[3]=1;
    assert(keyboard_open_call(1,&kb)==IME_INVALID_RESERVED);
    kb.reserved2[3]=0;
    assert(keyboard_open_call(1,&kb)==0 && keyboard_open_call(1,&kb)==IME_BUSY && ime_update(NULL)==0);
    assert(keyboard_close(1)==0 && keyboard_close(1)==IME_NOT_OPENED);
    int32_t color=-1;
    assert(user_color(USER_ID,&color)==0 && color==0 && user_color(7,&color)==USER_INVALID_ARGUMENT);

    /* MsgDialog: a user message finishes with OK; a progress bar runs until closed. */
    assert(msg_init()==0);
    memset(param+56,0,4); param[56]=1;
    assert(msg_open(param)==0 && msg_status()==DIALOG_FINISHED);
    assert(msg_result(result)==0);
    memcpy(&value,result+4,4); assert(value==1);
    param[56]=2;
    assert(msg_open(param)==0 && msg_status()==DIALOG_RUNNING);
    assert(msg_result(result)==DIALOG_NOT_FINISHED);
    assert(msg_close()==0 && msg_status()==DIALOG_FINISHED && msg_close()==DIALOG_NOT_RUNNING);
    assert(msg_result(NULL)==DIALOG_ARG_NULL);
    assert(msg_term()==0 && msg_open(NULL)==DIALOG_ARG_NULL);

    /* SaveDataDialog: mode at offset 52 (5 = progress bar); result after the mode field. */
    assert(save_init()==0);
    param[52]=5;
    assert(save_open(param)==0 && save_status()==DIALOG_RUNNING);
    assert(save_close()==0 && save_result(result)==0);
    memcpy(&value,result+8,4); assert(value==0); /* closed by the game: no button */
    param[52]=2;
    assert(save_open(param)==0 && save_status()==DIALOG_FINISHED && save_result(result)==0);
    memcpy(&value,result+8,4); assert(value==1);
    assert(save_term()==0);

    /* Save list: the player's choice comes back as the save directory. */
    char names[2][32]={"SAVE0001","SAVE0002"}, chosen[32];
    unsigned char items[64]={0}, list[128]={0}, out[64]={0};
    const char *names_ptr=names[0]; uint32_t name_count=2;
    struct { const char *title; const unsigned char *icon; uint64_t size; } new_item={"New",NULL,0};
    const void *new_title=&new_item;
    memcpy(items+16,&names_ptr,8); memcpy(items+24,&name_count,4); memcpy(items+32,&new_title,8);
    unsigned char *items_ptr=items; char *chosen_ptr=chosen;
    list[52]=1; list[56]=2; memcpy(list+72,&items_ptr,8);
    memcpy(out+16,&chosen_ptr,8);
    assert(save_init()==0 && save_open(list)==0 && save_status()==DIALOG_RUNNING);
    choice_next=2; /* entries: New save, SAVE0001, SAVE0002 */
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && !strcmp(chosen,"SAVE0002"));
    memcpy(&value,out+4,4); assert(value==0);
    memcpy(&value,out+8,4); assert(value==1);
    assert(save_open(list)==0);
    choice_next=0;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && chosen[0]==0); /* new save */
    assert(save_open(list)==0);
    choice_next=-2;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    memcpy(&value,out+4,4); assert(value==SAVE_RESULT_CANCELED);

    /* Overwriting must not return a slot until the second, explicit confirmation. */
    list[56]=1;
    assert(save_open(list)==0);
    choice_next=2;
    assert(save_status()==DIALOG_RUNNING);
    assert(save_result(out)==DIALOG_NOT_FINISHED);
    assert(choice_count==2 && choice_focus==1 && !strcmp(shown_items[1],"Cancel"));
    assert(preview_calls==1 && preview_size==0);
    assert(!strstr(shown_title,"SAVE0002"));
    choice_next=0;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && !strcmp(chosen,"SAVE0002"));

    /* Both the Cancel option and Back return to the same slot without finishing. */
    for (int cancel=1;cancel>=-2;cancel-=3) {
        assert(save_open(list)==0);
        choice_next=2; assert(save_status()==DIALOG_RUNNING);
        choice_next=cancel; assert(save_status()==DIALOG_RUNNING);
        assert(choice_count==3 && choice_focus==2);
        choice_next=-2;
        assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
        memcpy(&value,out+4,4); assert(value==SAVE_RESULT_CANCELED);
    }

    /* A new slot and a non-list progress dialog keep their existing behavior. */
    assert(save_open(list)==0);
    choice_next=0;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && !chosen[0]);
    list[52]=5;
    assert(save_open(list)==0 && save_status()==DIALOG_RUNNING);
    assert(save_close()==0 && save_status()==DIALOG_FINISHED);
    list[52]=1;

    /* No New save row: entry zero is an existing slot and still needs confirmation. */
    memset(items+32,0,8);
    assert(save_open(list)==0);
    choice_next=0; assert(save_status()==DIALOG_RUNNING);
    choice_next=0;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && !strcmp(chosen,"SAVE0001"));
    memcpy(items+32,&new_title,8);

    /* A failed confirmation window must never grant permission to overwrite. */
    assert(save_open(list)==0);
    choice_available=0; choice_next=1;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    memcpy(&value,out+4,4); assert(value==SAVE_RESULT_CANCELED);
    choice_available=1;

    /* The new item's image is copied at open, never taken from the destination slot. */
    const unsigned char signature[8]={137,80,78,71,13,10,26,10};
    unsigned char supplied[8]; memcpy(supplied,signature,sizeof(supplied));
    new_item.icon=supplied; new_item.size=sizeof(supplied);
    assert(save_open(list)==0);
    memset(supplied,0,sizeof(supplied));
    choice_next=2; assert(save_status()==DIALOG_RUNNING);
    assert(preview_size==sizeof(signature) && !memcmp(preview_bytes,signature,sizeof(signature)));
    choice_next=1; assert(save_status()==DIALOG_RUNNING);
    choice_next=2; assert(save_status()==DIALOG_RUNNING);
    assert(preview_size==sizeof(signature) && !memcmp(preview_bytes,signature,sizeof(signature)));
    choice_next=1; assert(save_status()==DIALOG_RUNNING);
    choice_next=-2; assert(save_status()==DIALOG_FINISHED);
    new_item.size=UINT64_MAX;
    assert(save_open(list)==0);
    choice_next=2; assert(save_status()==DIALOG_RUNNING && preview_size==0);
    choice_next=1; assert(save_status()==DIALOG_RUNNING);
    choice_next=-2; assert(save_status()==DIALOG_FINISHED);
    new_item.icon=NULL; new_item.size=0;

    memcpy(items+32,&new_title,8);

    /* A save's subtitle and detail appear in its row; the overwrite question shows the subtitle
     * in place of the title. Detail text in Latin-1 is shown as UTF-8. */
    save_param_available=1;
    strcpy((char *)save_param,"DECEPTION");
    strcpy((char *)save_param+128,"Arcade, Veteran");
    strcpy((char *)save_param+256,"Pontua\xe7\xe3o 1234567");
    int64_t timestamp=1720000000; memcpy(save_param+1288,&timestamp,8);
    assert(save_open(list)==0);
    assert(strstr(shown_items[1],"DECEPTION") && strstr(shown_items[1],"Arcade, Veteran"));
    assert(strstr(shown_items[1],"Pontua\xc3\xa7\xc3\xa3o 1234567"));
    choice_next=1; assert(save_status()==DIALOG_RUNNING);
    assert(strstr(shown_title,"Arcade, Veteran") && strstr(shown_title,"1234567"));
    assert(!strstr(shown_title,"DECEPTION") && !strstr(shown_title,"SAVE0001"));
    choice_next=0; assert(save_status()==DIALOG_FINISHED);

    /* Valid UTF-8 stays intact; absent and unterminated metadata are bounded. */
    strcpy((char *)save_param+256,"N\xc3\xadvel 5");
    assert(save_open(list)==0 && strstr(shown_items[1],"N\xc3\xadvel 5"));
    memset(save_param+128,0,1152);
    /* Without them the row is the title and the date, two lines. */
    assert(save_open(list)==0 && strchr(shown_items[1],'\n') && !strchr(strchr(shown_items[1],'\n')+1,'\n'));
    assert(!strstr(shown_items[1],"unavailable"));
    memset(save_param,'X',sizeof(save_param));
    assert(save_open(list)==0 && strlen(shown_items[1])<sizeof(shown_items[1])-1);
    save_param_available=0;
    assert(save_open(list)==0 && strstr(shown_items[1],"SAVE0001") && strstr(shown_items[1],"unavailable"));
    assert(save_term()==0);

    /* ---- Save list ordering (newest first, overwriting) ---- */
    assert(save_init()==0);
    char sort_names[3][32]={"SAVE0001","SAVE0002","SAVE0003"};
    const char *sort_names_ptr=sort_names[0];
    uint32_t sort_count=3;
    unsigned char sort_items[64]={0}, sort_list[128]={0};
    memcpy(sort_items+16,&sort_names_ptr,8);
    memcpy(sort_items+24,&sort_count,4);
    unsigned char *sort_items_ptr=sort_items;
    sort_list[52]=1; /* mode 1: list */
    sort_list[56]=2; /* type 2: Load game */
    memcpy(sort_list+72,&sort_items_ptr,8);

    /* 1. Initial save ordering: newest first. */
    test_save_mtimes_count=3;
    strcpy(test_save_mtimes[0].dir,"SAVE0001"); test_save_mtimes[0].mtime=100;
    strcpy(test_save_mtimes[1].dir,"SAVE0002"); test_save_mtimes[1].mtime=300; /* newest */
    strcpy(test_save_mtimes[2].dir,"SAVE0003"); test_save_mtimes[2].mtime=200;
    assert(save_open(sort_list)==0 && save_status()==DIALOG_RUNNING);
    assert(choice_count==3);
    assert(choice_focus==0);
    assert(strstr(shown_items[0],"SAVE0002")!=NULL);
    assert(strstr(shown_items[1],"SAVE0003")!=NULL);
    assert(strstr(shown_items[2],"SAVE0001")!=NULL);

    /* 2. Correct slot loaded after reordering. */
    choice_next=0; /* pick top save (SAVE0002) */
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    assert(!strcmp(chosen,"SAVE0002"));

    assert(save_open(sort_list)==0);
    choice_next=1; /* pick second save (SAVE0003) */
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    assert(!strcmp(chosen,"SAVE0003"));

    assert(save_open(sort_list)==0);
    choice_next=2; /* pick third save (SAVE0001) */
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    assert(!strcmp(chosen,"SAVE0001"));

    /* 3. Overwriting an older slot moves it to the top on list refresh. */
    test_save_mtimes[0].mtime=400; /* SAVE0001 overwritten, now newest */
    assert(save_open(sort_list)==0);
    assert(strstr(shown_items[0],"SAVE0001")!=NULL);
    assert(strstr(shown_items[1],"SAVE0002")!=NULL);
    assert(strstr(shown_items[2],"SAVE0003")!=NULL);
    choice_next=0; assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    assert(!strcmp(chosen,"SAVE0001"));

    /* 4. Identical timestamps: deterministic alphabetical tie-breaking. */
    test_save_mtimes[0].mtime=200;
    test_save_mtimes[1].mtime=500;
    test_save_mtimes[2].mtime=500; /* SAVE0002 and SAVE0003 tie at 500 */
    assert(save_open(sort_list)==0);
    assert(strstr(shown_items[0],"SAVE0002")!=NULL);
    assert(strstr(shown_items[1],"SAVE0003")!=NULL);
    assert(strstr(shown_items[2],"SAVE0001")!=NULL);
    choice_next=-2; assert(save_status()==DIALOG_FINISHED);

    /* 5. Missing / invalid timestamps placed at the bottom, sorted by name. */
    test_save_mtimes[0].mtime=0;   /* missing */
    test_save_mtimes[1].mtime=250; /* valid */
    test_save_mtimes[2].mtime=-1;  /* invalid */
    assert(save_open(sort_list)==0);
    assert(strstr(shown_items[0],"SAVE0002")!=NULL);
    assert(strstr(shown_items[1],"SAVE0001")!=NULL);
    assert(strstr(shown_items[2],"SAVE0003")!=NULL);
    choice_next=-2; assert(save_status()==DIALOG_FINISHED);

    /* 5b. All missing or negative timestamps (0, -10, -100): all treated as invalid category, sorted by name. */
    test_save_mtimes[0].mtime=-100;
    test_save_mtimes[1].mtime=-10;
    test_save_mtimes[2].mtime=0;
    assert(save_open(sort_list)==0);
    assert(strstr(shown_items[0],"SAVE0001")!=NULL);
    assert(strstr(shown_items[1],"SAVE0002")!=NULL);
    assert(strstr(shown_items[2],"SAVE0003")!=NULL);
    choice_next=-2; assert(save_status()==DIALOG_FINISHED);

    /* 5c. Negative (-50) vs zero (0) vs valid (100): valid first, invalid tie-break by name. */
    test_save_mtimes[0].mtime=-50;  /* SAVE0001 */
    test_save_mtimes[1].mtime=100;  /* SAVE0002 (valid) */
    test_save_mtimes[2].mtime=0;    /* SAVE0003 */
    assert(save_open(sort_list)==0);
    assert(strstr(shown_items[0],"SAVE0002")!=NULL);
    assert(strstr(shown_items[1],"SAVE0001")!=NULL);
    assert(strstr(shown_items[2],"SAVE0003")!=NULL);
    choice_next=-2; assert(save_status()==DIALOG_FINISHED);

    /* 5d. Date display in save_label matches save_mtime. */
    save_param_available=1;
    test_save_mtimes[0].mtime=1720000000; /* 2024-07-03 */
    assert(save_open(sort_list)==0);
    assert(strstr(shown_items[1],"2024-07-03")!=NULL);
    choice_next=-2; assert(save_status()==DIALOG_FINISHED);
    save_param_available=0;

    /* 6. The game's own save among others: placed by its time like any save. */
    strcpy(sort_names[0],"savedata");
    strcpy(sort_names[1],"SAVE0001");
    strcpy(sort_names[2],"SAVE0002");
    /* 6a. savedata is newest: appears at top. */
    strcpy(test_save_mtimes[0].dir,"savedata"); test_save_mtimes[0].mtime=600;
    strcpy(test_save_mtimes[1].dir,"SAVE0001"); test_save_mtimes[1].mtime=500;
    strcpy(test_save_mtimes[2].dir,"SAVE0002"); test_save_mtimes[2].mtime=400;
    assert(save_open(sort_list)==0);
    assert(strstr(shown_items[0],"savedata")!=NULL);
    assert(strstr(shown_items[1],"SAVE0001")!=NULL);
    assert(strstr(shown_items[2],"SAVE0002")!=NULL);
    choice_next=0; assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    assert(!strcmp(chosen,"savedata"));

    /* 6b. Manual save newer than savedata: manual save at top, savedata second. */
    test_save_mtimes[1].mtime=700; /* SAVE0001 is newer than savedata */
    assert(save_open(sort_list)==0);
    assert(strstr(shown_items[0],"SAVE0001")!=NULL);
    assert(strstr(shown_items[1],"savedata")!=NULL);
    assert(strstr(shown_items[2],"SAVE0002")!=NULL);
    choice_next=0; assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    assert(!strcmp(chosen,"SAVE0001"));
    assert(save_open(sort_list)==0);
    choice_next=1; assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    assert(!strcmp(chosen,"savedata"));

    /* 7. Save/delete navigation consistency. */
    /* 7a. Save mode (type 1) with new_item: row 0 is 'New save', followed by sorted saves. */
    sort_list[56]=1; /* type 1: Save game */
    memcpy(sort_items+32,&new_title,8);
    assert(save_open(sort_list)==0);
    assert(choice_count==4);
    assert(choice_focus==0);
    assert(strstr(shown_items[0],"New")!=NULL || strstr(shown_items[0],"Novo")!=NULL);
    assert(strstr(shown_items[1],"SAVE0001")!=NULL);
    assert(strstr(shown_items[2],"savedata")!=NULL);
    assert(strstr(shown_items[3],"SAVE0002")!=NULL);
    /* Selecting 'New save' (row 0) produces empty name. */
    choice_next=0; assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    assert(chosen[0]=='\0');
    /* Selecting an existing save (row 1: SAVE0001) triggers overwrite confirmation. */
    assert(save_open(sort_list)==0);
    choice_next=1; assert(save_status()==DIALOG_RUNNING); /* prompt displayed */
    choice_next=1; assert(save_status()==DIALOG_RUNNING); /* cancel overwrite */
    assert(choice_focus==1); /* cursor preserved on row 1 */
    choice_next=1; assert(save_status()==DIALOG_RUNNING); /* prompt displayed again */
    choice_next=0; assert(save_status()==DIALOG_FINISHED && save_result(out)==0); /* confirm overwrite */
    assert(!strcmp(chosen,"SAVE0001"));

    /* 7b. Save mode without new_item (all slots full): existing saves start at row 0. */
    memset(sort_items+32,0,8);
    assert(save_open(sort_list)==0);
    assert(choice_count==3);
    assert(strstr(shown_items[0],"SAVE0001")!=NULL);
    choice_next=-2; assert(save_status()==DIALOG_FINISHED);

    /* 7c. Delete mode (type 3): existing saves start at row 0 sorted newest first. */
    sort_list[56]=3; /* type 3: Delete save */
    assert(save_open(sort_list)==0);
    assert(choice_count==3);
    assert(strstr(shown_items[0],"SAVE0001")!=NULL);
    assert(strstr(shown_items[1],"savedata")!=NULL);
    assert(strstr(shown_items[2],"SAVE0002")!=NULL);
    choice_next=0; assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    assert(!strcmp(chosen,"SAVE0001"));

    assert(save_term()==0);
    test_save_mtimes_count=0;


    /* Trophies: nothing unlocked; without trophy00.trp the count is still the game's 65. */
    uint32_t flags[4]={~0u,~0u,~0u,~0u}, count=7;
    assert(trophy_unlock_state(1,1,flags,&count)==0 && !flags[0] && !flags[3] && count==TROPHY_GAME_COUNT);
    assert(trophy_unlock_state(1,1,NULL,&count)==TROPHY_INVALID && trophy_release(0)==TROPHY_INVALID);
    /* No socket ever exists. */
    unsigned char sock_info[64];
    assert(net_sock_info(-1,NULL,0,0)==0 && net_sock_info(-1,sock_info,4,0)==0);
    assert(net_sock_info(3,sock_info,4,0)==NET_EBADF && net_errno==9);
    assert(net_sock_info(3,NULL,0,0)==NET_EINVAL && net_sock_info(-1,sock_info,0,0)==NET_EINVAL);
    /* The Store dialog finishes with nothing bought. */
    unsigned char commerce[16];
    assert(commerce_init()==0 && commerce_result(commerce)==DIALOG_NOT_FINISHED);
    assert(commerce_open(NULL)==0 && commerce_result(NULL)==DIALOG_ARG_NULL && commerce_result(commerce)==0);
    int32_t commerce_result_code=-1;
    memcpy(&commerce_result_code,commerce,4); assert(commerce_result_code==1 && commerce[4]==0 && commerce_term()==0);

    /* Trophies: directory resolution, text loading, unlock notifications and persistence across restart. */
    assert(!strcmp(trophy_dir(),MOCK_USER));
    const char *tmp=getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp=getenv("TEMP");
    if (!tmp || !tmp[0]) tmp=getenv("TMP");
    if (!tmp || !tmp[0]) tmp="/tmp";

    char test_user[256], test_txt[512], test_dat[512];
    snprintf(test_user,sizeof(test_user),"%s/deception_test_user_%d",tmp,(int)test_getpid());
    snprintf(test_txt,sizeof(test_txt),"%s/trophy_names.txt",test_user);
    snprintf(test_dat,sizeof(test_dat),"%s/trophies.dat",test_user);

    snprintf(mock_user_dir,sizeof(mock_user_dir),"%s",test_user);
    assert(!strcmp(trophy_dir(),test_user));
#ifdef _WIN32
    _putenv("DECEPTION_USER_DIR=out/test_trophy_env");
#else
    setenv("DECEPTION_USER_DIR","out/test_trophy_env",1);
#endif
    assert(!strcmp(trophy_dir(),"out/test_trophy_env"));
#ifdef _WIN32
    _putenv("DECEPTION_USER_DIR=");
#else
    unsetenv("DECEPTION_USER_DIR");
#endif
    assert(!strcmp(trophy_dir(),test_user));

    /* Clean up any leftovers before setup */
    remove(test_dat);
    remove(test_txt);
    test_rmdir(test_user);

    /* Set up isolated test user dir with custom trophy texts */
    test_mkdir(test_user);
    FILE *ft=fopen(test_txt,"wb");
    assert(ft!=NULL);
    fputs("@heading=Custom Trophies\n"
          "0=Last Human | Unlock every trophy\n"
          "1=Saviour | Rescue a human\n",ft);
    fclose(ft);
    remove(test_dat);

    /* Reset in-memory trophy state and load from mock_user_dir */
    trophy_ready=0;
    memset(trophy_flags,0,sizeof(trophy_flags));
    memset(trophy_names,0,sizeof(trophy_names));
    memset(trophy_details,0,sizeof(trophy_details));
    trophy_load_once();

    /* Verified: names are resolved from mock_user_dir/trophy_names.txt */
    assert(!strcmp(trophy_names[0],"Last Human"));
    assert(!strcmp(trophy_names[1],"Saviour"));
    assert(!strcmp(trophy_details[1],"Rescue a human"));

    /* Verified: unlocking trophy 1 shows decoded name in notification and persists to disk */
    last_toast_title[0]=0;
    assert(trophy_unlock(1,1,1,NULL)==0);
    assert(!strcmp(last_toast_title,"Saviour"));
    assert(!strcmp(last_toast_detail,"Rescue a human"));
    assert((trophy_flags[0]&(1u<<1))!=0);
    /* Again: the code the game's unlock callback accepts as done (SCE_NP_TROPHY_ERROR_TROPHY_ALREADY_UNLOCKED). */
    assert(trophy_unlock(1,1,1,NULL)==(int32_t)0x8055160C);

    /* Verified: persistence across application restart */
    trophy_ready=0;
    memset(trophy_flags,0,sizeof(trophy_flags));
    trophy_load_once();
    assert((trophy_flags[0]&(1u<<1))!=0);

    /* Clean up test files */
    remove(test_dat);
    remove(test_txt);
    test_rmdir(test_user);
    snprintf(mock_user_dir,sizeof(mock_user_dir),MOCK_USER);
    /* Trophy names: a UTF-8 BOM and comments are skipped, "ID=Name | Description" is split. */
    {
        FILE *texts=fopen("trophy_names.txt","wb");
        assert(texts);
        fputs("\xEF\xBB\xBF# comment\n3=Sr. M\xC3\xA3o | Ache a sala\n",texts);
        fclose(texts);
#ifdef _WIN32
        _putenv("DECEPTION_USER_DIR=.");
#else
        setenv("DECEPTION_USER_DIR",".",1);
#endif
        trophy_load_names();
        assert(!strcmp(trophy_names[3],"Sr. M\xC3\xA3o") && !strcmp(trophy_details[3],"Ache a sala"));
        remove("trophy_names.txt");
#ifdef _WIN32
        _putenv("DECEPTION_USER_DIR=");
#else
        unsetenv("DECEPTION_USER_DIR");
#endif
    }

    float ratio=0; assert(safe_area(&ratio)==0 && ratio==1.0f && safe_area(NULL)==SYSTEM_PARAMETER);

    /* System parameter 1: language ID from DECEPTION_LANGUAGE (default 1 = English US, 16 = Polish) */
    int32_t lang=0;
    assert(system_param(1,&lang)==0 && lang==1);
#ifdef _WIN32
    _putenv("DECEPTION_LANGUAGE=16");
#else
    setenv("DECEPTION_LANGUAGE","16",1);
#endif
    assert(system_param(1,&lang)==0 && lang==16);
#ifdef _WIN32
    _putenv("DECEPTION_LANGUAGE=");
#else
    unsetenv("DECEPTION_LANGUAGE");
#endif
    assert(system_param(1,&lang)==0 && lang==1);

    /* System parameter 1000: the enter button is Cross. */
    int32_t enter_btn=0;
    assert(system_param(1000,&enter_btn)==0 && enter_btn==1);

    puts("services test: ok");
    return 0;
}
