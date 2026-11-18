	.section .text.start, "ax", @progbits
	.globl	_start
	.type	_start, @function
_start:
	addi	sp, sp, -16
	sw	ra, 12(sp)
	sw	s0, 8(sp)
	addi	s0, sp, 16

	/* argc en a0, argv en a1 (convención C desde elf_run) */
	call	main

	/* exit(main_ret) */
	call	exit

	lw	ra, 12(sp)
	lw	s0, 8(sp)
	addi	sp, sp, 16
	ret
	.size	_start, . - _start
