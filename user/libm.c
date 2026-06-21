/* SecOS libc - minimal libm. Hardware double (SSE) for the basics; range-reduced
 * Taylor/Newton series for transcendentals. Targets "port lua/sqlite from source
 * and get correct-looking results", not bit-exact IEEE. Compiled with SSE (the
 * default for x86-64 user code); the kernel enables SSE for ring 3 at boot. */
#include <math.h>

union du { double d; unsigned long u; };

int isnan(double x){ union du v; v.d=x; unsigned long e=(v.u>>52)&0x7FF, m=v.u&0xFFFFFFFFFFFFFUL; return e==0x7FF && m!=0; }
int isinf(double x){ union du v; v.d=x; unsigned long e=(v.u>>52)&0x7FF, m=v.u&0xFFFFFFFFFFFFFUL; return e==0x7FF && m==0; }
int isfinite(double x){ union du v; v.d=x; return ((v.u>>52)&0x7FF)!=0x7FF; }

double fabs(double x){ union du v; v.d=x; v.u &= 0x7FFFFFFFFFFFFFFFUL; return v.d; }
double copysign(double x, double y){ union du a,b; a.d=x; b.d=y; a.u=(a.u&0x7FFFFFFFFFFFFFFFUL)|(b.u&0x8000000000000000UL); return a.d; }

double sqrt(double x){ double r; __asm__("sqrtsd %1,%0":"=x"(r):"x"(x)); return r; }

double trunc(double x){
    if(!isfinite(x)) return x;
    if(fabs(x) >= 4503599627370496.0) return x; /* >= 2^52: already integral */
    long i=(long)x; return (double)i;
}
double floor(double x){ double t=trunc(x); return (t>x)? t-1.0 : t; }
double ceil(double x){ double t=trunc(x); return (t<x)? t+1.0 : t; }
double round(double x){ return (x>=0.0)? floor(x+0.5) : ceil(x-0.5); }

double fmod(double x, double y){
    if(y==0.0 || !isfinite(x) || isnan(y)) return NAN;
    double r = x - trunc(x/y)*y;
    return r;
}
double modf(double x, double* ip){ double t=trunc(x); *ip=t; return x-t; }

double ldexp(double x, int e){
    /* multiply by 2^e via the exponent field, in chunks to avoid overflow */
    while(e>1000){ x*=8.98846567431158e307; e-=1000; }      /* 2^1023-ish */
    while(e<-1000){ x*=1.0/8.98846567431158e307; e+=1000; }
    union du v; if(e>=0){ v.u=(unsigned long)(1023+e)<<52; } else { v.u=(unsigned long)(1023+e)<<52; }
    return x*v.d;
}
double frexp(double x, int* e){
    if(x==0.0 || !isfinite(x)){ *e=0; return x; }
    union du v; v.d=x; int ex=(int)((v.u>>52)&0x7FF)-1022;
    v.u = (v.u & ~(0x7FFUL<<52)) | (1022UL<<52);
    *e=ex; return v.d;
}

/* exp via 2^k * series, k = round(x/ln2). */
double exp(double x){
    if(isnan(x)) return x;
    if(x>709.0) return HUGE_VAL;
    if(x<-745.0) return 0.0;
    double k = floor(x/M_LN2 + 0.5);
    double r = x - k*M_LN2;       /* |r| <= ln2/2 */
    double term=1.0, sum=1.0;
    for(int n=1;n<18;n++){ term*=r/n; sum+=term; }
    return ldexp(sum, (int)k);
}
/* log via frexp + atanh series: log(m) where m in [sqrt(.5),sqrt(2)). */
double log(double x){
    if(x<0.0) return NAN;
    if(x==0.0) return -HUGE_VAL;
    if(!isfinite(x)) return x;
    int e; double m=frexp(x,&e);
    if(m < 0.70710678118654752440){ m*=2.0; e-=1; }
    double t=(m-1.0)/(m+1.0), t2=t*t, sum=0.0, p=t;
    for(int n=1;n<32;n+=2){ sum+=p/n; p*=t2; }
    return 2.0*sum + e*M_LN2;
}
double log2(double x){ return log(x)/M_LN2; }
double log10(double x){ return log(x)/M_LN10; }

double pow(double x, double y){
    if(y==0.0) return 1.0;
    if(x==1.0) return 1.0;
    if(y==(double)(long)y && fabs(y)<1024.0){
        /* exact-ish integer power by squaring */
        long n=(long)y; int neg=n<0; if(neg) n=-n;
        double r=1.0, b=x; while(n){ if(n&1) r*=b; b*=b; n>>=1; }
        return neg? 1.0/r : r;
    }
    if(x<0.0) return NAN;
    if(x==0.0) return (y>0.0)?0.0:HUGE_VAL;
    return exp(y*log(x));
}

/* sin/cos via range reduction mod 2pi + Taylor. */
static double sin_core(double r){ /* |r| <= pi */
    double r2=r*r, term=r, sum=r;
    for(int n=1;n<12;n++){ term*=-r2/((2*n)*(2*n+1)); sum+=term; }
    return sum;
}
double sin(double x){
    if(!isfinite(x)) return NAN;
    double q = x/(2.0*M_PI); x -= floor(q+0.5)*(2.0*M_PI);   /* x in [-pi,pi] */
    return sin_core(x);
}
double cos(double x){ return sin(x + M_PI/2.0); }
double tan(double x){ return sin(x)/cos(x); }

double atan(double x){
    int inv=0, neg=0; if(x<0){ neg=1; x=-x; }
    if(x>1.0){ inv=1; x=1.0/x; }
    double x2=x*x, term=x, sum=x;
    for(int n=1;n<60;n++){ term*=-x2; sum+=term/(2*n+1); }
    if(inv) sum=M_PI/2.0 - sum;
    return neg? -sum : sum;
}
double atan2(double y, double x){
    if(x>0.0) return atan(y/x);
    if(x<0.0) return atan(y/x) + (y>=0.0? M_PI : -M_PI);
    if(y>0.0) return M_PI/2.0;
    if(y<0.0) return -M_PI/2.0;
    return 0.0;
}
double asin(double x){ if(x<=-1.0) return -M_PI/2.0; if(x>=1.0) return M_PI/2.0; return atan(x/sqrt(1.0-x*x)); }
double acos(double x){ return M_PI/2.0 - asin(x); }

double sinh(double x){ double e=exp(x); return (e-1.0/e)/2.0; }
double cosh(double x){ double e=exp(x); return (e+1.0/e)/2.0; }
double tanh(double x){ double e=exp(2.0*x); return (e-1.0)/(e+1.0); }

double fmin(double a, double b){ return (a<b)?a:b; }
double fmax(double a, double b){ return (a>b)?a:b; }
double hypot(double a, double b){ return sqrt(a*a+b*b); }
