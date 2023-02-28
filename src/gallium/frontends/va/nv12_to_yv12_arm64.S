.section .rodata
.align 4
.text

#if defined(__aarch64__)

.globl nv12_to_yv12_arm64
nv12_to_yv12_arm64:
  nv12toyv12Loop:
    mov         x8, x0
    mov         x9, x6
    mov         x10, x2
    mov         x11, x4
      1:
      ld2         {v0.16b,v1.16b}, [x8], #32
      prfm        pldl1keep, [x8, 448]
      subs        x9, x9, #16
      st1         {v0.16b}, [x10], #16
      st1         {v1.16b}, [x11], #16
      b.gt        1b
    add         x0, x0, x1
    add         x2, x2, x3
    add         x4, x4, x5
  subs        x7, x7, #1
  cmp         x7, #0
  b.gt        nv12toyv12Loop
  ret
#endif