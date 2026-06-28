org 100h

start:
    mov dx, msg_start
    call puts
    call print_tail
    call crlf

    mov ah, 00h
    int 1Ah
    mov [start_tick], dx

    call workload

    mov ah, 00h
    int 1Ah
    sub dx, [start_tick]
    mov [elapsed], dx

    mov dx, msg_ticks
    call puts
    mov ax, [elapsed]
    call puthex4
    call crlf

    mov dx, msg_end
    call puts
    call print_tail
    call crlf

    mov ax, 4C00h
    int 21h

workload:
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

msg_start db 'BENCH_START', 0
msg_ticks db 'BENCH_TICKS ', 0
msg_end   db 'BENCH_END', 0
start_tick dw 0
elapsed dw 0
