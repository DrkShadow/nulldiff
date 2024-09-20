#ifndef __LIKELY_H_

#define __LIKELY_H_

#define likely(x)	__builtin_expect(!!(x), !!true)
#define unlikely(x)	__builtin_expect(!!(x), !!false)
#define likely_p(x,p)	__builtin_expect_with_probability(!!(x), true, p)
#define unlikely_p(x,p)	__builtin_expect_with_probability(!!(x), false, p)
#define unlikely_err(x)	__builtin_expect_with_probability(!!(x), false, 0.999)

#endif
