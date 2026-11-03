// Floating point and libm.
#include <cmath>
#include <cstdio>

int main() {
    std::printf("sqrt=%.17g\n", std::sqrt(2.0));
    std::printf("sin=%.17g cos=%.17g\n", std::sin(1.0), std::cos(1.0));
    std::printf("exp=%.17g log=%.17g\n", std::exp(1.0), std::log(10.0));
    std::printf("pow=%.17g fmod=%.17g\n", std::pow(2.0, 0.5), std::fmod(10.0, 3.0));
    std::printf("atan2=%.17g hypot=%.17g\n", std::atan2(1.0, 2.0),
                std::hypot(3.0, 4.0));
    std::printf("floor=%.1f ceil=%.1f round=%.1f trunc=%.1f\n",
                std::floor(-1.5), std::ceil(1.2), std::round(2.5),
                std::trunc(-2.7));
    std::printf("ldexp=%.17g frexp_m=", std::ldexp(1.0, 10));
    int e = 0;
    double m = std::frexp(1024.0, &e);
    std::printf("%.17g e=%d\n", m, e);
    std::printf("isnan=%d isinf=%d signbit=%d\n", std::isnan(0.0 / 0.0),
                std::isinf(1.0 / 0.0), std::signbit(-0.0));

    long double ld = 1.0L / 3.0L;
    std::printf("longdouble=%.20Lg\n", ld);

    float f = 1.0f / 3.0f;
    std::printf("float=%.9g\n", static_cast<double>(f));

    double acc = 0;
    for (int i = 0; i < 1000; i++)
        acc += std::sin(i * 0.001) * std::cos(i * 0.002);
    std::printf("acc=%.15g\n", acc);
    return 0;
}
