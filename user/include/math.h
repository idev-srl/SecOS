/* SecOS libc - minimal math.h (hardware double via SSE; series for
 * transcendentals). Good enough to port lua/sqlite from source; not IEEE-exact
 * for the transcendental functions. */
#ifndef _MATH_H
#define _MATH_H

#define M_PI   3.14159265358979323846
#define M_E    2.71828182845904523536
#define M_LN2  0.69314718055994530942
#define M_LN10 2.30258509299404568402

#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN      (__builtin_nanf(""))

int    isnan(double x);
int    isinf(double x);
int    isfinite(double x);

double fabs(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double sqrt(double x);
double fmod(double x, double y);
double pow(double x, double y);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double ldexp(double x, int e);
double frexp(double x, int* e);
double modf(double x, double* ip);
double fmin(double a, double b);
double fmax(double a, double b);
double copysign(double x, double y);
double hypot(double x, double y);

#endif
