# OpenRR: cinco correções do teste do mega update (2026-11-03)

Teste do usuário no Cyberpunk (RX 9070 XT): OpenRR (perf-18, backend D3D12) + mochizuki 0.0.8.2 na imagem denoised.
OptiScaler: branch `mega-update` (wt-mega), 4 commits a partir de 5bb1cbf11. Runtime (MIT): branch `perf-18-nr`
(worktree `OpenNR-Denoiser-nrfix`, a partir de `perf-18` 3cc8d13), 3 commits. Nada instalado, nada enviado.

## 1. Instabilidade do NR no display

Vídeo (16-52-50, cena parada): mediana por pixel de |quadro − anterior| 0,55 (8 bits), contra 0,005 do off e 0,29
do loop nos vídeos de 30/10. O tremor fica nas bordas e detalhes finos.

Causa: o NR roda na imagem de render com jitter, então o delta que o display soma muda a cada fase do jitter. O
histórico do delta misturava a `max(0,3, a2)` (~3 quadros), enquanto o RR acumula a própria imagem a `a2` (poucos %).
Com 0,3 a variação de um quadro para o outro fica em ~0,42 do delta, e isso aparecia na tela.

Correção (runtime, `post_dlt_f.spv`): o delta mistura a `max(0,3 · m, a2)`, onde m vai de 0 parado a 1 com meio pixel
de movimento. Parado ele acumula no ritmo do display e tira a média do jitter. Em movimento continua igual a antes.

Medida no validador (D3D12, cena estática sintética, a imagem do salto editada com um realce que segue o jitter, no
lugar do mochizuki), variação quadro a quadro da saída (×1e-3): off 0,189, display antes 0,461, **display agora 0,210**.
A parte do NR passou de 0,382 para 0,094 (de 43% para 12% do tamanho dele). Pan: o NR na tela igual ao de antes.
Detalhes em `OpenNR-Denoiser-nrfix\docs\FASE5-NR.md`, seção 12.

## 2. Modo "In the loop" removido

`NrOnDenoised` aceita off e display. No menu virou uma checkbox "NR on denoised image". Um ini com `loop` lê como
display (Config.cpp). O runtime ainda tem o loop no ABI, mas o OptiScaler não pede mais.

## 3. Pasta Guides

Na seção OpenRR, as opções de guias (máscara de desoclusão: usar, inverter, limitar, escala e presets; guia SSS;
responsivity mask; colour before transparency; depth of field guide; reflection MVs; opacidade e MVs da camada
transparente) ficam num TreeNode "Guides", fechado. A primeira vista mostra NR, Backend e o estado.

## 4. Versão 0.1.0

`OpenNrRrFeatureDx12::Version()` devolvia `{ONR_RR_ABI_VERSION, 0, 0}` (ABI 1 → "1.0.0"). Agora é `{0, 1, 0}`, num lugar
só (o header da feature). Overlay e menu mostram "OpenRR 0.1.0".

## 5. Tempo do NR fora do upscaler

O tempo do upscaler são timestamps em volta do evaluate inteiro, e com o NR na imagem denoised as listas do mochizuki
rodam entre a rede e o post, dentro dele. Agora cada quadro tem timestamps D3D12 no começo da lista produtora e no
fim da consumidora (o trabalho do mochizuki fica entre elas). O valor é lido quando o slot volta (anel de 3), só se o
slot de fato rodou. A feature desconta esse tempo do "Upscaler Time", lista "NR (mochizuki)" nos tempos detalhados e
manda o valor para o `ReportNeuralMs`. Assim aparecem a linha "Neural pass: x ms" no menu NR, a linha NR do overlay e
o badge "NR ON". Não testado no jogo: conferir se o upscaler cai mais ou menos o que o neural pass mostra.

## Bit a bit sem NR

Só `post_dlt*.spv` mudou no runtime; os outros 48 SPIR-V do stage e o código da DLL estão iguais aos do perf-18 (os do
jogo conferidos por hash). Validador: off e display sem edição dão o mesmo hash por quadro do perf-18 (estático
`496dc7cfa5c9397c` no quadro 47, pan `7bc3b433095e58a1` no 23, PSNR do pan contra o NGX igual: 60,08–64,46 dB). O
`chain`/`seq` não rodou (os casos `gpu-cases` não existem mais), mas os dois só usam o caminho off, cujos kernels não
mudaram.

## Instalar (jogo fechado, com backup)

Pasta `E:\Games\Cyberpunk2077\bin\x64`:

1. `E:\Projetos\opti_rr_dlss5\wt-mega\exports\mega\OptiScaler.dll` → `dxgi.dll`
2. `E:\Projetos\opti_rr_dlss5\OpenNR-Denoiser-nrfix\out\perf-18-nr-stage\opennr-rr\shaders\post_dlt_f.spv` →
   `opennr-rr\shaders\post_dlt_f.spv` (sobrescrever; o `OpenNrRrRuntime.dll` do perf-18 continua)

Um ini com `NrOnDenoised=loop` passa a rodar como display. Não precisa mexer nele.

## O que olhar

- Display parado: as bordas estáveis como no off, e o NR com a mesma força.
- Display andando e girando a câmera: sem rastro novo do NR (lá a regra é a de antes).
- Overlay: "Upscaler Time" volta perto do OpenRR sozinho, e o tempo do NR aparece na linha NR.
- Menu OpenRR: checkbox do NR, pasta Guides fechada, versão 0.1.0.

## Merge com `openrr-vulkan`

Os commits são pequenos e separados. No OptiScaler, só a feature D3D12, o menu e o Config. No runtime, um shader
include, o validador e o doc.
