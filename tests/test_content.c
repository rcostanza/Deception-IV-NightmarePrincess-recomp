/* Add-ons: the save folder's addcont/ and an extra DLC folder (shadPS4's addcont, with its title ID
 * folder, or a plain folder of add-ons) are read together; the game asks for each label it knows
 * (sceAppContentGetAddcontInfo), and a label in neither folder has no entitlement. /temp0 is made
 * and mounted on the first sceAppContentTemporaryDataMount2, not at start. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_content.c"
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static char user_dir[256];
const char *runtime_file_user_dir(void) { return user_dir; }
static char mounted_guest[32], mounted_host[700];
static int mount_calls;
int runtime_file_mount(const char *guest,const char *host) {
    snprintf(mounted_guest,sizeof(mounted_guest),"%s",guest); snprintf(mounted_host,sizeof(mounted_host),"%s",host);
    ++mount_calls; return 0;
}

static void folder(const char *path) { assert(!mkdir(path,0755) || errno==EEXIST); }
static void file(const char *path) { FILE *f=fopen(path,"wb"); assert(f); fputs("x",f); fclose(f); }

/* Whether the game sees `label` as installed: the info names it, with status 4 (installed). */
static int installed(const char *label) {
    char want[17]={0}; unsigned char info[24];
    snprintf(want,sizeof(want),"%s",label);
    int32_t r=addon_info(0,want,info);
    if (r) { assert(r==NO_ENTITLEMENT); return 0; }
    uint32_t status; memcpy(&status,info+20,4);
    assert(!strcmp((const char *)info,label) && status==4);
    return 1;
}
static void rescan(void) { addon_count=0; addons_scanned=0; }

int main(void) {
    const char *tmp=getenv("TMP");
    if (!tmp) tmp=getenv("TEMP");
    if (!tmp) tmp="/tmp";
    char root[256], path[700];
    snprintf(root,sizeof(root),"%s/deception-content-test-%d",tmp,(int)getpid());
    folder(root);
    snprintf(user_dir,sizeof(user_dir),"%s/save",root); folder(user_dir);
    snprintf(path,sizeof(path),"%s/addcont",user_dir); folder(path);
    snprintf(path,sizeof(path),"%s/addcont/SAVEONLY",user_dir); folder(path);
    snprintf(path,sizeof(path),"%s/addcont/SHARED",user_dir); folder(path);
    /* shadPS4's layout: <addcont>/<title ID>/<label>/ */
    snprintf(path,sizeof(path),"%s/shadps4",root); folder(path);
    snprintf(path,sizeof(path),"%s/shadps4/CUSA00038",root); folder(path);
    snprintf(path,sizeof(path),"%s/shadps4/CUSA00038/EXTRA",root); folder(path);
    snprintf(path,sizeof(path),"%s/shadps4/CUSA00038/SHARED",root); folder(path);
    snprintf(path,sizeof(path),"%s/shadps4/CUSA00038/notes.txt",root); file(path);
    snprintf(path,sizeof(path),"%s/shadps4/CUSA00038/LABEL_LONGER_THAN_16",root); folder(path);

    uint32_t profile[5]={1,0,0,0,0};
    unsigned char init[32]={0}, boot[40];
    runtime_content_configure(profile);
    assert(!module_load(0xb4));
    assert(!content_init(init,boot));

    /* No extra folder: only the save folder's add-ons. */
    assert(installed("SAVEONLY") && installed("SHARED") && !installed("EXTRA"));

    /* shadPS4's addcont: its CUSA00038 folder is the one read; files and overlong names are not
     * add-ons. */
    rescan();
    snprintf(path,sizeof(path),"%s/shadps4",root);
    runtime_content_extra_dir(path,"CUSA00038");
    assert(installed("EXTRA") && installed("SAVEONLY") && installed("SHARED"));
    assert(!installed("notes.txt") && !installed("MISSING"));
    assert(addon_count==3 && !strcmp(addons[find_addon("SHARED")].folder+strlen(user_dir),"/addcont/SHARED"));

    /* The title ID folder itself works the same. */
    rescan();
    snprintf(path,sizeof(path),"%s/shadps4/CUSA00038",root);
    runtime_content_extra_dir(path,"CUSA00038");
    assert(installed("EXTRA") && addon_count==3);

    /* A folder that is gone leaves only the save folder's add-ons. */
    rescan();
    snprintf(path,sizeof(path),"%s/gone",root);
    runtime_content_extra_dir(path,"CUSA00038");
    assert(!installed("EXTRA") && installed("SHARED"));

    /* /temp0: nothing until the game asks; then <user>/temp0, mounted once. */
    struct stat st;
    snprintf(path,sizeof(path),"%s/temp0",user_dir);
    assert(stat(path,&st) && !mount_calls);
    char mount[16];
    assert(temp_mount(0,NULL)==PARAMETER);
    assert(!temp_mount(0,mount) && !strcmp(mount,"/temp0"));
    assert(!temp_mount(0,mount) && mount_calls==1);
    assert(!stat(path,&st) && !strcmp(mounted_guest,"/temp0") && !strcmp(mounted_host,path));

    const char *const made[]={"shadps4/CUSA00038/EXTRA","shadps4/CUSA00038/SHARED","shadps4/CUSA00038/LABEL_LONGER_THAN_16",
                              "shadps4/CUSA00038","shadps4","save/addcont/SAVEONLY","save/addcont/SHARED","save/addcont","save/temp0","save",""};
    snprintf(path,sizeof(path),"%s/shadps4/CUSA00038/notes.txt",root); remove(path);
    for (size_t i=0;i<sizeof(made)/sizeof(*made);++i) {
        snprintf(path,sizeof(path),"%s/%s",root,made[i]); rmdir(path);
    }
    puts("content test: ok");
    return 0;
}
