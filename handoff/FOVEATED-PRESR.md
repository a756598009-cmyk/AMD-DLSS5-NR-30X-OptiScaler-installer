# NR foveado antes do SR (pré-SR) — 2026-11-04

Worktree `wt-foveated-presr`, branch `foveated-presr` (de `mega-update` 4e8a5e649, o candidato 0.6.0-opennr). Só
commits locais, nada enviado nem instalado em jogo. Base: o protótipo de `foveated-nr` 535baf140 (imagem final),
trazido sem conflito (`handoff/FOVEATED-NR.md` continua valendo para a matemática do warp e para a imagem final).
Crédito do warp: FoveatedDLSS5 (MIT, Copyright (c) 2025 Carlos Lopez Jr.), em `Licenses/FoveatedDLSS5_LICENSE.txt` e
no topo de `Foveation.h`.

Commits:
- `921991cb8` porte do protótipo (Foveation.h, imagem final D3D12/D3D11, ini/menu, harness)
- `b2d40ac70` o pré-SR (AmdBridge + Foveation.h + menu/ini)
- `4bf92c5bb` harness para tamanhos de render (sem depth, jitter, ACO, feed do RenoDX), `gain.py`, `build-dll.cmd`
- (este handoff)

## Veredito

**Pronto para testar em jogo, como experimental opt-in.** Desligado por padrão; desligado, o caminho é o de antes
(o grid é a identidade, nada é gravado, o runtime recebe o mesmo `Frame`). O ganho é real (render 1080p: -21/-30/-46%
no tempo da rede). O risco é de imagem, não de estabilidade: no conteúdo HDR do teste, em render 720p/960p, o Médio e o
Forte mudam o "look" no centro (brilho +3-7%, PSNR 28-34 dB contra o NR cheio). O Leve fica perto (34-42 dB).
Nenhum teste em jogo foi feito: o caminho do bridge (lista dividida do jogo, estados, DRS) só compila; o harness roda
as mesmas passes de warp/unwarp em volta do runtime, mas sem o LmxxfBackend nem um upscaler depois.

Sugestão para a decisão do update: mandar só se o teste em Cyberpunk não mostrar quadro preto/crash/costura e o Leve
ou o Médio parecerem aceitáveis; o Forte provavelmente não vale (mudança de look grande demais em HDR a 720-960p).

## O que funciona

Em `AmdBridge.cpp` (`Run`, pré-SR, qualquer API que passe pelo bridge D3D12: D3D12, D3D11 via w/Dx12 e o seam Vulkan),
em volta de `b->Record`:

1. `MakeGrid` duas vezes: na extensão do quadro (o warp deste quadro) e na extensão fixa (`gateWidth/Height`: a alocação
   da cor sob o DRS do mochizuki, senão o próprio quadro). A fixa dimensiona as texturas warpadas, aparece no log e, se
   mudar, reinicia o histórico (`InvalidateHistory` + stabilizer). Um passo de DRS muda só o subrect dentro das mesmas
   texturas, como o jogo faz; o runtime (modo `auto`) já reinicia o histórico a cada subrect novo, e o bridge também.
2. `Warp` na lista do jogo **antes** do Record (fica na parte produtora do split), com a cor e os vetores do jogo; sem
   depth (o mochizuki pré-SR não lê depth). O runtime recebe a cor warpada (formato do jogo), vetores warpados R16G16F
   em pixels do grid de trabalho (escala 1), `width/height` = grid de trabalho.
3. `Unwarp` **depois** do Record (parte consumidora): `jogo + mudança da rede` reamostrada edge-aware, FP16, alocada na
   extensão da alocação da cor do jogo (estável sob DRS). Daí em diante tudo igual: person protection (não roda junto,
   ver abaixo), stabilizer, composição, SR, todos no grid de render original.

Vetores de movimento (a escolha): a posição é warpada e o vetor transformado para o espaço warpado
(`prev_w = Warp(n + mv) − Warp(n)`, por texel), não escalado por uma densidade local. Assim o histórico do runtime
fica coerente no espaço warpado sem nenhuma hipótese sobre a forma da rampa, e o warp não depende do jitter (é só
geometria do grid), então é estável quadro a quadro. O jitter do upscaler fica na cor: o resultado volta ao grid de
render, o SR vê o jitter intacto. Com `MochizukiJitterHistory=true` (padrão desligado) o passo do jitter
(`jitter anterior − atual`, pixels de render) entra no warp junto com o vetor e o runtime recebe jitter 0 (senão o
`MochizukiGuides` aplicaria o passo em pixels de render no espaço warpado, certo só no centro).

Outros detalhes:
- Extensão ímpar (1707 a 2560 Quality): o trabalho fica um pixel menor (1471, 1343, 1087) para a margem ser par e o
  centro cair em texels inteiros; a rede paga o mesmo múltiplo de 64.
- Falha (shader, alocação): log de erro, foveado desligado na sessão, NR segue cheio.
- lmxxf e danielblnc antes do SR: não ligados. Log uma vez: `AMD pre-SR: foveated NR runs on mochizuki only; this
  runtime gets the whole frame`. Motivo: lmxxf tem tiers fixos (o grid cairia no mesmo tier, sem ganho) e o danielblnc
  tem o próprio Model scale/fixedWidth; nenhum dos dois foi medido.
- Person protection (`NrPersonPartition`) ligada: foveado desligado, log `foveated NR is off while person protection
  runs` (o first pass sai no grid warpado e precisaria de um segundo unwarp).
- Vulkan: o seam Vulkan chama o mesmo `AmdBridge::Before` com uma lista D3D12 própria, então o foveado vale lá também
  sem código novo. Não testado.

## ini e menu

`[DlssNr]` (as mesmas chaves da imagem final; padrão desligado):
```
Foveation=auto              ; 0 off, 1 leve, 2 médio, 3 forte, 4 custom
FoveationCentreWidth=auto   ; custom: 0.5
FoveationCentreHeight=auto  ; custom: 0.6
FoveationFalloff=auto       ; custom: 0.12 (fração da altura)
FoveationEdge=auto          ; custom: 0.5 (0.2-1)
```
Menu: página do mochizuki, cabeçalho **Temporal**, logo abaixo de "Stabilizer threshold": combo
**Foveated NR (experimental)** (Off / Light / Medium / Strong / Custom (INI)). Agora aparece sempre na página do
mochizuki (antes só com NR sem upscaling). Muda ao vivo; cada troca reconstrói a rede (~1 s sem NR) e reinicia o
histórico.

Log (`OptiScaler.log` com LogToFile, e `mochizuki_nr.log`):
- `AMD pre-SR: foveated NR, the network at 1343x832 for 1707x960 (68% of the pixels; edge density 0.51 x 0.52)`
- `AMD pre-SR: foveated NR off at WxH` ao desligar
- `mochizuki_nr.log`: `network ready at 1343x832` e o status da sessão com o ms da rede.

## Números (RX 9070 XT, mochizuki do mega-update, Small, ACO, 1 passe, sem depth)

Harness `tools/foveation/mz_fov.cpp` (`run.ps1`, lock de GPU compartilhado), runtime e shaders compilados deste branch
(`exports/fov-runtime`), pesos do kit (fora do git). Tempo = mediana do status da rede, 300 quadros, 3 repetições
intercaladas, mediana das 3. A 3ª repetição teve interferência em alguns pontos (1080p 3.18/2.82/2.96 contra 2.8/2.5/1.9
nas outras duas); a mediana descarta. Execuções de 100 quadros dão números ~15-25% mais altos (clock subindo): só os de
300 quadros entram na tabela.

Tempo da rede (ms), grid de trabalho entre colchetes:

| render | desligado | leve | médio | forte | warp+unwarp |
|---|---|---|---|---|---|
| 1280x720 (1080p Quality) | 2.26 | 2.03 (-10%) [1088x704] | 1.63 (-28%) [1024x640] | 1.46 (-35%) [832x512] | 0.05 |
| 1707x960 (1440p Quality) | 2.78 | 2.34 (-16%) [1471x896] | 2.11 (-24%) [1343x832] | 1.79 (-36%) [1087x704] | 0.08 |
| 1920x1080 (4K Performance) | 3.57 | 2.82 (-21%) [1664x1024] | 2.49 (-30%) [1536x960] | 1.92 (-46%) [1216x768] | 0.11 |
| 2560x1440 (4K Quality) | 5.94 | 5.00 (-16%) [2240x1344] | 3.86 (-35%) [1984x1216] | 3.07 (-48%) [1664x1088] | 0.21 |

Em render pequeno a rede escala menos que os pixels (720p leve: 82% dos pixels, -10% no tempo), então o ganho em
1080p/1440p de saída é menor que no NR sem upscaling.

Qualidade, PSNR (sRGB) contra o NR cheio no mesmo tamanho, imagem parada, centro / quadro inteiro, e o ganho de
brilho médio do centro (foveado / cheio). Imagem 1 `test.png` (float, rota HDR relativa do runtime); imagem 2 `sb.png`
(Stellar Blade com HUD, também float). Referência: entrada x NR cheio = 25.7-26.4 dB (img 1) e 33.5-33.9 dB (img 2),
isto é, o tamanho do efeito do NR.

| | leve | médio | forte |
|---|---|---|---|
| img1 720 | 34.5 / 35.9, +3.7% | 34.3 / 36.1, +3.1% | 27.6 / 30.7, +7.3% |
| img1 960 | 37.3 / 38.7, +2.1% | 30.2 / 33.1, +5.5% | 28.6 / 31.4, +7.4% |
| img1 1080 | 41.9 / 42.0, -0.5% | 40.0 / 39.8, +0.9% | 35.6 / 34.1, +2.5% |
| img2 720 | 43.5 / 42.2, +1.8% | 45.0 / 41.5, +0.6% | 41.4 / 38.8, +0.8% |
| img2 960 | 45.5 / 43.9, +0.3% | 42.2 / 41.4, +1.0% | 40.2 / 39.1, +0.4% |
| img2 1080 | 42.9 / 42.3, -1.9% | 43.7 / 42.4, -1.9% | 40.0 / 37.4, -3.1% |

Em movimento (pan de 4 px/quadro + jitter Halton na cor, último de 4 quadros): img1 720 médio centro 38.0 dB, forte
28.9; img2 ~43-47 dB. Estabilidade temporal (|t − (t−1) deslocado| x 255, já dominada pelo jitter): foveado igual ao
cheio em leve/médio (diferença ≤ 0.3), menor no forte (o filtro do warp suaviza). O shimmer de borda que apareceu na
imagem final (2-3x) não aparece acima do ruído do jitter aqui; o SR depois ainda acumula por cima.

Crops (entrada | NR cheio | foveado) em `exports/fov-run/out/crops/`: `{test,sb}_{1280x720,1707x960,1920x1080}_f{1,2,3}_r1_{centre,corner,left}.png`
e `*_full_vs_fov.png`. Olhei os de 720p: sem costura nem desalinhamento; o forte fica um pouco mais claro e mais macio
no centro, as bordas perdem o microdetalhe que o NR adiciona (o detalhe do jogo fica).

### Brilho do centro: o que foi tentado

- Hipótese 1, o divisor HDR do estágio RenoDX (o medidor de conteúdo amostra uma grade 64x36 da imagem warpada, que
  super-representa o centro). Medido com `MochizukiRenoFeedMode=2` (divisor fixo 1): 720p médio +6.9% contra +6.2%
  com o medidor, forte +17.2% contra +16.3% (linear). **Não é o divisor.**
- Diagnóstico: downscale uniforme com o mesmo custo (1088x576, 68%, sem centro 1:1): centro 29.8 dB e +9.8% de brilho,
  pior que o médio foveado (34.3 dB, +6.6% linear). O desvio é da própria rede ao rodar num quadro menor/outro (ela tem
  contexto global), não do warp; o foveado é melhor que reduzir a resolução por igual. Para escala: o NR cheio do
  mesmo quadro em render 1080p vs 720p já difere +1.5-3.9% de brilho no centro (trocar o preset do DLSS muda o look
  na mesma ordem).
- Correção barata não encontrada: sem uma passada cheia de referência não há com o que casar a média do centro, e a
  direção/tamanho do desvio muda com a imagem (img1 sempre mais claro, img2 ±2%). Uma passada cheia ocasional exigiria
  um segundo histórico. Não implementado nada às cegas.

## Problemas conhecidos

1. Look do centro muda no médio/forte em conteúdo HDR a render baixo (tabela acima). Leve é o mais seguro.
2. Nada testado em jogo: estados da lista do jogo (descriptor heap/root signature trocados na lista dentro do evaluate,
   como o stabilizer e a composição já fazem), split produtor/consumidor do mochizuki com o warp antes e o unwarp
   depois, DRS do Cyberpunk (nova alocação a cada passo: o grid segue a alocação, então pode haver rebuild da rede em
   cada passo novo, como sem foveado).
3. lmxxf, danielblnc e person protection: foveado desligado antes do SR (log uma vez).
4. Trocar o preset = rebuild da rede (~1 s sem NR) e histórico novo; não deixar slider ao vivo.
5. O jitter dobrado nos vetores (`MochizukiJitterHistory=true`) não foi exercitado no harness (o harness chama o
   runtime direto, sem o `MochizukiGuides`); com a chave desligada (padrão) o caminho é o mesmo do NR cheio.
6. A saída do unwarp é FP16 (como a resposta normal do runtime); entrada UNORM continua funcionando (testada no
   protótipo).

## Build e arquivos

- `tools\foveation\junctions.cmd on` (submódulos de `wt-upstream-merge\external` + `ffx-sdk-v2.4.0`),
  `tools\foveation\build-dll.cmd` (mesmas flags do build do wt-mega, saída `exports\fov-build`),
  `tools\foveation\build-dll.cmd runtime` (runtime em `exports\fov-runtime`), `junctions.cmd off` antes de qualquer git.
- Harness: `tools\foveation\build.cmd` -> `exports\fov-run\mz_fov.exe`; `run.ps1 -Name x -Fov 2 -Size 1707x960 -Aco
  [-Jitter -Pan 4 -Dump 4] [-Feed 2]`; `analyze.py out 1707x960 <ref> <tags> --input=test.png --crops [--pan=4]`;
  `gain.py out 1707x960 test.png <ref> <tags>`. A pasta `exports\fov-run` precisa dos pesos (`dlssnr-amd\dlssnr*.bin`,
  copiados do kit, fora do git).
- **Para o jogo: `exports\fov\`** = `OptiScaler.dll` (sha256 EF5D5C9FAF78FA50…), `MochizukiNrRuntime.dll` e
  `dlssnr-amd\` (shaders, aco, int4; sem pesos). O runtime não mudou em relação ao mega-update (mesmo código); se o
  Cyberpunk já está com os arquivos do 0.6.0 RC, basta o `OptiScaler.dll`.

## Teste em Cyberpunk 2077 (passo a passo)

Antes de tudo: **faça backup** do `dxgi.dll` (é o OptiScaler no Cyberpunk) e do seu `OptiScaler.ini`, em
`E:\Games\Cyberpunk2077\bin\x64`, numa pasta nova `_optiscaler_backup_2026-11-04_foveated\` (as anteriores
`_optiscaler_backup_*` ficam como estão). Eu não mexi na pasta do jogo.

1. Copie `wt-foveated-presr\exports\fov\OptiScaler.dll` para `bin\x64\dxgi.dll` (substituindo). Se o jogo não estiver
   com o runtime do 0.6.0 RC, copie também `MochizukiNrRuntime.dll` e as pastas `dlssnr-amd\shaders*`, `aco`, `int4`
   (sem apagar os `dlssnr*.bin` que já estão lá).
2. No `OptiScaler.ini`, `[DlssNr]`: `Enabled=true`, `NrBackend=mochizuki`, `Foveation=auto` por enquanto (começa
   desligado). Ligue `LogToFile=true` se quiser o `OptiScaler.log`. Confira que `NrPersonPartition` está desligado.
3. Abra o jogo com DLSS/FSR em Quality (1440p -> render 1707x960 ou 1080p -> 1280x720). Sem DRS no primeiro teste.
4. Menu do OptiScaler (Insert) -> página do mochizuki -> **Temporal** -> **Foveated NR (experimental)**. Anote o
   "Neural pass" (ms) em Off, depois Light, Medium, Strong (cada troca: ~1 s sem NR enquanto a rede reconstrói).
5. Em cada um: centro igual ao Off a olho? Brilho/saturação do centro mudam (esperado no Strong)? Costura na transição
   (não deveria)? HUD e bordas nítidos? Shimmer nas bordas girando a câmera e andando de carro?
6. Alterne Off <-> Medium várias vezes, mude o preset do DLSS (render muda), abra/feche o mapa e o inventário: sem
   crash, sem quadro preto, sem travar.
7. Opcional: ligue o DRS do jogo com Medium e ande um pouco (cada passo de resolução pode reconstruir; ver se
   engasga mais que sem foveado).
8. Logs para me mandar: `mochizuki_nr.log` (linhas `network ready at WxH`, `session end ... ms`), `amd_bridge.log`,
   `OptiScaler.log` (linhas `AMD pre-SR: foveated NR ...`).
9. Para voltar: restaure o `dxgi.dll` e o ini do backup (ou só `Foveation=0`).
