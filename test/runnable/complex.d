// PERMUTE_ARGS:

import std.stdio;
import std.math;

/***************************************/

void test1()
{
    creal c = 3.0 + 4.0i;
    c = sqrt(c);
    printf("re = %Lg, im = %Lg\n", c.re, c.im);
    assert(c.re == 2.0);
    assert(c.im == 1.0);

    float f = sqrt(25.0f);
    assert(f == 5.0);
    double d = sqrt(4.0);
    assert(d == 2.0);
    real r = sqrt(9.0L);
    assert(r == 3.0);
}

/***************************************/

ireal f2() { return 1i; }

void test2()
{
    creal v = 0+0i;

    v += f2();
    assert(v == 0 + 1i);

    v = v + f2();
    assert(v == 0 + 2i);
}

/***************************************/

cdouble[1] a3;
cdouble[1] b3;

cdouble[] concat3() {
	return a3~b3;
}

void test3()
{
	a3[]=0.5+1.0i;
	b3[]=0.5+3.0i;

	cdouble[] arr=concat3();

	assert(arr.length==2);
	assert(arr[0]==0.5+1.0i);
	assert(arr[1]==0.5+3.0i);
}

/***************************************/

creal[1] a4;
creal[1] b4;

creal[] concat4() {
	return a4~b4;
}

void test4()
{
	a4[]=0.5+1.0i;
	b4[]=0.5+3.0i;

	creal[] arr=concat4();

	assert(arr.length==2);
	assert(arr[0]==0.5+1.0i);
	assert(arr[1]==0.5+3.0i);
}

/***************************************/

void test5()
{
	ifloat i=1.0fi;
//	i += 2.2;
//	assert(i == 1i);
}

/***************************************/

void test6()
{
	float i=1.0f;
//	i /= 2.2fi;
//	assert(i == 0);
}

/***************************************/

void test7()
{
	creal x=1.0i+2.0;
	creal[] arr;

	arr = arr ~ x;
	assert(arr.length==1);
	assert(arr[0]==1.0i+2.0);

	x=0.0i+5.0;
	assert(arr[0]==1.0i+2.0);
}

/****************************************/

creal[1] a8;
creal[1] b8;

creal[] concat8() {
    return a8 ~ b8;
}

void test8()
{
    a8[]=0.5L+1.0Li;
    b8[]=0.5L+3.0Li;

    creal[] arr=concat8();

    assert(arr.length==2);
    assert(arr[0]==0.5L+1.0Li);
    assert(arr[1]==0.5L+3.0Li);
}

/***************************************/

creal[1] a9;
creal[1] b9;

creal[] concat9() {
    return a9~b9;
}

void test9()
{
    a9[]=0.5L+1.0Li;
    b9[]=0.5L+3.0Li;

    creal[] arr=concat9();

    assert(arr.length==2);
    assert(arr[0]==0.5L+1.0Li);
    assert(arr[1]==0.5L+3.0Li);
}


/***************************************/

void test10()
{
    ifloat a = 1.0i;
    assert(a.im == 1.0);

    const ifloat b = 2.0i;
    static assert(b.im == 2.0); // FAIL

}

/***************************************/

void test11()
{
    real r = real.nan;
    assert( r!=0 );
    if (r==0) assert(0);

    ireal ir = ireal.nan;
    assert( ir!=0 );
    assert( ir!=0i );
    if (ir==0) assert(0);
    if (ir==0i) assert(0);

    creal cr = creal.nan;
    assert( cr!=0 );
    assert( cr!=0i );
    if (cr==0) assert(0);
    if (cr==0i) assert(0);

    double d = double.nan;
    assert( d!=0 );
    if (d==0) assert(0);

    idouble id = idouble.nan;
    assert( id!=0 );
    assert( id!=0i );
    if (id==0) assert(0);
    if (id==0i) assert(0);

    cdouble cd = cdouble.nan;
    assert( cd!=0 );
    assert( cd!=0i );
    if (cd==0) assert(0);
    if (cd==0i) assert(0);

    float f = float.nan;
    assert( f!=0 );
    if (f==0) assert(0);

    ifloat ifx = ifloat.nan;
    assert( ifx!=0 );
    assert( ifx!=0i );
    if (ifx==0) assert(0);
    if (ifx==0i) assert(0);

    cfloat cf = cfloat.nan;
    assert( cf!=0 );
    assert( cf!=0i );
    if (cf==0) assert(0);
    if (cf==0i) assert(0);
}

/***************************************/

void test12()
{
    real x = 3;
    creal a = (2 + 4i) % 3;
    writeln(a);
    assert(a == 2 + 1i);

    creal b = (2 + 4i) % x;
    writeln(b);
    assert(b == a);
}

/***************************************/

void test13()
{
        ireal a = 5i;
        ireal b = a % 2;
	writeln(b);
	assert(b == 1i);
}

/***************************************/

int main(char[][] args)
{

    test1();
    test2();
    test3();
    test4();
    test5();
    test6();
    test7();
    test8();
    test9();
    test10();
    test11();
    test12();
    test13();

    printf("Success!\n");
    return 0;
}

