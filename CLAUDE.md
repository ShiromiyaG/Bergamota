# runtime — instruções para o Claude

Sistema 3DS enxuto para os ports (Solstice, Duality, Tidefall): só as syscalls e os serviços que
esses jogos usam. Repositório **local** (sem remote); commit só quando o dono pedir (commits locais de
rotina permitidos, como no `decomp-tools`). Docs em inglês.

- Listas e motivos: `Solstice/docs/lean-runtime.md`.
- Threads cooperativas numa thread do host por enquanto (decidido em 2026-10-26); paralelizar depois.
- Código adaptado do Zakuro (MIT): manter o aviso de copyright no topo de cada arquivo copiado.
- Nada derivado dos jogos entra aqui.
