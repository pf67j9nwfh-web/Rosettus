#include <jni.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
JNIEXPORT jint JNICALL JNI_CreateJavaVM(JavaVM **vm, void **env, void *raw) {
    const char *root = getenv("ROSETTUS_ROOT");
    if (!root) root = "/Library/Rosettus";
    char library[2048], agent[2048];
    snprintf(library, sizeof library, "%s/System/Library/Frameworks/JavaVM.framework/Versions/1.5.0/Libraries/libclient.dylib", root);
    void *handle = dlopen(library, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) { fprintf(stderr,"vc-jvm: %s\n",dlerror()); return JNI_ERR; }
    jint (*create)(JavaVM **, void **, void *) = dlsym(handle,"JNI_CreateJavaVM");
    if (!create) create = dlsym(handle,"JNI_CreateJavaVM_Impl");
    if (!create) { fprintf(stderr,"vc-jvm: no JNI entry: %s\n",dlerror()); return JNI_ERR; }
    JavaVMInitArgs args = *(JavaVMInitArgs *)raw;
    JavaVMOption *options = calloc(args.nOptions + 3, sizeof *options);
    if (!options) return JNI_ENOMEM;
    memcpy(options,args.options,args.nOptions * sizeof *options);
    options[args.nOptions++].optionString = "-Xint";
    snprintf(agent,sizeof agent,"-javaagent:%s",getenv("VC_NULLAGENT") ? getenv("VC_NULLAGENT") : "/Library/Rosettus/versioncue/nullchecks.jar");
    options[args.nOptions++].optionString = agent;
    options[args.nOptions++].optionString = "-Djava.awt.headless=true";
    args.options = options;
    fprintf(stderr,"vc-jvm: starting exported PPC Java 5 in interpreter mode\n");
    jint result = create(vm,env,&args);
    free(options);
    return result;
}
