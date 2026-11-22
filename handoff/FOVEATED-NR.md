# NR foveado (protótipo) — 2026-10-28

Worktree `wt-foveated`, branch `foveated-nr` (de `dlss-neural-rendering` ca2544ec3). Só commits locais; nada
enviado, publicado ou instalado em jogo. Ideia e matemática do warp: FoveatedDLSS5
(github.com/maximedgoulet/FoveatedDLSS5, MIT, Copyright (c) 2025 Carlos Lopez Jr.), crédito em
`Licenses/FoveatedDLSS5_LICENSE.txt` (copiado para o pacote pelo post-build do vcxproj) e no topo de `Foveation.h`.

## 1. Ideia

O NR é uma rede de mesma resolução: não dá para pedir resolução variável por pixel. Então o quadro é **comprimido por
eixo** antes da rede e o resultado é **esticado de volta**:

- por eixo, densidade 1 numa zona central, rampa smoothstep (C1, sem costura), densidade `edge` depois dela;
  cantos ficam com edge x edge;
- a rede roda **uma vez** no quadro menor, com **um** histórico temporal;
- depth e motion vectors são reamostrados no mesmo grid (depth: vizinho mais próximo; MV: o deslocamento passa pelo
  warp, `prev = cur + mv`, em pixels do grid de trabalho, escala 1);
- inversa do warp com Newton (4 passos) só na rampa; centro e borda em forma fechada;
- economia < 5% => desliga sozinho; mudar o grid reinicia o histórico (a rede reconstrói no novo tamanho).

### O que muda em relação ao upstream

1. **Unwarp pela mudança da rede, não pela saída.** `saída(p) = jogo(p) + Σ w·(NR_w(q) − entrada_w(q)) / Σ w`, com
   q os 4 texels bilineares em volta da posição warpada de p e w = bilinear × exp(−((L(entrada_w(q)) − L(jogo(p)))/σ)²)
   (bilateral conjunto na luminância, σ = 0.08, L = y/(1+y)). O detalhe do próprio jogo fica em todo pixel (texto do
   HUD nas bordas continua nítido); só o efeito do NR fica mais suave nas bordas. No centro a posição cai num texel
   inteiro e a saída é a resposta da rede, sem reamostragem.
2. **Tamanho de trabalho alinhado ao padding da rede.** O mochizuki (Small) arredonda a extensão da rede para
   múltiplos de 64 (128 quando `up((n+1)/2,4) % 8 == 4`: 1080 -> 1152; medido, 1920x1032 custa o mesmo que 1920x1152).
   O grid escolhe o múltiplo de 64 (abaixo ou acima do alvo) cuja densidade de borda resolvida fica mais perto da
   pedida, e **resolve a densidade de borda** para bater exatamente nesse tamanho (G(1) é linear em e). Assim o
   centro continua 1:1 exato, margem par (centro em texels inteiros), e nenhum pixel de padding é pago. Bônus: a
   1080p o NR cheio paga 1152 linhas; o foveado nunca paga padding.
3. Valores por eixo como floats (a, b, e, g) em root constants, sem o dword empacotado.
4. Tamanhos relativos à tela (fração da largura/altura), não pixels absolutos: um preset serve 1080p/1440p/4K.

## 2. Onde fica

Tudo em `OptiScaler/dlssnr/amd/Foveation.h` (um lugar só):
- host: `Axis`, `G`, `Warp`, `SolveEdge`, `Params`, `Preset`, `MakeAxis`, `MakeGrid` (+ `kMinSaving`, `kMinEdge`);
- HLSL (string, D3DCompile cs_5_0, padrão do RenoComposition.h): `WarpColour` (cor por cobertura de área; 1 pixel
  exato no centro), `WarpGuides` (depth + MV), `UnwarpResult` (upsample edge-aware da mudança);
- `Passes`: `Warp(cmd, Frame, Grid) -> Frame` (texturas nossas, guias na extensão da cor, escala 1) e
  `Unwarp(cmd, Frame cheio, resultado) -> FP16 cheio`. Só D3D12 + `AmdPreSr::Frame`, sem dependência de backend:
  serve aos três runtimes.

Formato da cor warpada: mesmo tipo do jogo (UNORM continua UNORM: o runtime lê UNORM como cor de display e float
como cor de cena). BGRA vira RGBA8 (store UAV tipado garantido). sRGB vira FP16 (a SRV lê linear).

### (a) NR sem upscaling (imagem final) — implementado

`PresentExperimental.h` (rota D3D12 e D3D11-sobre-D3D12): depois de copiar o backbuffer e montar flow/depth,
`FoveationGrid()` lê o ini; se ativo, `Warp` no mesmo `cmd` antes do `Record`, o runtime recebe o Frame warpado, e
`Unwarp` depois do resultado:
- mochizuki: Warp na lista produtora, Unwarp na consumidora (depois de `RecordConsumer`);
- danielblnc: Warp e Unwarp na mesma lista, em volta de `backend->Record`.
Depois disso tudo segue com o quadro cheio (stabilizer, Compose, tempo do Neural pass). Grid novo => log +
`InvalidateHistory` + `stabilizer->Invalidate()`.
Não ligado: Vulkan (`VulkanPresentExperimental.cpp`) e OpenGL (`GLPresentExperimental.cpp`), que têm caminho próprio.
O lmxxf não roda na imagem final.

### (b) Antes do SR (render res) — desenho, não implementado

Em `AmdBridge.cpp`, em volta de `backend->Record(list, f, settings)` (Host, qualquer runtime): `Warp` -> Record ->
`Unwarp` em render res, antes do RenoDX composition e do SR. Cuidados:
- jitter: o upscaler precisa do jitter do jogo intacto. O resultado desfeito volta ao grid de render original, então
  o SR vê o jitter certo; dentro da rede o jitter vira sub-pixel variável nas bordas (centro igual). `Frame.jitterX/Y`
  pode ser repassado como está (o mochizuki usa só no Jitter-aligned history).
- DRS / `fixedWidth`: calcular o grid da extensão fixa (maior quadro visto), não da atual, senão cada mudança de
  resolução muda o grid e reconstrói a rede. Hoje `Warp` zera `fixedWidth`.
- `afterUpscale` / `guideWidth`: já tratados no Warp (guias amostradas na extensão delas).
- `exposure` / `preExposure` passam iguais (são globais).
- lmxxf tem tiers fixos (1080 -> 1920x1152, ou 1088 com COMPACT_1080): o alinhamento precisa ser o do tier, não 64,
  senão o tamanho warpado cai no mesmo tier e não economiza nada. Medir antes.
- render res já é pequena (ex. 1080p Quality = 1280x720): a economia é a mesma em proporção, mas o centro 1:1 em
  720p já é baixo; preferir bordas mais suaves (edge >= 0.5).

### Entradas por runtime

| | cor | depth | motion | exposição/histórico |
|---|---|---|---|---|
| mochizuki | warpada (formato do jogo) | warpada R32F (lê depth para escolher MV na silhueta) | warpada R16G16F, escala 1 | histórico interno no grid warpado; grid novo => rede nova |
| lmxxf | idem | não usa (ABI sem depth) | idem | idem; tiers fixos |
| danielblnc | idem | warpada | warpada | histórico interno; Model scale soma com o foveado |

## 3. Presets (16:9)

Centro = fração da tela em densidade 1; fall-off = fração da altura, igual nos 4 lados; edge = densidade alvo
(a real sai do alinhamento e aparece no log).

| preset | centro L x A | fall-off | edge | 1080p grid (px) | 1440p grid | 4K grid (estimado) |
|---|---|---|---|---|---|---|
| leve (1) | 60% x 70% | 15% | 0.60 | 1664x1024 (82%) | 2240x1344 (82%) | ~3328x2048 (~82%) |
| médio (2) | 50% x 60% | 12% | 0.50 | 1536x960 (71%) | 1984x1216 (65%) | ~2944x1792 (~64%) |
| forte (3) | 40% x 50% | 10% | 0.35 | 1216x768 (45%) | 1664x1088 (49%) | ~2432x1600 (~47%) |
| custom (4) | ini | ini | ini | | | |

4K não foi medido (só calculado com a mesma regra).

## 4. Medição (RX 9070 XT, mochizuki 0.5.8 Small, ACO, 1 passe, 100 quadros, mediana da rede do status)

Harness: `tools/foveation/mz_fov.cpp` (mz_e2e + Foveation.h em volta do runtime), `run.ps1` (com o lock de GPU),
`analyze.py` (PSNR por zona contra o NR cheio, crops, erro temporal). Runtime e shaders de
`wt-rdna2/exports/base-058`, modelos da refwork do kit RX 7000. Saídas em `exports/foveation/run/out/`.

Tempo da rede (ms) e economia:

| | desligado | leve | médio | forte |
|---|---|---|---|---|
| 1080p | 4.59-4.65 | 3.54 (-23%) | 3.16 (-32%) | 2.40 (-48%) |
| 1440p | 7.95 | 6.77 (-15%) | 4.93 (-38%) | 3.89 (-51%) |

Warp + unwarp: ~0.12 ms em 1080p, ~0.25 ms em 1440p (timestamps D3D12). Tempo total do quadro no harness segue a
rede (1080p 5.45 -> 4.38 / 3.94 / 3.07 ms).

Qualidade, PSNR contra o NR cheio (sRGB). Imagem 1 = `test.png` (o quadro do kit, float/HDR); imagem 2 = quadro de
Stellar Blade com HUD (`sb.png`, SDR RGBA8):

| | zona | leve | médio | forte |
|---|---|---|---|---|
| img1 1080p | centro / borda | 41.8 / 41.6 | 40.2 / 39.4 | 35.5 / 33.9 |
| img1 1440p | centro / borda | 34.3 / 42.1 | 33.6 / 38.4 | 28.7 / 33.4 |
| img2 1080p | centro / borda | 42.1 / 41.7 | 40.8 / 43.0 | 27.4 / 30.5 |
| img2 1440p | centro / borda | 39.0 / 40.5 | 43.7 / 41.7 | 36.2 / 36.6 |

Referência de escala: entrada x NR cheio = 26 dB (o quanto o NR muda o quadro).

**O centro não é idêntico** (esperado > 50 dB, deu 34-44). Não é desalinhamento (deslocar 1 px derruba para 27-29 dB;
o alinhamento 0 é o melhor) nem não-determinismo (dois runs iguais são bit-idênticos). É a rede: ela tem contexto
global (atenção ViT global) e responde diferente ao mesmo conteúdo quando o quadro inteiro é outro/menor. Aparece
como ganho global: img1 1440p médio +8% de brilho no centro (39.6 dB depois de tirar o ganho), em SDR +3%; img2
forte 1080p fica visivelmente mais claro e saturado no quadro todo (`sb_1920x1080_f3_full_vs_fov.png`). É uniforme
(sem costura, centro e bordas vêm da mesma passada), mas é uma mudança de "look", maior no forte.

Bordas: o detalhe do jogo fica (texto do HUD nítido: `uid_corner_1080_input_off_strong.png`,
`phone_1080_input_off_med_strong.png`); o que perde é o detalhe que o NR adiciona (tecido, microtextura).

Temporal (pan de 4 px/quadro, |t − (t−1) deslocado| x 255, média): NR cheio 0.16-0.26 em todas as zonas; foveado
centro 0.23-0.35, bordas 0.25-0.64 (1080p leve 0.55, médio 0.39, forte 0.64; 1440p 0.42 / 0.25 / 0.52). Movimento
na borda vira sub-pixel no grid warpado, e o filtro de caixa do warp + bilinear do unwarp dão um pouco de shimmer.
Pequeno em absoluto, mas 2-3x o do NR cheio nas bordas.

PNGs (entrada | NR cheio | foveado), em `exports/foveation/run/out/crops/`:
- `still_*_f{1,2,3}_{centre,corner,left}.png`, `still_*_full_vs_fov.png` (img1)
- `sb_*_f{1,2,3}_{centre,corner,left}.png`, `sb_*_full_vs_fov.png` (img2)
- `phone_1080_input_off_med_strong.png`, `uid_corner_1080_input_off_strong.png` (HUD nas bordas)

## 5. ini e menu

`[DlssNr]` (padrão desligado):
```
Foveation=auto              ; 0 off, 1 leve, 2 médio, 3 forte, 4 custom
FoveationCentreWidth=auto   ; custom: 0.5
FoveationCentreHeight=auto  ; custom: 0.6
FoveationFalloff=auto       ; custom: 0.12 (fração da altura)
FoveationEdge=auto          ; custom: 0.5 (0.2-1)
```
Menu: combo "Foveated NR (experimental)" na página do mochizuki (só com NR sem upscaling ativo) e no bloco de imagem
final do danielblnc (abaixo de Model scale). Muda na hora (rede reconstrói).

## 6. Build

`tools/foveation/junctions.cmd on` (submódulos de `wt-upstream-merge\external`), `tools\build-optiscaler-fast.cmd`,
`tools/foveation/junctions.cmd off` antes de qualquer git. DLL: `exports\release-local\OptiScaler.dll`
(cópia em `exports\foveation\OptiScaler.dll`, sha256 46dd8a5e7be89a6c…). Harness: `tools\foveation\build.cmd` ->
`exports\foveation\mz_fov.exe`. O runtime do mochizuki não mudou (o foveado fica no host D3D12).

## 7. Teste em jogo (para o usuário)

Jogo: um D3D12 ou D3D11 sem upscaler que já roda NR sem upscaling com mochizuki (ex. Cities: Skylines, Sims 4) ou
danielblnc (RDR1). Instalar a DLL acima no lugar do dxgi.dll/OptiScaler.dll do jogo (com backup), ini:
```
[DlssNr]
Enabled=true
PresentWithoutUpscaler=true
NrBackend=mochizuki        ; ou o que o jogo já usa
Foveation=2
```
O que olhar:
1. Log (`OptiScaler.log`): `NR without upscaling: foveated NR, the network at 1536x960 for 1920x1080 (71% of the
   pixels; edge density 0.54 x 0.60)`; no `mochizuki_nr.log`: `network ready at 1536x960` e o status com o ms menor.
2. Menu: "Neural pass" em ms com Off / Light / Medium / Strong (troca no combo; ~1 s de rebuild a cada troca).
3. Imagem: centro igual ao Off a olho? Brilho/saturação mudam (principalmente no Strong)? Costura na transição da
   rampa (não deveria)? HUD nas bordas nítido? Shimmer nas bordas ao girar a câmera?
4. Alternar Off <-> Medium várias vezes e mudar a resolução da janela: sem crash, sem quadro preto, log mostra
   "foveated NR off at ..." quando a economia fica < 5%.

## 8. Riscos / aberto

1. **Look global muda** (seção 4): o NR é não-local; o centro não fica idêntico, e o forte muda brilho/saturação.
   Possível mitigação futura: casar a média/ganho global do centro entre quadros com e sem foveado (medir o ganho a
   cada N quadros com uma passada cheia) — custa uma passada cheia ocasional.
2. Shimmer nas bordas em movimento (2-3x o NR cheio). Opções: filtro de warp tenda/Lanczos em vez de caixa;
   `edge` mínima maior; stabilizer do host (MochizukiStabilizerStrength) só nas bordas.
3. Vulkan / OpenGL / pré-SR / lmxxf não ligados (ver 2b).
4. danielblnc com foveado não testado nem no harness (só compila; a ordem Warp -> Record -> Unwarp na mesma lista
   segue o contrato do Record). O alinhamento 64 é o do mochizuki; o do danielblnc é desconhecido.
5. Regra extra do plano do mochizuki: se as duas extensões forem múltiplos de 256 a largura ganha +64 (ex. 1536x1024);
   o grid não evita esse caso (custo ~4% nesse tamanho).
6. Backbuffer sRGB vira FP16 na entrada da rede (muda a rota do runtime); flip-model D3D12 não usa sRGB, então raro.
7. Troca de grid = rebuild da rede (~1 s no 9070 XT, mais em RDNA2/3 sem cache): não deixar slider ao vivo no grid
   custom (hoje só o combo e o ini).
8. Saída do unwarp é FP16 mesmo com entrada UNORM (o Compose aceita; valores podem passar de 1 e são cortados ali).
