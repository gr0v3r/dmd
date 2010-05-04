// Copyright (C) 1984-1998 by Symantec
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

#include        <stdio.h>
#include        <string.h>
#include        <time.h>
#include        "cc.h"
#include        "oper.h"
#include        "el.h"
#include        "code.h"
#include        "global.h"
#include        "type.h"
#if SCPP
#include        "exh.h"
#endif

static char __file__[] = __FILE__;      /* for tassert.h                */
#include        "tassert.h"

int cdcmp_flag;
extern signed char regtorm[8];

/********************************
 * Return mask of index registers used by addressing mode.
 * Index is rm of modregrm field.
 */

regm_t idxregm(unsigned rm,unsigned sib)
{
    static const unsigned char idxsib[8] = { mAX,mCX,mDX,mBX,0,mBP,mSI,mDI };
    static const unsigned char idxrm[8] = {mBX|mSI,mBX|mDI,mSI,mDI,mSI,mDI,0,mBX};
    regm_t idxm;

    idxm = 0;
    if ((rm & 0xC0) != 0xC0)            /* if register is not the destination */
    {
        if (I32)
        {
            if ((rm & 7) == 4)          /* if sib byte                  */
            {
                idxm = idxsib[(sib >> 3) & 7];  /* scaled index reg     */
                if ((sib & 7) == 5 && (rm & 0xC0) == 0)
                    ;
                else
                    idxm |= idxsib[sib & 7];
            }
            else
                idxm |= idxsib[rm & 7];
        }
        else
            idxm = idxrm[rm & 7];
    }
    return idxm;
}

#if TARGET_WINDOS
/***************************
 * Gen code for call to floating point routine.
 */

code *opdouble(elem *e,regm_t *pretregs,unsigned clib)
{
    regm_t retregs1,retregs2;
    code *cl, *cr, *c;

    if (config.inline8087)
        return orth87(e,pretregs);

    if (tybasic(e->E1->Ety) == TYfloat)
    {
        clib += CLIBfadd - CLIBdadd;    /* convert to float operation   */
        retregs1 = FLOATREGS;
        retregs2 = FLOATREGS2;
    }
    else
    {
        if (I32)
        {   retregs1 = DOUBLEREGS_32;
            retregs2 = DOUBLEREGS2_32;
        }
        else
        {   retregs1 = mSTACK;
            retregs2 = DOUBLEREGS_16;
        }
    }
    cl = codelem(e->E1, &retregs1,FALSE);
    if (retregs1 & mSTACK)
        cgstate.stackclean++;
    cr = scodelem(e->E2, &retregs2, retregs1 & ~mSTACK, FALSE);
    if (retregs1 & mSTACK)
        cgstate.stackclean--;
    c = callclib(e, clib, pretregs, 0);
    return cat3(cl, cr, c);
}
#endif

/*****************************
 * Handle operators which are more or less orthogonal
 * ( + - & | ^ )
 */

code *cdorth(elem *e,regm_t *pretregs)
{ tym_t ty,ty1,ty2;
  regm_t retregs,rretregs,posregs;
  unsigned reg,rreg,op1,op2,mode,test,byte;
  int rval;
  code *c,*cg,*cl,*cr,cs;
  targ_int i;
  elem *e1,*e2;
  int numwords;                         /* # of words to be operated on */
  unsigned char word;                   /* if word operands             */
  int e2oper;
  unsigned sz;
  static int nest;

  //printf("cdorth(e = %p, *pretregs = %s)\n",e,regm_str(*pretregs));
  e1 = e->E1;
  e2 = e->E2;
  if (*pretregs == 0)                   /* if don't want result         */
  {     c = codelem(e1,pretregs,FALSE); /* eval left leaf               */
        *pretregs = 0;                  /* in case they got set         */
        return cat(c,codelem(e2,pretregs,FALSE));
  }

  ty1 = tybasic(e1->Ety);
  if (tyfloating(ty1))
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_SOLARIS
        return orth87(e,pretregs);
#else
        return opdouble(e,pretregs,(e->Eoper == OPadd) ? CLIBdadd
                                                       : CLIBdsub);
#endif
  ty2 = tybasic(e2->Ety);
  e2oper = e2->Eoper;
  ty = tybasic(e->Ety);
  sz = tysize[ty];
  byte = (sz == 1);
  word = (I32 && sz == SHORTSIZE) ? CFopsize : 0;
  cs.Iflags = 0;
  cs.Ijty = 0;
  test = FALSE;                         /* assume we destroyed lvalue   */
  cr = CNIL;                            /* initialize                   */

  switch (e->Eoper)
  {     case OPadd:     mode = 0;
                        op1 = 0x03; op2 = 0x13; break;  /* ADD, ADC     */
        case OPmin:     mode = 5;
                        op1 = 0x2B; op2 = 0x1B; break;  /* SUB, SBB     */
        case OPor:      mode = 1;
                        op1 = 0x0B; op2 = 0x0B; break;  /* OR , OR      */
        case OPxor:     mode = 6;
                        op1 = 0x33; op2 = 0x33; break;  /* XOR, XOR     */
        case OPand:     mode = 4;
                        op1 = 0x23; op2 = 0x23;         /* AND, AND     */
                        if (tyreg(ty1) &&
                            *pretregs == mPSW)          /* if flags only */
                        {       test = TRUE;
                                op1 = 0x85;             /* TEST         */
                                mode = 0;
                        }
                        break;
        default:
                assert(0);
  }
  op1 ^= byte;                                  /* if byte operation    */

  /* Compute number of words to operate on.                             */
  numwords = 1;
  if (I32)
  {     /* Cannot operate on longs and then do a 'paint' to a far       */
        /* pointer, because far pointers are 48 bits and longs are 32.  */
        /* Therefore, numwords can never be 2.                          */
        assert(!(tyfv(ty1) && tyfv(ty2)));
        if (sz == 2 * REGSIZE)
        {
            numwords++;
        }
  }
  else
  {     /* If ty is a TYfptr, but both operands are long, treat the     */
        /* operation as a long.                                         */
        if ((tylong(ty1) || ty1 == TYhptr) &&
            (tylong(ty2) || ty2 == TYhptr))
            numwords++;
  }

  // Special cases where only flags are set
  if (test && tysize[ty1] <= REGSIZE &&
      (e1->Eoper == OPvar || (e1->Eoper == OPind && !e1->Ecount)))
  {
        // Handle the case of (var & const)
        if (e2->Eoper == OPconst)
        {   targ_int value;

            c = getlvalue(&cs,e1,0);
            value = e2->EV.Vint;
            if (sz == 2)
                value &= 0xFFFF;
            if (reghasvalue(byte ? BYTEREGS : ALLREGS,value,&reg))
                goto L11;
            op1 = 0xF7;
            cs.IEV2.Vint = value;
            cs.IFL2 = FLconst;
            goto L10;
        }

        // Handle (exp & reg)
        if (isregvar(e2,&retregs,&reg))
        {
            c = getlvalue(&cs,e1,0);
        L11:
            cs.Irm |= modregrm(0,reg,0);
        L10:
            cs.Iop = op1 ^ byte;
            cs.Iflags |= word | CFpsw;
            freenode(e1);
            freenode(e2);
            return gen(c,&cs);
        }
  }

  // Look for possible uses of LEA
  if (e->Eoper == OPadd &&
      !(*pretregs & mPSW) &&            /* flags aren't set by LEA      */
      !nest &&                          // could cause infinite recursion if e->Ecount
      sz == REGSIZE)                    // far pointers aren't handled
  {     int e1oper;

        // Handle the case of (e + &var)
        e1oper = e1->Eoper;
        if ((e2oper == OPrelconst && (config.target_cpu >= TARGET_Pentium || (!e2->Ecount && stackfl[el_fl(e2)])))
                || // LEA costs too much for simple EAs on older CPUs
            (e2oper == OPconst && (e1->Eoper == OPcall || e1->Eoper == OPcallns) && !(*pretregs & mAX)) ||
            (I32 && (isscaledindex(e1) || isscaledindex(e2))) ||
            (I32 && e1oper == OPvar && e1->EV.sp.Vsym->Sfl == FLreg && (e2oper == OPconst || (e2oper == OPvar && e2->EV.sp.Vsym->Sfl == FLreg))) ||
            (e2oper == OPconst && e1oper == OPeq && e1->E1->Eoper == OPvar) ||
            (I32 && e2oper == OPrelconst && !e1->Ecount &&
             (e1oper == OPmul || e1oper == OPshl) &&
             e1->E2->Eoper == OPconst &&
             ssindex(e1oper,e1->E2->EV.Vuns)
            ) ||
            (I32 && e1->Ecount)
           )
        {   int inc;

            inc = e->Ecount != 0;
            nest += inc;
            c = getlvalue(&cs,e,0);
            nest -= inc;
            c = cat(c,allocreg(pretregs,&reg,ty));
            cs.Iop = 0x8D;
            cs.Irm |= modregrm(0,reg,0);
            return gen(c,&cs);          /* LEA reg,EA                   */
        }

        // Handle the case of ((e + c) + e2)
        if (I32 &&
            e1oper == OPadd &&
            (e1->E2->Eoper == OPconst || e2oper == OPconst) &&
            !e1->Ecount
           )
        {   elem *e11;
            elem *ebase;
            elem *edisp;
            int ss;
            int ss2;
            unsigned reg1,reg2;
            code *c1,*c2,*c3;

            if (e2oper == OPconst)
            {   edisp = e2;
                ebase = e1->E2;
            }
            else
            {   edisp = e1->E2;
                ebase = e2;
            }

            e11 = e1->E1;
            retregs = *pretregs & ALLREGS;
            if (!retregs)
                retregs = ALLREGS;
            ss = 0;
            ss2 = 0;

            // Handle the case of (((e *  c1) + c2) + e2)
            // Handle the case of (((e << c1) + c2) + e2)
            if ((e11->Eoper == OPmul || e11->Eoper == OPshl) &&
                e11->E2->Eoper == OPconst &&
                !e11->Ecount
               )
            {   targ_size_t co1;

                co1 = el_tolong(e11->E2);
                if (e11->Eoper == OPshl)
                {
                    if (co1 > 3)
                        goto L13;
                    ss = co1;
                }
                else
                {
                    ss2 = 1;
                    switch (co1)
                    {
                        case  6:        ss = 1;                 break;
                        case 12:        ss = 1; ss2 = 2;        break;
                        case 24:        ss = 1; ss2 = 3;        break;
                        case 10:        ss = 2;                 break;
                        case 20:        ss = 2; ss2 = 2;        break;
                        case 40:        ss = 2; ss2 = 3;        break;
                        case 18:        ss = 3;                 break;
                        case 36:        ss = 3; ss2 = 2;        break;
                        case 72:        ss = 3; ss2 = 3;        break;
                        default:
                            ss2 = 0;
                            goto L13;
                    }
                }
                freenode(e11->E2);
                freenode(e11);
                e11 = e11->E1;
                goto L13;
            }
            else
            {   regm_t regm;
            L13:
                if (e11->Eoper == OPvar && isregvar(e11,&regm,&reg1))
                {
                    retregs = regm;
                    c1 = NULL;
                    freenode(e11);
                }
                else
                    c1 = codelem(e11,&retregs,FALSE);
            }
            rretregs = ALLREGS & ~retregs;
            c2 = scodelem(ebase,&rretregs,retregs,TRUE);
            {   regm_t sregs;

                sregs = *pretregs & ~rretregs;
                if (!sregs)
                    sregs = ALLREGS & ~rretregs;
                c3 = allocreg(&sregs,&reg,ty);
            }

            reg1 = findreg(retregs);
            reg2 = findreg(rretregs);

            if (ss2)
            {
                assert(reg != reg2);
                if (reg1 == BP)
                {   static unsigned imm32[4] = {1+1,2+1,4+1,8+1};

                    // IMUL reg,imm32
                    c = genc2(CNIL,0x69,modregrm(3,reg,reg1),imm32[ss]);
                }
                else
                {   // LEA reg,[reg1*ss][reg1]
                    c = gen2sib(CNIL,0x8D,modregrm(0,reg,4),modregrm(ss,reg1,reg1));
                }
                reg1 = reg;
                ss = ss2;                               // use *2 for scale
            }
            else
                c = NULL;
            c = cat4(c1,c2,c3,c);

            cs.Iop = 0x8D;                      // LEA reg,c[reg1*ss][reg2]
            cs.Irm = modregrm(2,reg,4);
            cs.Isib = modregrm(ss,reg1,reg2);
            cs.Iflags = CFoff;
            cs.Ijty = 0;
            cs.IFL1 = FLconst;
            cs.IEV1.Vuns = edisp->EV.Vuns;

            freenode(edisp);
            freenode(e1);
            c = gen(c,&cs);
            return cat(c,fixresult(e,mask[reg],pretregs));
        }
  }

  posregs = (byte) ? BYTEREGS : (mES | ALLREGS | mBP);
  retregs = *pretregs & posregs;
  if (retregs == 0)                     /* if no return regs speced     */
                                        /* (like if wanted flags only)  */
        retregs = ALLREGS & posregs;    // give us some

  if (tysize[ty1] > REGSIZE && numwords == 1)
  {     /* The only possibilities are (TYfptr + tyword) or (TYfptr - tyword) */
#if DEBUG
        if (tysize[ty2] != REGSIZE)
        {       printf("e = %p, e->Eoper = ",e);
                WROP(e->Eoper);
                printf(" e1->Ety = ");
                WRTYxx(ty1);
                printf(" e2->Ety = ");
                WRTYxx(ty2);
                printf("\n");
                elem_print(e);
        }
#endif
        assert(tysize[ty2] == REGSIZE);

        /* Watch out for the case here where you are going to OP reg,EA */
        /* and both the reg and EA use ES! Prevent this by forcing      */
        /* reg into the regular registers.                              */
        if ((e2oper == OPind ||
            (e2oper == OPvar && el_fl(e2) == FLfardata)) &&
            !e2->Ecount)
        {
                retregs = ALLREGS;
                assert(!TARGET_FLAT);
        }

        cl = codelem(e1,&retregs,test);
        reg = findreglsw(retregs);      /* reg is the register with the offset*/
  }
  else if (ty1 == TYhptr || ty2 == TYhptr)
  {     /* Generate code for add/subtract of huge pointers.
           No attempt is made to generate very good code.
         */
        unsigned mreg,lreg;
        unsigned lrreg;

        retregs = (retregs & mLSW) | mDX;
        if (ty1 == TYhptr)
        {   // hptr +- long
            rretregs = mLSW & ~(retregs | regcon.mvar);
            if (!rretregs)
                rretregs = mLSW;
            rretregs |= mCX;
            cl = codelem(e1,&rretregs,0);
            retregs &= ~rretregs;
            if (!(retregs & mLSW))
                retregs |= mLSW & ~rretregs;

            cr = scodelem(e2,&retregs,rretregs,TRUE);
        }
        else
        {   // long + hptr
            cl = codelem(e1,&retregs,0);
            rretregs = (mLSW | mCX) & ~retregs;
            if (!(rretregs & mLSW))
                rretregs |= mLSW;
            cr = scodelem(e2,&rretregs,retregs,TRUE);
        }
        cg = getregs(rretregs | retregs);
        mreg = DX;
        lreg = findreglsw(retregs);
        c = CNIL;
        if (e->Eoper == OPmin)
        {   // negate retregs
            c = gen2(c,0xF7,modregrm(3,3,mreg));        // NEG mreg
            gen2(c,0xF7,modregrm(3,3,lreg));            // NEG lreg
            code_orflag(c,CFpsw);
            genc2(c,0x81,modregrm(3,3,mreg),0);         // SBB mreg,0
        }
        lrreg = findreglsw(rretregs);
        c = genregs(c,0x03,lreg,lrreg);         // ADD lreg,lrreg
        code_orflag(c,CFpsw);
        genmovreg(c,lrreg,CX);                  // MOV lrreg,CX
        genc2(c,0x81,modregrm(3,2,mreg),0);     // ADC mreg,0
        genshift(c);                            // MOV CX,offset __AHSHIFT
        gen2(c,0xD3,modregrm(3,4,mreg));        // SHL mreg,CL
        genregs(c,0x03,mreg,lrreg);             // ADD mreg,MSREG(h)
        goto L5;
  }
  else
  {     regm_t regm;

        /* if (tyword + TYfptr) */
        if (tysize[ty1] == REGSIZE && tysize[ty2] > REGSIZE)
        {   retregs = ~*pretregs & ALLREGS;

            /* if retregs doesn't have any regs in it that aren't reg vars */
            if ((retregs & ~regcon.mvar) == 0)
                retregs |= mAX;
        }
        else if (numwords == 2 && retregs & mES)
            retregs = (retregs | mMSW) & ALLREGS;

        // Determine if we should swap operands, because
        //      mov     EAX,x
        //      add     EAX,reg
        // is faster than:
        //      mov     EAX,reg
        //      add     EAX,x
        else if (e2oper == OPvar &&
                 e1->Eoper == OPvar &&
                 e->Eoper != OPmin &&
                 isregvar(e1,&regm,NULL) &&
                 regm != retregs &&
                 tysize[ty1] == tysize[ty2])
        {   elem *es;

            es = e1;
            e1 = e2;
            e2 = es;
        }
        cl = codelem(e1,&retregs,test);         /* eval left leaf       */
        reg = findreg(retregs);
  }
  switch (e2oper)
  {
    case OPind:                                 /* if addressing mode   */
        if (!e2->Ecount)                        /* if not CSE           */
                goto L1;                        /* try OP reg,EA        */
        /* FALL-THROUGH */
    default:                                    /* operator node        */
    L2:
        rretregs = ALLREGS & ~retregs;
        /* Be careful not to do arithmetic on ES        */
        if (tysize[ty1] == REGSIZE && tysize[ty2] > REGSIZE && *pretregs != mPSW)
            rretregs = *pretregs & (mES | ALLREGS | mBP) & ~retregs;
        else if (byte)
            rretregs &= BYTEREGS;

        cr = scodelem(e2,&rretregs,retregs,TRUE);       /* get rvalue   */
        rreg = (tysize[ty2] > REGSIZE) ? findreglsw(rretregs) : findreg(rretregs);
        c = CNIL;
        if (numwords == 1)                              /* ADD reg,rreg */
        {
                /* reverse operands to avoid moving around the segment value */
                if (tysize[ty2] > REGSIZE)
                {       c = cat(c,getregs(rretregs));
                        c = genregs(c,op1,rreg,reg);
                        retregs = rretregs;     /* reverse operands     */
                }
                else
                {   c = genregs(c,op1,reg,rreg);
                    if (I32 && *pretregs & mPSW)
                        c->Iflags |= word;
                }
        }
        else /* numwords == 2 */                /* ADD lsreg,lsrreg     */
        {
            reg = findreglsw(retregs);
            rreg = findreglsw(rretregs);
            c = genregs(c,op1,reg,rreg);
            if (e->Eoper == OPadd || e->Eoper == OPmin)
                code_orflag(c,CFpsw);
            reg = findregmsw(retregs);
            rreg = findregmsw(rretregs);
            if (!(e2oper == OPu16_32 && // if second operand is 0
                  (op2 == 0x0B || op2 == 0x33)) // and OR or XOR
               )
                genregs(c,op2,reg,rreg);        // ADC msreg,msrreg
        }
        break;

    case OPrelconst:
        if (sz != REGSIZE)
                goto L2;
        if (segfl[el_fl(e2)] != 3)              /* if not in data segment */
                goto L2;
        if (evalinregister(e2))
                goto L2;
        cs.IEVoffset2 = e2->EV.sp.Voffset;
        cs.IEVsym2 = e2->EV.sp.Vsym;
        cs.Iflags |= CFoff;
        i = 0;                          /* no INC or DEC opcode         */
        rval = 0;
        goto L3;

    case OPconst:
        if (tyfv(ty2))
            goto L2;
        if (numwords == 1)
        {
                i = e2->EV.Vint;
                if (word)
                {
                    if (!(*pretregs & mPSW) &&
                        config.flags4 & CFG4speed &&
                        (e->Eoper == OPor || e->Eoper == OPxor || test ||
                         (e1->Eoper != OPvar && e1->Eoper != OPind)))
                    {   word = 0;
                        i &= 0xFFFF;
                    }
                }
                rval = reghasvalue(byte ? BYTEREGS : ALLREGS,i,&rreg);
                cs.IEV2.Vint = i;
        L3:
                op1 ^= byte;
                cs.Iflags |= word;
                if (rval)
                {   cs.Iop = op1 ^ 2;
                    mode = rreg;
                }
                else
                    cs.Iop = 0x81;
                cs.Irm = modregrm(3,mode,reg);
                cs.IFL2 = (e2->Eoper == OPconst) ? FLconst : el_fl(e2);
                /* Modify instruction for special cases */
                switch (e->Eoper)
                {   case OPadd:
                    {   int iop;

                        if (i == 1)
                            iop = 0;                    /* INC reg      */
                        else if (i == -1)
                            iop = 8;                    /* DEC reg      */
                        else
                            break;
                        cs.Iop = (0x40 | iop | reg) ^ byte;
                        if (byte && *pretregs & mPSW)
                        {   cs.Irm = modregrm(3,0,reg) | iop;
                            cs.Iop = 0xFF;
                        }
                        break;
                    }
                    case OPand:
                        if (test)
                            cs.Iop = rval ? op1 : 0xF7; // TEST
                        break;
                }
                if (*pretregs & mPSW)
                        cs.Iflags |= CFpsw;
                cs.Iop ^= byte;
                c = gen(CNIL,&cs);
                cs.Iflags &= ~CFpsw;
        }
        else if (numwords == 2)
        {       unsigned lsreg;
                targ_int msw;

                c = getregs(retregs);
                reg = findregmsw(retregs);
                lsreg = findreglsw(retregs);
                cs.Iop = 0x81;
                cs.Irm = modregrm(3,mode,lsreg);
                cs.IFL2 = FLconst;
                msw = MSREG(e2->EV.Vllong);
                cs.IEV2.Vint = e2->EV.Vlong;
                switch (e->Eoper)
                {   case OPadd:
                    case OPmin:
                        cs.Iflags |= CFpsw;
                        break;
                }
                c = gen(c,&cs);
                cs.Iflags &= ~CFpsw;

                cs.Irm = (cs.Irm & modregrm(3,7,0)) | reg;
                cs.IEV2.Vint = msw;
                if (e->Eoper == OPadd)
                        cs.Irm |= modregrm(0,2,0);      /* ADC          */
                c = gen(c,&cs);
        }
        else
                assert(0);
        freenode(e2);
        break;

    case OPvar:
    L1:
        if (tyfv(ty2))
                goto L2;
        c = loadea(e2,&cs,op1,
                ((numwords == 2) ? findreglsw(retregs) : reg),
                0,retregs,retregs);
        if (I32 && word)
        {   if (*pretregs & mPSW)
                code_orflag(c,word);
            else
            {   code *ce;

                ce = code_last(c);
                ce->Iflags &= ~word;
            }
        }
        else if (numwords == 2)
        {
            if (e->Eoper == OPadd || e->Eoper == OPmin)
                code_orflag(c,CFpsw);
            reg = findregmsw(retregs);
            if (EOP(e2))
            {   getlvalue_msw(&cs);
                cs.Iop = op2;
                NEWREG(cs.Irm,reg);
                c = gen(c,&cs);                 /* ADC reg,data+2 */
            }
            else
                c = cat(c,loadea(e2,&cs,op2,reg,REGSIZE,retregs,0));
        }
        freenode(e2);
        break;
  }
  if (sz <= REGSIZE && *pretregs & mPSW)
  {     code_orflag(c,CFpsw);
        *pretregs &= ~mPSW;                     /* flags already set    */
  }
  if (test)
        cg = NULL;                      /* didn't destroy any           */
  else
        cg = getregs(retregs);          /* we will trash these regs     */
L5:
  c = cat(c,fixresult(e,retregs,pretregs));
  return cat4(cl,cr,cg,c);
}

/*****************************
 * Handle multiply, divide, modulo and remquo.
 * Note that modulo isn't defined for doubles.
 */

code *cdmul(elem *e,regm_t *pretregs)
{   unsigned rreg,op,oper,lib,byte;
    regm_t resreg,retregs,rretregs;
    regm_t keepregs;
    tym_t uns;                          // 1 if unsigned operation, 0 if not
    tym_t tyml;
    code *c,*cg,*cl,*cr,cs;
    elem *e1,*e2;
    int sz;
    targ_size_t e2factor;
    int opunslng;
    int pow2;

    if (*pretregs == 0)                         // if don't want result
    {   c = codelem(e->E1,pretregs,FALSE);      // eval left leaf
        *pretregs = 0;                          // in case they got set
        return cat(c,codelem(e->E2,pretregs,FALSE));
    }

    keepregs = 0;
    cs.Iflags = 0;
    cs.Ijty = 0;
    c = cg = cr = CNIL;                         // initialize
    e2 = e->E2;
    e1 = e->E1;
    tyml = tybasic(e1->Ety);
    sz = tysize[tyml];
    byte = tybyte(e->Ety) != 0;
    uns = tyuns(tyml) || tyuns(e2->Ety);
    oper = e->Eoper;

    if (tyfloating(tyml))
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_SOLARIS
        return orth87(e,pretregs);
#else
        return opdouble(e,pretregs,(oper == OPmul) ? CLIBdmul : CLIBddiv);
#endif

    opunslng = I32 ? OPu32_64 : OPu16_32;
    switch (oper)
    {
        case OPmul:
            resreg = mAX;
            op = 5 - uns;
            lib = CLIBlmul;
            break;

        case OPdiv:
            resreg = mAX;
            op = 7 - uns;
            lib = uns ? CLIBuldiv : CLIBldiv;
            if (I32)
                keepregs |= mSI | mDI;
            break;

        case OPmod:
            resreg = mDX;
            op = 7 - uns;
            lib = uns ? CLIBulmod : CLIBlmod;
            if (I32)
                keepregs |= mSI | mDI;
            break;

        case OPremquo:
            resreg = mDX | mAX;
            op = 7 - uns;
            lib = uns ? CLIBuldiv : CLIBldiv;
            if (I32)
                keepregs |= mSI | mDI;
            break;

        default:
            assert(0);
    }

    if (sz <= REGSIZE)                  // dedicated regs for mul & div
    {   retregs = mAX;
        /* pick some other regs */
        rretregs = byte ? BYTEREGS & ~mAX
                        : ALLREGS & ~(mAX|mDX);
    }
    else
    {
        assert(sz <= 2 * REGSIZE);
        retregs = mDX | mAX;
        rretregs = mCX | mBX;           // second arg
    }

  switch (e2->Eoper)
  {
    case OPu16_32:
    case OPshtlng:
    case OPulngllng:
    case OPlngllng:
        if (sz != 2 * REGSIZE || oper != OPmul || e1->Eoper != e2->Eoper ||
            e1->Ecount || e2->Ecount)
            goto L2;
        op = (e2->Eoper == opunslng) ? 4 : 5;
        retregs = mAX;
        cl = codelem(e1->E1,&retregs,FALSE);    /* eval left leaf       */
        if (e2->E1->Eoper == OPvar ||
            (e2->E1->Eoper == OPind && !e2->E1->Ecount)
           )
        {
            cr = loadea(e2->E1,&cs,0xF7,op,0,mAX,mAX | mDX);
        }
        else
        {
            rretregs = ALLREGS & ~mAX;
            cr = scodelem(e2->E1,&rretregs,retregs,TRUE); // get rvalue
            cg = getregs(mAX | mDX);
            rreg = findreg(rretregs);
            cg = gen2(cg,0xF7,modregrm(3,op,rreg)); // OP AX,rreg
        }
        freenode(e->E1);
        freenode(e2);
        c = fixresult(e,mAX | mDX,pretregs);
        break;

    case OPconst:
        e2factor = el_tolong(e2);

        if (oper == OPmul && I32 && sz == REGSIZE * 2)
        {   targ_int msw,lsw;
            regm_t scratch;
            unsigned reg;
            targ_llong e2factor;

            cl = codelem(e1,&retregs,FALSE);    // eval left leaf
            /*  IMUL    EDX,EDX,lsw
                IMUL    reg,EAX,msw
                ADD     reg,EDX
                MOV     EDX,lsw
                MUL     EDX
                ADD     EDX,reg

                if (msw == 0)
                IMUL    reg,EDX,lsw
                MOV     EDX,lsw
                MUL     EDX
                ADD     EDX,reg
             */
            scratch = allregs & ~(mAX | mDX);
            cr = allocreg(&scratch,&reg,TYint);
            cg = getregs(mDX | mAX);

            e2factor = el_tolong(e2);
            lsw = e2factor & ((1LL << (REGSIZE * 8)) - 1);
            msw = e2factor >> (REGSIZE * 8);

            if (msw)
            {   cg = genmulimm(cg,DX,DX,lsw);
                cg = genmulimm(cg,reg,AX,msw);
                cg = gen2(cg,0x03,modregrm(3,reg,DX));
            }
            else
                cg = genmulimm(cg,reg,DX,lsw);

            cg = movregconst(cg,DX,lsw,0);              // MOV EDX,lsw
            cg = cat(cg,getregs(mDX));
            cg = gen2(cg,0xF7,modregrm(3,4,DX));        // MUL EDX
            gen2(cg,0x03,modregrm(3,DX,reg));           // ADD EDX,reg

            resreg = mDX | mAX;
            freenode(e2);
            goto L3;
        }

        if (oper != OPmul && e2factor == 10 && sz == REGSIZE &&
            config.flags4 & CFG4speed && !uns)
        {
            /* R1 / 10
             *
             *  MOV     EAX,0x66666667
             *  IMUL    R1
             *  MOV     EAX,R1
             *  SAR     EAX,31
             *  SAR     EDX,2
             *  SUB     EDX,EAX
             *  IMUL    EAX,EDX,10
             *  SUB     R1,EAX
             *
             * EDX = quotient
             * R1 = remainder
             */
            regm_t regm;
            unsigned reg;

            regm = allregs & ~(mAX | mDX);
            cl = codelem(e1,&regm,FALSE);       // eval left leaf
            reg = findreg(regm);
            cg = getregs(regm | mDX | mAX);

            cg = movregconst(cg, AX, 0x66666667, 0);    // MOV EAX,0x66666667
            cg = gen2(cg,0xF7,modregrm(3,5,reg));       // IMUL R1
            genmovreg(cg, AX, reg);                     // MOV EAX,R1
            genc2(cg,0xC1,modregrm(3,7,AX),31);         // SAR EAX,31
            genc2(cg,0xC1,modregrm(3,7,DX),2);          // SAR EDX,2
            gen2(cg,0x2B,modregrm(3,DX,AX));            // SUB EDX,EAX

            switch (oper)
            {   case OPdiv:
                    resreg = mDX;
                    break;

                case OPmod:
                    genmulimm(cg,AX,DX,10);             // IMUL EAX,EDX,10
                    gen2(cg,0x2B,modregrm(3,reg,AX));   // SUB R1,EAX
                    resreg = regm;
                    break;

                case OPremquo:
                    genmulimm(cg,AX,DX,10);             // IMUL EAX,EDX,10
                    gen2(cg,0x2B,modregrm(3,reg,AX));   // SUB R1,EAX
                    genmovreg(cg, AX, DX);              // MOV EAX,EDX
                    genmovreg(cg, DX, reg);             // MOV EDX,R1
                    resreg = mDX | mAX;
                    break;

                default:
                    assert(0);
            }
            freenode(e2);
            goto L3;
        }

        if (sz > REGSIZE)
            goto L2;

        if (oper == OPmul && config.target_cpu >= TARGET_80286)
        {   unsigned reg;
            int ss;

            freenode(e2);
            retregs = byte ? BYTEREGS : ALLREGS;
            resreg = *pretregs & (ALLREGS | mBP);
            if (!resreg)
                resreg = retregs;

            if (I32)
            {   // See if we can use an LEA instruction
                int ss2 = 0;
                int shift;

                switch (e2factor)
                {
                    case 12:    ss = 1; ss2 = 2; goto L4;
                    case 24:    ss = 1; ss2 = 3; goto L4;

                    case 6:
                    case 3:     ss = 1; goto L4;

                    case 20:    ss = 2; ss2 = 2; goto L4;
                    case 40:    ss = 2; ss2 = 3; goto L4;

                    case 10:
                    case 5:     ss = 2; goto L4;

                    case 36:    ss = 3; ss2 = 2; goto L4;
                    case 72:    ss = 3; ss2 = 3; goto L4;

                    case 18:
                    case 9:     ss = 3; goto L4;

                    L4:
                    {
#if 1
                        regm_t regm;
                        int r;

                        regm = byte ? BYTEREGS : ALLREGS;               // don't use EBP
                        cl = codelem(e->E1,&regm,TRUE);
                        r = findreg(regm);

                        if (ss2)
                        {   // Don't use EBP
                            resreg &= ~mBP;
                            if (!resreg)
                                resreg = retregs;
                        }
                        cg = allocreg(&resreg,&reg,tyml);

                        c = gen2sib(CNIL,0x8D,modregrm(0,reg,4),
                                              modregrm(ss,r,r));
                        if (ss2)
                        {
                            gen2sib(c,0x8D,modregrm(0,reg,4),
                                           modregrm(ss2,reg,5));
                            code_last(c)->IFL1 = FLconst;
                            code_last(c)->IEV1.Vint = 0;
                        }
                        else if (!(e2factor & 1))    // if even factor
                            genregs(c,0x03,reg,reg); // ADD reg,reg
                        cg = cat(cg,c);
                        goto L3;
#else

                                // Don't use EBP
                                resreg &= ~mBP;
                                if (!resreg)
                                    resreg = retregs;

                                cl = codelem(e->E1,&resreg,FALSE);
                                reg = findreg(resreg);
                                cg = getregs(resreg);
                                c = gen2sib(CNIL,0x8D,modregrm(0,reg,4),
                                                      modregrm(ss,reg,reg));
                                if (ss2)
                                {
                                    gen2sib(c,0x8D,modregrm(0,reg,4),
                                                   modregrm(ss2,reg,5));
                                    code_last(c)->IFL1 = FLconst;
                                    code_last(c)->IEV1.Vint = 0;
                                }
                                else if (!(e2factor & 1))    // if even factor
                                    genregs(c,0x03,reg,reg); // ADD reg,reg
                                cg = cat(cg,c);
                                goto L3;
#endif
                    }
                    case 37:
                    case 74:    shift = 2;
                                goto L5;
                    case 13:
                    case 26:    shift = 0;
                                goto L5;
                    L5:
                    {           regm_t sregm;
                                unsigned sreg;

                                // Don't use EBP
                                resreg &= ~mBP;
                                if (!resreg)
                                    resreg = retregs;
                                cl = allocreg(&resreg,&reg,TYint);

                                sregm = ALLREGS & ~resreg;
                                cl = cat(cl,codelem(e->E1,&sregm,FALSE));
                                sreg = findreg(sregm);
                                cg = getregs(resreg | sregm);
                                // LEA reg,[sreg * 4][sreg]
                                // SHL sreg,shift
                                // LEA reg,[sreg * 8][reg]
                                c = gen2sib(CNIL,0x8D,modregrm(0,reg,4),
                                                      modregrm(2,sreg,sreg));
                                if (shift)
                                    genc2(c,0xC1,modregrm(3,4,sreg),shift);
                                gen2sib(c,0x8D,modregrm(0,reg,4),
                                                      modregrm(3,sreg,reg));
                                if (!(e2factor & 1))         // if even factor
                                    genregs(c,0x03,reg,reg); // ADD reg,reg
                                cg = cat(cg,c);
                                goto L3;
                    }
                }
            }

            cl = scodelem(e->E1,&retregs,0,TRUE);       // eval left leaf
            reg = findreg(retregs);
            cg = allocreg(&resreg,&rreg,e->Ety);

            /* IMUL reg,imm16   */
            cg = genc2(cg,0x69,modregrm(3,rreg,reg),e2factor);
            goto L3;
        }

        // Special code for signed divide or modulo by power of 2
        if (sz == REGSIZE && (oper == OPdiv || oper == OPmod) && !uns &&
            (pow2 = ispow2(e2factor)) != -1 &&
            !(config.target_cpu < TARGET_80286 && pow2 != 1 && oper == OPdiv)
           )
        {
            if (pow2 == 1 && oper == OPdiv && config.target_cpu > TARGET_80386)
            {
                //     test    eax,eax
                //     jns     L1
                //     add     eax,1
                // L1: sar     eax,1

                code *cnop;
                unsigned reg;

                retregs = allregs;
                cl = codelem(e->E1,&retregs,FALSE);     // eval left leaf
                reg = findreg(retregs);
                freenode(e2);
                cg = getregs(retregs);
                cg = gentstreg(cg,reg);                 // TEST reg,reg
                cnop = gennop(CNIL);
                genjmp(cg,JNS,FLcode,(block *)cnop);    // JNS cnop
                gen1(cg,0x40 + reg);                    // INC reg
                cg = cat(cg,cnop);
                gen2(cg,0xD1,modregrm(3,7,reg));        // SAR reg,1
                resreg = retregs;
                goto L3;
            }
            cl = codelem(e->E1,&retregs,FALSE); // eval left leaf
            freenode(e2);
            cg = getregs(mAX | mDX);            // trash these regs
            cg = gen1(cg,0x99);                         // CWD
            if (pow2 == 1)
            {
                if (oper == OPdiv)
                {   gen2(cg,0x2B,modregrm(3,AX,DX));    // SUB AX,DX
                    gen2(cg,0xD1,modregrm(3,7,AX));     // SAR AX,1
                }
                else // OPmod
                {   gen2(cg,0x33,modregrm(3,AX,DX));    // XOR AX,DX
                    genc2(cg,0x81,modregrm(3,4,AX),1);  // AND AX,1
                    gen2(cg,0x03,modregrm(3,DX,AX));    // ADD DX,AX
                }
            }
            else
            {   targ_ulong m;

                m = (1 << pow2) - 1;
                if (oper == OPdiv)
                {   genc2(cg,0x81,modregrm(3,4,DX),m);  // AND DX,m
                    gen2(cg,0x03,modregrm(3,AX,DX));    // ADD AX,DX
                    // Be careful not to generate this for 8088
                    assert(config.target_cpu >= TARGET_80286);
                    genc2(cg,0xC1,modregrm(3,7,AX),pow2); // SAR AX,pow2
                }
                else // OPmod
                {   gen2(cg,0x33,modregrm(3,AX,DX));    // XOR AX,DX
                    gen2(cg,0x2B,modregrm(3,AX,DX));    // SUB AX,DX
                    genc2(cg,0x81,modregrm(3,4,AX),m);  // AND AX,mask
                    gen2(cg,0x33,modregrm(3,AX,DX));    // XOR AX,DX
                    gen2(cg,0x2B,modregrm(3,AX,DX));    // SUB AX,DX
                    resreg = mAX;
                }
            }
            goto L3;
        }
        goto L2;
    case OPind:
        if (!e2->Ecount)                        /* if not CSE           */
                goto L1;                        /* try OP reg,EA        */
        goto L2;
    default:                                    /* OPconst and operators */
    L2:
        cl = codelem(e1,&retregs,FALSE);        /* eval left leaf       */
        cr = scodelem(e2,&rretregs,retregs,TRUE);       /* get rvalue   */
        if (sz <= REGSIZE)
        {   cg = getregs(mAX | mDX);            /* trash these regs     */
            if (op == 7)                        /* signed divide        */
                cg = gen1(cg,0x99);             /* CWD                  */
            else if (op == 6)                   /* unsigned divide      */
            {   cg = movregconst(cg,DX,0,0);    // MOV DX,0
                cg = cat(cg,getregs(mDX));
            }
            rreg = findreg(rretregs);
            cg = gen2(cg,0xF7 ^ byte,modregrm(3,op,rreg)); /* OP AX,rreg */
        L3:
            c = fixresult(e,resreg,pretregs);
        }
        else if (sz == 2 * REGSIZE)
        {
            if (config.target_cpu >= TARGET_PentiumPro && oper == OPmul)
            {
                /*  IMUL    ECX,EAX
                    IMUL    EDX,EBX
                    ADD     ECX,EDX
                    MUL     EBX
                    ADD     EDX,ECX
                 */
                 cg = getregs(mAX|mDX|mCX);
                 cg = gen2(cg,0x0FAF,modregrm(3,CX,AX));
                 gen2(cg,0x0FAF,modregrm(3,DX,BX));
                 gen2(cg,0x03,modregrm(3,CX,DX));
                 gen2(cg,0xF7,modregrm(3,4,BX));
                 gen2(cg,0x03,modregrm(3,DX,CX));
                 c = fixresult(e,mDX|mAX,pretregs);
            }
            else
                c = callclib(e,lib,pretregs,keepregs);
        }
        else
                assert(0);
        break;
    case OPvar:
    L1:
        if (I32 && sz <= REGSIZE)
        {
            if (oper == OPmul && sz > 1)        /* no byte version      */
            {
                /* Generate IMUL r32,r/m32      */
                retregs = *pretregs & (ALLREGS | mBP);
                if (!retregs)
                    retregs = ALLREGS;
                cl = codelem(e1,&retregs,FALSE);        /* eval left leaf */
                resreg = retregs;
                cr = loadea(e2,&cs,0x0FAF,findreg(resreg),0,retregs,retregs);
                freenode(e2);
                goto L3;
            }
        }
        else
        {
            if (sz == 2 * REGSIZE)
            {   int reg;

                if (oper != OPmul || e->E1->Eoper != opunslng ||
                    e1->Ecount)
                    goto L2;            // have to handle it with codelem()

                retregs = ALLREGS & ~(mAX | mDX);
                cl = codelem(e1->E1,&retregs,FALSE);    // eval left leaf
                reg = findreg(retregs);
                cl = cat(cl,getregs(mAX));
                cl = genmovreg(cl,AX,reg);              // MOV AX,reg
                cr = loadea(e2,&cs,0xF7,4,REGSIZE,mAX | mDX | mskl(reg),mAX | mDX);     // MUL EA+2
                cg = getregs(retregs);
                cg = gen1(cg,0x90 + reg);               // XCHG AX,reg
                cg = cat(cg,getregs(mAX | mDX));
                if ((cs.Irm & 0xC0) == 0xC0)            // if EA is a register
                    cg = cat(cg,loadea(e2,&cs,0xF7,4,0,mAX | mskl(reg),mAX | mDX)); // MUL EA
                else
                {   getlvalue_lsw(&cs);
                    gen(cg,&cs);                        // MUL EA
                }
                gen2(cg,0x03,modregrm(3,DX,reg));       // ADD DX,reg

                freenode(e1);
                c = fixresult(e,mAX | mDX,pretregs);
                break;
            }
            assert(sz <= REGSIZE);
        }

        /* loadea() handles CWD or CLR DX for divides   */
        cl = codelem(e->E1,&retregs,FALSE);     /* eval left leaf       */
        cr = loadea(e2,&cs,0xF7 ^ byte,op,0,
                (oper == OPmul) ? mAX : mAX | mDX,
                mAX | mDX);
        freenode(e2);
        goto L3;
  }
  return cat4(cl,cr,cg,c);
}

/***************************
 * Handle OPnot and OPbool.
 * Generate:
 *      c:      [evaluate e1]
 *      cfalse: [save reg code]
 *              clr     reg
 *              jmp     cnop
 *      ctrue:  [save reg code]
 *              clr     reg
 *              inc     reg
 *      cnop:   nop
 */

code *cdnot(elem *e,regm_t *pretregs)
{   unsigned reg;
    tym_t forflags;
    code *c1,*c,*cfalse,*ctrue,*cnop;
    unsigned sz;
    regm_t retregs;
    elem *e1;
    int op;

    e1 = e->E1;
    if (*pretregs == 0)
        goto L1;
    if (*pretregs == mPSW)
    {   /*assert(e->Eoper != OPnot && e->Eoper != OPbool);*/ /* should've been optimized */
    L1:
        return codelem(e1,pretregs,FALSE);      /* evaluate e1 for cc   */
    }

    op = e->Eoper;
    sz = tysize(e1->Ety);
    if (!tyfloating(e1->Ety))
    {
    if (sz <= REGSIZE && e1->Eoper == OPvar)
    {   code cs;

        c = getlvalue(&cs,e1,0);
        freenode(e1);
        if (I32 && sz == 2)
            cs.Iflags |= CFopsize;

        retregs = *pretregs & (ALLREGS | mBP);
        if (config.target_cpu >= TARGET_80486 &&
            tysize(e->Ety) == 1)
        {
            if (reghasvalue((sz == 1) ? BYTEREGS : ALLREGS,0,&reg))
                cs.Iop = 0x39;
            else
            {   cs.Iop = 0x81;
                reg = 7;
                cs.IFL2 = FLconst;
                cs.IEV2.Vint = 0;
            }
            cs.Iop ^= (sz == 1);
            cs.Irm |= modregrm(0,reg,0);
            c = gen(c,&cs);                             // CMP e1,0

            retregs &= BYTEREGS;
            if (!retregs)
                retregs = BYTEREGS;
            c1 = allocreg(&retregs,&reg,TYint);

            int iop;
            if (op == OPbool)
            {
                iop = 0x0F95;   // SETNZ rm8
            }
            else
            {
                iop = 0x0F94;   // SETZ rm8
            }
            c1 = gen2(c1,iop,modregrm(3,0,reg));
            if (op == OPbool)
                *pretregs &= ~mPSW;
            goto L4;
        }

        if (reghasvalue((sz == 1) ? BYTEREGS : ALLREGS,1,&reg))
            cs.Iop = 0x39;
        else
        {   cs.Iop = 0x81;
            reg = 7;
            cs.IFL2 = FLconst;
            cs.IEV2.Vint = 1;
        }
        cs.Iop ^= (sz == 1);
        cs.Irm |= modregrm(0,reg,0);
        c = gen(c,&cs);                         // CMP e1,1

        c1 = allocreg(&retregs,&reg,TYint);
        op ^= (OPbool ^ OPnot);                 // switch operators
        goto L2;
    }
    else if (sz <= REGSIZE &&
        // NEG bytereg is too expensive
        (sz != 1 || config.target_cpu < TARGET_PentiumPro))
    {
        retregs = *pretregs & (ALLREGS | mBP);
        if (sz == 1 && !(retregs &= BYTEREGS))
            retregs = BYTEREGS;
        c = codelem(e->E1,&retregs,FALSE);
        reg = findreg(retregs);
        c1 = getregs(retregs);
        c1 = gen2(c1,0xF7 ^ (sz == 1),modregrm(3,3,reg));       // NEG reg
        code_orflag(c1,CFpsw);
        if (I32 && sz == SHORTSIZE)
            code_orflag(c1,CFopsize);
    L2:
        c1 = genregs(c1,0x19,reg,reg);                  // SBB reg,reg
        // At this point, reg==0 if e1==0, reg==-1 if e1!=0
        if (op == OPnot)
            gen1(c1,0x40 + reg);                        // INC reg
        else
            gen2(c1,0xF7,modregrm(3,3,reg));            // NEG reg
        if (*pretregs & mPSW)
        {   code_orflag(c1,CFpsw);
            *pretregs &= ~mPSW;         // flags are always set anyway
        }
    L4:
        return cat3(c,c1,fixresult(e,retregs,pretregs));
    }
  }
  cnop = gennop(CNIL);
  ctrue = gennop(CNIL);
  c = logexp(e->E1,(op == OPnot) ? FALSE : TRUE,FLcode,ctrue);
  forflags = *pretregs & mPSW;
  assert(tysize(e->Ety) <= REGSIZE);            // result better be int
  cfalse = allocreg(pretregs,&reg,e->Ety);      // allocate reg for result
  for (c1 = cfalse; c1; c1 = code_next(c1))
        gen(ctrue,c1);                          // duplicate reg save code
  cfalse = movregconst(cfalse,reg,0,forflags);  // mov 0 into reg
  regcon.immed.mval &= ~mask[reg];              // mark reg as unavail
  ctrue = movregconst(ctrue,reg,1,forflags);    // mov 1 into reg
  regcon.immed.mval &= ~mask[reg];              // mark reg as unavail
  genjmp(cfalse,JMP,FLcode,(block *) cnop);     // skip over ctrue
  c = cat4(c,cfalse,ctrue,cnop);
  return c;
}

/************************
 * Complement operator
 */

code *cdcom(elem *e,regm_t *pretregs)
{ unsigned reg,op;
  regm_t retregs,possregs;
  code *c,*c1,*cg;
  tym_t tym;
  int sz;

  if (*pretregs == 0)
        return codelem(e->E1,pretregs,FALSE);
  tym = tybasic(e->Ety);
  sz = tysize[tym];
  possregs = (sz == 1) ? BYTEREGS : allregs;
  retregs = *pretregs & possregs;
  if (retregs == 0)
        retregs = possregs;
  c1 = codelem(e->E1,&retregs,FALSE);
  cg = getregs(retregs);                /* retregs will be destroyed    */
#if 0
  if (sz == 4 * REGSIZE)
  {
        c = gen2(CNIL,0xF7,modregrm(3,2,AX));   // NOT AX
        gen2(c,0xF7,modregrm(3,2,BX));          // NOT BX
        gen2(c,0xF7,modregrm(3,2,CX));          // NOT CX
        gen2(c,0xF7,modregrm(3,2,DX));          // NOT DX
  }
  else
#endif
  {
        reg = (sz <= REGSIZE) ? findreg(retregs) : findregmsw(retregs);
        op = (sz == 1) ? 0xF6 : 0xF7;
        c = genregs(CNIL,op,2,reg);             // NOT reg
        if (sz == 2 * REGSIZE)
        {   reg = findreglsw(retregs);
            genregs(c,op,2,reg);                // NOT reg+1
        }
  }
  return cat4(c1,cg,c,fixresult(e,retregs,pretregs));
}

/************************
 * Bswap operator
 */

code *cdbswap(elem *e,regm_t *pretregs)
{   unsigned reg,op;
    regm_t retregs;
    code *c,*c1,*cg;
    tym_t tym;
    int sz;

    if (*pretregs == 0)
        return codelem(e->E1,pretregs,FALSE);

    tym = tybasic(e->Ety);
    assert(tysize[tym] == 4);
    retregs = *pretregs & allregs;
    if (retregs == 0)
        retregs = allregs;
    c1 = codelem(e->E1,&retregs,FALSE);
    cg = getregs(retregs);              // retregs will be destroyed
    reg = findreg(retregs);
    c = gen2(CNIL,0x0FC8 + reg,0);      // BSWAP reg
    return cat4(c1,cg,c,fixresult(e,retregs,pretregs));
}

/*************************
 * ?: operator
 */

code *cdcond(elem *e,regm_t *pretregs)
{ regm_t psw;
  code *cc,*c,*c1,*cnop1,*c2,*cnop2;
  con_t regconold,regconsave;
  unsigned stackpushold,stackpushsave;
  int ehindexold,ehindexsave;
  unsigned jop;
  unsigned op1;
  unsigned sz1;
  unsigned sz2;
  elem *e1;
  elem *e2;
  elem *e21;
  elem *e22;

  /* vars to save state of 8087 */
  int stackusedold,stackusedsave;
  NDP _8087old[arraysize(_8087elems)];
  NDP _8087save[arraysize(_8087elems)];

  _chkstack();

  //dbg_printf("cdcond(e = %p, *pretregs = x%x)\n",e,*pretregs);
  e1 = e->E1;
  e2 = e->E2;
  e21 = e2->E1;
  e22 = e2->E2;
  cc = docommas(&e1);
  cgstate.stackclean++;
  psw = *pretregs & mPSW;               /* save PSW bit                 */
  op1 = e1->Eoper;
  sz1 = tysize(e1->Ety);
  jop = jmpopcode(e1);

  if (!OTrel(op1) && e1 == e21 &&
      sz1 <= REGSIZE && !tyfloating(e1->Ety))
  {     // Recognize (e ? e : f)
        regm_t retregs;

        cnop1 = gennop(CNIL);
        retregs = *pretregs | mPSW;
        c = codelem(e1,&retregs,FALSE);

        c = cat(c,cse_flush(1));                // flush CSEs to memory
        c = genjmp(c,jop,FLcode,(block *)cnop1);
        freenode(e21);

        regconsave = regcon;
        stackpushsave = stackpush;

        retregs |= psw;
        if (retregs & (mBP | ALLREGS))
            regimmed_set(findreg(retregs),0);
        c2 = codelem(e22,&retregs,FALSE);

        andregcon(&regconsave);
        assert(stackpushsave == stackpush);

        *pretregs = retregs;
        freenode(e2);
        c = cat4(cc,c,c2,cnop1);
        goto Lret;
  }

  if (OTrel(op1) && sz1 <= REGSIZE && tysize(e2->Ety) <= REGSIZE &&
        !e1->Ecount &&
        (jop == JC || jop == JNC) &&
        (sz2 = tysize(e2->Ety)) <= REGSIZE &&
        e21->Eoper == OPconst &&
        e22->Eoper == OPconst
     )
  {     regm_t retregs;
        unsigned reg;
        targ_long v1,v2;
        int opcode;

        retregs = *pretregs & (ALLREGS | mBP);
        if (!retregs)
            retregs = ALLREGS;
        cdcmp_flag = 1;
        c = codelem(e1,&retregs,FALSE);
        reg = findreg(retregs);
        v1 = e21->EV.Vlong;
        v2 = e22->EV.Vlong;
        if (jop == JNC)
        {   v1 = v2;
            v2 = e21->EV.Vlong;
        }

        opcode = 0x81;
        switch (sz2)
        {   case 1:     opcode--;
                        v1 = (signed char) v1;
                        v2 = (signed char) v2;
                        break;
            case 2:     v1 = (short) v1;
                        v2 = (short) v2;
                        break;
        }

        if (v1 == 0 && v2 == -1L)
            c = gen2(c,0xF6 + (opcode & 1),modregrm(3,2,reg));  // NOT reg
        else
        {
            v1 -= v2;
            c = genc2(c,opcode,modregrm(3,4,reg),v1);   // AND reg,v1-v2
            if (v2 == 1)
                gen1(c,0x40 + reg);                     // INC reg
            else if (v2 == -1L)
                gen1(c,0x48 + reg);                     // DEC reg
            else
                genc2(c,opcode,modregrm(3,0,reg),v2);   // ADD reg,v2
        }

        freenode(e21);
        freenode(e22);
        freenode(e2);

        c = cat(c,fixresult(e,retregs,pretregs));
        goto Lret;
  }

  if (op1 != OPcond && op1 != OPandand && op1 != OPoror &&
      op1 != OPnot && op1 != OPbool &&
      e21->Eoper == OPconst &&
      sz1 <= REGSIZE &&
      *pretregs & (mBP | ALLREGS) &&
      tysize(e21->Ety) <= REGSIZE && !tyfloating(e21->Ety))
  {     // Recognize (e ? c : f)
        unsigned reg;
        regm_t retregs;

        cnop1 = gennop(CNIL);
        retregs = mPSW;
        jop = jmpopcode(e1);            // get jmp condition
        c = codelem(e1,&retregs,FALSE);

        // Set the register with e21 without affecting the flags
        retregs = *pretregs & (ALLREGS | mBP);
        if (retregs & ~regcon.mvar)
            retregs &= ~regcon.mvar;    // don't disturb register variables
        c = regwithvalue(c,retregs,e21->EV.Vint,&reg,8);
        retregs = mask[reg];

        c = cat(c,cse_flush(1));                // flush CSE's to memory
        c = genjmp(c,jop,FLcode,(block *)cnop1);
        freenode(e21);

        regconsave = regcon;
        stackpushsave = stackpush;

        c2 = codelem(e22,&retregs,FALSE);

        andregcon(&regconsave);
        assert(stackpushsave == stackpush);

        freenode(e2);
        c = cat6(cc,c,c2,cnop1,fixresult(e,retregs,pretregs),NULL);
        goto Lret;
  }

  cnop1 = gennop(CNIL);
  cnop2 = gennop(CNIL);         /* dummy target addresses       */
  c = logexp(e1,FALSE,FLcode,cnop1);    /* evaluate condition           */
  regconold = regcon;
  stackusedold = stackused;
  stackpushold = stackpush;
  memcpy(_8087old,_8087elems,sizeof(_8087elems));
  c1 = codelem(e21,pretregs,FALSE);

#if SCPP
  if (CPP && e2->Eoper == OPcolon2)
  {     code cs;

        // This is necessary so that any cleanup code on one branch
        // is redone on the other branch.
        cs.Iop = ESCAPE;
        cs.Iop2 = ESCmark2;
        cs.Iflags = 0;
        cs.Ijty = 0;
        c1 = cat(gen(CNIL,&cs),c1);
        cs.Iop2 = ESCrelease2;
        c1 = gen(c1,&cs);
  }
#endif

  regconsave = regcon;
  regcon = regconold;

  stackpushsave = stackpush;
  stackpush = stackpushold;

  stackusedsave = stackused;
  stackused = stackusedold;

  memcpy(_8087save,_8087elems,sizeof(_8087elems));
  memcpy(_8087elems,_8087old,sizeof(_8087elems));

  *pretregs |= psw;                     /* PSW bit may have been trashed */
  c2 = codelem(e22,pretregs,FALSE); /* use same regs as E1 */
  andregcon(&regconold);
  andregcon(&regconsave);
  assert(stackused == stackusedsave);
  assert(stackpush == stackpushsave);
  memcpy(_8087elems,_8087save,sizeof(_8087elems));
  freenode(e2);
  c = cat(cc,c);
  c = cat6(c,c1,genjmp(CNIL,JMP,FLcode,(block *) cnop2),cnop1,c2,cnop2);
  if (*pretregs & mST0)
        note87(e,0,0);
Lret:
  cgstate.stackclean--;
  return c;
}

/*********************
 * Comma operator
 */

code *cdcomma(elem *e,regm_t *pretregs)
{ regm_t retregs;
  code *cl,*cr;

  retregs = 0;
  cl = codelem(e->E1,&retregs,FALSE);   /* ignore value from left leaf  */
  cr = codelem(e->E2,pretregs,FALSE);   /* do right leaf                */
  return cat(cl,cr);
}


/*********************************
 * Do && and || operators.
 * Generate:
 *              (evaluate e1 and e2, if TRUE goto cnop1)
 *      cnop3:  NOP
 *      cg:     [save reg code]         ;if we must preserve reg
 *              CLR     reg             ;FALSE result (set Z also)
 *              JMP     cnop2
 *
 *      cnop1:  NOP                     ;if e1 evaluates to TRUE
 *              [save reg code]         ;preserve reg
 *
 *              MOV     reg,1           ;TRUE result
 *                  or
 *              CLR     reg             ;if return result in flags
 *              INC     reg
 *
 *      cnop2:  NOP                     ;mark end of code
 */

code *cdloglog(elem *e,regm_t *pretregs)
{ regm_t retregs;
  unsigned reg;
  code *c;
  code *cl,*cr,*cg,*cnop1,*cnop2,*cnop3;
  register code *c1;
  con_t regconsave;
  unsigned stackpushsave;
  int jcond;
  elem *e2;

  /* We can trip the assert with the following:                         */
  /*    if ( (b<=a) ? (c<b || a<=c) : c>=a )                            */
  /* We'll generate ugly code for it, but it's too obscure a case       */
  /* to expend much effort on it.                                       */
  /*assert(*pretregs != mPSW);*/

  cgstate.stackclean++;
  cnop1 = gennop(CNIL);
  cnop3 = gennop(CNIL);
  jcond = 0;
  e2 = e->E2;
  cl = (e->Eoper == OPoror)
        ? logexp(e->E1,jcond | 1,FLcode,cnop1)
        : logexp(e->E1,jcond,FLcode,cnop3);
  regconsave = regcon;
  stackpushsave = stackpush;
  if (*pretregs == 0)                   /* if don't want result         */
  {     int noreturn = el_noreturn(e2);

        cr = codelem(e2,pretregs,FALSE);
        if (noreturn)
        {
            regconsave.used |= regcon.used;
            regcon = regconsave;
        }
        else
            andregcon(&regconsave);
        assert(stackpush == stackpushsave);
        c = cat4(cl,cr,cnop3,cnop1);    // eval code, throw away result
        goto Lret;
  }
  cnop2 = gennop(CNIL);
  if (tybasic(e2->Ety) == TYbool &&
      tysize(e->Ety) == tysize(e2->Ety) &&
      !(*pretregs & mPSW) &&
      e2->Eoper == OPcall)
  {
        cr = codelem(e2,pretregs,FALSE);

        andregcon(&regconsave);

        // stack depth should not change when evaluating E2
        assert(stackpush == stackpushsave);

        assert(tysize(e->Ety) <= REGSIZE);      // result better be int
        retregs = *pretregs & allregs;
        cnop1 = cat(cnop1,allocreg(&retregs,&reg,TYint));       // allocate reg for result
        cg = genjmp(NULL,JMP,FLcode,(block *) cnop2);           // JMP cnop2
        cnop1 = movregconst(cnop1,reg,e->Eoper == OPoror,0);    // reg = 1
        regcon.immed.mval &= ~mask[reg];                        // mark reg as unavail
        *pretregs = retregs;
        if (e->Eoper == OPoror)
            c = cat6(cl,cr,cnop3,cg,cnop1,cnop2);
        else
            c = cat6(cl,cr,cg,cnop3,cnop1,cnop2);

        goto Lret;
  }
  cr = logexp(e2,1,FLcode,cnop1);
  andregcon(&regconsave);

  /* stack depth should not change when evaluating E2   */
  assert(stackpush == stackpushsave);

  assert(tysize(e->Ety) <= REGSIZE);    // result better be int
  retregs = *pretregs & (ALLREGS | mBP);
  if (!retregs) retregs = ALLREGS;      // if mPSW only
  cg = allocreg(&retregs,&reg,TYint);   // allocate reg for result
  for (c1 = cg; c1; c1 = code_next(c1)) // for each instruction
        gen(cnop1,c1);                  // duplicate it
  cg = movregconst(cg,reg,0,*pretregs & mPSW);  // MOV reg,0
  regcon.immed.mval &= ~mask[reg];                      // mark reg as unavail
  genjmp(cg,JMP,FLcode,(block *) cnop2);                // JMP cnop2
  cnop1 = movregconst(cnop1,reg,1,*pretregs & mPSW);    // reg = 1
  regcon.immed.mval &= ~mask[reg];                      // mark reg as unavail
  *pretregs = retregs;
  c = cat6(cl,cr,cnop3,cg,cnop1,cnop2);
Lret:
  cgstate.stackclean--;
  return c;
}

/*********************
 * Generate code for shift left or shift right (OPshl,OPshr,OPashr).
 */

code *cdshift(elem *e,regm_t *pretregs)
{ unsigned resreg,shiftcnt,sreg,byte;
  unsigned s1,s2,oper;
  tym_t tyml,uns;
  int sz;
  regm_t retregs,rretregs;
  code *cg,*cl,*cr;
  code *c;
  elem *e1;
  elem *e2;
  regm_t forccs,forregs;
  bool e2isconst;

  e1 = e->E1;
  if (*pretregs == 0)                   // if don't want result
  {     c = codelem(e1,pretregs,FALSE); // eval left leaf
        *pretregs = 0;                  // in case they got set
        return cat(c,codelem(e->E2,pretregs,FALSE));
  }

  tyml = tybasic(e1->Ety);
  sz = tysize[tyml];
  assert(!tyfloating(tyml));
  uns = tyuns(tyml);
  oper = e->Eoper;

#if SCPP
  // Do this until the rest of the compiler does OPshr/OPashr correctly
  if (oper == OPshr)
        oper = (uns) ? OPshr : OPashr;
#endif

  switch (oper)
  {     case OPshl:
            s1 = 4;                     // SHL
            s2 = 2;                     // RCL
            break;
        case OPshr:
            s1 = 5;                     // SHR
            s2 = 3;                     // RCR
            break;
        case OPashr:
            s1 = 7;                     // SAR
            s2 = 3;                     // RCR
            break;
        default:
            assert(0);
  }

  c = cg = cr = CNIL;                   /* initialize                   */
  e2 = e->E2;
  forccs = *pretregs & mPSW;            /* if return result in CCs      */
  forregs = *pretregs & (ALLREGS | mBP); // mask of possible return regs
  e2isconst = FALSE;                    /* assume for the moment        */
  byte = (sz == 1);
  switch (e2->Eoper)
  {
    case OPconst:
        e2isconst = TRUE;               /* e2 is a constant             */
        shiftcnt = e2->EV.Vint;         /* get shift count              */
        if ((I32 && sz <= REGSIZE) ||
            shiftcnt <= 4 ||            /* if sequence of shifts        */
            (sz == 2 &&
                (shiftcnt == 8 || config.target_cpu >= TARGET_80286)) ||
            (sz == 2 * REGSIZE && shiftcnt == 8 * REGSIZE)
           )
        {       retregs = (forregs) ? forregs
                                    : ALLREGS;
                if (byte)
                {   retregs &= BYTEREGS;
                    if (!retregs)
                        retregs = BYTEREGS;
                }
                else if (sz > REGSIZE && sz <= 2 * REGSIZE &&
                         !(retregs & mMSW))
                    retregs |= mMSW & ALLREGS;
                if (s1 == 7)    /* if arithmetic right shift */
                {
                    if (shiftcnt == 8)
                        retregs = mAX;
                    else if (sz == 2 * REGSIZE && shiftcnt == 8 * REGSIZE)
                        retregs = mDX|mAX;
                }

                if (sz == 2 * REGSIZE && shiftcnt == 8 * REGSIZE &&
                    oper == OPshl &&
                    !e1->Ecount &&
                    (e1->Eoper == OPshtlng  || e1->Eoper == OPu16_32 ||
                     e1->Eoper == OPlngllng || e1->Eoper == OPulngllng)
                   )
                {   // Handle (shtlng)s << 16
                    regm_t r;

                    r = retregs & mMSW;
                    cl = codelem(e1->E1,&r,FALSE);      // eval left leaf
                    cl = regwithvalue(cl,retregs & mLSW,0,&resreg,0);
                    cg = getregs(r);
                    retregs = r | mask[resreg];
                    if (forccs)
                    {   sreg = findreg(r);
                        cg = gentstreg(cg,sreg);
                        *pretregs &= ~mPSW;             // already set
                    }
                    freenode(e1);
                    freenode(e2);
                    break;
                }

                // See if we should use LEA reg,xxx instead of shift
                if (I32 && shiftcnt >= 1 && shiftcnt <= 3 &&
                    sz == REGSIZE && oper == OPshl &&
                    e1->Eoper == OPvar &&
                    !(*pretregs & mPSW) &&
                    config.flags4 & CFG4speed
                   )
                {   Symbol *s1 = e1->EV.sp.Vsym;
                    unsigned reg;
                    regm_t regm;
                    code cs;

                    if (isregvar(e1,&regm,&reg) && !(regm & retregs))
                    {
                        cl = allocreg(&retregs,&resreg,e->Ety);
                        buildEA(&cs,-1,reg,1 << shiftcnt,0);
                        cs.Iop = 0x8D;
                        cs.Irm |= modregrm(0,resreg,0);
                        cs.Iflags = 0;
                        cg = gen(NULL,&cs);             // LEA resreg,[reg * ss]
                        freenode(e1);
                        freenode(e2);
                        break;
                    }
                }

                cl = codelem(e1,&retregs,FALSE); // eval left leaf
                //assert((retregs & regcon.mvar) == 0);
                cg = getregs(retregs);          // trash these regs

                {
                    if (sz == 2 * REGSIZE)
                    {   resreg = findregmsw(retregs);
                        sreg = findreglsw(retregs);
                    }
                    else
                        resreg = findreg(retregs);
                    if (config.target_cpu >= TARGET_80286 &&
                        sz <= REGSIZE)
                    {
                        /* SHL resreg,shiftcnt  */
                        assert(!(sz == 1 && (mask[resreg] & ~BYTEREGS)));
                        c = genc2(CNIL,0xC1 ^ byte,modregrm(3,s1,resreg),shiftcnt);
                        if (shiftcnt == 1)
                            c->Iop += 0x10;     /* short form of shift  */
                        // See if we need operand size prefix
                        if (I32 && oper != OPshl && sz == 2)
                            c->Iflags |= CFopsize;
                        if (forccs)
                            c->Iflags |= CFpsw;         // need flags result
                    }
                    else if (shiftcnt == 8)
                    {   if (!(retregs & BYTEREGS))
                        {
                            cl = cat(cl,cg);
                            goto L1;
                        }

                        if (pass != PASSfinal && (!forregs || forregs & (mSI | mDI)))
                        {
                            // e1 might get into SI or DI in a later pass,
                            // so don't put CX into a register
                            cg = cat(cg, getregs(mCX));
                        }

                        if (oper == OPshl)
                        {       /* MOV regH,regL        XOR regL,regL   */
                                c = genregs(CNIL,0x8A,resreg+4,resreg);
                                genregs(c,0x32,resreg,resreg);
                        }
                        else            // OPshr/OPashr
                        {
                            /* MOV regL,regH    */
                            c = genregs(CNIL,0x8A,resreg,resreg+4);
                            if (oper == OPashr)
                                gen1(c,0x98);           /* CBW          */
                            else
                                genregs(c,0x32,resreg+4,resreg+4); /* CLR regH */
                        }
                        if (forccs)
                                gentstreg(c,resreg);
                    }
                    else if (shiftcnt == REGSIZE * 8)   // it's an lword
                    {
                        if (oper == OPshl)
                            swap((int *) &resreg,(int *) &sreg);
                        c = genmovreg(CNIL,sreg,resreg);        // MOV sreg,resreg
                        if (oper == OPashr)
                            gen1(c,0x99);                       // CWD
                        else
                            movregconst(c,resreg,0,0);          // MOV resreg,0
                        if (forccs)
                        {       gentstreg(c,sreg);
                                *pretregs &= mBP | ALLREGS | mES;
                        }
                    }
                    else
                    {   c = CNIL;
                        if (oper == OPshl && sz == 2 * REGSIZE)
                            swap((int *) &resreg,(int *) &sreg);
                        while (shiftcnt--)
                        {   c = gen2(c,0xD1 ^ byte,modregrm(3,s1,resreg));
                            if (sz == 2 * REGSIZE)
                                gen2(c,0xD1,modregrm(3,s2,sreg));
                        }
                        if (forccs)
                            code_orflag(c,CFpsw);
                    }
                    if (sz <= REGSIZE)
                        *pretregs &= mBP | ALLREGS;     // flags already set
                }
                freenode(e2);
                break;
        }
        /* FALL-THROUGH */
    default:
        retregs = forregs & ~mCX;               /* CX will be shift count */
        if (sz <= REGSIZE)
        {
            if (forregs & ~regcon.mvar && !(retregs & ~regcon.mvar))
                retregs = ALLREGS & ~mCX;       /* need something       */
            else if (!retregs)
                retregs = ALLREGS & ~mCX;       /* need something       */
            if (sz == 1)
            {   retregs &= mAX|mBX|mDX;
                if (!retregs)
                    retregs = mAX|mBX|mDX;
            }
        }
        else
        {
            if (!(retregs & mMSW))
                retregs = ALLREGS & ~mCX;
        }
        cl = codelem(e->E1,&retregs,FALSE);     /* eval left leaf       */

        if (sz <= REGSIZE)
            resreg = findreg(retregs);
        else
        {
            resreg = findregmsw(retregs);
            sreg = findreglsw(retregs);
        }
    L1:
        rretregs = mCX;                 /* CX is shift count    */
        if (sz <= REGSIZE)
        {
            cr = scodelem(e2,&rretregs,retregs,FALSE); /* get rvalue */
            cg = getregs(retregs);      /* trash these regs             */
            c = gen2(CNIL,0xD3 ^ byte,modregrm(3,s1,resreg)); /* Sxx resreg,CX */

            // Note that a shift by CL does not set the flags if
            // CL == 0. If e2 is a constant, we know it isn't 0
            // (it would have been optimized out).
            if (e2isconst)
                *pretregs &= mBP | ALLREGS; // flags already set with result
        }
        else if (sz == 2 * REGSIZE &&
                 config.target_cpu >= TARGET_80386)
        {   unsigned hreg,lreg;

            hreg = resreg;
            lreg = sreg;
            if (e2isconst)
            {
                cr = NULL;
                cg = getregs(retregs);
                if (shiftcnt & (REGSIZE * 8))
                {
                    if (oper == OPshr)
                    {   //      SHR hreg,shiftcnt
                        //      MOV lreg,hreg
                        //      XOR hreg,hreg
                        c = genc2(NULL,0xC1,modregrm(3,s1,hreg),shiftcnt - (REGSIZE * 8));
                        c = genmovreg(c,lreg,hreg);
                        c = movregconst(c,hreg,0,0);
                    }
                    else if (oper == OPashr)
                    {   //      MOV     lreg,hreg
                        //      SAR     hreg,31
                        //      SHRD    lreg,hreg,shiftcnt
                        c = genmovreg(NULL,lreg,hreg);
                        c = genc2(c,0xC1,modregrm(3,s1,hreg),(REGSIZE * 8) - 1);
                        c = genc2(c,0x0FAC,modregrm(3,hreg,lreg),shiftcnt - (REGSIZE * 8));
                    }
                    else
                    {   //      SHL lreg,shiftcnt
                        //      MOV hreg,lreg
                        //      XOR lreg,lreg
                        c = genc2(NULL,0xC1,modregrm(3,s1,lreg),shiftcnt - (REGSIZE * 8));
                        c = genmovreg(c,hreg,lreg);
                        c = movregconst(c,lreg,0,0);
                    }
                }
                else
                {
                    if (oper == OPshr || oper == OPashr)
                    {   //      SHRD    lreg,hreg,shiftcnt
                        //      SHR/SAR hreg,shiftcnt
                        c = genc2(NULL,0x0FAC,modregrm(3,hreg,lreg),shiftcnt);
                        c = genc2(c,0xC1,modregrm(3,s1,hreg),shiftcnt);
                    }
                    else
                    {   //      SHLD hreg,lreg,shiftcnt
                        //      SHL  lreg,shiftcnt
                        c = genc2(NULL,0x0FA4,modregrm(3,lreg,hreg),shiftcnt);
                        c = genc2(c,0xC1,modregrm(3,s1,lreg),shiftcnt);
                    }
                }
                freenode(e2);
            }
            else if (config.target_cpu >= TARGET_80486 && REGSIZE == 2)
            {
                cr = scodelem(e2,&rretregs,retregs,FALSE); // get rvalue in CX
                cg = getregs(retregs);          // modify these regs
                if (oper == OPshl)
                {
                    /*
                        SHLD    hreg,lreg,CL
                        SHL     lreg,CL
                     */

                    c = gen2(NULL,0x0FA5,modregrm(3,lreg,hreg));
                    gen2(c,0xD3,modregrm(3,4,lreg));
                }
                else
                {
                    /*
                        SHRD    lreg,hreg,CL
                        SAR             hreg,CL

                        -- or --

                        SHRD    lreg,hreg,CL
                        SHR             hreg,CL
                     */
                    c = gen2(NULL,0x0FAD,modregrm(3,hreg,lreg));
                    gen2(c,0xD3,modregrm(3,s1,hreg));
                }
            }
            else
            {   code *cl1,*cl2;

                cr = scodelem(e2,&rretregs,retregs,FALSE); // get rvalue in CX
                cg = getregs(retregs | mCX);            // modify these regs
                                                        // TEST CL,0x20
                c = genc2(NULL,0xF6,modregrm(3,0,CX),REGSIZE * 8);
                if (oper == OPshl)
                {
                    /*  TEST    CL,20H
                        JNE     L1
                        SHLD    hreg,lreg,CL
                        SHL     lreg,CL
                        JMP     L2
                    L1: AND     CL,20H-1
                        SHL     lreg,CL
                        MOV     hreg,lreg
                        XOR     lreg,lreg
                    L2: NOP
                     */

                    cl1 = NULL;
                    if (REGSIZE == 2)
                        cl1 = genc2(NULL,0x80,modregrm(3,4,CX),REGSIZE * 8 - 1);
                    cl1 = gen2(cl1,0xD3,modregrm(3,4,lreg));
                    genmovreg(cl1,hreg,lreg);
                    genregs(cl1,0x31,lreg,lreg);

                    genjmp(c,JNE,FLcode,(block *)cl1);
                    gen2(c,0x0FA5,modregrm(3,lreg,hreg));
                    gen2(c,0xD3,modregrm(3,4,lreg));
                }
                else
                {   if (oper == OPashr)
                    {
                        /*  TEST        CL,20H
                            JNE         L1
                            SHRD        lreg,hreg,CL
                            SAR         hreg,CL
                            JMP         L2
                        L1: AND         CL,15
                            MOV         lreg,hreg
                            SAR         hreg,31
                            SHRD        lreg,hreg,CL
                        L2: NOP
                         */

                        cl1 = NULL;
                        if (REGSIZE == 2)
                            cl1 = genc2(NULL,0x80,modregrm(3,4,CX),REGSIZE * 8 - 1);
                        cl1 = genmovreg(cl1,lreg,hreg);
                        genc2(cl1,0xC1,modregrm(3,s1,hreg),31);
                        gen2(cl1,0x0FAD,modregrm(3,hreg,lreg));
                    }
                    else
                    {
                        /*  TEST        CL,20H
                            JNE         L1
                            SHRD        lreg,hreg,CL
                            SHR         hreg,CL
                            JMP         L2
                        L1: AND         CL,15
                            SHR         hreg,CL
                            MOV         lreg,hreg
                            XOR         hreg,hreg
                        L2: NOP
                         */

                        cl1 = NULL;
                        if (REGSIZE == 2)
                            cl1 = genc2(NULL,0x80,modregrm(3,4,CX),REGSIZE * 8 - 1);
                        cl1 = gen2(cl1,0xD3,modregrm(3,5,hreg));
                        genmovreg(cl1,lreg,hreg);
                        genregs(cl1,0x31,hreg,hreg);
                    }
                    genjmp(c,JNE,FLcode,(block *)cl1);
                    gen2(c,0x0FAD,modregrm(3,hreg,lreg));
                    gen2(c,0xD3,modregrm(3,s1,hreg));
                }
                cl2 = gennop(NULL);
                genjmp(c,JMPS,FLcode,(block *)cl2);
                c = cat3(c,cl1,cl2);
            }
            break;
        }
        else if (sz == 2 * REGSIZE)
        {
            c = CNIL;
            if (!e2isconst)                     // if not sure shift count != 0
                    c = genc2(c,0xE3,0,6);      // JCXZ .+6
            cr = scodelem(e2,&rretregs,retregs,FALSE);
            cg = getregs(retregs | mCX);
            if (oper == OPshl)
                    swap((int *) &resreg,(int *) &sreg);
            c = gen2(c,0xD1,modregrm(3,s1,resreg));
            code_orflag(c,CFtarg2);
            gen2(c,0xD1,modregrm(3,s2,sreg));
            genc2(c,0xE2,0,(targ_uns)-6);               // LOOP .-6
            regimmed_set(CX,0);         // note that now CX == 0
        }
        else
            assert(0);
        break;
  }
  c = cat(c,fixresult(e,retregs,pretregs));
  return cat4(cl,cr,cg,c);
}

/***************************
 * Perform a 'star' reference (indirection).
 */

code *cdind(elem *e,regm_t *pretregs)
{ code *c,*ce,cs;
  tym_t tym;
  regm_t idxregs,retregs;
  unsigned reg,nreg,byte;
  elem *e1;
  unsigned sz;

  //printf("cdind(e = %p, *pretregs = x%x)\n",e,*pretregs);
  tym = tybasic(e->Ety);
  if (tyfloating(tym))
  {
        if (config.inline8087)
        {
            if (*pretregs & mST0)
                return cdind87(e, pretregs);
            if (tycomplex(tym))
                return cload87(e, pretregs);
            if (*pretregs & mPSW)
                return cdind87(e, pretregs);
        }
  }

  e1 = e->E1;
  assert(e1);
  switch (tym)
  {     case TYstruct:
        case TYarray:
            // This case should never happen, why is it here?
            tym = TYnptr;               // don't confuse allocreg()
#if !TARGET_FLAT
            if (*pretregs & (mES | mCX) || e->Ety & mTYfar)
                    tym = TYfptr;
#endif

#if 0
  c = getlvalue(&cs,e,RMload);          // get addressing mode
  if (*pretregs == 0)
        return c;
  idxregs = idxregm(cs.Irm,cs.Isib);    /* mask of index regs used      */
  c = cat(c,fixresult(e,idxregs,pretregs));
  return c;
#endif
            break;
  }
  sz = tysize[tym];
  byte = tybyte(tym) != 0;

  c = getlvalue(&cs,e,RMload);          // get addressing mode
  /*fprintf(stderr,"cd2 :\n"); WRcodlst(c);*/
  if (*pretregs == 0)
        return c;

  idxregs = idxregm(cs.Irm,cs.Isib);    /* mask of index regs used      */

  if (*pretregs == mPSW)
  {
        if (I32 && tym == TYfloat)
        {       retregs = ALLREGS & ~idxregs;
                c = cat(c,allocreg(&retregs,&reg,TYfloat));
                cs.Iop = 0x8B;
                cs.Irm |= modregrm(0,reg,0);
                ce = gen(CNIL,&cs);                     /* MOV reg,lsw  */
                gen2(ce,0xD1,modregrm(3,4,reg));        /* SHL reg,1    */
        }
        else if (sz <= REGSIZE)
        {
                cs.Iop = 0x81 ^ byte;
                cs.Irm |= modregrm(0,7,0);
                cs.IFL2 = FLconst;
                cs.IEV2.Vint = 0;
                ce = gen(CNIL,&cs);             /* CMP [idx],0          */
        }
        else if (I32 && sz == REGSIZE + 2)      // if far pointer
        {       retregs = ALLREGS & ~idxregs;
                c = cat(c,allocreg(&retregs,&reg,TYint));
                cs.Iop = 0x0F;
                cs.Iop2 = 0xB7;
                cs.Irm |= modregrm(0,reg,0);
                getlvalue_msw(&cs);
                ce = gen(CNIL,&cs);             /* MOVZX reg,msw        */
                goto L4;
        }
        else if (sz <= 2 * REGSIZE)
        {       retregs = ALLREGS & ~idxregs;
                c = cat(c,allocreg(&retregs,&reg,TYint));
                cs.Iop = 0x8B;
                cs.Irm |= modregrm(0,reg,0);
                getlvalue_msw(&cs);
                ce = gen(CNIL,&cs);             /* MOV reg,msw          */
                if (I32)
                {   if (tym == TYdouble || tym == TYdouble_alias)
                        gen2(ce,0xD1,modregrm(3,4,reg)); // SHL reg,1
                }
                else if (tym == TYfloat)
                    gen2(ce,0xD1,modregrm(3,4,reg));    /* SHL reg,1    */
        L4:     cs.Iop = 0x0B;
                getlvalue_lsw(&cs);
                gen(ce,&cs);                    /* OR reg,lsw           */
        }
        else if (!I32 && sz == 8)
        {       *pretregs |= DOUBLEREGS_16;     /* fake it for now      */
                goto L1;
        }
        else
        {
                debugx(WRTYxx(tym));
                assert(0);
        }
        c = cat(c,ce);
  }
  else                                  /* else return result in reg    */
  {
  L1:   retregs = *pretregs;
        if (sz == 8 &&
            (retregs & (mPSW | mSTACK | ALLREGS | mBP)) == mSTACK)
        {   int i;

            /* Optimizer should not CSE these, as the result is worse code! */
            assert(!e->Ecount);

            cs.Iop = 0xFF;
            cs.Irm |= modregrm(0,6,0);
            cs.IEVoffset1 += 8 - REGSIZE;
            stackchanged = 1;
            i = 8 - REGSIZE;
            do
            {
                c = gen(c,&cs);                         /* PUSH EA+i    */
                c = genadjesp(c,REGSIZE);
                cs.IEVoffset1 -= REGSIZE;
                stackpush += REGSIZE;
                i -= REGSIZE;
            }
            while (i >= 0);
            goto L3;
        }
        if (!I32 && sz == 8)
            retregs = DOUBLEREGS_16;

        /* Watch out for loading an lptr from an lptr! We must have     */
        /* the offset loaded into a different register.                 */
        /*if (retregs & mES && (cs.Iflags & CFSEG) == CFes)
                retregs = ALLREGS;*/

        {
        assert(!byte || retregs & BYTEREGS);
        c = cat(c,allocreg(&retregs,&reg,tym)); /* alloc registers */
        }
        if (sz <= REGSIZE)
        {
                cs.Iop = 0x8B ^ byte;
        L2:     cs.Irm |= modregrm(0,reg,0);
                ce = gen(CNIL,&cs);     /* MOV reg,[idx]                */
        }
        else if ((tym == TYfptr || tym == TYhptr) && retregs & mES)
        {
                cs.Iop = 0xC4;          /* LES reg,[idx]                */
                goto L2;
        }
        else if (sz <= 2 * REGSIZE)
        {   unsigned lsreg;

            cs.Iop = 0x8B;
            /* Be careful not to interfere with index registers */
            if (I32)
            {
                /* Can't handle if both result registers are used in    */
                /* the addressing mode.                                 */
                if ((retregs & idxregs) == retregs)
                {
                    retregs = mMSW & allregs & ~idxregs;
                    if (!retregs)
                        retregs |= mCX;
                    retregs |= mLSW & ~idxregs;

                    // We can run out of registers, so if that's possible,
                    // give us *one* of the idxregs
                    if ((retregs & ~regcon.mvar & mLSW) == 0)
                    {   regm_t x;

                        x = idxregs & mLSW;
                        if (x)
                            retregs |= mask[findreg(x)];        // give us one idxreg
                    }
                    else if ((retregs & ~regcon.mvar & mMSW) == 0)
                    {   regm_t x;

                        x = idxregs & mMSW;
                        if (x)
                            retregs |= mask[findreg(x)];        // give us one idxreg
                    }

                    c = cat(c,allocreg(&retregs,&reg,tym));     /* alloc registers */
                    assert((retregs & idxregs) != retregs);
                }

                lsreg = findreglsw(retregs);
                if (mask[reg] & idxregs)                /* reg is in addr mode */
                {
                    cs.Irm |= modregrm(0,lsreg,0);
                    ce = gen(CNIL,&cs);                 /* MOV lsreg,lsw */
                    if (sz == REGSIZE + 2)
                        cs.Iflags |= CFopsize;
                    lsreg = reg;
                    getlvalue_msw(&cs);                 // MOV reg,msw
                }
                else
                {
                    cs.Irm |= modregrm(0,reg,0);
                    getlvalue_msw(&cs);
                    ce = gen(CNIL,&cs);                 // MOV reg,msw
                    if (sz == REGSIZE + 2)
                        ce->Iflags |= CFopsize;
                    getlvalue_lsw(&cs);                 // MOV lsreg,lsw
                }
                NEWREG(cs.Irm,lsreg);
                gen(ce,&cs);
            }
            else
            {
                /* Index registers are always the lsw!                  */
                cs.Irm |= modregrm(0,reg,0);
                getlvalue_msw(&cs);
                ce = gen(CNIL,&cs);     /* MOV reg,msw          */
                lsreg = findreglsw(retregs);
                NEWREG(cs.Irm,lsreg);
                getlvalue_lsw(&cs);     /* MOV lsreg,lsw        */
                gen(ce,&cs);
            }
        }
        else if (!I32 && sz == 8)
        {
                assert(reg == AX);
                cs.Iop = 0x8B;
                cs.IEVoffset1 += 6;
                ce = gen(CNIL,&cs);             /* MOV AX,EA+6          */
                cs.Irm |= modregrm(0,CX,0);
                cs.IEVoffset1 -= 4;
                gen(ce,&cs);                    /* MOV CX,EA+2          */
                NEWREG(cs.Irm,DX);
                cs.IEVoffset1 -= 2;
                gen(ce,&cs);                    /* MOV DX,EA            */
                cs.IEVoffset1 += 4;
                NEWREG(cs.Irm,BX);
                gen(ce,&cs);                    /* MOV BX,EA+4          */
        }
        else
                assert(0);
        c = cat(c,ce);
    L3:
        c = cat(c,fixresult(e,retregs,pretregs));
  }
  /*fprintf(stderr,"cdafter :\n"); WRcodlst(c);*/
  return c;
}



#if TARGET_FLAT
#define cod2_setES(ty) NULL
#else
/********************************
 * Generate code to load ES with the right segment value,
 * do nothing if e is a far pointer.
 */

STATIC code * cod2_setES(tym_t ty)
{   code *c2;
    int push;

    c2 = CNIL;
    switch (tybasic(ty))
    {
        case TYnptr:
            if (!(config.flags3 & CFG3eseqds))
            {   push = 0x1E;            /* PUSH DS              */
                goto L1;
            }
            break;
        case TYcptr:
            push = 0x0E;                /* PUSH CS              */
            goto L1;
        case TYsptr:
            if ((config.wflags & WFssneds) || !(config.flags3 & CFG3eseqds))
            {   push = 0x16;            /* PUSH SS              */
            L1:
                /* Must load ES */
                c2 = getregs(mES);
                c2 = gen1(c2,push);
                gen1(c2,0x07);          /* POP ES               */
            }
            break;
    }
    return c2;
}
#endif

/********************************
 * Generate code for intrinsic strlen().
 */

code *cdstrlen( elem *e, regm_t *pretregs)
{   code *c1,*c2,*c3,*c4;
    regm_t retregs;
    tym_t ty1;

    /* Generate strlen in CX:
        LES     DI,e1
        CLR     AX                      ;scan for 0
        MOV     CX,-1                   ;largest possible string
        REPNE   SCASB
        NOT     CX
        DEC     CX
     */

    retregs = mDI;
    ty1 = e->E1->Ety;
    if (!tyreg(ty1))
        retregs |= mES;
    c1 = codelem(e->E1,&retregs,FALSE);

    /* Make sure ES contains proper segment value       */
    c2 = cod2_setES(ty1);

    c3 = getregs_imm(mAX | mCX);
    c3 = movregconst(c3,AX,0,1);                /* MOV AL,0             */
    c3 = movregconst(c3,CX,-1,0);               /* MOV CX,-1            */
    c3 = cat(c3,getregs(mDI|mCX));
    c3 = gen1(c3,0xF2);                         /* REPNE                        */
    gen1(c3,0xAE);                              /* SCASB                */
    genregs(c3,0xF7,2,CX);                      /* NOT CX               */
    c4 = gen1(CNIL,0x48 + CX);                  /* DEC CX               */

    if (*pretregs & mPSW)
    {
        c4->Iflags |= CFpsw;
        *pretregs &= ~mPSW;
    }
    return cat6(c1,c2,c3,c4,fixresult(e,mCX,pretregs),CNIL);
}

/*********************************
 * Generate code for strcmp(s1,s2) intrinsic.
 */

code *cdstrcmp( elem *e, regm_t *pretregs)
{   code *c1,*c1a,*c2,*c3,*c4;
    regm_t retregs1;
    regm_t retregs;
    tym_t ty1,ty2;
    char need_DS;
    int segreg;

    /*
        MOV     SI,s1                   ;get destination pointer (s1)
        MOV     CX,s1+2
        LES     DI,s2                   ;get source pointer (s2)
        PUSH    DS
        MOV     DS,CX
        CLR     AX                      ;scan for 0
        MOV     CX,-1                   ;largest possible string
        REPNE   SCASB
        NOT     CX                      ;CX = string length of s2
        SUB     DI,CX                   ;point DI back to beginning
        REPE    CMPSB                   ;compare string
        POP     DS
        JE      L1                      ;strings are equal
        SBB     AX,AX
        SBB     AX,-1
    L1:
    */

    retregs1 = mSI;
    ty1 = e->E1->Ety;
    if (!tyreg(ty1))
        retregs1 |= mCX;
    c1 = codelem(e->E1,&retregs1,FALSE);

    retregs = mDI;
    ty2 = e->E2->Ety;
    if (!tyreg(ty2))
        retregs |= mES;
    c1 = cat(c1,scodelem(e->E2,&retregs,retregs1,FALSE));

    /* Make sure ES contains proper segment value       */
    c2 = cod2_setES(ty2);
    c3 = getregs_imm(mAX | mCX);

    /* Load DS with right value */
    switch (tybasic(ty1))
    {
        case TYnptr:
            need_DS = FALSE;
            break;
        case TYsptr:
            if (config.wflags & WFssneds)       /* if sptr can't use DS segment */
                segreg = SEG_SS;
            else
                segreg = SEG_DS;
            goto L1;
        case TYcptr:
            segreg = SEG_CS;
        L1:
            c3 = gen1(c3,0x1E);                         /* PUSH DS      */
            gen1(c3,0x06 + (segreg << 3));              /* PUSH segreg  */
            gen1(c3,0x1F);                              /* POP  DS      */
            need_DS = TRUE;
            break;
        case TYfptr:
        case TYvptr:
        case TYhptr:
            c3 = gen1(c3,0x1E);                         /* PUSH DS      */
            gen2(c3,0x8E,modregrm(3,SEG_DS,CX));        /* MOV DS,CX    */
            need_DS = TRUE;
            break;
        default:
            assert(0);
    }

    c3 = movregconst(c3,AX,0,0);                /* MOV AX,0             */
    c3 = movregconst(c3,CX,-1,0);               /* MOV CX,-1            */
    c3 = cat(c3,getregs(mSI|mDI|mCX));
    c3 = gen1(c3,0xF2);                         /* REPNE                        */
    gen1(c3,0xAE);                              /* SCASB                */
    genregs(c3,0xF7,2,CX);                      /* NOT CX               */
    genregs(c3,0x2B,DI,CX);                     /* SUB DI,CX            */
    gen1(c3,0xF3);                              /* REPE                 */
    gen1(c3,0xA6);                              /* CMPSB                */
    if (need_DS)
        gen1(c3,0x1F);                          /* POP DS               */
    c4 = gennop(CNIL);
    if (*pretregs != mPSW)                      /* if not flags only    */
    {
        genjmp(c3,JE,FLcode,(block *) c4);      /* JE L1                */
        c3 = cat(c3,getregs(mAX));
        genregs(c3,0x1B,AX,AX);                 /* SBB AX,AX            */
        genc2(c3,0x81,modregrm(3,3,AX),(targ_uns)-1);   /* SBB AX,-1            */
    }

    *pretregs &= ~mPSW;
    return cat6(c1,c2,c3,c4,fixresult(e,mAX,pretregs),CNIL);
}

/*********************************
 * Generate code for memcmp(s1,s2,n) intrinsic.
 */

code *cdmemcmp(elem *e,regm_t *pretregs)
{   code *c1,*c2,*c3,*c4;
    regm_t retregs1;
    regm_t retregs;
    regm_t retregs3;
    tym_t ty1,ty2;
    char need_DS;
    int segreg;
    elem *e1;

    /*
        MOV     SI,s1                   ;get destination pointer (s1)
        MOV     DX,s1+2
        LES     DI,s2                   ;get source pointer (s2)
        MOV     CX,n                    ;get number of bytes to compare
        PUSH    DS
        MOV     DS,DX
        XOR     AX,AX
        REPE    CMPSB                   ;compare string
        POP     DS
        JE      L1                      ;strings are equal
        SBB     AX,AX
        SBB     AX,-1
    L1:
    */

    e1 = e->E1;
    assert(e1->Eoper == OPparam);

    // Get s1 into DX:SI
    retregs1 = mSI;
    ty1 = e1->E1->Ety;
    if (!tyreg(ty1))
        retregs1 |= mDX;
    c1 = codelem(e1->E1,&retregs1,FALSE);

    // Get s2 into ES:DI
    retregs = mDI;
    ty2 = e1->E2->Ety;
    if (!tyreg(ty2))
        retregs |= mES;
    c1 = cat(c1,scodelem(e1->E2,&retregs,retregs1,FALSE));
    freenode(e1);

    // Get nbytes into CX
    retregs3 = mCX;
    c1 = cat(c1,scodelem(e->E2,&retregs3,retregs | retregs1,FALSE));

    /* Make sure ES contains proper segment value       */
    c2 = cod2_setES(ty2);

    /* Load DS with right value */
    c3 = NULL;
    switch (tybasic(ty1))
    {
        case TYnptr:
            need_DS = FALSE;
            break;
        case TYsptr:
            if (config.wflags & WFssneds)       /* if sptr can't use DS segment */
                segreg = SEG_SS;
            else
                segreg = SEG_DS;
            goto L1;
        case TYcptr:
            segreg = SEG_CS;
        L1:
            c3 = gen1(c3,0x1E);                         /* PUSH DS      */
            gen1(c3,0x06 + (segreg << 3));              /* PUSH segreg  */
            gen1(c3,0x1F);                              /* POP  DS      */
            need_DS = TRUE;
            break;
        case TYfptr:
        case TYvptr:
        case TYhptr:
            c3 = gen1(c3,0x1E);                         /* PUSH DS      */
            gen2(c3,0x8E,modregrm(3,SEG_DS,DX));        /* MOV DS,DX    */
            need_DS = TRUE;
            break;
        default:
            assert(0);
    }

#if 1
    c3 = cat(c3,getregs(mAX));
    c3 = gen2(c3,0x33,modregrm(3,AX,AX));       // XOR AX,AX
#else
    if (*pretregs != mPSW)                      // if not flags only
        c3 = regwithvalue(c3,mAX,0,NULL,0);     // put 0 in AX
#endif

    c3 = cat(c3,getregs(mCX | mSI | mDI));
    c3 = gen1(c3,0xF3);                         /* REPE                 */
    gen1(c3,0xA6);                              /* CMPSB                */
    if (need_DS)
        gen1(c3,0x1F);                          /* POP DS               */
    if (*pretregs != mPSW)                      /* if not flags only    */
    {
        c4 = gennop(CNIL);
        genjmp(c3,JE,FLcode,(block *) c4);      /* JE L1                */
        c3 = cat(c3,getregs(mAX));
        genregs(c3,0x1B,AX,AX);                 /* SBB AX,AX            */
        genc2(c3,0x81,modregrm(3,3,AX),(targ_uns)-1);   /* SBB AX,-1            */
        c3 = cat(c3,c4);
    }

    *pretregs &= ~mPSW;
    return cat4(c1,c2,c3,fixresult(e,mAX,pretregs));
}

/*********************************
 * Generate code for strcpy(s1,s2) intrinsic.
 */

code *cdstrcpy(elem *e,regm_t *pretregs)
{   code *c1,*c2,*c3,*c4;
    regm_t retregs;
    tym_t ty1,ty2;
    char need_DS;
    int segreg;

    /*
        LES     DI,s2                   ;ES:DI = s2
        CLR     AX                      ;scan for 0
        MOV     CX,-1                   ;largest possible string
        REPNE   SCASB                   ;find end of s2
        NOT     CX                      ;CX = strlen(s2) + 1 (for EOS)
        SUB     DI,CX
        MOV     SI,DI
        PUSH    DS
        PUSH    ES
        LES     DI,s1
        POP     DS
        MOV     AX,DI                   ;return value is s1
        REP     MOVSB
        POP     DS
    */

    stackchanged = 1;
    retregs = mDI;
    ty2 = tybasic(e->E2->Ety);
    if (!tyreg(ty2))
        retregs |= mES;
    c1 = codelem(e->E2,&retregs,FALSE);

    /* Make sure ES contains proper segment value       */
    c2 = cod2_setES(ty2);
    c3 = getregs_imm(mAX | mCX);
    c3 = movregconst(c3,AX,0,1);                /* MOV AL,0             */
    c3 = movregconst(c3,CX,-1,0);               /* MOV CX,-1            */
    c3 = cat(c3,getregs(mAX|mCX|mSI|mDI));
    c3 = gen1(c3,0xF2);                         /* REPNE                        */
    gen1(c3,0xAE);                              /* SCASB                */
    genregs(c3,0xF7,2,CX);                      /* NOT CX               */
    genregs(c3,0x2B,DI,CX);                     /* SUB DI,CX            */
    genmovreg(c3,SI,DI);                        /* MOV SI,DI            */

    /* Load DS with right value */
    switch (ty2)
    {
        case TYnptr:
            need_DS = FALSE;
            break;
        case TYsptr:
            if (config.wflags & WFssneds)       /* if sptr can't use DS segment */
                segreg = SEG_SS;
            else
                segreg = SEG_DS;
            goto L1;
        case TYcptr:
            segreg = SEG_CS;
        L1:
            c3 = gen1(c3,0x1E);                         /* PUSH DS      */
            gen1(c3,0x06 + (segreg << 3));              /* PUSH segreg  */
            genadjesp(c3,REGSIZE * 2);
            need_DS = TRUE;
            break;
        case TYfptr:
        case TYvptr:
        case TYhptr:
            segreg = SEG_ES;
            goto L1;
            break;
        default:
            assert(0);
    }

    retregs = mDI;
    ty1 = tybasic(e->E1->Ety);
    if (!tyreg(ty1))
        retregs |= mES;
    c3 = cat(c3,scodelem(e->E1,&retregs,mCX|mSI,FALSE));
    c3 = cat(c3,getregs(mAX|mCX|mSI|mDI));

    /* Make sure ES contains proper segment value       */
    if (ty2 != TYnptr || ty1 != ty2)
        c4 = cod2_setES(ty1);
    else
        c4 = CNIL;                              /* ES is already same as DS */

    if (need_DS)
        c4 = gen1(c4,0x1F);                     /* POP DS               */
    if (*pretregs)
        c4 = genmovreg(c4,AX,DI);               /* MOV AX,DI            */
    c4 = gen1(c4,0xF3);                         /* REP                  */
    gen1(c4,0xA4);                              /* MOVSB                */

    if (need_DS)
    {   gen1(c4,0x1F);                          /* POP DS               */
        genadjesp(c4,-(REGSIZE * 2));
    }
    return cat6(c1,c2,c3,c4,fixresult(e,mAX | mES,pretregs),CNIL);
}

/*********************************
 * Generate code for memcpy(s1,s2,n) intrinsic.
 *  OPmemcpy
 *   /   \
 * s1   OPparam
 *       /   \
 *      s2    n
 */

code *cdmemcpy(elem *e,regm_t *pretregs)
{   code *c1,*c2,*c3,*c4;
    regm_t retregs1;
    regm_t retregs2;
    regm_t retregs3;
    tym_t ty1,ty2;
    char need_DS;
    int segreg;
    elem *e2;

    /*
        MOV     SI,s2
        MOV     DX,s2+2
        MOV     CX,n
        LES     DI,s1
        PUSH    DS
        MOV     DS,DX
        MOV     AX,DI                   ;return value is s1
        REP     MOVSB
        POP     DS
    */

    e2 = e->E2;
    assert(e2->Eoper == OPparam);

    // Get s2 into DX:SI
    retregs2 = mSI;
    ty2 = e2->E1->Ety;
    if (!tyreg(ty2))
        retregs2 |= mDX;
    c1 = codelem(e2->E1,&retregs2,FALSE);

    // Get nbytes into CX
    retregs3 = mCX;
    c1 = cat(c1,scodelem(e2->E2,&retregs3,retregs2,FALSE));
    freenode(e2);

    // Get s1 into ES:DI
    retregs1 = mDI;
    ty1 = e->E1->Ety;
    if (!tyreg(ty1))
        retregs1 |= mES;
    c1 = cat(c1,scodelem(e->E1,&retregs1,retregs2 | retregs3,FALSE));

    /* Make sure ES contains proper segment value       */
    c2 = cod2_setES(ty1);

    /* Load DS with right value */
    c3 = NULL;
    switch (tybasic(ty2))
    {
        case TYnptr:
            need_DS = FALSE;
            break;
        case TYsptr:
            if (config.wflags & WFssneds)       /* if sptr can't use DS segment */
                segreg = SEG_SS;
            else
                segreg = SEG_DS;
            goto L1;
        case TYcptr:
            segreg = SEG_CS;
        L1:
            c3 = gen1(c3,0x1E);                         /* PUSH DS      */
            gen1(c3,0x06 + (segreg << 3));              /* PUSH segreg  */
            gen1(c3,0x1F);                              /* POP  DS      */
            need_DS = TRUE;
            break;
        case TYfptr:
        case TYvptr:
        case TYhptr:
            c3 = gen1(c3,0x1E);                         /* PUSH DS      */
            gen2(c3,0x8E,modregrm(3,SEG_DS,DX));        /* MOV DS,DX    */
            need_DS = TRUE;
            break;
        default:
            assert(0);
    }

    if (*pretregs)                              // if need return value
    {   c3 = cat(c3,getregs(mAX));
        c3 = genmovreg(c3,AX,DI);
    }

    if (0 && I32 && config.flags4 & CFG4speed)
    {
        /* This is only faster if the memory is dword aligned, if not
         * it is significantly slower than just a rep movsb.
         */
        /*      mov     EDX,ECX
         *      shr     ECX,2
         *      jz      L1
         *      repe    movsd
         * L1:  and     EDX,3
         *      jz      L2
         *      mov     ECX,EDX
         *      repe    movsb
         * L2:  nop
         */
        c3 = cat(c3,getregs(mSI | mDI | mCX | mDX));
        c3 = genmovreg(c3,DX,CX);               // MOV EDX,ECX
        c3 = genc2(c3,0xC1,modregrm(3,5,CX),2); // SHR ECX,2
        code *cx = genc2(CNIL, 0x81, modregrm(3,4,DX),3);       // AND EDX,3
        genjmp(c3, JE, FLcode, (block *)cx);                    // JZ L1
        gen1(c3,0xF3);                                          // REPE
        gen1(c3,0xA5);                                          // MOVSW
        c3 = cat(c3,cx);

        code *cnop = gennop(CNIL);
        genjmp(c3, JE, FLcode, (block *)cnop);  // JZ L2
        genmovreg(c3,CX,DX);                    // MOV ECX,EDX
        gen1(c3,0xF3);                          // REPE
        gen1(c3,0xA4);                          // MOVSB
        c3 = cat(c3, cnop);
    }
    else
    {
        c3 = cat(c3,getregs(mSI | mDI | mCX));
        if (!I32 && config.flags4 & CFG4speed)          // if speed optimization
        {   c3 = gen2(c3,0xD1,modregrm(3,5,CX));        // SHR CX,1
            gen1(c3,0xF3);                              // REPE
            gen1(c3,0xA5);                              // MOVSW
            gen2(c3,0x11,modregrm(3,CX,CX));            // ADC CX,CX
        }
        c3 = gen1(c3,0xF3);                             // REPE
        gen1(c3,0xA4);                                  // MOVSB
        if (need_DS)
            gen1(c3,0x1F);                              // POP DS
    }
    return cat4(c1,c2,c3,fixresult(e,mES|mAX,pretregs));
}

/*********************************
 * Generate code for memset(s,val,n) intrinsic.
 *      (s OPmemset (n OPparam val))
 */

#if 1
code *cdmemset(elem *e,regm_t *pretregs)
{   code *c1,*c2,*c3 = NULL,*c4;
    regm_t retregs1;
    regm_t retregs2;
    regm_t retregs3;
    unsigned reg,vreg;
    tym_t ty1;
    elem *e2,*e1;
    int segreg;
    unsigned remainder;
    targ_uns numbytes,numwords;
    int op;
    targ_uns value;

    //printf("cdmemset(*pretregs = x%x)\n", *pretregs);
    e1 = e->E1;
    e2 = e->E2;
    assert(e2->Eoper == OPparam);

    if (e2->E2->Eoper == OPconst)
    {
        value = el_tolong(e2->E2);
        value &= 0xFF;
        value |= value << 8;
        value |= value << 16;
    }

    if (e2->E1->Eoper == OPconst)
    {
        numbytes = el_tolong(e2->E1);
        if (numbytes <= REP_THRESHOLD && I32 && // doesn't work for 16 bits
            e2->E2->Eoper == OPconst)
        {
            targ_uns offset = 0;
            retregs1 = *pretregs;
            if (!retregs1)
                retregs1 = ALLREGS;
            c1 = codelem(e->E1,&retregs1,FALSE);
            reg = findreg(retregs1);
            if (e2->E2->Eoper == OPconst)
            {
                switch (numbytes)
                {
                    case 4:                     // MOV [reg],imm32
                        c3 = genc2(CNIL,0xC7,modregrm(0,0,reg),value);
                        goto fixres;
                    case 2:                     // MOV [reg],imm16
                        c3 = genc2(CNIL,0xC7,modregrm(0,0,reg),value);
                        c3->Iflags = CFopsize;
                        goto fixres;
                    case 1:                     // MOV [reg],imm8
                        c3 = genc2(CNIL,0xC6,modregrm(0,0,reg),value);
                        goto fixres;
                }
            }

            c1 = regwithvalue(c1, BYTEREGS & ~retregs1, value, &vreg, 0);
            freenode(e2->E2);
            freenode(e2);

            while (numbytes >= REGSIZE)
            {                           // MOV dword ptr offset[reg],vreg
                c2 = gen2(CNIL,0x89,modregrm(2,vreg,reg));
                c2->IEVoffset1 = offset;
                c2->IFL1 = FLconst;
                numbytes -= REGSIZE;
                offset += REGSIZE;
                c3 = cat(c3,c2);
            }
            if (numbytes & 2)
            {                           // MOV word ptr offset[reg],vreg
                c2 = gen2(CNIL,0x89,modregrm(2,vreg,reg));
                c2->IEVoffset1 = offset;
                c2->IFL1 = FLconst;
                c2->Iflags = CFopsize;
                offset += 2;
                c3 = cat(c3,c2);
            }
            if (numbytes & 1)
            {                           // MOV byte ptr offset[reg],vreg
                c2 = gen2(CNIL,0x88,modregrm(2,vreg,reg));
                c2->IEVoffset1 = offset;
                c2->IFL1 = FLconst;
                c3 = cat(c3,c2);
            }
fixres:
            return cat3(c1,c3,fixresult(e,retregs1,pretregs));
        }
    }

    // Get nbytes into CX
    retregs2 = mCX;
    if (I32 && e2->E1->Eoper == OPconst && e2->E2->Eoper == OPconst)
    {
        remainder = numbytes & (REGSIZE - 1);
        numwords  = numbytes / REGSIZE;         // number of words
        op = 0xAB;                              // moving by words
        c1 = getregs(mCX);
        c1 = movregconst(c1,CX,numwords,0);     // # of bytes/words
    }
    else
    {
        remainder = 0;
        op = 0xAA;                              // must move by bytes
        c1 = codelem(e2->E1,&retregs2,FALSE);
    }

    // Get val into AX

    retregs3 = mAX;
    if (I32 && e2->E2->Eoper == OPconst)
    {
        c1 = regwithvalue(c1, mAX, value, NULL, 0);
        freenode(e2->E2);
    }
    else
    {
        c1 = cat(c1,scodelem(e2->E2,&retregs3,retregs2,FALSE));
#if 0
        if (I32)
        {
            c1 = gen2(c1,0x8A,modregrm(3,AH,AL));       // MOV AH,AL
            c1 = genc2(c1,0xC1,modregrm(3,4,AX),8);     // SHL EAX,8
            c1 = gen2(c1,0x8A,modregrm(3,AL,AH));       // MOV AL,AH
            c1 = genc2(c1,0xC1,modregrm(3,4,AX),8);     // SHL EAX,8
            c1 = gen2(c1,0x8A,modregrm(3,AL,AH));       // MOV AL,AH
        }
#endif
    }
    freenode(e2);

    // Get s into ES:DI
    retregs1 = mDI;
    ty1 = e->E1->Ety;
    if (!tyreg(ty1))
        retregs1 |= mES;
    c1 = cat(c1,scodelem(e->E1,&retregs1,retregs2 | retregs3,FALSE));
    reg = DI; //findreg(retregs1);

    // Make sure ES contains proper segment value
    c2 = cod2_setES(ty1);

    c3 = NULL;
    if (*pretregs)                              // if need return value
    {   c3 = getregs(mBX);
        c3 = genmovreg(c3,BX,DI);
    }

    c3 = cat(c3,getregs(mDI | mCX));
    if (!I32 && config.flags4 & CFG4speed)      // if speed optimization
    {
        c3 = cat(c3,getregs(mAX));
        c3 = gen2(c3,0x8A,modregrm(3,AH,AL));   // MOV AH,AL
        gen2(c3,0xD1,modregrm(3,5,CX));         // SHR CX,1
        gen1(c3,0xF3);                          // REP
        gen1(c3,0xAB);                          // STOSW
        gen2(c3,0x11,modregrm(3,CX,CX));        // ADC CX,CX
        op = 0xAA;
    }

    c3 = gen1(c3,0xF3);                         // REP
    gen1(c3,op);                                // STOSD
    if (remainder & 2)
    {
        code *ctmp;
        ctmp = gen2(CNIL,0x89,modregrm(2,AX,reg));
        ctmp->Iflags = CFopsize;
        ctmp->IFL1 = FLconst;
        c3 = cat(c3,ctmp);
    }
    if (remainder & 1)
    {
        code *ctmp;
        ctmp = gen2(CNIL,0x88,modregrm(2,AX,reg));
        ctmp->IEVoffset1 = (remainder & 2) ? 2 : 0;
        ctmp->IFL1 = FLconst;
        c3 = cat(c3,ctmp);
    }
    regimmed_set(CX,0);
    return cat4(c1,c2,c3,fixresult(e,mES|mBX,pretregs));
}
#else
// BUG: Pat made many improvements in the linux version, I need
// to verify they work for 16 bits and fold them in. -Walter

code *cdmemset(elem *e,regm_t *pretregs)
{   code *c1,*c2,*c3 = NULL,*c4;
    regm_t retregs1;
    regm_t retregs2;
    regm_t retregs3;
    tym_t ty1;
    elem *e2;
    targ_size_t value;

    /*
        les     DI,s
        mov     BX,DI           ;Return value.
        mov     CX,n
        mov     AL,val
        mov     AH,AL           ;Set up a 16 bit pattern.
        shr     CX,1
        rep     stosw
        adc     CX,CX
        rep     stosb
    */

    e2 = e->E2;
    assert(e2->Eoper == OPparam);

    // Get nbytes into CX
    retregs2 = mCX;
    c1 = codelem(e2->E1,&retregs2,FALSE);

    // Get val into AX
    retregs3 = mAX;
    c1 = cat(c1,scodelem(e2->E2,&retregs3,retregs2,FALSE));
    freenode(e2);

    // Get s into ES:DI
    retregs1 = mDI;
    ty1 = e->E1->Ety;
    if (!tyreg(ty1))
        retregs1 |= mES;
    c1 = cat(c1,scodelem(e->E1,&retregs1,retregs2 | retregs3,FALSE));

    /* Make sure ES contains proper segment value       */
    c2 = cod2_setES(ty1);

    c3 = NULL;
    if (*pretregs)                              // if need return value
    {   c3 = getregs(mBX);
        c3 = genmovreg(c3,BX,DI);
    }

    c3 = cat(c3,getregs(mDI | mCX));
    if (!I32 && config.flags4 & CFG4speed)      // if speed optimization
    {
        c3 = cat(c3,getregs(mAX));
        c3 = gen2(c3,0x8A,modregrm(3,AH,AL));   // MOV AH,AL
        gen2(c3,0xD1,modregrm(3,5,CX));         // SHR CX,1
        gen1(c3,0xF3);                          // REP
        gen1(c3,0xAB);                          // STOSW
        gen2(c3,0x11,modregrm(3,CX,CX));        // ADC CX,CX
    }
    c3 = gen1(c3,0xF3);                         // REP
    gen1(c3,0xAA);                              // STOSB
    regimmed_set(CX,0);
    return cat4(c1,c2,c3,fixresult(e,mES|mBX,pretregs));
}
#endif

/**********************
 * Do structure assignments.
 * This should be fixed so that (s1 = s2) is rewritten to (&s1 = &s2).
 * Mebbe call cdstreq() for double assignments???
 */

code *cdstreq(elem *e,regm_t *pretregs)
{ code *c1,*c2,*c3;
  code *c1a;
  regm_t srcregs,dstregs;               /* source & destination reg masks */
  targ_uns numbytes;
  char need_DS = FALSE;
  elem *e1 = e->E1,*e2 = e->E2;
  int segreg;

  numbytes = e->Enumbytes;              /* # of bytes in structure/union */

    //printf("cdstreq(e = %p, *pretregs = x%x)\n", e, *pretregs);

    /* First, load pointer to rvalue into SI                            */
    srcregs = mSI;                      /* source is DS:SI              */
    c1 = docommas(&e2);
    if (e2->Eoper == OPind)             /* if (.. = *p)                 */
    {   elem *e21 = e2->E1;

        segreg = SEG_DS;
        switch (tybasic(e21->Ety))
        {
            case TYsptr:
                if (config.wflags & WFssneds)   /* if sptr can't use DS segment */
                    segreg = SEG_SS;
                break;
            case TYcptr:
                if (!(config.exe & EX_flat))
                    segreg = SEG_CS;
                break;
            case TYfptr:
            case TYvptr:
            case TYhptr:
                srcregs |= mCX;         /* get segment also             */
                need_DS = TRUE;
                break;
        }
        c1a = codelem(e21,&srcregs,FALSE);
        freenode(e2);
        if (segreg != SEG_DS)           /* if not DS                    */
        {   c1a = cat(c1a,getregs(mCX));
            c1a = gen2(c1a,0x8C,modregrm(3,segreg,CX)); /* MOV CX,segreg */
            need_DS = TRUE;
        }
    }
    else if (e2->Eoper == OPvar)
    {
#if !TARGET_FLAT
        if (e2->EV.sp.Vsym->ty() & mTYfar) // if e2 is in a far segment
        {   srcregs |= mCX;             /* get segment also             */
            need_DS = TRUE;
            c1a = cdrelconst(e2,&srcregs);
        }
        else
#endif
        {
            c1a = cdrelconst(e2,&srcregs);
            segreg = segfl[el_fl(e2)];
            if ((config.wflags & WFssneds) && segreg == SEG_SS || /* if source is on stack */
                segreg == SEG_CS)               /* if source is in CS */
            {   code *c;

                need_DS = TRUE;         /* we need to reload DS         */
                // Load CX with segment
                srcregs |= mCX;
                c = getregs(mCX);
                c = gen2(c,0x8C,                /* MOV CX,[SS|CS]       */
                    modregrm(3,segreg,CX));
                c1a = cat(c,c1a);
            }
        }
        freenode(e2);
    }
    else
    {
        if (!(config.exe & EX_flat))
        {   need_DS = TRUE;
            srcregs |= mCX;
        }
        c1a = codelem(e2,&srcregs,FALSE);
    }
    c1 = cat(c1,c1a);

  /* now get pointer to lvalue (destination) in ES:DI                   */
  dstregs = (config.exe & EX_flat) ? mDI : mES|mDI;
  if (e1->Eoper == OPind)               /* if (*p = ..)                 */
  {
        if (tyreg(e1->E1->Ety))
            dstregs = mDI;
        c2 = cod2_setES(e1->E1->Ety);
        c2 = cat(c2,scodelem(e1->E1,&dstregs,srcregs,FALSE));
  }
  else
        c2 = cdrelconst(e1,&dstregs);
  freenode(e1);

  c3 = getregs((srcregs | dstregs) & (mLSW | mDI));
  if (need_DS)
  {     assert(!(config.exe & EX_flat));
        c3 = gen1(c3,0x1E);                     /* PUSH DS              */
        gen2(c3,0x8E,modregrm(3,SEG_DS,CX));    /* MOV DS,CX            */
  }
  if (numbytes <= REGSIZE * (6 + (REGSIZE == 4)))
  {     while (numbytes >= REGSIZE)
        {   c3 = gen1(c3,0xA5);         /* MOVSW                        */
            numbytes -= REGSIZE;
        }
        //if (numbytes)
        //    printf("cdstreq numbytes %d\n",numbytes);
        while (numbytes--)
            c3 = gen1(c3,0xA4);         /* MOVSB                        */
  }
    else
    {
#if 1
        unsigned remainder;

        remainder = numbytes & (REGSIZE - 1);
        numbytes /= REGSIZE;            // number of words
        c3 = cat(c3,getregs_imm(mCX));
        c3 = movregconst(c3,CX,numbytes,0);     // # of bytes/words
        gen1(c3,0xF3);                          // REP
        gen1(c3,0xA5);                  // REP MOVSD
        regimmed_set(CX,0);             // note that CX == 0
        for (; remainder; remainder--)
        {
            gen1(c3, 0xA4);             // MOVSB
        }
#else
        unsigned movs;

        if (numbytes & (REGSIZE - 1))   /* if odd                       */
                movs = 0xA4;            /* MOVSB                        */
        else
        {       movs = 0xA5;            /* MOVSW                        */
                numbytes /= REGSIZE;    /* # of words                   */
        }
        c3 = cat(c3,getregs_imm(mCX));
        c3 = movregconst(c3,CX,numbytes,0);     /* # of bytes/words     */
        gen1(c3,0xF3);                          /* REP                  */
        gen1(c3,movs);
        regimmed_set(CX,0);             /* note that CX == 0            */
#endif
    }
    if (need_DS)
        gen1(c3,0x1F);                          // POP  DS
    assert(!(*pretregs & mPSW));
    if (*pretregs)
    {   /* ES:DI points past what we want       */
        regm_t retregs;

        genc2(c3,0x81,modregrm(3,5,DI),e->Enumbytes);   /* SUB DI,numbytes */
        retregs = mDI;
        if (*pretregs & mMSW && !(config.exe & EX_flat))
            retregs |= mES;
        c3 = cat(c3,fixresult(e,retregs,pretregs));
    }
    return cat3(c1,c2,c3);
}

/**********************
 * Get the address of.
 * Is also called by cdstreq() to set up pointer to a structure.
 */

code *cdrelconst(elem *e,regm_t *pretregs)
{ code *c,*c1;
  enum SC sclass;
  unsigned mreg,                /* segment of the address (TYfptrs only) */
        lreg;                   /* offset of the address                */
  tym_t tym;

  //printf("cdrelconst(e = %p)\n", e);

  c = CNIL;

  /* The following should not happen, but cgelem.c is a little stupid.  */
  /* Assertion can be tripped by func("string" == 0); and similar       */
  /* things. Need to add goals to optelem() to fix this completely.     */
  /*assert((*pretregs & mPSW) == 0);*/
  if (*pretregs & mPSW)
  {     *pretregs &= ~mPSW;
        c = gentstreg(c,SP);            // SP is never 0
  }
  if (!*pretregs)
        return c;

  assert(e);
  tym = tybasic(e->Ety);
  switch (tym)
  {     case TYstruct:
        case TYarray:
        case TYldouble:
        case TYildouble:
        case TYcldouble:
            tym = TYnptr;               // don't confuse allocreg()
#if !TARGET_FLAT
            if (*pretregs & (mES | mCX) || e->Ety & mTYfar)
            {
                    tym = TYfptr;
            }
#endif
            break;
        case TYifunc:
            tym = TYfptr;
            break;
        default:
            if (tyfunc(tym))
                tym = tyfarfunc(tym) ? TYfptr : TYnptr;
            break;
  }
  /*assert(tym & typtr);*/              /* don't fail on (int)&a        */

  c = cat(c,allocreg(pretregs,&lreg,tym));
  if (tysize[tym] > REGSIZE)            /* fptr could've been cast to long */
  {     tym_t ety;
        symbol *s;

        //elem_print(e);
        assert(!TARGET_FLAT);

        if (*pretregs & mES)
        {       regm_t scratch = (mAX|mBX|mDX|mDI) & ~mask[lreg];
                /* Do not allocate CX or SI here, as cdstreq() needs    */
                /* them preserved. cdstreq() should use scodelem()...   */

                c = cat(c,allocreg(&scratch,&mreg,TYint));
        }
        else
        {       mreg = lreg;
                lreg = findreglsw(*pretregs);
        }

        /* if (get segment of function that isn't necessarily in the    */
        /* current segment (i.e. CS doesn't have the right value in it) */
        s = e->EV.sp.Vsym;
        if (s->Sfl == FLdatseg)
        {   assert(0);
            goto loadreg;
        }
        sclass = (enum SC) s->Sclass;
        ety = tybasic(s->ty());
        if ((tyfarfunc(ety) || ety == TYifunc) &&
            (sclass == SCextern || ClassInline(sclass) || config.wflags & WFthunk)
            || s->Sfl == FLfardata
            || (s->ty() & mTYcs && s->Sseg != cseg && (LARGECODE || s->Sclass == SCcomdat))
//          || (s->Sfl == FLextern && s->ty() & mTYcs)
//          || (LARGECODE && s->Sclass == SCcomdat)
           )
        {       /* MOV mreg,seg of symbol       */
                c1 = gencs(CNIL,0xB8 + mreg,0,FLextern,s);
                c1->Iflags = CFseg;
                c = cat(c,c1);
                assert(!TARGET_FLAT);
        }
        else
        {   int fl;

        loadreg:
            fl = s->Sfl;
            if (s->ty() & mTYcs)
                fl = FLcsdata;
            c = gen2(c,0x8C,            /* MOV mreg,SEG REGISTER        */
                modregrm(3,segfl[fl],mreg));
        }
        if (*pretregs & mES)
                gen2(c,0x8E,modregrm(3,0,mreg));        /* MOV ES,mreg  */
  }
  return cat(c,getoffset(e,lreg));
}

/*********************************
 * Load the offset portion of the address represented by e into
 * reg.
 */

code *getoffset(elem *e,unsigned reg)
{ code cs;
  code *c;
  enum FL fl;

  cs.Iflags = 0;
  cs.Ijty = 0;
  assert(e->Eoper == OPvar || e->Eoper == OPrelconst);
  fl = el_fl(e);
  switch (fl)
  {
    case FLdatseg:
        cs.IEV2._EP.Vpointer = e->EV.Vpointer;
        goto L3;

    case FLfardata:
        assert(!TARGET_FLAT);
        goto L4;

    case FLtlsdata:
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_SOLARIS
    {   /* Generate:
         *      MOV reg,GS:[00000000]
         *      ADD reg, offset s@TLS_LE
         * for locals, and for globals:
         *      MOV reg,GS:[00000000]
         *      ADD reg, s@TLS_IE
         * note different fixup
         */
      L5:
        int stack = 0;
        c = NULL;
        if (reg == STACK)
        {   regm_t retregs = ALLREGS;

            c = allocreg(&retregs,&reg,TYoffset);
            reg = findreg(retregs);
            stack = 1;
        }

        code css;
        css.Iop = 0x8B;
        css.Irm = modregrm(0, reg, BPRM);
        css.Iflags = CFgs;
        css.Ijty = 0;
        css.IFL1 = FLconst;
        css.IEV1.Vuns = 0;
        c = gen(c, &css);               // MOV reg,GS:[00000000]

        if (e->EV.sp.Vsym->Sclass == SCstatic || e->EV.sp.Vsym->Sclass == SClocstat)
        {   // ADD reg, offset s
            cs.Iop = 0x81;
            cs.Irm = modregrm(3,0,reg);
            cs.Iflags = CFoff;
            css.Ijty = 0;
            cs.IFL2 = fl;
            cs.IEVsym2 = e->EV.sp.Vsym;
            cs.IEVoffset2 = e->EV.sp.Voffset;
        }
        else
        {   // ADD reg, s
            cs.Iop = 0x03;
            cs.Irm = modregrm(0,reg,BPRM);
            cs.Iflags = CFoff;
            css.Ijty = 0;
            cs.IFL1 = fl;
            cs.IEVsym1 = e->EV.sp.Vsym;
            cs.IEVoffset1 = e->EV.sp.Voffset;
        }
        c = gen(c, &cs);                // ADD reg, xxxx

        if (stack)
        {
            c = gen1(c,0x50 + reg);                     /* PUSH reg     */
            c = genadjesp(c,REGSIZE);
            stackchanged = 1;
        }
        break;
    }
#else
        goto L4;
#endif

    case FLfunc:
        fl = FLextern;                  /* don't want PC relative addresses */
        goto L4;

    case FLextern:
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_SOLARIS
        if (e->EV.sp.Vsym->ty() & mTYthread)
            goto L5;
#endif
    case FLdata:
    case FLudata:
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_SOLARIS
    case FLgot:
    case FLgotoff:
#endif
    case FLcsdata:
    L4:
        cs.IEVsym2 = e->EV.sp.Vsym;
        cs.IEVoffset2 = e->EV.sp.Voffset;
    L3:
        if (reg == STACK)
        {   stackchanged = 1;
            cs.Iop = 0x68;              /* PUSH immed16                 */
            c = genadjesp(NULL,REGSIZE);
        }
        else
        {   cs.Iop = 0xB8 + reg;        /* MOV reg,immed16              */
            c = NULL;
        }
        cs.Iflags = CFoff;              /* want offset only             */
        cs.IFL2 = fl;
        c = gen(c,&cs);
        break;

#if 0 && TARGET_LINUX
    case FLgot:
    case FLgotoff:
        {
        gotref = 1;
        symbol *s = e->EV.sp.Vsym;
        // When using 8B (MOV), indicating that rm is used
        // rm operands are always placed in IEV1 not IEV2
        cs.IEVsym1 = s;
        cs.IEVoffset1 = e->EV.sp.Voffset;
        cs.Irm = modregrm(2,reg,BX);    // reg,disp32[EBX]
        cs.IFL1 = fl;
        cs.Iop = (fl == FLgotoff)
                ? 0x8D                  // LEA reg, s[EBX]
                : 0x8B;                 // MOV reg, s[EBX]
        cs.Iflags = CFoff;              // want offset only
        c = gen(NULL,&cs);
        break;
        }
#endif

    case FLreg:
        /* Allow this since the tree optimizer puts & in front of       */
        /* register doubles.                                            */
        goto L2;
    case FLauto:
    case FLtmp:
    case FLbprel:
    case FLfltreg:
        reflocal = TRUE;
        goto L2;
    case FLpara:
        refparam = TRUE;
    L2:
        if (reg == STACK)
        {   regm_t retregs = ALLREGS;

            c = allocreg(&retregs,&reg,TYoffset);
            reg = findreg(retregs);
            c = cat(c,loadea(e,&cs,0x8D,reg,0,0,0));    /* LEA reg,EA   */
            c = gen1(c,0x50 + reg);                     /* PUSH reg     */
            c = genadjesp(c,REGSIZE);
            stackchanged = 1;
        }
        else
            c = loadea(e,&cs,0x8D,reg,0,0,0);   /* LEA reg,EA           */
        break;
    default:
#ifdef DEBUG
        elem_print(e);
        debugx(WRFL(fl));
#endif
        assert(0);
  }
  return c;
}

/******************
 * Negate, sqrt operator
 */

code *cdneg(elem *e,regm_t *pretregs)
{ unsigned byte;
  regm_t retregs,possregs;
  int reg;
  int sz;
  tym_t tyml;
  code *c,*c1,*cg;

  //printf("cdneg()\n");
  //elem_print(e);
  if (*pretregs == 0)
        return codelem(e->E1,pretregs,FALSE);
  tyml = tybasic(e->E1->Ety);
  sz = tysize[tyml];
  if (tyfloating(tyml))
  {     if (tycomplex(tyml))
            return neg_complex87(e, pretregs);
        if (config.inline8087 && ((*pretregs & (ALLREGS | mBP)) == 0 || e->Eoper == OPsqrt))
                return neg87(e,pretregs);
        retregs = (!I32 && sz == 8) ? DOUBLEREGS_16 : ALLREGS;
        c1 = codelem(e->E1,&retregs,FALSE);
        c1 = cat(c1,getregs(retregs));
        if (I32)
        {   reg = (sz == 8) ? findregmsw(retregs) : findreg(retregs);
            c1 = genc2(c1,0x81,modregrm(3,6,reg),0x80000000); /* XOR EDX,sign bit */
        }
        else
        {   reg = (sz == 8) ? AX : findregmsw(retregs);
            c1 = genc2(c1,0x81,modregrm(3,6,reg),0x8000);     /* XOR AX,0x8000 */
        }
        return cat(c1,fixresult(e,retregs,pretregs));
  }

  byte = sz == 1;
  possregs = (byte) ? BYTEREGS : allregs;
  retregs = *pretregs & possregs;
  if (retregs == 0)
        retregs = possregs;
  c1 = codelem(e->E1,&retregs,FALSE);
  cg = getregs(retregs);                /* retregs will be destroyed    */
  if (sz <= REGSIZE)
  {     unsigned reg;

        reg = findreg(retregs);
        c = gen2(CNIL,0xF7 ^ byte,modregrm(3,3,reg));   /* NEG reg      */
        if (I32 && tysize[tyml] == SHORTSIZE && *pretregs & mPSW)
            c->Iflags |= CFopsize | CFpsw;
        *pretregs &= mBP | ALLREGS;             // flags already set
  }
  else if (sz == 2 * REGSIZE)
  {     unsigned msreg,lsreg;

        msreg = findregmsw(retregs);
        c = gen2(CNIL,0xF7,modregrm(3,3,msreg)); /* NEG msreg           */
        lsreg = findreglsw(retregs);
        gen2(c,0xF7,modregrm(3,3,lsreg));       /* NEG lsreg            */
        genc2(c,0x81,modregrm(3,3,msreg),0);    /* SBB msreg,0          */
  }
  else
        assert(0);
  return cat4(c1,cg,c,fixresult(e,retregs,pretregs));
}

/******************
 * Absolute value operator
 */

code *cdabs( elem *e, regm_t *pretregs)
{ unsigned byte;
  regm_t retregs,possregs;
  int reg;
  tym_t tyml;
  code *c,*c1,*cg;
  int sz;

  if (*pretregs == 0)
        return codelem(e->E1,pretregs,FALSE);
  tyml = tybasic(e->E1->Ety);
  sz = tysize[tyml];
  if (tyfloating(tyml))
  {     if (config.inline8087 && (*pretregs & (ALLREGS | mBP)) == 0)
                return neg87(e,pretregs);
        retregs = (!I32 && sz == 8) ? DOUBLEREGS_16 : ALLREGS;
        c1 = codelem(e->E1,&retregs,FALSE);
        /*cg = callclib(e,CLIBdneg,pretregs,0);*/
        c1 = cat(c1,getregs(retregs));
        if (I32)
        {   reg = (sz == 8) ? findregmsw(retregs) : findreg(retregs);
            c1 = genc2(c1,0x81,modregrm(3,4,reg),0x7FFFFFFF); /* AND EDX,~sign bit */
        }
        else
        {   reg = (sz == 8) ? AX : findregmsw(retregs);
            c1 = genc2(c1,0x81,modregrm(3,4,reg),0x7FFF);     /* AND AX,0x7FFF */
        }
        return cat(c1,fixresult(e,retregs,pretregs));
  }

  byte = sz == 1;
  assert(byte == 0);
  byte = 0;
  possregs = (sz <= REGSIZE) ? mAX : allregs;
  retregs = *pretregs & possregs;
  if (retregs == 0)
        retregs = possregs;
  c1 = codelem(e->E1,&retregs,FALSE);
  cg = getregs(retregs);                /* retregs will be destroyed    */
  if (sz <= REGSIZE)
  {     unsigned reg;
        code *c2;

        /*      cwd
                xor     AX,DX
                sub     AX,DX
         */

        cg = cat(cg,getregs(mDX));
        reg = findreg(retregs);
        if (I32 && sz == SHORTSIZE)
            cg = gen1(cg,0x98);                         // CWDE
        cg = gen1(cg,0x99);                             // CWD
        gen2(cg,0x33 ^ byte,modregrm(3,AX,DX));         // XOR EAX,EDX
        c = gen2(CNIL,0x2B ^ byte,modregrm(3,AX,DX));   // SUB EAX,EDX
        if (I32 && sz == SHORTSIZE && *pretregs & mPSW)
            c->Iflags |= CFopsize | CFpsw;
        if (*pretregs & mPSW)
            c->Iflags |= CFpsw;
        *pretregs &= ~mPSW;                     // flags already set
  }
  else if (sz == 2 * REGSIZE)
  {     unsigned msreg,lsreg;
        code *cnop;

        /*      tst     DX
                jns     L2
                neg     DX
                neg     AX
                sbb     DX,0
            L2:
         */

        cnop = gennop(CNIL);
        msreg = findregmsw(retregs);
        lsreg = findreglsw(retregs);
        c = genorreg(CNIL,msreg,msreg);
        c = genjmp(c,JNS,FLcode,(block *)cnop);
        c = gen2(c,0xF7,modregrm(3,3,msreg));   // NEG msreg
        gen2(c,0xF7,modregrm(3,3,lsreg));       // NEG lsreg+1
        genc2(c,0x81,modregrm(3,3,msreg),0);    // SBB msreg,0
        c = cat(c,cnop);
  }
  else
        assert(0);
  return cat4(c1,cg,c,fixresult(e,retregs,pretregs));
}

/**************************
 * Post increment and post decrement.
 */

code *cdpost(elem *e,regm_t *pretregs)
{ code cs,*c1,*c2,*c3,*c4,*c5,*c6;
  unsigned reg,op,byte;
  tym_t tyml;
  regm_t retregs,possregs,idxregs;
  targ_int n;
  elem *e2;
  int sz;
  int stackpushsave;

  retregs = *pretregs;
  op = e->Eoper;                                /* OPxxxx               */
  if (retregs == 0)                             /* if nothing to return */
        return cdaddass(e,pretregs);
  c4 = c5 = CNIL;
  tyml = tybasic(e->E1->Ety);
  sz = tysize[tyml];
  e2 = e->E2;

  if (tyfloating(tyml))
  {
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_SOLARIS
        return post87(e,pretregs);
#else
        if (config.inline8087)
                return post87(e,pretregs);
        assert(sz <= 8);
        c1 = getlvalue(&cs,e->E1,DOUBLEREGS);
        freenode(e->E1);
        idxregs = idxregm(cs.Irm,cs.Isib); /* mask of index regs used   */
        cs.Iop = 0x8B;                  /* MOV DOUBLEREGS,EA            */
        c2 = fltregs(&cs,tyml);
        stackchanged = 1;
        stackpushsave = stackpush;
        if (sz == 8)
        {
            if (I32)
            {
                gen1(c2,0x50 + DX);             /* PUSH DOUBLEREGS      */
                gen1(c2,0x50 + AX);
                stackpush += DOUBLESIZE;
                retregs = DOUBLEREGS2_32;
            }
            else
            {
                gen1(c2,0x50 + AX);
                gen1(c2,0x50 + BX);
                gen1(c2,0x50 + CX);
                gen1(c2,0x50 + DX);             /* PUSH DOUBLEREGS      */
                stackpush += DOUBLESIZE + DOUBLESIZE;

                gen1(c2,0x50 + AX);
                gen1(c2,0x50 + BX);
                gen1(c2,0x50 + CX);
                gen1(c2,0x50 + DX);             /* PUSH DOUBLEREGS      */
                retregs = DOUBLEREGS_16;
            }
        }
        else
        {
            stackpush += FLOATSIZE;     /* so we know something is on   */
            if (!I32)
                gen1(c2,0x50 + DX);
            gen1(c2,0x50 + AX);
            retregs = FLOATREGS2;
        }
        c2 = genadjesp(c2,stackpush - stackpushsave);

        cgstate.stackclean++;
        c3 = scodelem(e2,&retregs,idxregs,FALSE);
        cgstate.stackclean--;

        if (tyml == TYdouble || tyml == TYdouble_alias)
        {
            retregs = DOUBLEREGS;
            c4 = callclib(e,(op == OPpostinc) ? CLIBdadd : CLIBdsub,
                    &retregs,idxregs);
        }
        else /* tyml == TYfloat */
        {
            retregs = FLOATREGS;
            c4 = callclib(e,(op == OPpostinc) ? CLIBfadd : CLIBfsub,
                    &retregs,idxregs);
        }
        cs.Iop = 0x89;                  /* MOV EA,DOUBLEREGS            */
        c5 = fltregs(&cs,tyml);
        stackpushsave = stackpush;
        if (tyml == TYdouble || tyml == TYdouble_alias)
        {   if (*pretregs == mSTACK)
                retregs = mSTACK;       /* leave result on stack        */
            else
            {
                if (I32)
                {   gen1(c5,0x58 + AX);
                    gen1(c5,0x58 + DX);
                }
                else
                {   gen1(c5,0x58 + DX);
                    gen1(c5,0x58 + CX);
                    gen1(c5,0x58 + BX);
                    gen1(c5,0x58 + AX);
                }
                stackpush -= DOUBLESIZE;
                retregs = DOUBLEREGS;
            }
        }
        else
        {   gen1(c5,0x58 + AX);
            if (!I32)
                gen1(c5,0x58 + DX);
            stackpush -= FLOATSIZE;
            retregs = FLOATREGS;
        }
        c5 = genadjesp(c5,stackpush - stackpushsave);
        c6 = fixresult(e,retregs,pretregs);
        return cat6(c1,c2,c3,c4,c5,c6);
#endif
  }

  assert(e2->Eoper == OPconst);
  byte = (sz == 1);
  possregs = byte ? BYTEREGS : allregs;
  c1 = getlvalue(&cs,e->E1,0);
  freenode(e->E1);
  idxregs = idxregm(cs.Irm,cs.Isib);    /* mask of index regs used      */
  if (sz <= REGSIZE && *pretregs == mPSW && (cs.Irm & 0xC0) == 0xC0 &&
      (I32 || (idxregs & (mBX | mSI | mDI | mBP))))
  {     // Generate:
        //      TEST    reg,reg
        //      LEA     reg,n[reg]      // don't affect flags
        int rm;

        reg = cs.Irm & 7;
        cs.Iop = 0x85 ^ byte;
        cs.Irm |= modregrm(0,reg,0);
        cs.Iflags |= CFpsw;
        c2 = gen(NULL,&cs);             // TEST reg,reg

        // If lvalue is a register variable, we must mark it as modified
        c3 = modEA(cs.Irm);

        n = e2->EV.Vint;
        if (op == OPpostdec)
            n = -n;
        rm = reg;
        if (!I32)
            rm = regtorm[reg];
        c4 = genc1(NULL,0x8D,modregrm(2,reg,rm),FLconst,n);     // LEA reg,n[reg]
        return cat4(c1,c2,c3,c4);
  }
  else if (sz <= REGSIZE || tyfv(tyml))
  {     code cs2;

        cs.Iop = 0x8B ^ byte;
        retregs = possregs & ~idxregs & *pretregs;
        if (!tyfv(tyml))
        {       if (retregs == 0)
                        retregs = possregs & ~idxregs;
        }
        else /* tyfv(tyml) */
        {       if ((retregs &= mLSW) == 0)
                        retregs = mLSW & ~idxregs;
                /* Can't use LES if the EA uses ES as a seg override    */
                if (*pretregs & mES && (cs.Iflags & CFSEG) != CFes)
                {   cs.Iop = 0xC4;                      /* LES          */
                    c1 = cat(c1,getregs(mES));          /* allocate ES  */
                }
        }
        c2 = allocreg(&retregs,&reg,TYint);
        cs.Irm |= modregrm(0,reg,0);
        c3 = gen(CNIL,&cs);                     /* MOV reg,EA   */
        cs2 = cs;

        /* If lvalue is a register variable, we must mark it as modified */
        c3 = cat(c3,modEA(cs.Irm));

        cs.Iop = 0x81 ^ byte;
        cs.Irm &= ~modregrm(0,7,0);             /* reg field = 0        */
        if (op == OPpostdec)
                cs.Irm |= modregrm(0,5,0);      /* SUB                  */
        cs.IFL2 = FLconst;
        n = e2->EV.Vint;
        cs.IEV2.Vint = n;
        if (n == 1)                     /* can use INC or DEC           */
        {       cs.Iop |= 0xFE;         /* xFE is dec byte, xFF is word */
                if (op == OPpostdec)
                        NEWREG(cs.Irm,1);       // DEC EA
                else
                        NEWREG(cs.Irm,0);       // INC EA
        }
        else if (n == -1)               // can use INC or DEC
        {       cs.Iop |= 0xFE;         // xFE is dec byte, xFF is word
                if (op == OPpostinc)
                        NEWREG(cs.Irm,1);       // DEC EA
                else
                        NEWREG(cs.Irm,0);       // INC EA
        }

        // For scheduling purposes, we wish to replace:
        //      MOV     reg,EA
        //      OP      EA
        // with:
        //      MOV     reg,EA
        //      OP      reg
        //      MOV     EA,reg
        //      ~OP     reg
        if (sz <= REGSIZE && (cs.Irm & 0xC0) != 0xC0 &&
            config.target_cpu >= TARGET_Pentium &&
            config.flags4 & CFG4speed)
        {
            // Replace EA in cs with reg
            cs.Irm = (cs.Irm & ~modregrm(3,0,7)) | modregrm(3,0,reg);
            gen(c3,&cs);                        // ADD/SUB reg,const

            // Reverse MOV direction
            cs2.Iop ^= 2;
            gen(c3,&cs2);                       // MOV EA,reg

            // Toggle INC <-> DEC, ADD <-> SUB
            cs.Irm ^= (n == 1 || n == -1) ? modregrm(0,1,0) : modregrm(0,5,0);
            gen(c3,&cs);

            if (*pretregs & mPSW)
            {   *pretregs &= ~mPSW;             // flags already set
                code_orflag(c3,CFpsw);
            }
        }
        else
            gen(c3,&cs);                        // ADD/SUB EA,const

        freenode(e2);
        if (tyfv(tyml))
        {       unsigned preg;

                getlvalue_msw(&cs);
                if (*pretregs & mES)
                {       preg = ES;
                        /* ES is already loaded if CFes is 0            */
                        cs.Iop = ((cs.Iflags & CFSEG) == CFes) ? 0x8E : NOP;
                        NEWREG(cs.Irm,0);       /* MOV ES,EA+2          */
                }
                else
                {
                        retregs = *pretregs & mMSW;
                        if (!retregs)
                            retregs = mMSW;
                        c3 = cat(c3,allocreg(&retregs,&preg,TYint));
                        cs.Iop = 0x8B;
                        if (I32)
                            cs.Iflags |= CFopsize;
                        NEWREG(cs.Irm,preg);    /* MOV preg,EA+2        */
                }
                c3 = cat(c3,getregs(mask[preg]));
                gen(c3,&cs);
                retregs = mask[reg] | mask[preg];
        }
        return cat4(c1,c2,c3,fixresult(e,retregs,pretregs));
  }
  else if (tyml == TYhptr)
  {
        unsigned long rvalue;
        unsigned lreg;
        unsigned rtmp;
        regm_t mtmp;

        rvalue = e2->EV.Vlong;
        freenode(e2);

        // If h--, convert to h++
        if (e->Eoper == OPpostdec)
            rvalue = -rvalue;

        retregs = mLSW & ~idxregs & *pretregs;
        if (!retregs)
            retregs = mLSW & ~idxregs;
        c1 = cat(c1,allocreg(&retregs,&lreg,TYint));

        // Can't use LES if the EA uses ES as a seg override
        if (*pretregs & mES && (cs.Iflags & CFSEG) != CFes)
        {   cs.Iop = 0xC4;
            retregs |= mES;
            c1 = cat(c1,getregs(mES|mCX));      // allocate ES
            cs.Irm |= modregrm(0,lreg,0);
            c2 = gen(CNIL,&cs);                 // LES lreg,EA
        }
        else
        {   cs.Iop = 0x8B;
            retregs |= mDX;
            c1 = cat(c1,getregs(mDX|mCX));
            cs.Irm |= modregrm(0,lreg,0);
            c2 = gen(CNIL,&cs);                 // MOV lreg,EA
            NEWREG(cs.Irm,DX);
            getlvalue_msw(&cs);
            gen(c2,&cs);                        // MOV DX,EA+2
            getlvalue_lsw(&cs);
        }

        // Allocate temporary register, rtmp
        mtmp = ALLREGS & ~mCX & ~idxregs & ~retregs;
        c2 = cat(c2,allocreg(&mtmp,&rtmp,TYint));

        movregconst(c2,rtmp,rvalue >> 16,0);    // MOV rtmp,e2+2
        c3 = getregs(mtmp);
        cs.Iop = 0x81;
        NEWREG(cs.Irm,0);
        cs.IFL2 = FLconst;
        cs.IEV2.Vint = rvalue;
        c3 = gen(c3,&cs);                       // ADD EA,e2
        code_orflag(c3,CFpsw);
        genc2(c3,0x81,modregrm(3,2,rtmp),0);    // ADC rtmp,0
        genshift(c3);                           // MOV CX,offset __AHSHIFT
        gen2(c3,0xD3,modregrm(3,4,rtmp));       // SHL rtmp,CL
        cs.Iop = 0x01;
        NEWREG(cs.Irm,rtmp);                    // ADD EA+2,rtmp
        getlvalue_msw(&cs);
        gen(c3,&cs);
        return cat4(c1,c2,c3,fixresult(e,retregs,pretregs));
  }
  else if (sz == 2 * REGSIZE)
  {     unsigned sreg;

        retregs = allregs & ~idxregs & *pretregs;
        if ((retregs & mLSW) == 0)
                retregs |= mLSW & ~idxregs;
        if ((retregs & mMSW) == 0)
                retregs |= ALLREGS & mMSW;
        assert(retregs & mMSW && retregs & mLSW);
        c2 = allocreg(&retregs,&reg,tyml);
        sreg = findreglsw(retregs);
        cs.Iop = 0x8B;
        cs.Irm |= modregrm(0,sreg,0);
        c3 = gen(CNIL,&cs);             /* MOV sreg,EA                  */
        NEWREG(cs.Irm,reg);
        getlvalue_msw(&cs);
        gen(c3,&cs);                    /* MOV reg,EA+2                 */
        cs.Iop = 0x81;
        cs.Irm &= ~modregrm(0,7,0);     /* reg field = 0 for ADD        */
        if (op == OPpostdec)
            cs.Irm |= modregrm(0,5,0);  /* SUB                          */
        getlvalue_lsw(&cs);
        cs.IFL2 = FLconst;
        cs.IEV2.Vlong = e2->EV.Vlong;
        gen(c3,&cs);                    /* ADD/SUB EA,const             */
        code_orflag(c3,CFpsw);
        getlvalue_msw(&cs);
        cs.IEV2.Vlong = 0;
        if (op == OPpostinc)
            cs.Irm ^= modregrm(0,2,0);  /* ADC                          */
        else
            cs.Irm ^= modregrm(0,6,0);  /* SBB                          */
        cs.IEV2.Vlong = e2->EV.Vullong >> (REGSIZE * 8);
        gen(c3,&cs);                    /* ADC/SBB EA,0                 */
        freenode(e2);
        return cat4(c1,c2,c3,fixresult(e,retregs,pretregs));
  }
  else
  {     assert(0);
        /* NOTREACHED */
        return 0;
  }
}


code *cderr(elem *e,regm_t *pretregs)
{
#if DEBUG
        elem_print(e);
#endif
//printf("op = %d, %d\n", e->Eoper, OPstring);
//printf("string = %p, len = %d\n", e->EV.ss.Vstring, e->EV.ss.Vstrlen);
//printf("string = '%.*s'\n", e->EV.ss.Vstrlen, e->EV.ss.Vstring);
        assert(0);
        return 0;
}

code *cdinfo(elem *e,regm_t *pretregs)
{
#if SCPP
    code cs;
    code *c;
    regm_t retregs;

    switch (e->E1->Eoper)
    {
        case OPdtor:
            c = cdcomma(e,pretregs);
            break;
        case OPctor:
            c = codelem(e->E2,pretregs,FALSE);
            retregs = 0;
            c = cat(c,codelem(e->E1,&retregs,FALSE));
            break;
        case OPmark:
            if (0 &&  config.exe == EX_NT)
            {   unsigned idx;

                idx = except_index_get();
                except_mark();
                c = codelem(e->E2,pretregs,FALSE);
                if (config.exe == EX_NT && idx != except_index_get())
                {   usednteh |= NTEHcleanup;
                    c = cat(c,nteh_gensindex(idx - 1));
                }
                except_release();
                assert(idx == except_index_get());
            }
            else
            {
#if 0
                usednteh |= EHcleanup;
                if (config.exe == EX_NT)
                    usednteh |= NTEHcleanup;
#endif
                cs.Iop = ESCAPE;
                cs.Iop2 = ESCmark;
                cs.Iflags = 0;
                cs.Ijty = 0;
                c = gen(CNIL,&cs);
                c = cat(c,codelem(e->E2,pretregs,FALSE));
                cs.Iop2 = ESCrelease;
                gen(c,&cs);
            }
            freenode(e->E1);
            break;
        default:
            assert(0);
    }
    return c;
#else
    return NULL;
#endif
}

code *cdctor(elem *e,regm_t *pretregs)
{
#if SCPP
    code cs;
    code *c;

#if 0
    if (config.exe == EX_NT)
    {   usednteh |= NTEHcleanup;
        except_push(NULL,e,NULL);
        return nteh_gensindex(except_index_get() - 1);
    }
#else
    usednteh |= EHcleanup;
    if (config.exe == EX_NT)
        usednteh |= NTEHcleanup;
#endif
    assert(*pretregs == 0);
    cs.Iop = ESCAPE;
    cs.Iop2 = ESCctor;
    cs.Iflags = 0;
    cs.Ijty = 0;
    cs.IFL1 = FLctor;
    cs.IEV1.Vtor = e;
    c = gen(CNIL,&cs);
    //except_push(c,e,NULL);
    return c;
#else
    return NULL;
#endif
}

code *cddtor(elem *e,regm_t *pretregs)
{
#if SCPP
    code cs;
    code *c;

#if 0
    if (config.exe == EX_NT)
    {   usednteh |= NTEHcleanup;
        except_pop(NULL,e,NULL);
        return nteh_gensindex(except_index_get() - 1);
    }
#else
    usednteh |= EHcleanup;
    if (config.exe == EX_NT)
        usednteh |= NTEHcleanup;
#endif
    assert(*pretregs == 0);
    cs.Iop = ESCAPE;
    cs.Iop2 = ESCdtor;
    cs.Iflags = 0;
    cs.Ijty = 0;
    cs.IFL1 = FLdtor;
    cs.IEV1.Vtor = e;
    c = gen(CNIL,&cs);
    //except_pop(c,e,NULL);
    return c;
#else
    return NULL;
#endif
}

code *cdmark(elem *e,regm_t *pretregs)
{
    return NULL;
}

#if !NTEXCEPTIONS
code *cdsetjmp(elem *e,regm_t *pretregs)
{
    assert(0);
    return NULL;
}
#endif

/*****************************************
 */

code *cdvoid(elem *e,regm_t *pretregs)
{
    assert(*pretregs == 0);
    return codelem(e->E1,pretregs,FALSE);
}

/*****************************************
 */

code *cdhalt(elem *e,regm_t *pretregs)
{
    assert(*pretregs == 0);
    return gen1(NULL, 0xF4);            // HLT
}

/****************************************
 * Check to see if pointer is NULL.
 */

code *cdnullcheck(elem *e,regm_t *pretregs)
{   regm_t retregs;
    regm_t scratch;
    unsigned reg;
    code *c;
    code *cs;

    assert(I32);
    retregs = *pretregs;
    if ((retregs & allregs) == 0)
        retregs |= allregs;
    c = codelem(e->E1,&retregs,FALSE);
    scratch = allregs & ~retregs;
    cs = allocreg(&scratch,&reg,TYint);
    cs = genc1(cs,0x8B,modregrm(2,reg,findreg(retregs)),FLconst,0);     // MOV reg,0[e]
    return cat3(c,cs,fixresult(e,retregs,pretregs));
}

#endif // !SPP