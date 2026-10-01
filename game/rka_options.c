#include "rka_options.h"
#include "rka_video.h"
#include "rka_cheats.h"
#include <stdio.h>
#include <string.h>

static char s_path[1024],s_error[160];
/* Spawn range and cheats change simulation, so both are in the shared
 * session seal: "rka.spawn_range=<mode>;cheats=E,S,L,H,I" (48-byte line).
 * Adopting the host's values does not overwrite the local preference file. */
void rka_config_image(char *out, size_t cap)
{
    char cheats[32];
    rka_cheats_image(cheats,sizeof cheats);
    snprintf(out,cap,"rka.spawn_range=%s;%s",rka_video_spawn_mode(),cheats);
}
int rka_config_adopt(const char *line)
{
    const char *prefix="rka.spawn_range=";
    if(strncmp(line,prefix,strlen(prefix)))return 0;
    char mode[16];
    const char *cheats=strchr(line,';');
    size_t n=cheats?(size_t)(cheats-line)-strlen(prefix):strlen(line+strlen(prefix));
    if(n>=sizeof mode)return -1;
    memcpy(mode,line+strlen(prefix),n);mode[n]=0;
    if(!rka_video_set_spawn(mode))return -1;
    if(cheats&&!rka_cheats_adopt(cheats+1))return -1;
    if(!cheats)rka_cheats_off();
    return 0;
}
void rka_load_settings(const char *path)
{
    snprintf(s_path,sizeof s_path,"%s",path?path:"");
    char *a=strrchr(s_path,'/'),*b=strrchr(s_path,'\\');
    char *sep=!a?b:!b?a:a>b?a:b;
    size_t offset=sep?(size_t)(sep+1-s_path):0;
    snprintf(s_path+offset,sizeof s_path-offset,"rka-video.ini");
    rka_video_set_spawn("expanded");
    FILE *f=fopen(s_path,"r");
    if (!f) return;
    char line[128],value[32];
    while(fgets(line,sizeof line,f)) {
        if(sscanf(line,"spawn_range=%31s",value)==1) rka_video_set_spawn(value);
        else rka_cheats_load_line(line);
    }
    fclose(f);
}
#if RECOMP_LAUNCHER
#include "recomp_launcher.h"
static const RecompLauncherCModProvider *s_base;
#define COPY(dst,src) snprintf(dst,sizeof(dst),"%s",src)
static int owns(const char *p,const char *f)
{ return p&&f&&!strcmp(p,"custom-widescreen")&&!strcmp(f,"widescreen"); }
static int base_count(int (*count)(void *)) { return count?count(s_base->ctx):0; }

/* Each playtest cheat is its own built-in mod: package "rka-<cheat id>" with a
 * single feature of the same id, appended after the engine's packages. */
static int cheat_of_package(const char *p)
{ return p&&!strncmp(p,"rka-",4)?rka_cheats_find(p+4):-1; }
static int cheat_of(const char *p,const char *f)
{ int i=cheat_of_package(p);return i>=0&&f&&!strcmp(f,rka_cheats_id(i))?i:-1; }
static int package_count(void *ctx) { (void)ctx;return base_count(s_base->package_count)+rka_cheats_count(); }
static int package_get(void *ctx,int i,RecompLauncherCModPackage *out)
{
    (void)ctx;
    int c=i-base_count(s_base->package_count);
    if(c>=0) {
        if(!out||c>=rka_cheats_count())return 0;
        memset(out,0,sizeof *out);
        snprintf(out->id,sizeof out->id,"rka-%s",rka_cheats_id(c));
        COPY(out->name,rka_cheats_name(c));COPY(out->version,"1");
        COPY(out->author,"GenesisRecomp contributors");COPY(out->description,rka_cheats_description(c));
        out->enabled=rka_cheats_on(c);out->option_count=0;return 1;
    }
    int ok=s_base->package_get(s_base->ctx,i,out);
    if(ok && !strcmp(out->id,"custom-widescreen")) out->option_count=2;
    return ok;
}
static int feature_count(void *ctx) { (void)ctx;return base_count(s_base->feature_count)+rka_cheats_count(); }
static int feature_get(void *ctx,int i,RecompLauncherCModFeature *out)
{
    (void)ctx;
    int c=i-base_count(s_base->feature_count);
    if(c>=0) {
        if(!out||c>=rka_cheats_count())return 0;
        memset(out,0,sizeof *out);
        COPY(out->id,rka_cheats_id(c));snprintf(out->package_id,sizeof out->package_id,"rka-%s",rka_cheats_id(c));
        COPY(out->package_name,rka_cheats_name(c));COPY(out->package_version,"1");
        COPY(out->name,rka_cheats_name(c));COPY(out->group,"Cheats");
        COPY(out->author,"GenesisRecomp contributors");COPY(out->description,rka_cheats_description(c));
        out->enabled=rka_cheats_on(c);out->option_count=!strcmp(rka_cheats_id(c),"start-stage");
        COPY(out->status,out->enabled?"Enabled":"Disabled");return 1;
    }
    int ok=s_base->feature_get(s_base->ctx,i,out);
    if(ok && owns(out->package_id,out->id)) {
        out->option_count=2;
        COPY(out->description,"Draw the wider Rocket Knight Adventures world without stretching. Adaptive fits the window; choose when offscreen enemies become active.");
    }
    return ok;
}
static int option_get(void *ctx,const char *p,const char *f,int i,RecompLauncherCModOption *out)
{
    (void)ctx;
    int c=cheat_of(p,f);
    if(c>=0) {
        if(!out||i!=0||strcmp(rka_cheats_id(c),"start-stage"))return 0;
        memset(out,0,sizeof *out);out->step=1;out->type=RECOMP_MOD_OPTION_CHOICE;out->choice_count=7;
        COPY(out->id,"stage");COPY(out->label,"Stage");
        COPY(out->description,"Stage a new game begins at.");
        snprintf(out->value,sizeof out->value,"%d",rka_cheats_stage());COPY(out->default_value,"1");
        return 1;
    }
    if(!owns(p,f)||i!=1) return s_base->feature_option_get(s_base->ctx,p,f,i,out);
    if(!out)return 0;
    memset(out,0,sizeof *out);out->type=RECOMP_MOD_OPTION_CHOICE;out->step=1;out->choice_count=2;
    COPY(out->id,"spawn-range");COPY(out->label,"Enemy activation");
    COPY(out->description,"Expanded activates enemies across the wider view. Original keeps the original screen's spawn and despawn timing.");
    COPY(out->value,rka_video_spawn_mode());COPY(out->default_value,"expanded");return 1;
}
static int choice_get(void *ctx,const char *p,const char *f,const char *o,int i,RecompLauncherCModChoice *out)
{
    (void)ctx;
    if(cheat_of(p,f)>=0) {
        if(!out||!o||strcmp(o,"stage")||i<0||i>6)return 0;
        memset(out,0,sizeof *out);
        snprintf(out->value,sizeof out->value,"%d",i+1);
        snprintf(out->label,sizeof out->label,"Stage %d",i+1);return 1;
    }
    if(!owns(p,f)||!o||strcmp(o,"spawn-range"))return s_base->feature_choice_get(s_base->ctx,p,f,o,i,out);
    if(!out||i<0||i>1)return 0;
    memset(out,0,sizeof *out);
    COPY(out->value,i?"original":"expanded");COPY(out->label,i?"Original Spawn Range":"Expanded Spawn Range");return 1;
}
static int feature_enable(void *ctx,const char *p,const char *f,int on)
{
    (void)ctx;
    int c=cheat_of(p,f);
    if(c>=0){rka_cheats_enable(c,on);return 1;}
    return s_base->feature_enable(s_base->ctx,p,f,on);
}
static int set_enabled(void *ctx,const char *p,int on)
{
    (void)ctx;
    int c=cheat_of_package(p);
    if(c>=0){rka_cheats_enable(c,on);return 1;}
    return s_base->set_enabled?s_base->set_enabled(s_base->ctx,p,on):0;
}
static int set_option(void *ctx,const char *p,const char *f,const char *o,const char *v)
{
    (void)ctx;
    if(cheat_of(p,f)>=0)return o&&!strcmp(o,"stage")&&rka_cheats_set_stage(v);
    if(owns(p,f)&&o&&!strcmp(o,"spawn-range"))return rka_video_set_spawn(v);
    return s_base->feature_set_option(s_base->ctx,p,f,o,v);
}
static int diagnostic_count(void *ctx,const char *p,const char *f)
{
    (void)ctx;
    if(cheat_of_package(p)>=0)return 0;
    return s_base->diagnostic_count?s_base->diagnostic_count(s_base->ctx,p,f):0;
}
static int commit(void *ctx,const char *image)
{
    (void)ctx;s_error[0]=0;
    if(!s_base->commit(s_base->ctx,image))return 0;
    FILE *f=fopen(s_path,"w");
    if(!f){COPY(s_error,"Unable to save RKA mod settings");return 0;}
    int ok=fprintf(f,"spawn_range=%s\n",rka_video_spawn_mode())>0;
    rka_cheats_save(f);
    if(ferror(f))ok=0;
    if(fclose(f))ok=0;
    if(!ok)COPY(s_error,"Unable to save RKA mod settings");
    return ok;
}
static const char *last_error(void *ctx)
{ (void)ctx;return s_error[0]?s_error:s_base->last_error(s_base->ctx); }
const RecompLauncherCModProvider *rka_mods(const RecompLauncherCModProvider *base)
{
    static RecompLauncherCModProvider provider;
    if(!base)return NULL;
    s_base=base;provider=*base;
    provider.package_count=package_count;provider.package_get=package_get;
    provider.feature_count=feature_count;provider.feature_get=feature_get;
    provider.feature_option_get=option_get;provider.feature_choice_get=choice_get;
    provider.feature_enable=feature_enable;provider.set_enabled=set_enabled;
    provider.feature_set_option=set_option;provider.diagnostic_count=diagnostic_count;
    provider.commit=commit;provider.last_error=last_error;
    return &provider;
}
#else
const struct RecompLauncherCModProvider *rka_mods(const struct RecompLauncherCModProvider *base) { return base; }
#endif
