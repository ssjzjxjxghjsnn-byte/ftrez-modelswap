.section .rodata
.balign 16
.global ft_lobby_code_start
.hidden ft_lobby_code_start
ft_lobby_code_start:
.incbin FT_LOBBY_CODE_PATH
.global ft_lobby_code_end
.hidden ft_lobby_code_end
ft_lobby_code_end:
.balign 16
.global ft_lobby_shader_start
.hidden ft_lobby_shader_start
ft_lobby_shader_start:
.incbin FT_LOBBY_SHADER_PATH
.global ft_lobby_shader_end
.hidden ft_lobby_shader_end
ft_lobby_shader_end:
.section .note.GNU-stack,"",%progbits
