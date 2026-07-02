org 100h

start:
    mov dx, msg_start
    call puts
    call print_tail
    call crlf

    mov dx, msg_alu
    mov si, bench_alu
    call run_case

    mov dx, msg_branch
    mov si, bench_branch
    call run_case

    mov dx, msg_stack
    mov si, bench_stack
    call run_case

    mov dx, msg_mem
    mov si, bench_mem
    call run_case

    mov dx, msg_smc
    mov si, bench_smc
    call run_case

    mov dx, msg_end
    call puts
    call print_tail
    call crlf

    mov ax, 4C00h
    int 21h

run_case:
    push dx
    push si
    mov ah, 00h
    int 1Ah
    mov [start_tick], dx
    pop si
    call si
    mov ah, 00h
    int 1Ah
    sub dx, [start_tick]
    mov [elapsed], dx
    pop dx
    call puts
    mov ax, [elapsed]
    call puthex4
    call crlf
    ret

bench_alu:
    mov cx, 64
.outer:
    mov bx, 0FFFFh
.inner:
    add ax, bx
    xor dx, ax
    rol ax, 1
    not dx
    dec bx
    jnz .inner
    loop .outer
    ret

bench_branch:
    mov cx, 64
.outer:
    mov bx, 0FFFFh
.inner:
    test bx, 1
    jz .even
    inc ax
    jmp .join
.even:
    dec ax
.join:
    dec bx
    jnz .inner
    loop .outer
    ret

bench_stack:
    mov cx, 64
.outer:
    mov bx, 4096
.inner:
    push ax
    push bx
    call stack_leaf
    pop bx
    pop ax
    dec bx
    jnz .inner
    loop .outer
    ret

stack_leaf:
    xor ax, bx
    ret

bench_mem:
    push ds
    pop es
    mov cx, 128
.outer:
    mov di, mem_buf
    mov bx, 128
.inner:
    mov ax, [di]
    xor ax, bx
    mov [di], ax
    cmp ax, [di]
    jne .bad
    add di, 2
    dec bx
    jnz .inner
    loop .outer
.bad:
    ret

bench_smc:
    mov cx, 512
.loop:
    mov byte [smc_target], 40h
    call smc_target
    mov byte [smc_target], 48h
    call smc_target
    loop .loop
    ret

smc_target:
    inc ax
    ret

puts:
    push ax
    push bx
    push dx
    mov bx, dx
.next:
    mov al, [bx]
    or al, al
    jz .done
    call putc
    inc bx
    jmp .next
.done:
    pop dx
    pop bx
    pop ax
    ret

print_tail:
    push ax
    push cx
    push si
    xor cx, cx
    mov cl, [80h]
    jcxz .done
    mov si, 81h
.trim:
    cmp cx, 0
    je .done
    mov al, [si]
    cmp al, ' '
    jne .emit
    inc si
    dec cx
    jmp .trim
.emit:
    mov al, ' '
    call putc
.loop:
    cmp cx, 0
    je .done
    lodsb
    cmp al, 0Dh
    je .done
    call putc
    dec cx
    jmp .loop
.done:
    pop si
    pop cx
    pop ax
    ret

puthex4:
    push ax
    push cx
    mov cx, 4
.digit:
    rol ax, 4
    push ax
    and al, 0Fh
    cmp al, 10
    jb .num
    add al, 'A' - 10
    jmp .out
.num:
    add al, '0'
.out:
    call putc
    pop ax
    loop .digit
    pop cx
    pop ax
    ret

crlf:
    mov al, 0Dh
    call putc
    mov al, 0Ah
    call putc
    ret

putc:
    push ax
    push dx
    mov dl, al
    mov ah, 02h
    int 21h
    pop dx
    pop ax
    push dx
    mov dx, 0402h
    out dx, al
    pop dx
    ret

msg_start  db 'BENCH_START', 0
msg_alu    db 'BENCH_CASE ALU ', 0
msg_branch db 'BENCH_CASE BRANCH ', 0
msg_stack  db 'BENCH_CASE STACK ', 0
msg_mem    db 'BENCH_CASE MEM ', 0
msg_smc    db 'BENCH_CASE SMC ', 0
msg_end    db 'BENCH_END', 0
start_tick dw 0
elapsed    dw 0
mem_buf    times 256 dw 0A55Ah
