#include "rka_options.h"
#include "rka_video.h"
#include <stdio.h>
#include <string.h>

static char s_path[1024],s_error[160];
/* Spawn range changes simulation, so include it in the shared session seal.
 * Adopting the host's value does not overwrite the local preference file. */
void rka_config_image(char *out, size_t cap)
{ snprintf(out,cap,"rka.spawn_range=%s",rka_video_spawn_mode()); }
int rka_config_adopt(const char *line)
{
    const char *prefix="rka.spawn_range=";
    if(strncmp(line,prefix,strlen(prefix)))return 0;
    return rka_video_set_spawn(line+strlen(prefix))?0:-1;
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
    while(fgets(line,sizeof line,f))
        if(sscanf(line,"spawn_range=%31s",value)==1) rka_video_set_spawn(value);
    fclose(f);
}
#if RECOMP_LAUNCHER
#include "recomp_launcher.h"
static const RecompLauncherCModProvider *s_base;
#define COPY(dst,src) snprintf(dst,sizeof(dst),"%s",src)
static int owns(const char *p,const char *f)
{ return p&&f&&!strcmp(p,"custom-widescreen")&&!strcmp(f,"widescreen"); }
static int package_get(void *ctx,int i,RecompLauncherCModPackage *out)
{
    (void)ctx;
    int ok=s_base->package_get(s_base->ctx,i,out);
    if(ok && !strcmp(out->id,"custom-widescreen")) out->option_count=2;
    return ok;
}
static int feature_get(void *ctx,int i,RecompLauncherCModFeature *out)
{
    (void)ctx;
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
    if(!owns(p,f)||!o||strcmp(o,"spawn-range"))return s_base->feature_choice_get(s_base->ctx,p,f,o,i,out);
    if(!out||i<0||i>1)return 0;
    memset(out,0,sizeof *out);
    COPY(out->value,i?"original":"expanded");COPY(out->label,i?"Original Spawn Range":"Expanded Spawn Range");return 1;
}
static int set_option(void *ctx,const char *p,const char *f,const char *o,const char *v)
{
    (void)ctx;
    if(owns(p,f)&&o&&!strcmp(o,"spawn-range"))return rka_video_set_spawn(v);
    return s_base->feature_set_option(s_base->ctx,p,f,o,v);
}
static int commit(void *ctx,const char *image)
{
    (void)ctx;s_error[0]=0;
    if(!s_base->commit(s_base->ctx,image))return 0;
    FILE *f=fopen(s_path,"w");
    if(!f){COPY(s_error,"Unable to save RKA enemy activation setting");return 0;}
    int ok=fprintf(f,"spawn_range=%s\n",rka_video_spawn_mode())>0;
    if(fclose(f))ok=0;
    if(!ok)COPY(s_error,"Unable to save RKA enemy activation setting");
    return ok;
}
static const char *last_error(void *ctx)
{ (void)ctx;return s_error[0]?s_error:s_base->last_error(s_base->ctx); }
const RecompLauncherCModProvider *rka_mods(const RecompLauncherCModProvider *base)
{
    static RecompLauncherCModProvider provider;
    if(!base)return NULL;
    s_base=base;provider=*base;
    provider.package_get=package_get;provider.feature_get=feature_get;
    provider.feature_option_get=option_get;provider.feature_choice_get=choice_get;
    provider.feature_set_option=set_option;provider.commit=commit;provider.last_error=last_error;
    return &provider;
}
#else
const struct RecompLauncherCModProvider *rka_mods(const struct RecompLauncherCModProvider *base) { return base; }
#endif
