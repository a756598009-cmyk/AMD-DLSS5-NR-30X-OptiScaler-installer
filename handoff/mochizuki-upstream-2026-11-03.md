# Handoff: mochizuki v0.0.7 -> v0.0.8.2 (2026-11-03)

Branch `mochizuki-0.0.8.2`, worktree `wt-mochi0082`, a partir de `pendencias-2026-11-03` com o revert e0c75ad30 (o rescale de histórico de 3eff79ec5 continua fora). Upstream agora no GitLab
(gitlab.com/mochizuki0323/DLSSNR-AMD), tag v0.0.8.2 = `c53878b` (0.0.8.1 = `339938c`, com o PR #26 do TheAutomatic).
Detalhes técnicos e números em `third_party/mochizuki/UPSTREAM.md` (seção "Sync to v0.0.8.2", patches 25-27, RDNA3).

## Como foi feito

Merge de três vias só nos caminhos vendorizados (base `cc27501` = v0.0.7, ours = nosso vendor, theirs = `c53878b`),
num repo de rascunho. Conflitos em `nr_graph.cpp`, `nrvk.hpp`, `nr_pal_binary.hpp`, `nr_shader_manifest.hpp` e nos
dois `build_network.py`; `nr_runtime.cpp/.hpp` ficaram com a nossa versão e recebem só o lado de rede do upstream
(abaixo). Os arquivos que o upstream acrescentou e que não usamos (wilsjo2, NR Control, RDNA3 do upstream, scripts de
pacote) ficaram de fora.

## O que mudou no upstream que importa no Windows

- **Memória de vídeo (0.0.8.2):** slots que nenhum kernel escolhido toca não ganham bytes, saída de camada dentro de
  run persistente reaproveita a de duas camadas antes, arena "maior primeiro", segunda saída do post block 1x1, tabela
  de ruído seed 0 num buffer próprio (binding 5 do pre block). No harness: arena de ativação 1080p small 410 -> 168 MB,
  310.8 392 -> 211 MB, 720p small 194 -> 89 MB; o processo cresce 784 -> 475 MB ao montar a rede 1080p small.
- **History Precision (0.0.8.2):** histórico temporal em RGBA16F como a DLL da NVIDIA (padrão do upstream).
- **Shaders RDNA4:** leitura do ruído do buffer, `post_no_out1`, e os defines do CC_SMALL_FP8_EXPERIMENTAL (desligados).
  Records ACO novos: 62/62 (padrão) e 75/75 (int4) batem com o SPIR-V que buildamos, byte a byte.
- **Estágio RenoDX:** Passes (fast) (Extrapolation, Feedback, Hybrid, Edit strength), Preprocess refeito com Inverse,
  `legacy_decode_reduce`, `renodx_debug`, blits em compute para listas só-compute, menos tempo de GPU.
- **ACO/PAL:** o adaptador PAL aceita código gfx11/gfx11.5 e record v2; falha do próprio código ACO desliga o ACO e
  refaz com o driver; cache de import com chave da build.
- **Caminhos UTF-8** em tudo (pasta de jogo com qualquer letra).
- Upstream tirou a composição do runtime (transfer, Model resolution, multi-pass, cascade, Preprocess próprio) e foi
  para o host deles (wilsjo2). Mantivemos a nossa (patch 25).

## Patches locais

- Mantidos: 1-12, 14-16, 18-24 (build cancel, `NR_EDGE_BODIES=0`, intensidade >1, cascade edge-aware, anti-halo,
  pass3, 30 passes, first pass, first do estágio, patch do `v6_common`, e todo o RDNA3 nosso).
- 13: em parte agora é do upstream (o TheAutomatic portou as mesmas mudanças do fork do mauri870: config FP16, twins,
  aliases, `plain_tiles`, `kswap`). Ficou a estrutura do upstream onde era a mesma coisa e a nossa onde difere
  (posse dos twins do patch 15, `g_rdna3`, `NR_RDNA3_PERSIST`, `aco_push_gap`).
- 17: `takes_machine` agora aceita gfx11 (o adaptador do upstream já aceita); a trava continua: rede RDNA3 só usa ACO
  vinda de um bundle `aco-gfx11`.
- Novos: 25 (runtime mantém a composição + porta ruído no binding 5, History Precision como `RuntimeConfig::history_f16`,
  depth 1x1 com Motion Without Depth), 26 (`Preprocess::inverse`), 27 (`build_shared` preenche a tabela de ruído).
- Nenhum patch caiu de vez.

## RDNA3: decisão

Ficou a nossa rede RDNA3 como padrão. A do upstream (TheAutomatic, #26) roda só como código ACO gfx11 (sem ACO não há
NR no RX 7000 lá), mantém `attn` com `NR_QPAD 4` e o `vitattn` coopmat com V^T ColumnMajor — exatamente o par que
estava errado e variando no RX 7900 XTX aqui (kits 5/6). Sob ACO o bug do LLPC pode não existir, mas não temos como
rodar gfx11 aqui para conferir. A nossa compila pelo driver e foi validada no 7900 XTX, 7800 XT e 8060S. Os records
dele são nomeados pelo hash do SPIR-V dele, não servem para o nosso `shaders-rdna3`. Como o PAL já aceita gfx11, um
opt-in futuro é: enviar a pasta dele como `aco-gfx11` (SPIR-V + shells + records) e checar o layout de push num RX 7000.
Nada disso foi vendorizado agora.

RDNA2 (outras branches): o rebase vai encontrar `nr_graph.cpp`/`nrvk.hpp`/`nr_runtime.cpp` mexidos (slots mortos,
layout da arena, binding 5 = buffer de ruído; um pre block RDNA2 que lê ruído do blob de pesos continua funcionando,
o binding 5 recebe um stand-in de 16 bytes), `Kernel` com campos novos, PAL aceitando gfx11 (não gfx10.3) e
`runtime_blit` na lista `RUNTIME`.

## Resolução dinâmica: decisão

Mantive 5421603fd como está (auto agrupa as realocações do Cyberpunk num bucket, sem rebuild; histórico reinicia quando
o subrect muda). O upstream 0.0.8.2 monta um runtime por tamanho e também **reinicia** o histórico a cada tamanho novo,
como a DLL da NVIDIA; a opção `[Network] Resize History = 1` (desligada por padrão) usa `carry_history`, que é um blit
bilinear do histórico inteiro para o tamanho novo — a mesma ideia do 3eff79ec5 que deixou as transições piores no
jogo. Então não há abordagem melhor no upstream para adotar, e não expus opção (seria trazer de volta o que foi
revertido). Ideia para depois, se o reset continuar incomodando: rodar a rede sempre no tamanho do bucket com o
subrect reamostrado para ele (histórico em espaço normalizado, sem reset por degrau), ao custo de duas reamostragens
e de rodar a rede maior — mudança grande no host, não feita.

## Chaves novas no INI ([DlssNr])

- `MochizukiHistoryPrecision` = 16 (padrão) | 32. 32 = imagem idêntica ao 0.5.9. Menu: "16-bit history" na seção
  Temporal. Muda a rede (rebuild).
- `MochizukiPreprocessInverse` = true (padrão). Menu: Preprocess > "Inverse".
- Passes (fast) do estágio RenoDX, grupo Passes do menu: `MochizukiRenoPassesMode` (0 legacy, 1 fast),
  `MochizukiRenoFastMethod` (0-3), `MochizukiRenoFast{Extrapolation,Feedback,Hybrid}Passes` (1-8, padrão 3),
  `...HaloLimit`, `...Fine` (padrões 0 / 1 / 1), `MochizukiRenoFastEditStrength` (0-4, padrão 2).
- `MochizukiNrControls` cresceu depois de `style_grade` (struct menor mantém os padrões).

## Validação (RX 9070 XT, harness `mz_e2e`, 40 frames, contra o runtime do release 0.5.9)

Scripts em `exports\mochi0082-verify\` (`tests.ps1`, `prewarm.ps1`; pastas `drel` = 0.5.9, `dfix` = esta build).

| Caso | resultado |
|---|---|
| RDNA4, histórico 32 (NR_HIST16=0), small/310.8, 720p/1080p, driver e ACO | 8 de 8 idênticos byte a byte ao 0.5.9 |
| idem com pan de 8 px/frame (ACO, 720p/1080p) | idênticos |
| como vai (histórico 16 com ACO) | 54,7-57,3 dB do 0.5.9 (0,1-0,2% mais escuro em média); pan 56,1-57,1 dB |
| driver sem ACO | sem post RGBA16F, continua idêntico ao 0.5.9 |
| RDNA3 forçado, 3 rodadas | determinístico em 1152x648, 720p, 1080p; 648p idêntico ao 0.5.9, 720p/1080p a poucos valores (max 0,0005) |
| checagem de pipelines RDNA3 | 109 de 109 (eram 99: +10 shaders novos do estágio) |
| ACO vs upstream | o SPIR-V RDNA4 bate com os 62 records + 75 int4 do upstream (mesmo código de máquina) |
| `history_precision = 32` pelos controles (pan, ACO, 1080p) | idêntico ao 0.5.9 com os mesmos controles |
| Passes (fast), métodos 0-3 (pan, ACO, 1080p) | rodam sem erro, +0,7 ms, mudam a imagem como esperado (26-30 dB da base) |
| Preprocess on, Inverse on/off | os dois rodam; diferem (34 dB), como esperado |

Tempo de rede (GPU), 0.5.9 / agora: ACO small 720p 3,15 / 2,82 ms, 1080p 5,25 / 5,20; 310.8 720p 5,05 / 4,89, 1080p
7,45 / 7,07. Driver: small 720p 4,09 / 4,33, 1080p 6,99 / 6,08; 310.8 720p 6,74 / 5,81, 1080p 9,96 / 9,08 (uma rodada
cada, ruído de décimos). RDNA3 forçado igual ao 0.5.9 (6,9 / 8,4 / 13,0-13,4 ms).

Não deu para comparar com a build "do próprio upstream" rodando: o deles é um plugin .asi do OptiScaler original. A
comparação foi no nível do código: o mesmo SPIR-V e os mesmos records ACO.

Prewarm refeito (ACO off, 720p/1080p/1440p/4K + 1080p 310.8): 38 pipelines, em `exports\mochi0082-mz\dlssnr-amd\prewarm`.

## Commits (locais, sem push)

- `0dd273b7a` vendor v0.0.8.2 + `build-mochizuki-renodx.py`
- `197e1fd9c` OptiScaler: History Precision, Preprocess Inverse, Passes (fast)
- o commit deste handoff + `UPSTREAM.md`

## Teste em jogo

Arquivos:
- `exports\mochi0082\OptiScaler.dll` -> `dxgi.dll` do jogo
- `exports\mochi0082-mz\MochizukiNrRuntime.dll`
- a pasta `exports\mochi0082-mz\dlssnr-amd\` inteira (shaders, renodx, shaders-rdna3, aco, int4, prewarm mudaram), por
  cima da do jogo mantendo os modelos (`dlssnr.bin`, `dlssnr-small.bin`, `dlssnr-int4.bin`, que não estão no build).
  Apagar `dlssnr-amd\aco-cache\` e `pipeline.cache` do jogo não é necessário (a chave muda), mas deixa o primeiro
  start igual ao de um usuário.

O que olhar:
- Cyberpunk com DRS: sem rebuild nos degraus (log `dynamic resolution (auto)`), o reset de histórico por degrau
  continua (comportamento esperado, igual ao upstream/NVIDIA). Comparar se ficou igual ao 5421603fd sozinho.
- Imagem vs 0.5.9: quase idêntica; para idêntico, `MochizukiHistoryPrecision=32`. VRAM menor (a rede ~300 MB a menos
  em 1080p).
- Passes (fast) no grupo Passes do menu (novidade): Extrapolation sem vetores, Feedback/Hybrid com vetores.
- FSR 2.4 (d37c5b447) continua OK (nada nesta sync mexe nele, mas é a mesma branch).
- RX 7000: se alguém testar, a rede é a mesma do 0.5.9 (check de pipelines agora compila 109).
