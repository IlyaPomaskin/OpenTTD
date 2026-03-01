# Autodetect if ARM NEON is available.
# NEON is mandatory on AArch64, so this mainly checks we can include the header.

include(CheckCXXSourceCompiles)

check_cxx_source_compiles("
    #include <arm_neon.h>
    int main() { uint16x8_t v = vdupq_n_u16(0); return 0; }"
    NEON_FOUND
)
