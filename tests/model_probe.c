/* Runs the exact C engine, without installing hooks or touching user data. */
#include <windows.h>
#include "../src/local_model.h"

static DSModel model;
int main(int argc, char** argv)
{
    if (argc!=2) return 2;
    FILE* f=NULL; fopen_s(&f,argv[1],"rb");
    if (!f) return 3;
    int loaded=DSLoad(&model,f); fclose(f);
    if (!loaded) return 4;
    char line[1024];
    while (fgets(line,sizeof(line),stdin)) {
        char* ctx=NULL;
        char* command=strtok_s(line,"\t\r\n",&ctx);
        char* original=strtok_s(NULL,"\t\r\n",&ctx);
        char* target=strtok_s(NULL,"\t\r\n",&ctx);
        char* label=strtok_s(NULL,"\t\r\n",&ctx);
        if (!command || !original || !target) return 5;
        wchar_t a[65],b[65];
        if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,original,-1,a,65) ||
            !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,target,-1,b,65)) return 6;
        if (!strcmp(command,"learn")) {
            if (!label) return 7;
            DSRemember(&model,a,b,atoi(label)); DSRefit(&model);
        }
        DSDecision d=DSDecide(&model,a,b);
        printf("%d %.9g %ls %u\n",d.apply,d.probability,d.reason,model.feedback_count);
    }
    free(model.words);
    return 0;
}
