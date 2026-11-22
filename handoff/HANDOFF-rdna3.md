# Handoff RDNA3 (RX 7000) - 2026-10-19, passos 1 a 4

Nada commitado. Worktree `wt-rdna3`, branch `mochizuki-rdna3`. O fork está clonado (só leitura) em
`E:\Projetos\opti_rr_dlss5\DLSSNR-RDNA3`, checkout `aa392284`; foi adicionado ali um remote `upstream`
(mochizuki0323/DLSSNR-AMD) só para achar a base do fork: `939b607` (0.0.6-linux-preview).

## 1. O que foi vendorizado (`third_party/mochizuki`)

- `linux/shaders/rdna3/` inteiro, menos `pipelines-int4.json`; `linux/build/arch/rdna3.sh`; `LICENSE-rdna3` (o
  arquivo do fork; o aviso MIT é o do upstream, o fork não acrescentou o seu).
- `build_network.py` do Linux não precisou mudar (já é genérico por arch; a lista RUNTIME do fork só acrescenta os
  passes de temporal reuse, que ficaram de fora).
- Host: o diff do fork em `linux/src/core` (commits 7c6361d, 767911f, eecb894, 612b0ba) portado para
  `windows/src/core`: twins FP16 das arenas de ativação e de pesos, bindings alias 16+k, `kernel_set` com twins,
  pesos `kswap`, tiles FFN `plain_tiles`, config FP16 x FP16 -> FP32 em vez de FP8, zero-fill do twin.
- Deixado de fora: int4 do fork (pipelines e reescrita iu4 via pipeline binaries do RADV), temporal reuse
  (`runtime_reuse_*`, `reuse_every`), o ganho de edit abaixo de 100% em `runtime_transfer.comp` (32f3b1c; muda a
  imagem do RDNA4), `--value-stats`, `linux/test`, `linux/src/pe`, empacotamento, docs.
- Tudo documentado em `UPSTREAM.md`, seção "RDNA3 (RX 7000, no FP8)", patches locais 12 a 14.

## 2. Host: uma DLL, escolha em runtime

`rdna3.sh` = defines do `rdna4.sh` + `NR_ARCH_RDNA3=1`. Esse único define virou runtime: `RuntimeConfig::rdna3` ->
`nrvk::Context::rdna3` (antes do `adopt`, e na sessão do arena probe). O core continua compilado uma vez com os
defines do `rdna4.sh`. Com `rdna3`:
- shaders em `dlssnr-amd\shaders-rdna3\` (rede + `runtime\` + `temporal\`), cache `pipeline-rdna3.cache`, prewarm
  `prewarm\manifest-rdna3.txt` (`mzi::Capture` ganhou um `variant`);
- o grafo usa o kernel set da rede Linux, igual ao caminho ACO (`linux_network()` em `nr_graph.cpp`: tile counters
  no ViT/decoder/encoder, `gemmprojc`, tick no `fswin32`, pesos N-pair da atenção). A rede RDNA3 tem exatamente os
  mesmos 52 pipelines e os mesmos kernels NR_TCHAIN da rede Linux RDNA4;
- `gemm*` e `vitattn` empurram o push block sem o endereço da arena (`aco_push_gap`, como o ACO int4), porque o
  layout Linux não tem esse campo;
- os twins são destruídos junto com as arenas (no fork eles vazavam a cada rebuild);
- `kswap` em `shader-constants.txt` tem que bater com o host (pasta errada é recusada com mensagem).

`MochizukiNrRuntime.cpp` (`Vulkan::Create`):
- dispositivo AMD (identidade pelo driver da AMD, passando pelo spoofing) sem VK_EXT_shader_float8 ou sem as features
  FP8, e com a config FP16 16x16x16 subgroup -> rede RDNA3; float8 não é exigido nem habilitado (fora da cadeia e da
  lista de extensões);
- device 0x75xx (RX 9000) sem FP8 continua recusado como antes (senão um driver velho no RDNA4 cairia na rede RDNA3
  com imagem errada);
- com dois ICDs no mesmo adapter, FP8 tem preferência;
- `MZ_FORCE_RDNA3=1` força a rede RDNA3 em qualquer placa;
- ACO desligado (`nr::binary::disable` antes do `prepare`), int4 forçado a 0;
- log: `[mochizuki] the RDNA3 network, on FP16 matrices: <motivo>` ou `[mochizuki] the FP8 network (VK_EXT_shader_float8)`;
- estimativa de VRAM soma os twins (336 MB + 384 B por pixel do modelo; 1080p medido 1877 MB contra 799 da FP8).

## Arquivos alterados

- `third_party/mochizuki/windows/src/core/nrvk.hpp`, `nr_graph.cpp`, `nr_runtime.cpp`, `nr_runtime.hpp`,
  `nr_shader_manifest.hpp`
- `third_party/mochizuki/windows/build/build_network.py` (`--tree linux`), `unroll_glsl.py` (remove `[[dont_unroll]]`)
- `third_party/mochizuki/linux/shaders/rdna3/pipelines.json` (`NR_EDGE_BODIES=0` nos 7 persistentes)
- `third_party/mochizuki/UPSTREAM.md`, `LICENSE-rdna3`, `linux/build/arch/rdna3.sh`, `linux/shaders/rdna3/`
- `OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp`, `mz_interpose.cpp`, `mz_interpose.h`
- `tools/build-mochizuki-runtime.cmd`

## 3. Build

`exports\rdna3-work\build.cmd` (vcvars 14.44 Community + SDK 1.4.357) -> `exports\mochizuki-rdna3`: BUILD_OK, sem
warnings novos. `shaders-rdna3`: 52 network, 8 temporal, 15 runtime.

O build puro do fork (`linux/build/build_network.py rdna3`, mesmo script, glslang 16.5.0 e defines; por leitura dá a
mesma SPIR-V do fork) **derruba o LLPC** (driver 26.9.2): segfault dentro de `vkCreateComputePipelines` em attn,
ffwd3/ffwd3w, quase todos os fswin e temporal_post. A rede Linux RDNA4 (a do ACO) cai do mesmo jeito; não é coisa do
gfx11. Por isso o rdna3 é compilado pelo script do Windows: `build_network.py rdna3 --tree linux --no-quad` (glslang -E +
`unroll_glsl.py`). Mais duas coisas foram necessárias: o unroll ignora `[[dont_unroll]]` (`NR_EXPAND_HLOOP` nos C=32,
`NR_EXPAND_KLOOP` nos persistentes; o LLPC também cai nesses loops enrolados) e `NR_EDGE_BODIES=0` (sem isso o
`fswinp256` cai). O `.length()` = 8 do unroller está certo aqui (só acumuladores pedem `.length()` na rede RDNA3).
Sem quad quant (é escrito para os loops de par do FP8). Conclusão: a SPIR-V que vai para o Windows **não** é a do fork,
é a mesma fonte desenrolada; a aritmética é a mesma.

Ferramenta: `exports\rdna3-work\pipecheck\` (compila cada .spv num processo, layout lido do módulo).
`crash1.txt` = build puro, `crash3.txt` = build final (60/60 VkResult 0).

## Identidade RDNA4

- SPIR-V: `dlssnr-amd\shaders` (rede, runtime, temporal) idêntica byte a byte a `exports\rdna3-work\base` (diff -r).
- Saída: DLL do HEAD (`exports\rdna3-work\ref-rdna4`, compilado de `git archive HEAD`) contra a DLL nova, harness,
  RGBA16F, 20/12 frames: **idêntica byte a byte** em 1080p (último frame e o anterior), 720p e 4K. Tempos iguais
  (1080p 7.5 ms, 720p 4.2, 4K 28.2-28.6; modelo original, sem dlssnr-small.bin).

## 4. Smoke test com `MZ_FORCE_RDNA3=1` (RX 9070 XT, AMD Software 26.9.2)

- Todos os pipelines compilam; rede montada; `fp16 twins ... activation 784.5 MB, weights 344.1 MB` (1080p),
  `k-pair swap (kswap): odd rows of 69 Swin weight matrices`.
- Build frio a 1080p: 16.4 s ("the core made 48 pipelines (32 distinct) in 13.60 s"); depois 1.6-2.0 s; 4K 5.1 s.
- Frames rodam sem erro nem travamento: 720p 3.46 ms, 1080p 6.20 ms, 4K 21.06 ms (tempo de rede do runtime; sem
  valor como medida, a imagem está errada).
- Imagem errada, como esperado (layout gfx12 != gfx11): sem NaN/Inf, média 0.242 contra 0.184, correlação 0.53, padrão
  de grade e tom azulado (`reference\forced-rdna3-on-rdna4_1080.png`).
- Único erro visto foi meu e já corrigido: o arena probe não herdava `rdna3` ("arena reuse: the layout probe did not
  finish").
- Referência RDNA4 para o kit: `exports\rdna3-work\reference\rdna4_{720,1080,2160}_rgba16f.bin` + `rdna4_1080.png`
  (harness, `original_sem_nr.png`, modelo 310.8, 1 passe, sem pan). SHA-256 do 1080p: `8f4b2e85...7a1207c`.

## Riscos para os testers RX 7000

1. **Layout de fragmento no LLPC.** Os kernels supõem o que o fork mediu no RADV/gfx1100: componente `c` do lane `l`
   do acumulador = elemento `(2c + l/16, l%16)`, fragmento A/B com 16 componentes por lane, e o WMMA calcula a linha
   `m` só com a metade `m%2` dos lanes (base do `kswap`). O comportamento do WMMA é do hardware, mas a ordem dos
   elementos que o LLPC dá a cada componente de A/B/acumulador pode não ser a do RADV. Se for diferente, roda e a
   imagem sai errada (como no RDNA4 forçado). Nada disso foi testado em gfx11.
2. **Compilador.** O LLPC no gfx11 nunca viu esse código. Pode cair em outro pipeline, ou demorar muito (o código
   desenrolado é grande). O BuildMarker liga o log por pipeline na tentativa seguinte, e o `pipecheck` acha o culpado.
3. **Driver.** Precisa de VK_KHR_cooperative_matrix com a config FP16 16x16x16 subgroup, e de
   VK_KHR_workgroup_memory_explicit_layout (8/16 bit). Sem isso: `[unsupported]` com o motivo.
4. **Desempenho.** O fork afinou para RADV/ACO (15 ms a 1080p numa 7900 XTX). No LLPC, desenrolado e sem quad quant,
   pode ser bem pior. Registradores e spills no gfx11 desconhecidos.
5. **VRAM**: os twins dobram a rede (1080p ~1.9 GB, 4K ~6 GB medido no 9070 XT). Placas de 8 GB a 4K são o limite.
6. `shaderInt16` não é habilitado (53 shaders RDNA3 declaram Int16; os RDNA4 já declaravam em 3, sem problema no
   driver da AMD). Só aparece com validation layers.
7. A regra 0x75xx = RDNA4 é heurística. iGPUs RDNA3/3.5 (780M, 890M) também cairiam na rede RDNA3, sem teste.
8. Sem ACO e sem int4 na RX 7000.

## Passo 5 (kit do tester) vai precisar

- Uma sonda de layout do LLPC no gfx11 (como o `wmma_half_probe` e a sonda de acumulador do fork, em Vulkan puro):
  responde o risco 1 em segundos, antes de qualquer imagem. É a coisa mais útil do kit.
- `pipecheck.exe` + lista de .spv (acha crash do compilador por pipeline).
- O harness (`mz_e2e.exe` + `run.cmd`), `MochizukiNrRuntime.dll`, `dlssnr-amd\shaders-rdna3` (e `shaders\renodx`), a
  imagem de teste e as referências RDNA4; um `cmp.py`/PSNR contra `rdna4_1080_rgba16f.bin`. O modelo NÃO vai no kit:
  o tester gera `dlssnr.bin` do próprio `nvngx_dlssnr.dll` (install.ps1 / model-tools).
- Instruções para mandar de volta: `mochizuki_nr.log`, saída do pipecheck, o .bin/PNG, nome da placa e versão do driver.
- Depois: installer/payload precisam levar `shaders-rdna3` (o OptiScaler não muda; a DLL decide sozinha).

## Artefatos (exports, fora do git)

- `exports\mochizuki-rdna3\` DLL + `dlssnr-amd` (com `dlssnr.bin` copiado: não publicar).
- `exports\rdna3-work\`: `base` (SPIR-V RDNA4 de antes), `spv\` (builds de teste), `harness\`, `pipecheck\`,
  `reference\`, `ref-rdna4\` + `headsrc\` + `build-head.cmd` (DLL do HEAD para o A/B), `build.cmd`, `unroll\exp.py`.
