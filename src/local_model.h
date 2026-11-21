/* Offline language scoring and bounded per-user learning. No network dependency.
   Wire format is little endian, IEEE float32; Windows x86/x64 builds supported. */
#ifndef DISWITCHER_LOCAL_MODEL_H
#define DISWITCHER_LOCAL_MODEL_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <wchar.h>

#define DS_N 34
#define DS_TABLE (DS_N*DS_N*DS_N)
#define DS_FEATURES 8
#define DS_PERSONAL_MAX 1024

typedef struct { wchar_t word[65]; float frequency; } DSWord;
typedef struct { wchar_t original[65], target[65]; int label; } DSFeedback;
typedef struct {
    int ready;
    uint32_t fingerprint, count, feedback_count;
    float baseline[8], weights[8], base_bias, bias, threshold;
    float tables[2][DS_TABLE];
    DSWord* words;
    DSFeedback feedback[DS_PERSONAL_MAX];
} DSModel;
typedef struct {
    int apply;
    float probability, original_frequency, target_frequency;
    float original_score, target_score;
    const wchar_t* reason;
} DSDecision;

static const wchar_t* DSAlphabet(int lang)
{
    return lang ? L"абвгдеёжзийклмнопрстуфхцчшщъыьэюя" : L"abcdefghijklmnopqrstuvwxyz";
}
static wchar_t DSLower(wchar_t c)
{
    if (c >= L'A' && c <= L'Z') return c+32;
    if (c >= L'А' && c <= L'Я') return c+32;
    if (c == L'Ё') return L'ё';
    return c;
}
static int DSLanguage(const wchar_t* word)
{
    if (!word || !word[0]) return -1;
    for (int lang=0; lang<2; ++lang) {
        int ok=1;
        for (const wchar_t* p=word; *p; ++p)
            if (!wcschr(DSAlphabet(lang), DSLower(*p))) { ok=0; break; }
        if (ok) return lang;
    }
    return -1;
}
static void DSNormalize(const wchar_t* in, wchar_t out[65])
{
    size_t i=0;
    for (; in[i] && i<64; ++i) out[i]=DSLower(in[i]);
    out[i]=0;
}
static int DSSourceLanguage(const wchar_t* word)
{
    int lang=DSLanguage(word), letters=0;
    if (lang>=0) return lang;
    for (const wchar_t* p=word; *p; ++p) {
        wchar_t c=DSLower(*p);
        if (c>=L'a' && c<=L'z') ++letters;
        else if (!wcschr(L"[];',.`",c)) return -1;
    }
    return letters?0:-1;
}
static int DSRead(FILE* f, void* out, size_t bytes, uint32_t* hash)
{
    if (fread(out,1,bytes,f)!=bytes) return 0;
    const unsigned char* p=(const unsigned char*)out;
    for (size_t i=0; i<bytes; ++i) { *hash ^= p[i]; *hash *= 16777619u; }
    return 1;
}
static int DSLoad(DSModel* m, FILE* f)
{
    char magic[8]; uint32_t features=0, count=0, hash=2166136261u;
    float values[10];
    m->ready=0;
    if (!f || !DSRead(f,magic,8,&hash) || memcmp(magic,"DSLM0001",8) ||
        !DSRead(f,&features,4,&hash) || features!=8 ||
        !DSRead(f,&count,4,&hash) || !count || count>200000 ||
        !DSRead(f,values,sizeof(values),&hash)) return 0;
    for (int i=0;i<10;++i) if (!isfinite(values[i])) return 0;
    if (values[9]<.5f || values[9]>1.f) return 0;
    if (!DSRead(f,m->tables,sizeof(m->tables),&hash)) return 0;
    for (int l=0;l<2;++l) for (int i=0;i<DS_TABLE;++i)
        if (!isfinite(m->tables[l][i]) || m->tables[l][i]>0.f || m->tables[l][i]<-100.f) return 0;
    DSWord* words=(DSWord*)calloc(count,sizeof(DSWord));
    if (!words) return 0;
    for (uint32_t i=0;i<count;++i) {
        uint16_t length=0, chars[64]; float frequency=0;
        if (!DSRead(f,&length,2,&hash) || !length || length>64 ||
            !DSRead(f,&frequency,4,&hash) || !isfinite(frequency) || frequency<0.f || frequency>10.f ||
            !DSRead(f,chars,length*2u,&hash)) { free(words); return 0; }
        for (uint16_t j=0;j<length;++j) {
            if (!chars[j]) { free(words); return 0; }
            words[i].word[j]=(wchar_t)chars[j];
        }
        words[i].frequency=frequency;
        if (DSLanguage(words[i].word)<0 || (i && wcscmp(words[i-1].word,words[i].word)>=0)) {
            free(words); return 0;
        }
    }
    if (fgetc(f)!=EOF) { free(words); return 0; }
    free(m->words); m->words=words; m->count=count; m->fingerprint=hash; m->feedback_count=0;
    memcpy(m->baseline,values,8*sizeof(float)); memcpy(m->weights,values,8*sizeof(float));
    m->base_bias=m->bias=values[8]; m->threshold=values[9]; m->ready=1;
    return 1;
}
static float DSFrequency(const DSModel* m, const wchar_t* word)
{
    uint32_t lo=0, hi=m->count;
    while (lo<hi) {
        uint32_t mid=lo+(hi-lo)/2;
        int cmp=wcscmp(word,m->words[mid].word);
        if (!cmp) return m->words[mid].frequency;
        if (cmp<0) hi=mid; else lo=mid+1;
    }
    return 0.f;
}
static float DSScore(const DSModel* m, const wchar_t* word, int lang)
{
    int a=33,b=33; float sum=0.f; size_t n=wcslen(word);
    const wchar_t* alphabet=DSAlphabet(lang);
    for (size_t i=0;i<=n;++i) {
        int c=33;
        if (i<n) {
            const wchar_t* at=wcschr(alphabet,word[i]);
            if (!at) return -20.f;
            c=(int)(at-alphabet);
        }
        sum+=m->tables[lang][(a*DS_N+b)*DS_N+c]; a=b; b=c;
    }
    return sum/(float)(n+1);
}
static void DSFeatures(const DSModel* m, const wchar_t* original, const wchar_t* target, float x[8])
{
    int lang=DSSourceLanguage(original);
    float s=DSScore(m,original,lang), t=DSScore(m,target,1-lang);
    float f=DSFrequency(m,original), g=DSFrequency(m,target);
    size_t n=wcslen(original);
    x[0]=(t-s)/4.f; x[1]=s/8.f; x[2]=t/8.f;
    x[3]=f/8.f; x[4]=g/8.f; x[5]=(g-f)/8.f;
    x[6]=(float)(n>20?20:n)/20.f; x[7]=(float)lang;
}
static float DSPredict(const DSModel* m, const float x[8])
{
    float z=m->bias;
    for (int i=0;i<8;++i) z+=m->weights[i]*x[i];
    if (z>30.f) z=30.f; if (z< -30.f) z=-30.f;
    return 1.f/(1.f+expf(-z));
}
static void DSRemember(DSModel* m, const wchar_t* original, const wchar_t* target, int label)
{
    if (!m->ready || !original || !target || wcslen(original)>64 || wcslen(target)>64 ||
        DSSourceLanguage(original)<0 || DSLanguage(target)<0) return;
    wchar_t a[65],b[65]; DSNormalize(original,a); DSNormalize(target,b);
    uint32_t at=m->feedback_count;
    for (uint32_t i=0;i<m->feedback_count;++i)
        if (!wcscmp(a,m->feedback[i].original)) { at=i; break; }
    if (at<m->feedback_count) {
        memmove(m->feedback+at,m->feedback+at+1,(m->feedback_count-at-1)*sizeof(DSFeedback));
        --m->feedback_count;
    } else if (m->feedback_count==DS_PERSONAL_MAX) {
        memmove(m->feedback,m->feedback+1,(DS_PERSONAL_MAX-1)*sizeof(DSFeedback));
        --m->feedback_count;
    }
    DSFeedback* item=&m->feedback[m->feedback_count++];
    memcpy(item->original,a,(wcslen(a)+1)*sizeof(wchar_t));
    memcpy(item->target,b,(wcslen(b)+1)*sizeof(wchar_t)); item->label=label?1:0;
}
/* Refit from latest explicit labels, anchored to the shipped model. Repeated toggles
   replace a label rather than multiplying its influence. Never learn from silence. */
static void DSRefit(DSModel* m)
{
    memcpy(m->weights,m->baseline,sizeof(m->weights)); m->bias=m->base_bias;
    for (int epoch=0;epoch<3;++epoch) for (uint32_t j=0;j<m->feedback_count;++j) {
        DSFeedback* item=&m->feedback[j]; float x[8];
        DSFeatures(m,item->original,item->target,x);
        float err=(float)item->label-DSPredict(m,x);
        for (int i=0;i<8;++i) {
            float delta=m->weights[i]-m->baseline[i];
            delta+=.01f*(err*x[i]-.1f*delta);
            if (delta>.25f) delta=.25f; if (delta<-.25f) delta=-.25f;
            m->weights[i]=m->baseline[i]+delta;
        }
        float delta=m->bias-m->base_bias+.01f*err;
        if (delta>.25f) delta=.25f; if (delta<-.25f) delta=-.25f;
        m->bias=m->base_bias+delta;
    }
}
static DSDecision DSDecide(const DSModel* m, const wchar_t* original, const wchar_t* target)
{
    DSDecision d={0}; d.reason=L"unsupported";
    size_t n=wcslen(original);
    if (!m->ready) { d.reason=L"model_missing"; return d; }
    if (n<3 || n>64 || wcslen(target)>64 || DSSourceLanguage(original)<0 || DSLanguage(target)<0) return d;
    wchar_t a[65],b[65]; DSNormalize(original,a); DSNormalize(target,b);
    float x[8]; DSFeatures(m,a,b,x); d.probability=DSPredict(m,x);
    int lang=DSSourceLanguage(a);
    d.original_frequency=DSFrequency(m,a); d.target_frequency=DSFrequency(m,b);
    d.original_score=DSScore(m,a,lang); d.target_score=DSScore(m,b,1-lang);
    for (uint32_t i=0;i<m->feedback_count;++i) if (!wcscmp(a,m->feedback[i].original)) {
        d.apply=m->feedback[i].label; d.reason=d.apply?L"personal_convert":L"personal_keep"; return d;
    }
    int allUpper=1;
    for (size_t i=0;i<n;++i) if (original[i]==DSLower(original[i])) { allUpper=0; break; }
    if (allUpper && (n<6 || d.original_frequency>0.f || d.target_frequency<4.f)) { d.reason=L"acronym"; return d; }
    if (d.original_frequency>=3.f) { d.reason=L"known_original"; return d; }
    if (d.target_frequency<(n<=3?4.5f:3.f)) { d.reason=L"unknown_target"; return d; }
    d.apply=d.probability>=m->threshold;
    d.reason=d.apply?L"model_accept":L"uncertain";
    return d;
}
#endif
