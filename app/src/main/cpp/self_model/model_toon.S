.section .rodata
.balign 16
.global ft_model_toon_start
.hidden ft_model_toon_start
ft_model_toon_start:
.incbin FT_MODEL_TOON_PATH
.global ft_model_toon_end
.hidden ft_model_toon_end
ft_model_toon_end:
.section .note.GNU-stack,"",%progbits
