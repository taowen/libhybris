.text
.p2align 2
.global BORINGSSL_bcm_text_start
.global BORINGSSL_bcm_text_end
#ifdef MISSING_BOUNDS
.hidden BORINGSSL_bcm_text_end
#endif
.global fixture_tls_read
.type fixture_tls_read, %function
BORINGSSL_bcm_text_start:
fixture_tls_read:
  mrs x0, tpidr_el0
  ret
BORINGSSL_bcm_text_end:
.size fixture_tls_read, .-fixture_tls_read
#ifndef STATIC_FORMAT
.section .rodata,"a",%progbits
.global BORINGSSL_bcm_rodata_start
.global BORINGSSL_bcm_rodata_end
BORINGSSL_bcm_rodata_start:
  .asciz "integrity-fixture-protected-rodata"
BORINGSSL_bcm_rodata_end:
#endif
.section .rodata,"a",%progbits
.global fixture_expected_hash
.hidden fixture_expected_hash
.type fixture_expected_hash, %object
fixture_expected_hash:
  .byte 0x93, 0x77, 0x51, 0x68
  .zero 28
.size fixture_expected_hash, .-fixture_expected_hash
#ifdef DUPLICATE_HASH
.global fixture_duplicate_hash
.hidden fixture_duplicate_hash
fixture_duplicate_hash:
  .zero 32
#endif
.section .note.GNU-stack,"",%progbits
