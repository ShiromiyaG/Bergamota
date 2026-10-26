# runtime — instruções para o Claude

Sistema 3DS enxuto para os ports (Solstice, Duality, Tidefall): só as syscalls e os serviços que
esses jogos usam. Repositório privado `ShiromiyaG/recomp-runtime`; commits locais de rotina permitidos, push só
quando o dono pedir. Docs em inglês.

- C++20, interface em C puro (`include/runtime/runtime.h`); CMake + Ninja + clang (decidido em
  2026-10-26: os ports são C/C++, com SDL3, Vulkan e Dear ImGui).
- Listas e motivos: `ports/Solstice/docs/lean-runtime.md`.
- Threads cooperativas numa thread do host por enquanto; paralelizar depois.
- Código traduzido do Zakuro (Rust, MIT): aviso de copyright no topo de cada arquivo traduzido;
  conferir contra o Zakuro na mesma sessão.
- Nada derivado dos jogos entra aqui.
