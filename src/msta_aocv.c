/** AOCV depth tables. Native, documented text format; no vendor parser dependency. */
#include "msta_sdc.h"
#include "msta_net.h"
#include "msta_util.h"
#include <fnmatch.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <limits.h>

/* scope pattern clock|data rise|fall early|late depth factor */
int Msta_AocvRead( MstaSdc *p, const char *pFile, int fVerbose )
{
    FILE *pIn=fopen(pFile,"r");
    MstaAocvTableArray Tables;
    char Line[2048], Scope[32], Pattern[256], Path[32], Edge[32], Corner[32], Extra[2], DepthText[64], FactorText[64];
    int nLine=0, fOk=1, Depth, nRows=0, i;
    char *pEnd;
    long DepthValue;
    double Factor;
    MstaAocvTableArrayInit(&Tables);
    if ( !pIn ) { Msta_Error("read_aocv：无法读取 %s\n",pFile); return 0; }
    while ( fgets(Line,sizeof(Line),pIn) )
    {
        char *Text=Line, *Comment=strchr(Line,'#');
        MstaAocvTable *pT=NULL;
        nLine++;
        if ( !strchr(Line,'\n') && !feof(pIn) ) { fOk=0; break; }
        if ( Comment ) *Comment=0;
        while ( isspace((unsigned char)*Text) ) Text++;
        if ( !*Text ) continue;
        if ( sscanf(Text,"%31s %255s %31s %31s %31s %63s %63s %1s",Scope,Pattern,Path,Edge,Corner,DepthText,FactorText,Extra)!=7 )
        { fOk=0; break; }
        errno=0;
        DepthValue=strtol(DepthText,&pEnd,10);
        if ( errno || *pEnd || DepthValue<1 || DepthValue>1000000 ) { fOk=0; break; }
        Depth=(int)DepthValue;
        errno=0;
        Factor=strtod(FactorText,&pEnd);
        if ( errno || *pEnd || !isfinite(Factor) ) { fOk=0; break; }
        if (
             (strcmp(Scope,"design") && strcmp(Scope,"cell") && strcmp(Scope,"instance")) ||
             (!strcmp(Scope,"design") && strcmp(Pattern,"*")) ||
             (strcmp(Path,"clock") && strcmp(Path,"data")) ||
             (strcmp(Edge,"rise") && strcmp(Edge,"fall")) ||
             (strcmp(Corner,"early") && strcmp(Corner,"late")) ||
             Depth<1 || Depth>1000000 || !isfinite(Factor) || Factor<=0 ||
             (!strcmp(Corner,"early") ? Factor>1 : Factor<1) ) { fOk=0; break; }
        char Kind=!strcmp(Scope,"design") ? 'G' : !strcmp(Scope,"instance") ? 'I' : 'L';
        MstaId Name=Msta_NameId(Pattern);
        int Clock=!strcmp(Path,"clock"), Rise=!strcmp(Edge,"rise"), Max=!strcmp(Corner,"late");
        for ( i=0; i<Tables.nSize; i++ )
        {
            MstaAocvTable *pOld=MstaAocvTableArrayAt(&Tables,i);
            if ( pOld->Kind==Kind && pOld->Pattern==Name && pOld->fClock==Clock &&
                 pOld->fRise==Rise && pOld->fMax==Max ) { pT=pOld; break; }
        }
        if ( !pT )
        {
            pT=MstaAocvTableArrayAppend(&Tables);
            pT->Kind=Kind; pT->Pattern=Name; pT->fClock=Clock; pT->fRise=Rise; pT->fMax=Max;
        }
        int n=pT->nPoints;
        /* 单调曲线使最短结构深度成为保守的 GBA 上界/下界。 */
        if ( n==MSTA_AOCV_MAX_POINTS || (n && (Depth<=pT->Depth[n-1] ||
             (Max ? Factor>pT->Factor[n-1] : Factor<pT->Factor[n-1]))) ) { fOk=0; break; }
        pT->Depth[n]=Depth; pT->Factor[n]=Factor; pT->nPoints++; nRows++;
    }
    if ( ferror(pIn) ) fOk=0;
    fclose(pIn);
    if ( !fOk || !nRows )
    {
        Msta_Error("read_aocv：%s:%d 无效表格；需正整数递增 depth、有限系数，early <=1 单调递增，late >=1 单调递减；原表保留\n",pFile,nLine);
        MstaAocvTableArrayFree(&Tables);
        return 0;
    }
    /* 全文件验证成功才替换：坏文件不改变表和 enable 状态。 */
    MstaAocvTableArrayFree(&p->vAocv);
    p->vAocv=Tables; p->fAocv=1;
    if ( fVerbose ) printf("AOCV：%d 张深度表（%d 行）\n",Tables.nSize,nRows);
    return 1;
}

static double AocvLookup( const MstaAocvTable *pT, int Depth )
{
    int i;
    if ( Depth<=pT->Depth[0] ) return pT->Factor[0];
    for ( i=1; i<pT->nPoints; i++ )
        if ( Depth<=pT->Depth[i] )
        {
            double T=(double)(Depth-pT->Depth[i-1])/(pT->Depth[i]-pT->Depth[i-1]);
            return pT->Factor[i-1]+T*(pT->Factor[i]-pT->Factor[i-1]);
        }
    return pT->Factor[pT->nPoints-1];
}

double Msta_AocvFactor( MstaSdc *p, MstaInst *pInst, int fClock, int fRise, int fMax, int Depth )
{
    const MstaAocvTable *pBest=NULL;
    int i, Best=-1;
    if ( !p->fAocv || !pInst ) return 1;
    for ( i=0; i<p->vAocv.nSize; i++ )
    {
        const MstaAocvTable *pT=MstaAocvTableArrayAt(&p->vAocv,i);
        const char *pName=pT->Kind=='I' ? Msta_NameStr(pInst->Name) : Msta_NameStr(pInst->pCell->Name);
        int Priority=pT->Kind=='I' ? 2 : pT->Kind=='L' ? 1 : 0;
        if ( pT->fClock!=fClock || pT->fRise!=fRise || pT->fMax!=fMax || Priority<Best ) continue;
        if ( pT->Kind!='G' && fnmatch(Msta_NameStr(pT->Pattern),pName,0) ) continue;
        pBest=pT; Best=Priority; /* 同优先级后写/后读表覆盖 */
    }
    return pBest ? AocvLookup(pBest,Depth) : 1;
}

void Msta_AocvReport( MstaSdc *p, FILE *pOut )
{
    int i,j;
    fprintf(pOut,"AOCV depth GBA : %s, %d tables\n",p->fAocv ? "enabled" : "disabled",p->vAocv.nSize);
    for ( i=0; i<p->vAocv.nSize; i++ )
    {
        MstaAocvTable *pT=MstaAocvTableArrayAt(&p->vAocv,i);
        fprintf(pOut,"%s %s %s %s %s:",pT->Kind=='G' ? "design" : pT->Kind=='I' ? "instance" : "cell",
                Msta_NameStr(pT->Pattern),pT->fClock ? "clock" : "data",pT->fRise ? "rise" : "fall",pT->fMax ? "late" : "early");
        for ( j=0; j<pT->nPoints; j++ ) fprintf(pOut," %d:%.6g",pT->Depth[j],pT->Factor[j]);
        fprintf(pOut,"\n");
    }
}
