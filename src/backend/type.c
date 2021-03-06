// Copyright (C) 1985-1998 by Symantec
// Copyright (C) 2000-2009 by Digital Mars
// All Rights Reserved
// http://www.digitalmars.com
// Written by Walter Bright
/*
 * This source file is made available for personal use
 * only. The license is in /dmd/src/dmd/backendlicense.txt
 * or /dm/src/dmd/backendlicense.txt
 * For any other uses, please contact Digital Mars.
 */

#if !SPP

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cc.h"
#include "global.h"
#include "type.h"
#include "el.h"
#include "parser.h"
#if TARGET_MAC
#include "TG.h"
#else
#undef MEM_PH_MALLOC
#define MEM_PH_MALLOC mem_fmalloc
#endif

static char __file__[] = __FILE__;      /* for tassert.h                */
#include        "tassert.h"

static type *type_list = NULL;          // free list of types
static param_t *param_list = NULL;      // free list of params

#ifdef DEBUG
static int type_num,type_max;   /* gather statistics on # of types      */
#endif

typep_t tstypes[TYMAX];
typep_t tsptr2types[TYMAX];

typep_t tstrace,tsclib,tsjlib,tsdlib,
        tslogical;
typep_t tspvoid,tspcvoid;
typep_t tsptrdiff, tssize;

/*******************************
 * Compute size of type in bytes.
 */

targ_size_t type_size(type *t)
{   targ_size_t s;
    unsigned long u;
    tym_t tyb;

    type_debug(t);
    tyb = tybasic(t->Tty);
#if TARGET_MAC
    if (!ANSI && tyb == TYenum)
        tyb = tybasic(t->Tnext->Tty);
#endif
#ifdef DEBUG
    if (tyb >= TYMAX)
        /*type_print(t),*/
        dbg_printf("tyb = x%lx\n",tyb);
#endif
    assert(tyb < TYMAX);
    s = tysize[tyb];
    if (s == (targ_size_t) -1)
    {
        switch (tyb)
        {
            // in case program plays games with function pointers
            case TYffunc:
            case TYfpfunc:
#if TARGET_MAC
            case TYpsfunc:
#endif
#if TX86
            case TYnfunc:       /* in case program plays games with function pointers */
            case TYhfunc:
            case TYnpfunc:
            case TYnsfunc:
            case TYfsfunc:
            case TYf16func:
            case TYifunc:
            case TYjfunc:
#endif
#if SCPP
                if (ANSI)
                    synerr(EM_unknown_size,"function"); /* size of function is not known */
#endif
                s = 1;
                break;
            case TYarray:
                if (t->Tflags & TFsizeunknown)
                {
#if SCPP
                    synerr(EM_unknown_size,"array");    /* size of array is unknown     */
#endif
                    t->Tflags &= ~TFsizeunknown;
                }
                if (t->Tflags & TFvla)
                {
                    s = tysize[pointertype];
                    break;
                }
                s = type_size(t->Tnext);
                u = t->Tdim * (unsigned long) s;
#if TX86 && SCPP
                type_chksize(u);
#endif
                s = u;
                break;
            case TYstruct:
                t = t->Ttag->Stype;     /* find main instance           */
                                        /* (for const struct X)         */
                if (t->Tflags & TFsizeunknown)
                {
#if SCPP
                    template_instantiate_forward(t->Ttag);
                    if (t->Tflags & TFsizeunknown)
                        synerr(EM_unknown_size,t->Tty & TYstruct ? prettyident(t->Ttag) : "struct");
                    t->Tflags &= ~TFsizeunknown;
#endif
                }
                assert(t->Ttag);
                s = t->Ttag->Sstruct->Sstructsize;
                break;
#if SCPP
            case TYenum:
                if (t->Ttag->Senum->SEflags & SENforward)
                    synerr(EM_unknown_size, prettyident(t->Ttag));
                s = type_size(t->Tnext);
                break;
#endif
            case TYvoid:
#if SCPP && TARGET_WINDOS               // GNUC allows it, so we will, too
                synerr(EM_void_novalue);        // voids have no value
#endif
                s = 1;
                break;
#if SCPP
            case TYref:
            case TYmemptr:
            case TYvtshape:
                s = tysize(tym_conv(t));
                break;

            case TYident:
                synerr(EM_unknown_size, t->Tident);
                s = 1;
                break;
#endif
#if MARS
            case TYref:
                s = tysize(TYnptr);
                break;
#endif
            default:
#ifdef DEBUG
                WRTYxx(t->Tty);
#endif
                assert(0);
        }
    }
    return s;
}

/********************************
 * Return the size of a type for alignment purposes.
 */

unsigned type_alignsize(type *t)
{   targ_size_t sz;

L1:
    type_debug(t);

    sz = tyalignsize(t->Tty);
    if (sz == (targ_size_t)-1)
    {
        switch (tybasic(t->Tty))
        {
            case TYarray:
                if (t->Tflags & TFsizeunknown)
                    goto err1;
                t = t->Tnext;
                goto L1;
            case TYstruct:
                t = t->Ttag->Stype;         // find main instance
                                            // (for const struct X)
                if (t->Tflags & TFsizeunknown)
                    goto err1;
                sz = t->Ttag->Sstruct->Salignsize;
                if (sz > t->Ttag->Sstruct->Sstructalign)
                    sz = t->Ttag->Sstruct->Sstructalign;
                break;

            case TYldouble:
                assert(0);

            default:
            err1:                   // let type_size() handle error messages
                sz = type_size(t);
                break;
        }
    }

    //printf("type_alignsize() = %d\n", sz);
    return sz;
}

/*****************************
 * Compute the size of parameters for function call.
 * Used for stdcall name mangling.
 * Note that hidden parameters do not contribute to size.
 */

targ_size_t type_paramsize(type *t)
{
    targ_size_t sz = 0;
    if (tyfunc(t->Tty))
    {
        for (param_t *p = t->Tparamtypes; p; p = p->Pnext)
        {
            size_t n = type_size(p->Ptype);
            n = align(REGSIZE,n);       // align to REGSIZE boundary
            sz += n;
        }
    }
    return sz;
}

/*****************************
 * Create a type & initialize it.
 * Input:
 *      ty = TYxxxx
 * Returns:
 *      pointer to newly created type.
 */

type *type_alloc(tym_t ty)
{   type *t;
    static type tzero;

    assert(tybasic(ty) != TYtemplate);
    if (type_list)
    {   t = type_list;
        type_list = t->Tnext;
    }
    else
#if TX86
        t = (type *) mem_fmalloc(sizeof(type));
#else
        t = (type *) MEM_PH_MALLOC(sizeof(type));
#endif
    tzero.Tty = ty;
    *t = tzero;
#if SRCPOS_4TYPES
    if (PARSER && config.fulltypes)
        t->Tsrcpos = getlinnum();
#endif
#ifdef DEBUG
    t->id = IDtype;
    type_num++;
    if (type_num > type_max)
        type_max = type_num;
#endif
    //dbg_printf("type_alloc() = %p ",t); WRTYxx(t->Tty); dbg_printf("\n");
    //if (t == (type*)0xB6B744) *(char*)0=0;
    return t;
}

/*************************************
 * Allocate a TYtemplate.
 */

type *type_alloc_template(symbol *s)
{   type *t;

#if TX86
    t = (type *) mem_fcalloc(sizeof(typetemp_t));
#else
    t = (type *) MEM_PH_MALLOC(sizeof(typetemp_t));
    memset(t, 0, sizeof(typetemp_t));
#endif
    t->Tty = TYtemplate;
    if (s->Stemplate->TMprimary)
        s = s->Stemplate->TMprimary;
    ((typetemp_t *)t)->Tsym = s;
#if SRCPOS_4TYPES
    if (PARSER && config.fulltypes)
        t->Tsrcpos = getlinnum();
#endif
#ifdef DEBUG
    t->id = IDtype;
    type_num++;
    if (type_num > type_max)
        type_max = type_num;
    //dbg_printf("Alloc'ing template type %p ",t); WRTYxx(t->Tty); dbg_printf("\n");
#endif
    return t;
}

/*****************************
 * Fake a type & initialize it.
 * Input:
 *      ty = TYxxxx
 * Returns:
 *      pointer to newly created type.
 */

type *type_fake(tym_t ty)
{   type *t;

#if MARS
    assert(ty != TYstruct);
#endif
    t = type_alloc(ty);
    if (typtr(ty) || tyfunc(ty))
    {   t->Tnext = type_alloc(TYvoid);  /* fake with pointer to void    */
        t->Tnext->Tcount = 1;
    }
    return t;
}

/*****************************
 * Allocate a type of ty with a Tnext of tn.
 */

type *type_allocn(tym_t ty,type *tn)
{   type *t;

    //printf("type_allocn(ty = x%x, tn = %p)\n", ty, tn);
    assert(tn);
    type_debug(tn);
    t = type_alloc(ty);
    t->Tnext = tn;
    tn->Tcount++;
    //printf("\tt = %p\n", t);
    return t;
}

/******************************
 * Allocate a TYmemptr type.
 */

type *type_allocmemptr(Classsym *stag,type *tn)
{   type *t;

    symbol_debug(stag);
    assert(stag->Sclass == SCstruct || tybasic(stag->Stype->Tty) == TYident);
    t = type_allocn(TYmemptr,tn);
    t->Ttag = stag;
    //printf("type_allocmemptr() = %p\n", t);
    //type_print(t);
    return t;
}

/*****************************
 * Free up data type.
 */

void type_free(type *t)
{   type *tn;
    tym_t ty;

    while (t)
    {
        //dbg_printf("type_free(%p, Tcount = %d)\n", t, t->Tcount);
        type_debug(t);
        assert((int)t->Tcount != -1);
        if (--t->Tcount)                /* if usage count doesn't go to 0 */
            break;
        ty = tybasic(t->Tty);
        if (tyfunc(ty))
        {   param_free(&t->Tparamtypes);
            list_free(&t->Texcspec, (list_free_fp)type_free);
        }
        else if (ty == TYtemplate)
            param_free(&t->Tparamtypes);
        else if (ty == TYident)
            MEM_PH_FREE(t->Tident);
        else if (t->Tflags & TFvla && t->Tel)
            el_free(t->Tel);
#if SCPP
        else if (t->Talternate && typtr(ty))
            type_free(t->Talternate);
#endif
#if MARS
        else if (t->Tkey && typtr(ty))
            type_free(t->Tkey);
#endif
#ifdef DEBUG
        type_num--;
        //dbg_printf("Free'ing type %p ",t); WRTYxx(t->Tty); dbg_printf("\n");
        t->id = 0;                      /* no longer a valid type       */
#endif
        tn = t->Tnext;
        t->Tnext = type_list;
        type_list = t;                  /* link into free list          */
        t = tn;
    }
}

#ifdef STATS
/* count number of free types available on type list */
type_count_free()
    {
    type *t;
    int count;

    for(t=type_list;t;t=t->Tnext)
        count++;
    dbg_printf("types on free list %d with max of %d\n",count,type_max);
    }
#endif

/**********************************
 * Initialize type package.
 */

STATIC type * __near type_allocbasic(tym_t ty)
{   type *t;

    t = type_alloc(ty);
    t->Tmangle = mTYman_c;
    t->Tcount = 1;              /* so it is not inadvertantly free'd    */
    return t;
}

void type_init()
{   int i;

    tsbool    = type_allocbasic(TYbool);
    tswchar_t = type_allocbasic(TYwchar_t);
    tsdchar   = type_allocbasic(TYdchar);
    tsvoid    = type_allocbasic(TYvoid);
    tsnullptr = type_allocbasic(TYnullptr);
    tschar16  = type_allocbasic(TYchar16);
    tsuchar   = type_allocbasic(TYuchar);
    tsschar   = type_allocbasic(TYschar);
    tschar    = type_allocbasic(TYchar);
    tsshort   = type_allocbasic(TYshort);
    tsushort  = type_allocbasic(TYushort);
    tsint     = type_allocbasic(TYint);
    tsuns     = type_allocbasic(TYuint);
    tslong    = type_allocbasic(TYlong);
    tsulong   = type_allocbasic(TYulong);
    tsllong   = type_allocbasic(TYllong);
    tsullong  = type_allocbasic(TYullong);
    tsfloat   = type_allocbasic(TYfloat);
    tsdouble  = type_allocbasic(TYdouble);
    tsifloat   = type_allocbasic(TYifloat);
    tsidouble  = type_allocbasic(TYidouble);
    tscfloat   = type_allocbasic(TYcfloat);
    tscdouble  = type_allocbasic(TYcdouble);
#if TX86
    tsreal64  = type_allocbasic(TYdouble_alias);
    tsldouble  = type_allocbasic(TYldouble);
    tsildouble  = type_allocbasic(TYildouble);
    tscldouble  = type_allocbasic(TYcldouble);
#else
#if TARGET_POWERPC
// for powerPC the size of  long double is user determined
    if (config.flags & CFGldblisdbl) {
        tsldouble = type_allocbasic(TYdouble);  /* ldouble is same as double per user's request */
    } else {
        tsldouble = type_allocbasic(TYldouble);
    }
#else
    tsldouble = type_allocbasic(TYldouble);
#endif
    tscomp = type_allocbasic(TYcomp);
    chartype = tschar;                          /* default is signed chars */
#endif
    if (I64)
    {
        TYptrdiff = TYllong;
        TYsize = TYullong;
        tsptrdiff = tsllong;
        tssize = tsullong;
    }
    else
    {
        TYptrdiff = TYint;
        TYsize = TYuint;
        tsptrdiff = tsint;
        tssize = tsuns;
    }

#if TX86
    chartype = (config.flags3 & CFG3ju) ? tsuchar : tschar;

    // Type of far library function
    tsclib =    type_fake(LARGECODE ? TYfpfunc : TYnpfunc);
    tsclib->Tmangle = mTYman_c;
    tsclib->Tcount++;

    // Type of trace function
    tstrace =   type_fake(I16 ? TYffunc : TYnfunc);
    tstrace->Tmangle = mTYman_c;
    tstrace->Tcount++;

    tspvoid = type_allocn(pointertype,tsvoid);
    tspvoid->Tmangle = mTYman_c;
    tspvoid->Tcount++;

    // Type of far library function
    tsjlib =    type_fake(TYjfunc);
    tsjlib->Tmangle = mTYman_c;
    tsjlib->Tcount++;

    tsdlib = tsjlib;

#if SCPP
    tspcvoid = type_alloc(mTYconst | TYvoid);
    tspcvoid = newpointer(tspcvoid);
    tspcvoid->Tmangle = mTYman_c;
    tspcvoid->Tcount++;
#endif

    // Type of logical expression
    tslogical = (config.flags4 & CFG4bool) ? tsbool : tsint;

    for (i = 0; i < TYMAX; i++)
    {
        if (tstypes[i])
        {   tsptr2types[i] = type_allocn(pointertype,tstypes[i]);
            tsptr2types[i]->Tcount++;
        }
    }
#else
    type_list = NULL;
    tsclib = type_fake( TYffunc );
    tsclib->Tmangle = mTYman_c;
    tsclib->Tcount++;
#ifdef DEBUG
    type_num = 0;
    type_max = 0;
#endif /* DEBUG */
#endif /* TARGET_MACHINE */
}

/**********************************
 * Free type_list.
 */

#if TERMCODE
void type_term()
{   type *tn;
    param_t *pn;
    int i;

    for (i = 0; i < arraysize(tstypes); i++)
    {   type *t = tsptr2types[i];

        if (t)
        {   assert(!(t->Tty & (mTYconst | mTYvolatile | mTYimmutable | mTYshared)));
            assert(!(t->Tflags));
            assert(!(t->Tmangle));
            type_free(t);
        }
        type_free(tstypes[i]);
    }

    type_free(tsclib);
    type_free(tspvoid);
    type_free(tspcvoid);
    type_free(tsjlib);
#if TX86
    type_free(tstrace);
#endif

    while (type_list)
    {   tn = type_list->Tnext;
#if TX86
        mem_ffree(type_list);
#else
        MEM_PH_FREE(type_list);
#endif
        type_list = tn;
    }

    while (param_list)
    {   pn = param_list->Pnext;
#if TX86
        mem_ffree(param_list);
#else
        MEM_PH_FREE(param_list);
#endif
        param_list = pn;
    }

#ifdef DEBUG
    dbg_printf("Max # of types = %d\n",type_max);
    if (type_num != 0)
        dbg_printf("type_num = %d\n",type_num);
/*    assert(type_num == 0);*/
#endif
}
#endif // TERMCODE

/*******************************
 * Type type information.
 */

/**************************
 * Make copy of a type.
 */

type *type_copy(type *t)
{   type *tn;
    param_t *p;

    type_debug(t);
    if (tybasic(t->Tty) == TYtemplate)
    {
        tn = type_alloc_template(((typetemp_t *)t)->Tsym);
    }
    else
        tn = type_alloc(t->Tty);
    *tn = *t;
    switch (tybasic(tn->Tty))
    {       case TYtemplate:
                ((typetemp_t *)tn)->Tsym = ((typetemp_t *)t)->Tsym;
                goto L1;

            case TYident:
                tn->Tident = (char *)MEM_PH_STRDUP(t->Tident);
                break;

            case TYarray:
                if (tn->Tflags & TFvla)
                    tn->Tel = el_copytree(tn->Tel);
                break;

            default:
                if (tyfunc(tn->Tty))
                {
                L1:
                    tn->Tparamtypes = NULL;
                    for (p = t->Tparamtypes; p; p = p->Pnext)
                    {   param_t *pn;

                        pn = param_append_type(&tn->Tparamtypes,p->Ptype);
                        if (p->Pident)
                        {
                            pn->Pident = (char *)MEM_PH_STRDUP(p->Pident);
                        }
                        assert(!p->Pelem);
                    }
                }
#if SCPP
                else if (tn->Talternate && typtr(tn->Tty))
                    tn->Talternate->Tcount++;
#endif
#if MARS
                else if (tn->Tkey && typtr(tn->Tty))
                    tn->Tkey->Tcount++;
#endif
                break;
    }
    if (tn->Tnext)
    {   type_debug(tn->Tnext);
        tn->Tnext->Tcount++;
    }
    tn->Tcount = 0;
    return tn;
}

/************************************
 */

#if SCPP

elem *type_vla_fix(type **pt)
{
    type *t;
    elem *e = NULL;

    for (t = *pt; t; t = t->Tnext)
    {
        type_debug(t);
        if (tybasic(t->Tty) == TYarray && t->Tflags & TFvla && t->Tel)
        {   symbol *s;
            elem *ec;

            s = symbol_genauto(tsuns);
            ec = el_var(s);
            ec = el_bint(OPeq, tsuns, ec, t->Tel);
            e = el_combine(e, ec);
            t->Tel = el_var(s);
        }
    }
    return e;
}

#endif

/****************************
 * Modify the tym_t field of a type.
 */

type *type_setty(type **pt,long newty)
{   type *t;

    t = *pt;
    type_debug(t);
    if ((tym_t)newty != t->Tty)
    {   if (t->Tcount > 1)              /* if other people pointing at t */
        {   type *tn;

            tn = type_copy(t);
            tn->Tcount++;
            type_free(t);
            t = tn;
            *pt = t;
        }
        t->Tty = newty;
    }
    return t;
}

/******************************
 * Set type field of some object to t.
 */

type *type_settype(type **pt, type *t)
{
    if (t)
    {   type_debug(t);
        t->Tcount++;
    }
    type_free(*pt);
    return *pt = t;
}

/****************************
 * Modify the Tmangle field of a type.
 */

type *type_setmangle(type **pt,mangle_t mangle)
{   type *t;

    t = *pt;
    type_debug(t);
    if (mangle != type_mangle(t))
    {
        if (t->Tcount > 1)              // if other people pointing at t
        {   type *tn;

            tn = type_copy(t);
            tn->Tcount++;
            type_free(t);
            t = tn;
            *pt = t;
        }
        t->Tmangle = mangle;
    }
    return t;
}

/******************************
 * Set/clear const and volatile bits in *pt according to the settings
 * in cv.
 */

type *type_setcv(type **pt,tym_t cv)
{   unsigned long ty;

    type_debug(*pt);
    ty = (*pt)->Tty & ~(mTYconst | mTYvolatile | mTYimmutable | mTYshared);
    return type_setty(pt,ty | (cv & (mTYconst | mTYvolatile | mTYimmutable | mTYshared)));
}

/*****************************
 * Set dimension of array.
 */

type *type_setdim(type **pt,targ_size_t dim)
{   type *t = *pt;

    type_debug(t);
    if (t->Tcount > 1)                  /* if other people pointing at t */
    {   type *tn;

        tn = type_copy(t);
        tn->Tcount++;
        type_free(t);
        t = tn;
    }
    t->Tflags &= ~TFsizeunknown; /* we have determined its size */
    t->Tdim = dim;              /* index of array               */
    return *pt = t;
}


/*****************************
 * Create a 'dependent' version of type t.
 */

type *type_setdependent(type *t)
{
    type_debug(t);
    if (t->Tcount > 0 &&                        /* if other people pointing at t */
        !(t->Tflags & TFdependent))
    {
        t = type_copy(t);
    }
    t->Tflags |= TFdependent;
    return t;
}

/************************************
 * Determine if type t is a dependent type.
 */

int type_isdependent(type *t)
{   param_t *p;
    Symbol *stempl;
    type *tstart;

    //printf("type_isdependent(%p)\n", t);
    //type_print(t);
    for (tstart = t; t; t = t->Tnext)
    {
        type_debug(t);
        if (t->Tflags & TFdependent)
            goto Lisdependent;
        if (tyfunc(t->Tty) || tybasic(t->Tty) == TYtemplate)
        {
            for (param_t *p = t->Tparamtypes; p; p = p->Pnext)
            {
                if (p->Ptype && type_isdependent(p->Ptype))
                    goto Lisdependent;
                if (p->Pelem && el_isdependent(p->Pelem))
                    goto Lisdependent;
            }
        }
        else if (type_struct(t) &&
                 (stempl = t->Ttag->Sstruct->Stempsym) != NULL)
        {
            for (param_t *p = t->Ttag->Sstruct->Sarglist; p; p = p->Pnext)
            {
                if (p->Ptype && type_isdependent(p->Ptype))
                    goto Lisdependent;
                if (p->Pelem && el_isdependent(p->Pelem))
                    goto Lisdependent;
            }
        }
    }
    //printf("\tis not dependent\n");
    return 0;

Lisdependent:
    //printf("\tis dependent\n");
    // Dependence on a dependent type makes this type dependent as well
    tstart->Tflags |= TFdependent;
    return 1;
}


/*******************************
 * Recursively check if type u is embedded in type t.
 * Returns:
 *      != 0 if embedded
 */

int type_embed(type *t,type *u)
{   param_t *p;

    for (; t; t = t->Tnext)
    {
        type_debug(t);
        if (t == u)
            return 1;
        if (tyfunc(t->Tty))
        {
            for (p = t->Tparamtypes; p; p = p->Pnext)
                if (type_embed(p->Ptype,u))
                    return 1;
        }
    }
    return 0;
}


/***********************************
 * Determine if type is a VLA.
 */

int type_isvla(type *t)
{
    while (t)
    {
        if (tybasic(t->Tty) != TYarray)
            break;
        if (t->Tflags & TFvla)
            return 1;
        t = t->Tnext;
    }
    return 0;
}

/*************************************
 * Determine if type can be passed in a register.
 */

int type_jparam(type *t)
{
    targ_size_t sz;
    type_debug(t);
    return tyjparam(t->Tty) ||
                ((tybasic(t->Tty) == TYstruct || tybasic(t->Tty) == TYarray) &&
                 (sz = type_size(t)) <= NPTRSIZE &&
                 (sz == 1 || sz == 2 || sz == 4 || sz == 8));
}


/**********************************
 * Pretty-print a type.
 */

#ifdef DEBUG

void type_print(type *t)
{
  type_debug(t);
  dbg_printf("Tty="); WRTYxx(t->Tty);
  dbg_printf(" Tmangle=%d",t->Tmangle);
  dbg_printf(" Tflags=x%x",t->Tflags);
  dbg_printf(" Tcount=%d",t->Tcount);
  if (!(t->Tflags & TFsizeunknown) &&
        tybasic(t->Tty) != TYvoid &&
        tybasic(t->Tty) != TYident &&
        tybasic(t->Tty) != TYmfunc &&
        tybasic(t->Tty) != TYarray &&
        tybasic(t->Tty) != TYtemplate)
      dbg_printf(" Tsize=%ld",type_size(t));
  dbg_printf(" Tnext=%p",t->Tnext);
  switch (tybasic(t->Tty))
  {     case TYstruct:
        case TYmemptr:
            dbg_printf(" Ttag=%p,'%s'",t->Ttag,t->Ttag->Sident);
#if TARGET_MAC
            dbg_printf(" Sfldlst=x%08lx Sflags=x%x",
                            t->Ttag->Sstruct->Sfldlst,t->Ttag->Sstruct->Sflags);
#else
            //dbg_printf(" Sfldlst=%p",t->Ttag->Sstruct->Sfldlst);
#endif
            break;
        case TYarray:
            dbg_printf(" Tdim=%ld",t->Tdim);
            break;
        case TYident:
            dbg_printf(" Tident='%s'",t->Tident);
            break;
        case TYtemplate:
            dbg_printf(" Tsym='%s'",((typetemp_t *)t)->Tsym->Sident);
            {   param_t *p;
                int i;

                i = 1;
                for (p = t->Tparamtypes; p; p = p->Pnext)
                {   dbg_printf("\nTP%d (%p): ",i++,p);
                    fflush(stdout);

dbg_printf("Pident=%p,Ptype=%p,Pelem=%p,Pnext=%p ",p->Pident,p->Ptype,p->Pelem,p->Pnext);
                    param_debug(p);
                    if (p->Pident)
                        printf("'%s' ", p->Pident);
                    if (p->Ptype)
                        type_print(p->Ptype);
                    if (p->Pelem)
                        elem_print(p->Pelem);
                }
            }
            break;
        default:
            if (tyfunc(t->Tty))
            {   param_t *p;
                int i;

                i = 1;
                for (p = t->Tparamtypes; p; p = p->Pnext)
                {   dbg_printf("\nP%d (%p): ",i++,p);
                    fflush(stdout);

dbg_printf("Pident=%p,Ptype=%p,Pelem=%p,Pnext=%p ",p->Pident,p->Ptype,p->Pelem,p->Pnext);
                    param_debug(p);
                    if (p->Pident)
                        printf("'%s' ", p->Pident);
                    type_print(p->Ptype);
                }
            }
            break;
  }
  dbg_printf("\n");
  if (t->Tnext) type_print(t->Tnext);
}

/*******************************
 * Pretty-print a param_t
 */

void param_t::print()
{
    dbg_printf("Pident=%p,Ptype=%p,Pelem=%p,Psym=%p,Pnext=%p\n",Pident,Ptype,Pelem,Psym,Pnext);
    if (Pident)
        dbg_printf("\tPident = '%s'\n", Pident);
    if (Ptype)
    {   dbg_printf("\tPtype =\n");
        type_print(Ptype);
    }
    if (Pelem)
    {   dbg_printf("\tPelem =\n");
        elem_print(Pelem);
    }
    if (Pdeftype)
    {   dbg_printf("\tPdeftype =\n");
        type_print(Pdeftype);
    }
    if (Psym)
    {   dbg_printf("\tPsym = '%s'\n", Psym->Sident);
    }
    if (Pptpl)
    {   dbg_printf("\tPptpl = %p\n", Pptpl);
    }
}

void param_t::print_list()
{
    for (param_t *p = this; p; p = p->Pnext)
        p->print();
}

#endif /* DEBUG */

/**********************************
 * Hydrate a type.
 */

#if HYDRATE
void type_hydrate(type **pt)
{
    type *t;

    assert(pt);
    while (isdehydrated(*pt))
    {
        t = (type *) ph_hydrate(pt);
        type_debug(t);
#if !TX86
        if (t->Tflags & TFhydrated)
            return;
#if SOURCE_4TYPES
        t->Tsrcpos.Sfilnum += File_Hydrate_Num; /* file number relative header build */
#endif
        t->Tflags |= TFhydrated;
#endif
        switch (tybasic(t->Tty))
        {
            case TYstruct:
            case TYenum:
            case TYmemptr:
            case TYvtshape:
                // Cannot assume symbol is hydrated, because entire HX file
                // may not have been hydrated.
                Classsym_hydrate(&t->Ttag);
                symbol_debug(t->Ttag);
                break;
            case TYident:
                ph_hydrate(&t->Tident);
                break;
            case TYtemplate:
                symbol_hydrate(&((typetemp_t *)t)->Tsym);
                param_hydrate(&t->Tparamtypes);
                break;
            case TYarray:
                if (t->Tflags & TFvla)
                    el_hydrate(&t->Tel);
                break;
            default:
                if (tyfunc(t->Tty))
                {   param_hydrate(&t->Tparamtypes);
                    list_hydrate(&t->Texcspec, (list_free_fp)type_hydrate);
                }
#if SCPP
                else if (t->Talternate && typtr(t->Tty))
                    type_hydrate(&t->Talternate);
#endif
#if MARS
                else if (t->Tkey && typtr(t->Tty))
                    type_hydrate(&t->Tkey);
#endif
                break;
        }
        pt = &t->Tnext;
    }
}
#endif

/**********************************
 * Dehydrate a type.
 */

#if DEHYDRATE
void type_dehydrate(type **pt)
{
    type *t;

    while ((t = *pt) != NULL && !isdehydrated(t))
    {
        ph_dehydrate(pt);
#if DEBUG_XSYMGEN
        /* don't dehydrate types in HEAD when creating XSYM */
        if (xsym_gen && (t->Tflags & TFhydrated))
            return;
#endif
        type_debug(t);
        switch (tybasic(t->Tty))
        {
            case TYstruct:
            case TYenum:
            case TYmemptr:
            case TYvtshape:
                Classsym_dehydrate(&t->Ttag);
                break;
            case TYident:
                ph_dehydrate(&t->Tident);
                break;
            case TYtemplate:
                symbol_dehydrate(&((typetemp_t *)t)->Tsym);
                param_dehydrate(&t->Tparamtypes);
                break;
            case TYarray:
                if (t->Tflags & TFvla)
                    el_dehydrate(&t->Tel);
                break;
            default:
                if (tyfunc(t->Tty))
                {   param_dehydrate(&t->Tparamtypes);
                    list_dehydrate(&t->Texcspec, (list_free_fp)type_dehydrate);
                }
#if SCPP
                else if (t->Talternate && typtr(t->Tty))
                    type_dehydrate(&t->Talternate);
#endif
#if MARS
                else if (t->Tkey && typtr(t->Tty))
                    type_dehydrate(&t->Tkey);
#endif
                break;
        }
        pt = &t->Tnext;
    }
}
#endif

/****************************
 * Allocate a param_t.
 */

param_t *param_calloc()
{
    static param_t pzero;
    param_t *p;

#if !TX86
    debug_assert(PARSER);
#endif
    if (param_list)
    {
        p = param_list;
        param_list = p->Pnext;
    }
    else
    {
#if TX86
        p = (param_t *) mem_fmalloc(sizeof(param_t));
#else
        p = (param_t *) MEM_PH_MALLOC(sizeof(param_t));
#endif
    }
    *p = pzero;
#ifdef DEBUG
    p->id = IDparam;
#endif
    return p;
}

/***************************
 * Allocate a param_t of type t, and append it to parameter list.
 */

param_t *param_append_type(param_t **pp,type *t)
{   param_t *p;

    p = param_calloc();
    while (*pp)
    {   param_debug(*pp);
        pp = &((*pp)->Pnext);   /* find end of list     */
    }
    *pp = p;                    /* append p to list     */
    type_debug(t);
    p->Ptype = t;
    t->Tcount++;
    return p;
}

/************************
 * Version of param_free() suitable for list_free().
 */

void param_free_l(param_t *p)
{
    param_free(&p);
}

/***********************
 * Free parameter list.
 * Output:
 *      paramlst = NULL
 */

void param_free(param_t **pparamlst)
{   param_t *p,*pn;

#if !TX86
    debug_assert(PARSER);
#endif
    for (p = *pparamlst; p; p = pn)
    {   param_debug(p);
        pn = p->Pnext;
        type_free(p->Ptype);
#if TX86
        mem_free(p->Pident);
#else
        MEM_PH_FREE(p->Pident);
#endif
        el_free(p->Pelem);
        type_free(p->Pdeftype);
        if (p->Pptpl)
            param_free(&p->Pptpl);
#ifdef DEBUG
        p->id = 0;
#endif
        p->Pnext = param_list;
        param_list = p;
    }
    *pparamlst = NULL;
}

/***********************************
 * Compute number of parameters
 */

unsigned param_t::length()
{
    unsigned nparams = 0;
    param_t *p;

    for (p = this; p; p = p->Pnext)
        nparams++;
    return nparams;
}

/*************************************
 * Create template-argument-list blank from
 * template-parameter-list
 * Input:
 *      ptali   initial template-argument-list
 */

param_t *param_t::createTal(param_t *ptali)
{
    param_t *ptalistart = ptali;
    param_t *ptal = NULL;
    param_t **pp = &ptal;
    param_t *p;

    for (p = this; p; p = p->Pnext)
    {
        *pp = param_calloc();
        if (p->Pident)
        {
            // Should find a way to just point rather than dup
            (*pp)->Pident = (char *)MEM_PH_STRDUP(p->Pident);
        }
        if (ptali)
        {
            if (ptali->Ptype)
            {   (*pp)->Ptype = ptali->Ptype;
                (*pp)->Ptype->Tcount++;
            }
            if (ptali->Pelem)
            {
                elem *e = el_copytree(ptali->Pelem);
#if SCPP
                if (p->Ptype)
                {   type *t = p->Ptype;
                    t = template_tyident(t, ptalistart, this, 1);
                    e = poptelem3(typechk(e, t));
                    type_free(t);
                }
#endif
                (*pp)->Pelem = e;
            }
            (*pp)->Psym = ptali->Psym;
            (*pp)->Pflags = ptali->Pflags;
            assert(!ptali->Pptpl);
            ptali = ptali->Pnext;
        }
        pp = &(*pp)->Pnext;
    }
    return ptal;
}

/**********************************
 * Look for Pident matching id
 */

param_t *param_t::search(char *id)
{   param_t *p;

    for (p = this; p; p = p->Pnext)
    {
        if (p->Pident && strcmp(p->Pident, id) == 0)
            break;
    }
    return p;
}

/**********************************
 * Look for Pident matching id
 */

int param_t::searchn(char *id)
{   param_t *p;
    int n = 0;

    for (p = this; p; p = p->Pnext)
    {
        if (p->Pident && strcmp(p->Pident, id) == 0)
            return n;
        n++;
    }
    return -1;
}

/*************************************
 * Search for member, create symbol as needed.
 * Used for symbol tables for VLA's such as:
 *      void func(int n, int a[n]);
 */

symbol *param_search(const char *name, param_t **pp)
{   symbol *s = NULL;
    param_t *p;

    p = (*pp)->search((char *)name);
    if (p)
    {
        s = p->Psym;
        if (!s)
        {
            s = symbol_calloc(p->Pident);
            s->Sclass = SCparameter;
            s->Stype = p->Ptype;
            s->Stype->Tcount++;
#if SOURCE_4PARAMS
            s->Ssrcpos = p->Psrcpos;
#endif
            p->Psym = s;
        }
    }
    return s;
}

/**********************************
 * Hydrate/dehydrate a type.
 */

#if HYDRATE
void param_hydrate(param_t **pp)
{
    param_t *p;

    assert(pp);
    if (isdehydrated(*pp))
    {   while (*pp)
        {   assert(isdehydrated(*pp));
            p = (param_t *) ph_hydrate(pp);
#if SOURCE_4PARAMS
            p->Psrcpos.Sfilnum += File_Hydrate_Num;     /* file number relative header build */
#endif
            param_debug(p);

            type_hydrate(&p->Ptype);
            if (p->Ptype)
                type_debug(p->Ptype);
            ph_hydrate(&p->Pident);
            if (CPP)
            {
                el_hydrate(&p->Pelem);
                if (p->Pelem)
                    elem_debug(p->Pelem);
                type_hydrate(&p->Pdeftype);
                if (p->Pptpl)
                    param_hydrate(&p->Pptpl);
                if (p->Psym)
                    symbol_hydrate(&p->Psym);
                if (p->PelemToken)
                    token_hydrate(&p->PelemToken);
            }

            pp = &p->Pnext;
        }
    }
}
#endif

#if DEHYDRATE
void param_dehydrate(param_t **pp)
{
    param_t *p;

    assert(pp);
    while ((p = *pp) != NULL && !isdehydrated(p))
    {   param_debug(p);

        ph_dehydrate(pp);
        if (p->Ptype && !isdehydrated(p->Ptype))
            type_debug(p->Ptype);
        type_dehydrate(&p->Ptype);
        ph_dehydrate(&p->Pident);
        if (CPP)
        {
            el_dehydrate(&p->Pelem);
            type_dehydrate(&p->Pdeftype);
            if (p->Pptpl)
                param_dehydrate(&p->Pptpl);
            if (p->Psym)
                symbol_dehydrate(&p->Psym);
            if (p->PelemToken)
                token_dehydrate(&p->PelemToken);
        }
        pp = &p->Pnext;
    }
}
#endif

/*************************************************
 * A cheap version of exp2.typematch() and exp2.paramlstmatch(),
 * so that we can get cpp_mangle() to work for MARS.
 * It's less complex because it doesn't do templates and
 * can rely on strict typechecking.
 * Returns:
 *      !=0 if types match.
 */

#if MARS

int typematch(type *t1,type *t2,int relax)
{ tym_t t1ty, t2ty;
  tym_t tym;

  tym = ~(mTYimport | mTYnaked);

  return t1 == t2 ||
            t1 && t2 &&

            (
                /* ignore name mangling */
                (t1ty = (t1->Tty & tym)) == (t2ty = (t2->Tty & tym))
            )
                 &&

            (tybasic(t1ty) != TYarray || t1->Tdim == t2->Tdim ||
             t1->Tflags & TFsizeunknown || t2->Tflags & TFsizeunknown)
                 &&

            (tybasic(t1ty) != TYstruct
                && tybasic(t1ty) != TYenum
                && tybasic(t1ty) != TYmemptr
             || t1->Ttag == t2->Ttag)
                 &&

            typematch(t1->Tnext,t2->Tnext, 0)
                 &&

            (!tyfunc(t1ty) ||
             ((t1->Tflags & TFfixed) == (t2->Tflags & TFfixed) &&
                 paramlstmatch(t1->Tparamtypes,t2->Tparamtypes) ))
         ;
}

// Return TRUE if type lists match.

int paramlstmatch(param_t *p1,param_t *p2)
{
        return p1 == p2 ||
            p1 && p2 && typematch(p1->Ptype,p2->Ptype,0) &&
            paramlstmatch(p1->Pnext,p2->Pnext)
            ;
}

#endif

#endif /* !SPP */
