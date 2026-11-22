# Handoff: rede RDNA2 (RX 6000) no mega update (2026-11-03)

Branch `rdna2-mega` (worktree `wt-rdna2-mega`), a partir de `mega-update` 5bb1cbf11. Só commits locais: nada de push,
tag, upload ou post; nenhuma pasta de jogo, OptiScaler.ini, instalador ou pasta OpenNR-Denoiser tocada. Sala limpa
mantida (o port GPL de RDNA2, countervolts/d4r e `AMDSR\d4r`/`referencias\d4r` não foram abertos).

`external\`: junções para `wt-upstream-merge\external\*` e `FidelityFX-SDK-v2.4` -> `ffx-sdk-v2.4.0`, como no
`wt-mega` (git status limpo, o git não segue). `third_party\mochizuki\toolchain` -> o de `opti` (ignorado pelo git).

## Commits

1. `cdce6293e` merge de `rdna2-aco` (48dba6dd3, que contém `mochizuki-rdna2` inteiro e `rdna3-aco`).
2. `022f258af` host: primeiro start das redes FP16 sem a segunda compilação (patch local 32).
3. `fe5fcfefe` records ACO gfx10.3 do shaders-rdna2 rebaseado + checkbox ACO habilitado no RX 6000.
4. `4019ab7be` host: ACO do RDNA2/RDNA3 sem o bundle RDNA4 (`dlssnr-amd\aco`).
5. o commit do kit v6 (`tools/rx6000-kit`, `tools/rx7000-kit/mz_e2e.cpp`) + este handoff.

Identidade conferida de novo com a DLL final (`exports\verify\run3.out`): RDNA4 8 de 8, RDNA3 forçado 3 de 3, RDNA2
limpo 6 de 6.

## Merge e conflitos

- `nrvk.hpp`: `Context` fica com `nr::Arch arch` + `fp16()`; as funções novas do upstream (`target_name`,
  `target_float8`, `target_matrix_ab`, `has_matrix_config`) ficam e leem o arch (RDNA2: sem matrizes, sem config de
  matriz, `cooperativeMatrix` não exigido); `create()` (ferramentas standalone) põe o arch a partir do `nr_gpu_arch`.
- `nr_graph.cpp`: `tchain_active()` nunca no RDNA2; o argumento `ctx.rdna3` do upstream saiu.
- `nr_pal_binary.hpp`: o adaptador do lado RDNA2 (gfx12/gfx11/gfx10.3, `same_generation`, `synthesize`) + o caminho UTF-8
  do upstream; records NRPAL001 checados como o upstream checa (LDS >= uso, bit de scratch na máscara), pois os records
  RDNA4 do 0.0.8.2 são NRPAL001.
- `nr_pipeline_binary.hpp`: `create_aco` = corpo do upstream + o `synthesize` gfx10.3; bundles `aco-gfx11`/`aco-gfx103x`
  caem para o driver por pipeline (`create_fallback`); o bundle RDNA4 mantém o fallback do upstream (Error: ACO off
  para o processo).
- `UPSTREAM.md`: patches 25-27 do rdna2-aco viraram 28-30 (25-27 são do sync 0.0.8.2), o host RDNA2 é o 31.
- O que o sync avisava: ruído no binding 5 (o pre block RDNA2 lê do blob de pesos, binding 5 recebe o stand-in de 16
  bytes, funcionou sem mudança), arena/slots mortos (mesmo grafo do RDNA3), `runtime_blit` na lista RUNTIME (entrou no
  `shaders-rdna2\runtime`). Nenhuma mudança de shader RDNA2 foi necessária.

## Identidade RDNA4 / RDNA3 (RX 9070 XT, mz_e2e, contra o runtime do sync 0.0.8.2 = mega-update)

- SPIR-V: `shaders` 113, `shaders-rdna3` 86, `aco` 362, `int4` 381 arquivos byte-idênticos. `shaders-rdna2`: arquivos de
  rede idênticos ao rdna2-aco; `runtime\runtime_alpha` mudou e `runtime_blit` entrou (0.0.8.2, igual ao RDNA3).
- RDNA4 8 de 8 idênticos (small/310.8, 720p/1080p, driver e ACO): 258FEC01 / 01588CA6 / 2A308F2C / 04A421E7 (driver) e
  C4481BEC / E06D9063 / 0A1087B2 / 57964410 (ACO), antes e depois do patch 32.
- RDNA3 forçado idêntico: 4AABFA89 (1152x648), ED2C651E (720p), 23859411 (1080p), antes e depois do patch 32.

## RDNA2 forçado no 9070 XT

- Com histórico 32 bits (`NR_HIST16=0`): saída idêntica ao export do rdna2-aco (7A6DCFA0 / FD74ABAF small 648p/720p,
  B83A1C6E / 37A0B7D9 310.8; 20 quadros).
- Com o padrão novo (MochizukiHistoryPrecision 16, histórico RGBA16F como no RDNA4/RDNA3): 55,0-55,9 dB do anterior.
- Modo limpo (1 quadro, sem histórico): hashes do kit inalterados (E9F5E89B / D07096C4 / EB2040C4 small 720p/1080p/1440p,
  D0A1B50E / A5A0A394 / 3AA32BB2 310.8).
- Referências FP8 do kit v6: as 6 antigas reproduzidas byte a byte; novas 310.8 648p/1080p/1440p. RDNA2 forçado contra
  elas: small 49,46 / 47,79 / 52,92 / 52,99 dB, 310.8 46,32 / 49,72 / 50,28 / 51,95 dB (648p/720p/1080p/1440p).
- Tempo de rede (9070 XT forçado, não é o alvo): small 648p 25,5 ms, 720p 30,1; 310.8 648p 44,3, 720p 54,0.
- Checagem de pipelines do RDNA2 (child rundll32): 96 de 96 compilam (73 shaders-rdna2 + RenoDX).

## ACO RDNA2

- `produce-rdna2.ps1` com esta build (24 corridas de layout, 41 layouts): 33 records navi21 de 49 kernels, 0 scratch.
  Ficam com o driver 13 que o grafo nunca cria e `temporal_pre_fp32nh`, `temporal_post_fp32h16` + `_grade` (imagem
  amostrada antes de outro binding; com o histórico 16 bits a rede usa o `h16`, que tem o mesmo problema do `fp32`).
- Commitados em `third_party/mochizuki/windows/data/aco/gfx103x/` (como os gfx11): `records\`, `layouts.txt`,
  `produce-log.txt`, `summary.txt`. O bundle: `dlssnr-amd\aco-gfx103x\shaders` = shaders-rdna2 desta build +
  `records\`; pronto em `exports\aco103\aco-gfx103x` (não está no export do produto).
- `test-pal.cmd` (RDNA4, int4, gfx11, gfx11-coop e estes): 275 records, todo prólogo gfx12 = rdna3-k6, todos = dd989104c,
  synthesize = o synth da RX 6600, 0 falhas.
- 9070 XT forçado com ACO pedido + bundle: "machine 0x4e", 31 pipelines por build caem para o driver (28 "different
  generations", 3 sem record), saída idêntica à do driver nos 4 casos. Importação gfx10.3 de verdade só numa RX 6000
  (kit v6).
- Achado no kit: o `ConfigureAco` exigia `dlssnr-amd\aco` (RDNA4) para ligar o ACO; sem ele o RDNA2 nunca chegava ao
  `aco-gfx103x`. Agora, sem `aco\`, ele pega `aco-gfx103x` ou `aco-gfx11` e a rede certa usa (Rdna2Aco/Rdna3Aco); um
  device RDNA4 com só esses bundles desliga o ACO (`the FP8 network has no ACO records`). Testado: RDNA2 forçado com só o
  `aco-gfx103x` = saída do driver (31 fallbacks), RDNA4 com só o `aco-gfx103x` = saída do driver.
- Menu: checkbox "ACO machine code" habilitado na rede RX 6000 (texto: usa `aco-gfx103x` quando instalado,
  experimental); RX 7000 continua desabilitado. OptiScaler compila (`build-optiscaler-fast.cmd`).

## Host: não compilar duas vezes (patch 32)

Descoberta: o driver da AMD guarda o que compila para o executável que compilou. Testei passar o VkPipelineCache do
child (rundll32) para o jogo, com device igual ao da rede (nome da aplicação, features, extensões): 0 acertos; até um
`pipeline-rdna2.cache` escrito por um mz_e2e e lido por uma cópia renomeada do mesmo exe recompila tudo. O que
atravessa processos é o código de máquina via VK_KHR_pipeline_binary (é assim que o aco-cache funciona).

O que ficou: com uma lista de prewarm da rede (`prewarm\manifest-rdna2.txt` / `manifest-rdna3.txt`), o child da
checagem compila cada arquivo listado com os create infos do core e guarda o binário de cada pipeline
(`work\binaries\`, arquivos NRAC + `identity.txt`); depois da checagem passar, `AdoptBinaries` move para
`dlssnr-amd\binaries-rdna2\` (ou `-rdna3`); o device das redes FP16 liga VK_KHR_maintenance5 + VK_KHR_pipeline_binary;
o Capture do build importa cada pipeline do core que tem binário (o resto, e qualquer importação que falhe, compila como
antes) e o prewarm pula esses. Sem lista: como antes (checagem + build serial). `MZ_CHECK_CACHE=0`: caminho antigo.
Mesmo código de máquina, mesma saída.

Primeiro start a frio (shaders com palavra geradora nova, small 720p, 9070 XT):

| rede | antes | só a lista (prewarm paralelo) | agora (binários da checagem) |
|---|---|---|---|
| RDNA2 forçado | 62,3 s (checagem 17,5 + 36 pipelines em série 40,9) | 27,4 s | 22,7 s (checagem 18,2, os 36 importados) |
| RDNA3 forçado | 52,6 s (checagem 16,5 + série 32,4) | 26,4 s | 26,6 s (checagem 20,5; o 2º start 1,4 s) |
| RDNA4 | 7,5 s (lista dele, caminho não mudou) | | |

No RDNA3 a checagem ficou ~4 s mais lenta (create infos exatos + captura) e o primeiro build ainda gasta ~4 s criando
shader modules a frio, então empata com "só a lista". Na RX 6600 (check 29,6 s em 6 threads + 58,9 s em série no kit
v4) a expectativa é ~35-40 s; o kit v6 mede.

## Kit v6 (RX 6600)

`E:\Projetos\opti_rr_dlss5\rx6000-test-kit\send\OpenNR-RX6000-test-v6.zip` (32,7 MB, 247 arquivos, sem modelos),
SHA-256 `A523BBD1F8D109DF3A366D1556FB49257170C9E68CCC9E1D5F672742D8FE27A9` (`.sha256.txt` ao lado; cópia em
`rx6000-test-kit\OpenNR-RX6000-test.zip` + `SHA256.txt`). O testador descompacta, roda `run-test.bat`, espera 25-40 min
e manda `OpenNR-RX6000-result-<data>.zip`. Passos: sistema, modelos, pipecheck,
primeiro start a frio "now" (como sai) e "before" (sem lista, `MZ_CHECK_CACHE=0`, cópia com palavra geradora própria),
qualidade driver (small + 310.8 em 1152x648/720p/1080p/1440p, 3 corridas, PASS >= 40 dB e sem NaN), primeiro start ACO
(templates com palavra própria), qualidade ACO (mesmos 8 casos, também contra o driver), velocidade (8 casos, 3
rodadas, driver e ACO alternando, 30 quadros, clock da GPU), NR_STEP_TIMES de small 720p e 310.8 720p com os dois.
Antes da parte ACO grava `SUMMARY-partial.txt`; se travar, o testador zipa `result`. Os conjuntos têm palavras geradoras
próprias (0x7761 o que sai, 0x7762 o "before", 0x7763 os templates do ACO) para nenhum cache de driver ter visto.

Rodada no 9070 XT (forçado, `exports\rx6000-kit\run-v6-full1`, 22 min, GPU sob o lock): sistema OK, 96 de 96 no
pipecheck, primeiro start **now 24,0 s** (checagem 18,7 s, 36 de 36 pipelines do core importados) contra **before 73,1 s**
(checagem 17,5 s + 36 em série 50,8 s), imagens iguais; qualidade driver 8 de 8 PASS (49,46 / 47,79 / 52,92 / 52,99 dB
small, 46,32 / 49,72 / 50,28 / 51,95 dB 310.8; corridas idênticas; hashes = referências do RDNA2 forçado); velocidade
small 22,6 / 26,4 / 40,1 / 61,7 ms, 310.8 38,4 / 49,7 / 74,8 / 118,8 ms (648p/720p/1080p/1440p). Nessa rodada o ACO
foi pulado: o harness do kit não passava `E2E_CREATE_FLAGS` (corrigido) e o host não ligava ACO sem `aco\` (corrigido,
acima). Rodada rápida depois (`KIT_QUICK=1`, `run-v6-quick2.out`): ACO ligado, 31 fallbacks (gfx12), imagens idênticas
ao driver, velocidade igual (é o mesmo código do driver aqui).

Como ler o resultado da RX 6600: `FIRST START now/before/aco` (o "now" deve ficar bem abaixo do "before"; as linhas
`now:` dizem quantos pipelines vieram da checagem), `PASS/FAIL driver|aco <caso>` (>= 40 dB, corridas idênticas; o ACO
também "against the driver"), `INFO ACO pipelines: N imported` (esperado 33 menos os não usados no caso; os 3 temporais
ficam com o driver), `SPEED <caso>: driver ... ACO ...; ACO x% against the driver` e os `STEPS driver/aco` (por kernel,
para escolher quais records mandar). Decisão: ACO no payload se todos PASS e for mais rápido ou abrir mais rápido; se
misto, mandar só os records dos kernels que ganham (o resto cai para o driver sozinho).

## Para o instalador (outra branch; não mexi)

- Detecção: igual hoje (rede 3 = RX 6000, por device id Navi 21/22/23 0x73A0-0x73FF, Navi 24 0x7420-0x743F, APUs
  0x163F/0x1681/0x164E/0x1506; o runtime decide sozinho).
- Payload da rede RX 6000: `dlssnr-amd\shaders-rdna2\` (73 arquivos desta build) e agora também
  `dlssnr-amd\prewarm\manifest-rdna2.txt` (43 pipelines) e `manifest-rdna3.txt` (41) ao lado do `manifest.txt` (38);
  as três listas estão em `exports\rdna2-mega\dlssnr-amd\prewarm\`. Sem a lista o jogo funciona, só volta a compilar duas
  vezes.
- ACO RX 6000: componente separado `aco-gfx103x` (`dlssnr-amd\aco-gfx103x\shaders` = cópia do shaders-rdna2 + `records\`
  33 .nrp), só depois do kit v6 dizer que ACO é bom/mais rápido. ACO vem ligado por padrão (`MochizukiAco=true`), então
  com o componente instalado a RX 6000 usa ACO; sem ele, driver. Não depende do `dlssnr-amd\aco` (o bundle RDNA4)
  estar instalado. Os records valem só para o SPIR-V exato: qualquer
  rebuild do shaders-rdna2 pede `produce-rdna2.ps1` de novo.
- Limpeza (desinstalar/atualizar): `dlssnr-amd\binaries-rdna2\`, `binaries-rdna3\` (novos, gerados no primeiro start),
  além dos já conhecidos `rdna2-check*`, `rdna3-check*`, `pipeline-rdna2.cache`, `pipeline-rdna3.cache`, `aco-cache\`.
- Texto do bloco: "RX 6000 (experimental): rede FP16 do mochizuki com as matrizes emuladas em ALU. O primeiro start
  compila os shaders uma vez (dezenas de segundos numa RX 6600); depois abre em segundos. ACO (opcional, experimental):
  código de máquina pronto para gfx10.3." O menu já mostra "RX 6000 (experimental, FP16 network)".

## Arquivos fora do git (`exports\`)

- `exports\rdna2-mega\` export do runtime (DLL + `dlssnr-amd` com shaders, shaders-rdna3, shaders-rdna2, aco, int4,
  prewarm com as 3 listas); `exports\aco103\` saída do produce-rdna2 (bundle, layout-run).
- `exports\verify\`: `tests.ps1` (r4/r3/r2/r2c), `manifests.ps1`, `cold.ps1` (+ `cold\`, `cold*.out`), `acofb.ps1`,
  `xproc.ps1` (o teste que mostrou que o cache não atravessa executáveis), pastas `dmega`/`dnew`/`dprev`/`dman`.
- `exports\rx6000-kit\`: `refwork`, `reference` (8 casos), `tools`, `kit`, `run-v6.ps1` + `run-v6.out`, `refs.ps1`.
- `exports\build.cmd`, `exports\build-dll.cmd` (só a DLL), `exports\testpal\`.

## Riscos / aberto

- Nada rodou numa RX 6000 com este merge; ACO gfx10.3 de rede inteira nunca rodou em hardware (só os probes).
- O caminho de binários depende de VK_KHR_pipeline_binary no driver da RX 6600 (o probe 2 exportou binários no 26.8.1,
  então deve ter); se faltar, o child não guarda nada e o jogo cai na lista + prewarm (ainda melhor que antes).
- A lista de prewarm RDNA2 foi gravada no 9070 XT forçado; se o RX 6000 criar algum pipeline com create info diferente
  (ex.: sem robustness), esse compila no build como antes.
