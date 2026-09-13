### Weak Clock Fallback

If a bare-metal target build does not provide a platform clock
implementation, the weak fallback clock is linked. This produces
incorrect timer and timeout behavior. The CMake build emits a
warning when this occurs, and the macro `CANCESTRY_WEAK_CLOCK_ACTIVE`
is defined for conditional compilation.

Mitigation: All target builds must provide a platform clock
implementation. The weak fallback is for compilation only, not
for production use.
