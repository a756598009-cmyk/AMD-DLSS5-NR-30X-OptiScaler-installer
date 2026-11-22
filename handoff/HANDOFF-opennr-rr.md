# OpenNR RR: Ray Reconstruction própria no OptiScaler (teste no Cyberpunk)

2026-10-30. Branch `opennr-rr` (worktree `wt-opennr-rr`, a partir de `dlss-neural-rendering` 887bfb195 = 0.5.9).
Runtime MIT no repositório OpenNR-Denoiser, branch `runtime-dll` (worktree `OpenNR-Denoiser-3c`), detalhes em
`docs/FASE4.md` de lá. Nada publicado.

## O que é

"OpenNR RR" é um provedor de Ray Reconstruction (DLSS-D no D3D12) que roda o nosso port da rede rr2 (preset F) numa
RX 9000 (RDNA4). O OptiScaler (GPL) só carrega `OpenNrRrRuntime.dll` (MIT) pelo ABI C
`OptiScaler/upscalers/opennr_rr/opennr_rr_api.h` (cópia literal do arquivo do repositório MIT). Os pesos saem da
`nvngx_dlssd.dll` do próprio jogo (310.9.1.0 no Cyberpunk), lidos em tempo de execução; nada da NVIDIA é distribuído.

Um modo só, o **rápido** (acumulação fp32). O modo fiel (k8, acumulação fp16 a cada 8 K como a GPU de referência)
saiu do produto em 2026-11-02 (branch `opennr-rr-line`): fica só no harness do repositório MIT. Na validação pelo
caminho D3D12 o rápido fica em 60–65 dB contra a saída do NGX.

## Como funciona no OptiScaler

- `upscalers/opennr_rr/OpenNrRrFeature_Dx12.*`: `Upscaler::OpenNrRr` ("OpenNR RR"). Lê as entradas do DLSS-D dos
  parâmetros NGX como o FSR-RR (cor, albedos, normais, rugosidade e modo, profundidade e o tipo dela, MVs e escala,
  hit especular, jitter, reset, matrizes WorldToView/ViewToClip copiadas sem transpor).
- Por quadro: `RecordInputs` na lista do jogo, divisão da lista (`LmxxfCut::TrySplitAtEvaluate`, a mesma do
  mochizuki), `RecordOutput` na segunda metade, `Enqueue` no slot entre as metades (`SetPendingEnqueue` +
  `ArmBetweenSlot`). Os proxies de lista existem quando `[OpenNR-RR] Enabled=true` na abertura ou um runtime NR que
  divide listas está ativo (no Cyberpunk do usuário, `NrBackend=mochizuki`).
- Seleção: entrada extra "OpenNR RR" no combo de upscaler quando o jogo usa RR no DX12 numa GPU não NVIDIA. Escolher
  grava `Enabled=true`; trocar para outro grava `false`. Se não puder ser oferecido, uma linha cinza diz o motivo.
- Cabeçalho "OpenNR RR" no menu: status (estado, ms de GPU, mensagem, origem dos pesos) e as chaves das entradas
  opcionais: `UseSssGuide` (desligada por padrão), `UseDisocclusionMask`, `UseResponsivityMask`, `UseColorBeforeTransparency`,
  `UseDepthOfFieldGuide`, `UseReflectionMotionVectors`, `UseLayerOpacityAndMotion` (ligadas; só valem quando o jogo
  passa a entrada). Sem combo de modo.
- Queda: DLL ausente, `Check`/`Create` falhando, estado FAILED, divisão recusada ou erro de quadro → log, um aviso e
  volta para o FSR-RR, sem tentar de novo até reiniciar o jogo.
- INI: `[OpenNR-RR] Enabled=auto` (ligado onde o runtime aceita a GPU e a `nvngx_dlssd.dll`) e as chaves `Use*` acima.
  Um `Mode=` que sobrou de um ini antigo é ignorado.
- Com OpenNR RR ativo o NR antes do SR (mochizuki) não roda nesses quadros (como já acontece com o FSR-RR): a lista só
  pode ser dividida uma vez.

## Instalado no Cyberpunk (E:\Games\Cyberpunk2077\bin\x64)

- `dxgi.dll` trocado pelo build desta branch (sha256 e95c9f26…); o original está em
  `_optiscaler_backup_2026-10-30_opennr_rr\dxgi.dll` (979a8285…).
- Novos: `OpenNrRrRuntime.dll` (33126c00…) e a pasta `opennr-rr\shaders\` (43 SPIR-V, os dois modos; a build atual
  carrega 39, ver o stage `out\rr-line-stage\` do OpenNR-Denoiser).
- `OptiScaler.ini` do usuário **não foi tocado**. Lista em `_optiscaler_backup_2026-10-30_opennr_rr\installed.txt`.

## Como testar

1. Abrir o Cyberpunk, Gráficos: Path Tracing (ou Ray Tracing) ligado, DLSS como upscaler e **Ray Reconstruction
   ligado** (como já é usado com o FSR-RR).
2. No jogo, abrir o menu do OptiScaler (Insert), **Upscalers** → combo do upscaler → **OpenNR RR**.
3. Nos primeiros segundos a imagem é a cor sem denoise (a rede está sendo montada, ~6 s na primeira vez); depois entra
   o RR. No cabeçalho **OpenNR RR** do menu: status "ready", tempo de GPU e as chaves das entradas.
4. Testar movimento de câmera, cortes de cena, reflexos e trocar a qualidade do DLSS (muda a resolução de render).

Logs para mandar: `opennr_rr.log` (runtime) e `OptiScaler.log` (linhas "OpenNR RR"), ambos em `bin\x64`; se travar,
`%LOCALAPPDATA%\CD Projekt Red\Cyberpunk 2077\CrashInfo.json`.

## Reverter

Copiar `_optiscaler_backup_2026-10-30_opennr_rr\dxgi.dll` de volta para `bin\x64` e apagar `OpenNrRrRuntime.dll` e a
pasta `opennr-rr`. Chaves gravadas pelo menu em `[OpenNR-RR]` não fazem nada sem a DLL.

## Riscos

- Sem captura de referência para profundidade de hardware, câmera que se move (matrizes), `mv_scale` ≠ 1 e normais
  UNORM: as fórmulas seguem o NGX, mas só o jogo mostra se estão certas (sinal provável de erro: fantasmas/borrão em
  movimento ou reflexos que deslizam).
- Custo extra das cópias das guias (texturas inteiras por quadro) e do ponto de sincronia entre filas.
- Memória: algumas centenas de MB a mais (buffers por tensor + histórico interno a 1,5× a saída).
- O NR do mochizuki fica desligado nos quadros com OpenNR RR.
