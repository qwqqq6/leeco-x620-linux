/* Mobile key-layout adapter for the unmodified official Phoenix engine.
 * No dictionary, decoder or recognizer is implemented here. Fcitx reserves
 * number keys for candidates; a..h transport the eight 9-key buttons instead.
 * Built without libc dependencies, loaded only inside the glibc runtime.
 */
typedef unsigned long size_t;
typedef long ssize_t;
extern void *dlvsym(void *, const char *, const char *);
extern int open(const char *, int, ...);
extern ssize_t read(int, void *, size_t);
extern ssize_t write(int, const void *, size_t);
extern int close(int);

struct keylabel { unsigned short label; int x, y, flags; };
struct keyparam { int type; short count, edit; struct keylabel *labels; };
typedef int (*set_fn)(void *, void *, const char *, const char *);
typedef int (*keys_fn)(void *, const struct keyparam *, int *);
typedef int (*create_fn)(void *, void **);
typedef int (*destroy_fn)(void *, void *);
static void *(*original_dlsym)(void *, const char *);
static set_fn original_set;
static keys_fn original_keys;
static create_fn original_create;
static destroy_fn original_destroy;
static struct instance { void *instance, *manager; int layout, base; } instances[64];

static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int integer(const char *p) {
    int base = 10, n = 0, v;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {base=16; p+=2;}
    while (*p) {
        v = *p >= '0' && *p <= '9' ? *p-'0' :
            *p >= 'a' && *p <= 'f' ? *p-'a'+10 : -1;
        if (v < 0 || v >= base) break;
        n=n*base+v; p++;
    }
    return n;
}
static struct instance *find(void *inst, void *mgr) {
    int i;
    for (i=0; i<64; i++) if (instances[i].instance == inst) return &instances[i];
    if (mgr) for(i=0; i<64; i++) if (!instances[i].instance) {
        instances[i].instance=inst; instances[i].manager=mgr;
        instances[i].layout=26; instances[i].base=0x109;
        return &instances[i];
    }
    return 0;
}
static int wanted_layout(void) {
    char c='2'; int fd=open("/run/user/1000/iflytek-keyboard-layout",0);
    if (fd>=0) { read(fd,&c,1); close(fd); }
    return c=='9' ? 9 : 26;
}
static int set_method(struct instance *s, int layout) {
    char value[16]; int n, len=0, i, ret; char temp;
    n=(s->base & ~0xff00) | (layout==9 ? 0x200 : 0x100);
    do { value[len++]='0'+n%10; n/=10; } while(n && len<14);
    for(i=0;i<len/2;i++){temp=value[i]; value[i]=value[len-i-1];value[len-i-1]=temp;}
    value[len]=0;
    ret=original_set(s->manager,s->instance,"ipt_param_method",value);
    if (!ret) s->layout=layout;
    return ret;
}
static int mobile_create(void *manager, void **inst) {
    int r=original_create(manager,inst);
    if(!r && inst && *inst) find(*inst,manager);
    return r;
}
static int mobile_destroy(void *manager, void *inst) {
    struct instance *s=find(inst,0);
    int r=original_destroy(manager,inst);
    if(!r && s) {s->instance=0; s->manager=0; s->base=0; s->layout=0;}
    return r;
}
static int mobile_set(void *manager,void *inst,const char *key,const char *value) {
    struct instance *s=find(inst,manager);
    int r=original_set(manager,inst,key,value);
    if (!r && s && equal(key,"ipt_param_method")) {
        s->base=integer(value); s->layout=(s->base & 0xff00)==0x200 ? 9 : 26;
    }
    return r;
}
static int mobile_keys(void *inst,const struct keyparam *param,int *status) {
    struct instance *s=find(inst,0);
    struct keyparam copy; struct keylabel label;
    int layout=wanted_layout();
    if(s && original_set && layout!=s->layout) {
        int r=set_method(s,layout);
        if(r) return r;
    }
    if(layout==9 && s && s->layout==9 && param && param->type==0 &&
       param->count==1 && param->labels && param->labels[0].label>='a' &&
       param->labels[0].label<='h') {
        copy=*param; label=param->labels[0];
        label.label='2'+label.label-'a'; copy.labels=&label;
        return original_keys(inst,&copy,status);
    }
    return original_keys(inst,param,status);
}
void *dlsym(void *handle,const char *symbol) {
    void *result;
    if(!original_dlsym)
        original_dlsym=dlvsym((void *)-1L,"dlsym","GLIBC_2.17");
    if(!original_dlsym) return 0;
    result=original_dlsym(handle,symbol);
    if(!result) return result;
    if(equal(symbol,"Phn_ProcessKeys")){original_keys=result;return mobile_keys;}
    if(equal(symbol,"Phn_SetParam")){original_set=result;return mobile_set;}
    if(equal(symbol,"Phn_CreateInst")){original_create=result;return mobile_create;}
    if(equal(symbol,"Phn_DestoryInst")){original_destroy=result;return mobile_destroy;}
    return result;
}
