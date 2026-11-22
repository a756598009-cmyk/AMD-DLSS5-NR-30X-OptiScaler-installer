# Kit de teste RX 7000 (RDNA3) - 2026-10-19

Para uma voluntária (Natty, RX 7000, gfx11) validar o port Windows da rede RDNA3 do mochizuki. Nada commitado,
nada enviado.

- Kit: `wt-rdna3\exports\rx7000-kit\OpenNR-RX7000-test\` (`run-test.bat`, `README.txt`, `files\`)
- Zip para mandar: `wt-rdna3\exports\rx7000-kit\OpenNR-RX7000-test.zip`, **21.8 MB** (123 arquivos)
- Fontes: `wt-rdna3\tools\rx7000-kit\` (no git; os scripts gravam em `exports\rx7000-kit\`): `vkprobe.cpp`, `pipecheck.cpp`, `mz_e2e.cpp` (harness),
  `compare.cpp`, `child.hpp`, `probe\*.comp`, `kit.ps1`, `run-test.bat`, `README.txt`, `build-kit.cmd`,
  `make-refs.cmd`, `pack-kit.ps1`
- Refazer: `tools\rx7000-kit\build-kit.cmd` (ferramentas, DLL de `exports\mochizuki-rdna3`, shaders, imagem),
  `tools\rx7000-kit\make-refs.cmd` (referências; usa `refwork\dlssnr-amd\dlssnr.bin` e `dlssnr-small.bin`),
  `powershell -File tools\rx7000-kit\pack-kit.ps1` (apaga modelos, caches, logs e result do kit e zipa).

## O que vai no kit

`files\`: `MochizukiNrRuntime.dll` (a do worktree, a mesma do smoke test), `dlssnr-amd\shaders-rdna3\` (rede 52,
`runtime\` 15, `temporal\` 8 + txt), `dlssnr-amd\shaders\renodx\` (20; o harness roda no estágio RenoDX, que é o
padrão dos controles), as 4 ferramentas, `probe\*.spv` (6), `kit.ps1`, `test.png` (`arrumar_cor\original_sem_nr.png`,
2560x1440) e `reference\*.jxr`. Sem modelos, sem `.lib/.exp/obj`, sem caches, sem `aco\`/`int4\`, sem a rede FP8.
O core só abre `shaders-rdna3\` (rede, `runtime\`, `temporal\`) e `shaders\renodx` no caminho RDNA3; o ACO é
desligado pelo host e pelo core ("the RDNA3 network has no ACO records"), o harness não pede ACO nem int4.

A DLL carrega `vulkan-1.dll` do sistema; `vkprobe` e `pipecheck` também (delay-load, dizem "no Vulkan loader" em
vez de não abrir). Tudo `/MT`, sem redistribuível. Não precisa de admin, Python, VS nem jogo.

## O que o `run-test.bat` faz

O bat limpa `PSModulePath` (um PowerShell 7 no caminho quebra os módulos do 5.1: aconteceu aqui com
`Get-FileHash`) e chama `files\kit.ps1` com `-ExecutionPolicy Bypass`. Tudo vai para `result\`, que no fim vira
`OpenNR-RX7000-result-<aaaa-mm-dd-hhmm>.zip` ao lado do bat (zip .NET, nomes com `/`), e o Explorer abre com ele
selecionado. Cada ferramenta roda com limite de tempo; um crash ou timeout vira uma linha e o kit segue.

1. **vkprobe** (`system.txt`): Windows (ProductName, DisplayVersion, build.UBR), `HKLM\SOFTWARE\AMD\CN`
   DriverVersion/CNVersion, CPU/RAM; cada adapter DXGI com vendor/device/subsys/rev, VRAM e versão do UMD
   (`CheckInterfaceSupport`); cada device Vulkan: nome, ids, API, driverVersion, driverID/Name/Info, conformance,
   subgroup size e faixa do size control, required-size stages, shared memory, push constants, shaderFloat16,
   shaderInt8, shaderInt16, storageBuffer8/16BitAccess, bufferDeviceAddress, vulkanMemoryModel, subgroupSizeControl,
   computeFullSubgroups; VK_KHR_cooperative_matrix (features, stages), VK_EXT_shader_float8 (shaderFloat8,
   shaderFloat8CooperativeMatrix), VK_KHR_pipeline_binary (pipelineBinaries + as 5 propriedades internalCache...),
   VK_KHR_maintenance5, VK_KHR_workgroup_memory_explicit_layout (+ scalar/8/16 bit), VK_EXT_pipeline_robustness;
   toda `VkCooperativeMatrixPropertiesKHR` (M N K, tipos A/B/C/Result, scope, saturating); heaps; e
   "runtime would pick" com a mesma regra do `Vulkan::Create` (AMD com FP8 -> FP8; 0x75xx sem FP8 -> recusado;
   senão a config FP16 16x16x16 FP32 subgroup -> RDNA3).
2. **Sondas de layout** (mesmo `system.txt`, linhas `CHECK`), cada uma num processo filho com 60 s, wave32 por
   `requiredSubgroupSize = 32`, device com coopmat + FP16 + 16-bit storage + memory model. Escritas do zero, com a
   lógica das sondas do fork (`linux/test/wmma_half_probe.*`, o mapa do `coopmm.glsl`):
   - `acc-f32` / `acc-f16`: acumulador 16x16 carregado de uma matriz `16r + c + 1`; cada lane grava seus
     componentes. Espera `length() == 8` e componente `c` do lane `l` = `(2c + l/16, l%16)`. Também reconhece o
     mapa gfx12 `(8(l/16) + c, l%16)`; imprime o mapa visto nos lanes 0, 1, 2, 15, 16, 17, 31.
   - `frag-a` / `frag-b`: A e B f16, espera 16 componentes por lane, A = `(l%16, c)`, B = `(c, l%16)`, iguais nas
     duas metades (reconhece também o gfx12).
   - `wmma-lane-halves`: a sonda de metades do fork (marca uma metade de A ou B, multiplica pela identidade): a
     linha `m` sai só da metade `m%2`.
   - `kpair-swap`: 400 tiles de A e B com valores e4m3 aleatórios (102400 por operando); o produto com cada par de
     k trocado nas duas metades superiores tem de ser idêntico bit a bit ao normal, e o normal bate com a CPU
     (erro relativo 1e-5).
   Sem suporte (sem coopmat, sem wave32, sem a config) dá SKIPPED com o motivo; crash do driver dá CRASH com o código.
3. **pipecheck** (`pipecheck.txt`): os 95 `.spv` de `shaders-rdna3`, `shaders-rdna3\runtime`,
   `shaders-rdna3\temporal` e `shaders\renodx`, cada um em processo próprio (`pipecheck --one`), 300 s cada, como o
   runtime cria: wave32, layout lido do módulo, push 256, robust access desligado se houver
   VK_EXT_pipeline_robustness, sem pipeline cache. OK + ms / FAIL VkResult / CRASH código / TIMEOUT.
4. **Modelos**: `dlssnr.bin` e `dlssnr-small.bin` dos mesmos links do payload (`mochizuki-model` e
   `mochizuki-model-small`, Hugging Face `zmodelerlover/amd-nr`), tamanho e SHA-256 do payload, `Invoke-WebRequest`
   duas vezes e BITS na terceira; arquivo já verificado é mantido. Sem eles os passos 1-3 continuam e as rodadas
   ficam SKIPPED.
5. **Rede**: `mz_e2e.exe` (o harness de `rdna3-work`, recompilado contra os headers do worktree; mudanças: só o
   último frame é gravado, todos os tempos por frame impressos, stdout sem buffer, espera até 15 min por um build
   frio e para quando o runtime diz "build failed"). 6 rodadas, cada uma num processo: modelo novo (310.9 small,
   o padrão) e antigo (310.8, `LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL`) x 1280x720, 1920x1080, 2560x1440; 40 frames,
   1 passe, sem pan, RGBA16F, estágio RenoDX (padrão), ACO e int4 desligados. `MZ_FORCE_RDNA3` é apagado do
   ambiente: o runtime escolhe sozinho. Os caches de pipeline e o prewarm são apagados antes da primeira rodada (a
   primeira mede o build frio). Por rodada: `harness.txt` (inclui o stderr do core), `mochizuki_nr.log`,
   `compare.txt`, `diff_x16.png`, e em 1080p `output.jpg`. Se o runtime escolher a rede FP8 (placa que não é RX
   7000), as rodadas ficam SKIPPED, porque o kit não leva a rede FP8.
6. **compare.exe**: PSNR (pico 1.0), mean|d|, max|d|, correlação, % de valores idênticos, médias por canal,
   NaN/Inf, sobre RGB. PASS com PSNR >= 40 dB e nenhum NaN/Inf.
7. `result\SUMMARY.txt`: cabeçalho, uma linha por verificação (INFO / PASS / FAIL / SKIPPED + números), lista dos
   arquivos crus.

Interruptores só nossos: `KIT_FORCE_RDNA3=1` (vira `MZ_FORCE_RDNA3=1` nas rodadas) e `KIT_QUIET=1` (não abre o
Explorer).

## Referências

Feitas aqui (RX 9070 XT, driver 32.0.32015.2008 / AMD Software 26.9.2 LLPC, Windows 25H2 26200.9711) por
`tools\rx7000-kit\make-refs.cmd`: a mesma `MochizukiNrRuntime.dll` do kit e o mesmo `mz_e2e.exe`, mas com
`dlssnr-amd\shaders` (rede FP8 do worktree, idêntica byte a byte à de antes do port segundo o HANDOFF), em
`exports\rx7000-kit\refwork\`; mesma imagem, tamanhos, modelos, 40 frames, 1 passe, sem pan, RenoDX padrão, ACO
desligado (o harness não passa `LMXXF_NR_CREATE_FLAG_ACO`), sem `MZ_FORCE_RDNA3`. Log: "the FP8 network".

Guardadas como **JPEG XR sem perda dos bits half (RGB, alfa fora)** via WIC: o PNG de 48 bpp com os mesmos bits dava
8.9 MB por 1080p, o JXR 6.2 MB, e as 6 inteiras somariam ~38 MB. Por isso: **modelo novo 1080p inteiro**
(`ref_new_1080p.jxr`, 6.2 MB) e as outras cinco só no **canto inferior direito 640x360** (0.5-0.7 MB cada; é onde
os tiles da rede são preenchidos: 1080 -> 1088, 1280 -> 1344). `compare --pack` relê o arquivo e confere que
voltou idêntico. Total 9.1 MB.

Conferências: rodar de novo deu o mesmo resultado byte a byte (PSNR inf, 100% idêntico). Modelo antigo contra o
novo, 1080p: 34.4 dB (então trocar o modelo já reprova). A referência antiga (`rdna3-work\reference`, 20 frames)
contra a nova (40 frames): 53.2 dB no canto. O `forced-rdna3-on-rdna4_1080` antigo contra a referência: 9.6 dB,
correlação 0.50.

SHA-256 dos dumps RGBA16F em `refwork\`: new_1080p `01588ca6...e575bc6`, new_720p `258fec01...11b6a2`,
new_1440p `34a92933...c3eb610`, old_1080p `04a421e7...3c9d3893`, old_720p `2a308f2c...1f3e99`,
old_1440p `d2c3ee09...e3bd1185`. Tempos de rede (do runtime) na 9070 XT: novo 4.2 / 6.8 / 10.2 ms, antigo
6.4 / 9.7 / 14.6 ms (720p / 1080p / 1440p).

Expectativa para a RX 7000: o caminho RDNA3 reproduz os valores e4m3 (operandos FP16 exatos, acumulação FP32),
então um run certo deve dar PSNR muito alto contra a 9070 XT (o fork mediu ~0.1 dB de distância entre RDNA4 e
RDNA3 contra a NVIDIA); só a ordem da acumulação FP32 pode mudar bits. Layout de fragmento errado dá lixo: no 9070
XT forçado, correlação ~0.5, grade e tom azulado, ~10 dB.

## Testes aqui (RX 9070 XT), sempre de uma cópia nova do zip numa pasta com espaços

`exports\rx7000-kit\test run rdna4 final\` (sem forçar) e `test run forced\` (`KIT_FORCE_RDNA3=1`); os modelos
foram baixados pelo bat numa primeira cópia (as duas verificaram o SHA-256) e reaproveitados nas seguintes (o
caminho "já verificado, não baixa"). Os `.bin` das cópias de teste foram apagados depois; os `result\` e os zips de
resultado ficaram.

Sem forçar (SUMMARY):

    INFO runtime would pick: FP8 network
    FAIL layout acc-f32: length 8 (want 8), gfx12 map (8(l/16) + c, l%16), 32 of 256 components on the gfx11 map, wave 32
    FAIL layout acc-f16: length 8 (want 8), gfx12 map (8(l/16) + c, l%16), 32 of 256 components on the gfx11 map, wave 32
    FAIL layout frag-a: length 8 (want 16), gfx12 map (l%16, 8(l/16) + c), 128 of 256 components on the gfx11 map, wave 32
    FAIL layout frag-b: length 8 (want 16), gfx12 map (8(l/16) + c, l%16), 128 of 256 components on the gfx11 map, wave 32
    FAIL layout wmma-lane-halves: output row m from lane half m%2 of both operands: 640 of 1024 elements differ (384 look taken from the other half)
    PASS layout kpair-swap: 0 of 102400 products differ with every k pair swapped in the upper half; 0 off the CPU product (worst relative error 2.2e-07)
    PASS pipecheck: 95 of 95 pipelines compiled, 38 s in all; slowest g_fswinpup256.spv 2030 ms, g_fswinpds256.spv 1990 ms, g_fswinfusedup256.spv 1709 ms
    PASS model dlssnr.bin: SHA-256 verified
    PASS model dlssnr-small.bin: SHA-256 verified
    SKIPPED run new_720p: this card picked the FP8 network (not an RX 7000); the kit has only the RDNA3 network
    SKIPPED run new_1080p ... old_1440p: the FP8 network was picked (see run new_720p)

O gfx12 dá exatamente o mapa que o `coopmm.glsl` do fork descreve para o gfx12, então as sondas leem o layout
direito; no gfx11 as cinco primeiras devem dar PASS. O `kpair-swap` passa nos dois (troca consistente nas duas
pernas não muda a soma). O pipecheck compila frio a cada pasta nova (o cache do driver é por exe/caminho): na RX
7000 os ms são o tempo real do LLPC.

Forçado (`KIT_FORCE_RDNA3=1`, as linhas de layout e pipecheck iguais às de cima):

    INFO KIT_FORCE_RDNA3=1: the RDNA3 network is forced (MZ_FORCE_RDNA3=1)
    FAIL run new_720p: RDNA3 network, built in 17.4 s, network 3.30 ms (p95 3.56), frame 4.22 ms; vs RX 9070 XT: PSNR 10.50 dB, mean|d| 0.211159, max|d| 0.9327, corr 0.452338, 0.07% identical, NaN/Inf 0 (area 640x360)
    FAIL run new_1080p: RDNA3 network, built in 1.4 s, network 6.12 ms (p95 6.59), frame 6.93 ms; vs RX 9070 XT: PSNR 11.90 dB, mean|d| 0.164863, max|d| 1.2661, corr 0.502984, 0.96% identical, NaN/Inf 0 (area 1920x1080)
    FAIL run new_1440p: RDNA3 network, built in 3.1 s, network 10.33 ms (p95 10.47), frame 11.67 ms; vs RX 9070 XT: PSNR 10.33 dB, mean|d| 0.210237, max|d| 0.9279, corr 0.509514, 0.02% identical, NaN/Inf 0 (area 640x360)
    FAIL run old_720p: RDNA3 network, built in 2.4 s, network 5.10 ms (p95 5.73), frame 5.71 ms; vs RX 9070 XT: PSNR 9.69 dB, mean|d| 0.235455, max|d| 0.9326, corr 0.471607, 0.18% identical, NaN/Inf 0 (area 640x360)
    FAIL run old_1080p: RDNA3 network, built in 1.7 s, network 9.38 ms (p95 10.37), frame 9.73 ms; vs RX 9070 XT: PSNR 9.64 dB, mean|d| 0.231632, max|d| 0.9326, corr 0.499794, 0.11% identical, NaN/Inf 0 (area 640x360)
    FAIL run old_1440p: RDNA3 network, built in 1.9 s, network 12.70 ms (p95 16.68), frame 13.39 ms; vs RX 9070 XT: PSNR 9.55 dB, mean|d| 0.232389, max|d| 0.8848, corr 0.509414, 0.05% identical, NaN/Inf 0 (area 640x360)

O ramo RDNA3 do kit roda de ponta a ponta: o log diz "the RDNA3 network, on FP16 matrices: MZ_FORCE_RDNA3=1",
twins FP16 (novo 819 + 269 MB, antigo 784 + 344 MB a 1080p), todos os builds e frames sem erro, e a imagem errada
como esperado (grade e tom azulado no `output.jpg`, ~10 dB, correlação ~0.5; o old_1080p dá exatamente os
números do `forced-rdna3-on-rdna4` antigo). Zip de resultado: 3.4 MB (30 arquivos), cabe no Discord.

Bugs achados e corrigidos no caminho: `Get-FileHash`/`Compress-Archive` não carregam quando o PowerShell 5.1 herda
o `PSModulePath` do 7 (bat limpa a variável e o kit usa .NET para hash e zip); `LoadLibrary("MochizukiNrRuntime.dll")`
sem `.\` pegava a DLL da pasta do exe; o harness esperava 3 min um build que já tinha falhado; JPEG do WIC precisa
de BGR.

## Para a voluntária

- Descompactar a pasta toda, dois cliques em `run-test.bat`, esperar o "Done" e mandar o
  `OpenNR-RX7000-result-<data>.zip` que ele mostra (o Explorer abre com ele selecionado). Sem admin.
- Tempo: 10 a 30 min (download de ~250 MB; pipecheck frio de 95 pipelines, aqui 38 s, no gfx11 talvez alguns
  minutos; 6 builds de rede, o primeiro frio; 40 frames cada). Cada passo tem limite: sonda 60 s por verificação,
  pipecheck 300 s por pipeline, rede 30 min a primeira e 15 as outras.
- Disco: ~22 MB do kit + 262 MB de modelos (`files\dlssnr-amd\dlssnr.bin` 148 MB, `dlssnr-small.bin` 114 MB) +
  caches e resultado (poucos MB). O README pede 500 MB livres.
- VRAM: os twins FP16 dobram a rede (1080p ~1.9 GB, 1440p mais); fechar jogos antes.
- SmartScreen pode avisar no bat ("More info", "Run anyway"); os exe/dll têm o Mark of the Web removido pelo kit.
- Se travar ou fechar: mandar a pasta `result\` zipada e contar o que houve.

## Quando o zip dela voltar, olhar nesta ordem

1. `SUMMARY.txt`, linha `INFO runtime would pick:` e a `INFO vulkan device`: tem de ser "RDNA3 network (FP16
   matrices)" num device 0x74xx (RX 7000; 7900 XTX é 0x744c). Se for "refused", o motivo está em `system.txt` (falta coopmat FP16
   16x16x16, VK_KHR_workgroup_memory_explicit_layout, Vulkan 1.3).
2. As seis linhas `layout`. Todas PASS = o LLPC no gfx11 dá o layout que os kernels supõem (risco 1 do HANDOFF
   respondido). FAIL: `system.txt` mostra o mapa visto lane a lane; é isso que muda em `NR_ACC_ROW`/`NR_ACC_COL`
   (`coopmm.glsl`) e nos operandos, e a imagem vai sair errada. CRASH/TIMEOUT numa sonda já é problema de driver.
3. `pipecheck`: qualquer CRASH/TIMEOUT/FAIL nomeia o `.spv` (risco 2); os ms dão o custo do build frio no LLPC.
4. As linhas `run`: "RDNA3 network", PSNR >= 40 dB e NaN/Inf 0 = certo. Esperado bem acima de 40 (aqui, rodar
   duas vezes dá idêntico, e 20 contra 40 frames dá 53 dB). ~10 dB / corr ~0.5 = layout errado; 30-40 dB = algo
   parcial (ver `diff_x16.png`, onde está o erro). Tempos: `network X ms` é o tempo de GPU do runtime (o fork fez
   15 ms a 1080p na 7900 XTX com RADV/ACO; aqui pode ser pior, risco 4).
5. `runs\*\mochizuki_nr.log` e `harness.txt` (o stderr do core vai junto) para qualquer FAIL de build; `output.jpg`
   para ver a imagem.

