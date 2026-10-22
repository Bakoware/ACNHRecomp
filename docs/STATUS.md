# Estado atual

Medido no dump 3.0.2, Linux x86-64 (Ubuntu 26.04, clang 21, Intel Iris Xe / Mesa). Atualizado a cada avanço.

## Pipeline

| Etapa | Estado | Detalhes |
|---|---|---|
| Importação NSP base + update | ✅ | BKTR aplicado; 103.400 arquivos, 10,5 GB, ~5 min |
| Recompilação `main` | ✅ | 178k funções, 17,2 M instruções, 0 não tratadas |
| Recompilação `sdk` / `subsdk0` | ✅ | 1,7 M / 0,9 M instruções, 0 não tratadas |
| Instruções novas (SDK 20) | ✅ | 15 mnemônicos, validados contra o Unicorn |
| Relocações RELR (SDK 20) | ✅ | `main`: 7,5 mil RELA → 380 mil relocações no total |
| Shaders → SPIR-V | ✅ | 6.200 blocos, 0 inválidos, 0 exceções |
| Build Linux | ✅ | ~1h45 na primeira vez (12 threads) |
| Build macOS | ⏳ | não testado ainda (sem Mac aqui); CMake e scripts prontos |

## Execução

| Marco | Estado |
|---|---|
| Carrega e liga os 3 módulos | ✅ |
| `nn::init::Start`, `nnosInitialize`, construtores | ✅ |
| `sm:` e serviços (am, acc, aoc, pctl, time, hid, fs, audio, pl, nfp) | ✅ |
| Dispositivo NVN, janela, pools de memória | ✅ |
| AudioRenderer (o do próprio SDK, em processo) | ✅ renderizando a 48 kHz, saída `MainAudioOut` |
| Carregamento de assets (fontes, mensagens, layouts, efeitos, sons) | ✅ |
| Loop principal: leitura de controle, 30 fps de submits Vulkan | ✅ |
| Shaders: todos os desenhos com programa (0 sem shader) | ✅ |
| UI renderizada corretamente (elementos de layout conferidos por dump) | ✅ |
| Relógio local (data/hora do computador, fuso do sistema) | ✅ ("Good afternoon!" à tarde) |
| Save do aparelho (`MountDeviceSaveData`) em `private/acnh/user/device_save` | ✅ monta; gravação a confirmar |
| BCAT (conteúdo entregue pela Nintendo): vazio, como console nunca conectado | ✅ |
| **UI na tela**: tela de carregamento, diálogos do Timmy | ✅ |
| **Criação de personagem** (registro Nook Inc.) com o teclado do próprio jogo | ✅ digitação funciona |
| Renderização 3D (cena do oceano ao entardecer, terreno em alvos menores) | ✅ conferida por dump |
| Cena 3D da abertura (Timmy e Tommy no balcão da Nook Inc.) | ✅ |
| Registro completo: nome (`+` confirma), aniversário | ✅ (o jogo recusa nomes com mais de 5 algarismos, como no console) |
| DLC (`aoc:u` + `MountAddOnContent`): os dois pacotes do dump montados | ✅ |
| Escolhas da introdução: estilo, pele, hemisfério, mapa da ilha, avião | ✅ |
| Filme de abertura (`OpeningMovie.webm`, VP9 + Vorbis, 65 s) inteiro, em cores, com som | ✅ |
| Chegada à ilha ("We're here!"), diálogos do Timmy no píer | ✅ |
| Ilha em 3D: píer da Dodo Airlines, personagens, água, árvores, tela de título com a ilha | ✅ |
| Controle livre do personagem na ilha (andar, câmera acompanha) | ✅ |
| Alguns moradores (pássaros) com parte da cabeça preta | 🔧 os moradores iniciais são sorteados; não reproduz sempre |

## Problemas encontrados e resolvidos

1. `hactool` não lê o BKTR das NCAs do SDK 20 ("Invalid BKTR layout"): usamos o importer próprio.
2. `prod.keys` com entradas de tamanho ímpar quebrava o hactool: o setup filtra as linhas malformadas.
3. Tabelas de salto em funções com cauda compartilhada apontando para antes da entrada: viram entradas.
4. libdecor/GTK aborta o processo em alguns GNOME (schema sem `antialiasing`): preferimos X11/XWayland.
5. `svcGetInfo` tipos 24-28 (firmware 18+, AliasRegionExtraSize) adicionados.
6. **DT_RELR** não era lido: ponteiros ficavam sem a base (crash em `nn::lm`). Agora é decodificado
   (`main`: 7,5 mil → 380 mil relocações; a descoberta de funções também ganhou 0,8 M instruções).
7. `DT_INIT` não fica mais em +0x30 no SDK 20: lido do `.dynamic`.
8. Tabela de formatos do driver NVN em offset fixo do SDK 16.2: agora achada pelo conteúdo (`sdk+b7f7d8`).
9. `hid`: touch screen, cores do controle, LED, estilos suportados e modos de Joy-Con (HLE novos).
10. `IApplicationFunctions` 210: evento que o `nn::oe::GetOperationMode` espera; sem ele, laço infinito.
11. Áudio: offsets do AudioRenderSystem e id do comando DeviceSink mudaram no SDK 20; agora lidos do código.
12. `nn::pl::GetSharedFont` / `RequestSharedFontLoad`, `nn::nfp` (amiibo: sem leitor) implementados.
13. Orçamento de memória `cSys` do motor estourava (heaps de tarefa criados com "o resto" e aparados
    depois, em outra ordem que no console): +64 MiB vindos de `cOthers` (`games/acnh/game.cpp`).
14. Shaders das bibliotecas de amostra do SDK (`System/Font`, `System/PrimitiveRenderer`, `System/Agl`): novo
    formato no extrator; sem eles o passe final de tela não tinha vertex shader (9.366 blocos agora).

15. `nn::time` em nível de API (HLE): relógio do host e conversões com o fuso local. Sem isso o jogo mostrava
    "The system is set to a date outside the supported range." (`runtime/src/hle_time.cpp`).
16. Save do aparelho: `MountDeviceSaveData`, `Get/ExtendDeviceSaveDataSize`, `Commit`, `RenameDirectory`,
    `CleanDirectoryRecursively` (`runtime/src/hle_fs.cpp`).
17. `nn::bcat` (`runtime/src/hle_bcat.cpp`): armazenamento de entrega vazio.
18. Vulkan: recursos 1.1/1.2 (`shaderDrawParameters`) e `VK_EXT_fragment_shader_interlock` habilitados; sem eles
    as camadas de validação acusavam os shaders traduzidos.
19. Tela preta: o "clear on bind" do motor (`main+37fb390`) limpava o alvo HDR a cada troca de alvo da UI, e a
    composição final lia o alvo limpo. No console esses clears são controlados por estado que a GPU mantém; por
    enquanto os clears chamados de `main+37fb4fc` são ignorados (`games/acnh/game.cpp`, `NX_SKIP_CLEAR_AT=0` desliga).

20. **DLC**: o importer extrai a RomFS dos NCAs `PublicData` de cada add-on para `private/acnh/aoc/<índice>/`
    (`setup_acnh.sh --dlc <nsp>`); `nn::aoc::Count/ListAddOnContent` e `nn::fs::MountAddOnContent` respondem com o que
    está lá. Os dois DLCs do dump (300 Pocket Camp, 301 Nook Inc. Silk Rug) são vistos e montados pelo jogo.
21. `AudioOut` sem evento, formato/taxa/canais/volume (HLE): o filme de abertura toca o som por ele. Enquanto o
    AudioRenderer roda, o som do AudioOut é somado à mixagem final dele em vez de entrar na fila do dispositivo.
22. **Filme de abertura**: o `movie::Decoder` do SDK é o stagefright do Android sobre o NVDEC do console (aborta ao
    abrir o driver). O construtor é substituído para vídeo (VP8/VP9): o objeto recebe uma vtable nossa com o mesmo
    contrato (índices de buffers de entrada/saída, quatro eventos), decodificada com libvpx em NV12
    (`runtime/src/hle_movie.cpp`). O áudio (Vorbis) continua no decodificador do SDK, que é software.

23. **Kernel (compartilhado)**: `svcWaitSynchronization` descartava um `CancelSynchronization` que chegava antes da
    espera. No Horizon ele fica pendente até uma espera consumi-lo; é assim que `nn::os::MultiWait` acorda quem espera
    eventos. Sem isso, acordadas se perdiam ao acaso (o player do filme travava num quadro aleatório).
24. **NVN**: `nvnCommandBufferCopyBufferToTexture` não existia no runtime (caía num stub neutro): agora é gravado e
    reproduzido como `vkCmdCopyBufferToImage` a partir dos bytes do jogo. O filme copia seus planos NV12 assim.
25. `nn::friends` (offline: sem amigos, sem bloqueados, sem conta de rede): `GetBlockedUserList` lia um ponteiro nulo
    ao chegar na ilha.
26. Decodificador do filme: depois da entrada de fim de stream não oferece mais buffers (como o MediaCodec); senão a
    fila de mensagens do jogo enchia e o encerramento do player esperava para sempre.

27. **Cena 3D preta / parede por cima dos personagens**: o "clear on bind" do motor (`main+37fb390`) limpa também a
    profundidade (`main+37fb744`) a cada vez que um passe liga o alvo. O passe de cor religa o alvo da cena depois do
    pré-passe de profundidade, então a profundidade era apagada e o passe de cor (teste EQUAL) desenhava tudo em
    ordem: a parede do fundo cobria Timmy e Tommy, e o resultado ficava preto. Agora só a primeira limpeza de
    profundidade de cada alvo por quadro é mantida (`NX_CLEAR_ONCE_AT`, em `nvn_cmd.cpp`); as limpezas de cor dali
    continuam descartadas. Imagens de alvos de render ficam guardadas por memória (o jogo reinicializa os mesmos
    `NVNtexture` várias vezes por quadro).

28. **`setjmp` do SDK**: o `sdk` define o próprio `setjmp` e o chama pela PLT dele; o recompilador só reconhecia
    importados, então a libjpeg/libpng do SDK (fotos) fazia `longjmp` sem destino e o processo abortava. Agora chamadas
    a um `setjmp` definido no próprio módulo também ganham o `try` (`recomp/analysis.py`, `recomp/emit.py`).
29. `nvnCommandBufferCopyTextureToBuffer`: reproduzido como `vkCmdCopyImageToBuffer` e copiado para a memória do jogo
    depois do submit (o jogo lê de volta, por exemplo, a foto 500x500 do passaporte).

## Próximos passos (pedidos)

- **60 fps**: medido: a lógica do ACNH avança **por quadro** (uma fala é digitada em ~115 quadros tanto a 30 quanto
  a 60 apresentações por segundo, `NX_PRESENT_INTERVAL=1`), então só apresentar a 60 deixa o jogo 2x mais rápido. Um
  60 fps de verdade exige achar e reduzir à metade o passo de cada sistema que conta quadros (não há uma constante
  única: o `cFrameRate` do binário é só o fim da string `MusicBeatSyncFrameRate`).
- **Janela adaptável / resolução nativa**: hoje o quadro 1920x1080 é escalado para a janela. Renderizar no tamanho
  da janela exige redimensionar os alvos de render do motor (o framework já faz isso para o Wonder em
  `nvn_ultrawide.cpp`).
- **Hardware fraco**: perfil (perf) da cena 3D: o reverb I3DL2 do áudio, renderizado na CPU (no console é o DSP),
  era a função mais cara do processo (~20% das amostras, um núcleo inteiro). `NX_AUDIO_REVERB=0` (presets `intel`,
  `lightest`, `performance`) o desliga: a thread de áudio cai de 26% para 11%. Próximos ganhos: o despacho de
  chamadas indiretas (`nx::call`, 2%), reverb nativo em vez do recompilado, cache de registradores no recompilador.

## Ferramentas de diagnóstico

| Variável | O que faz |
|---|---|
| `NX_ALLOC_TRACE=1` | tamanhos dos alocadores e falhas de alocação, com backtrace (`games/acnh/diag.cpp`) |
| `NX_DUMP_FRAMES=N` | grava a cada N quadros o que foi apresentado em `private/acnh/frames/` |
| `NX_DUMP_TEXTURES=<hex>,...` | junto, grava essas texturas (alvos de render) |
| `NX_DEBUG_TEXTURES=<hex>,...` | descreve essas texturas no log (formato, memória, pool) |
| `NX_TRACE_DRAWS=<n>` | 80 desenhos a partir do n-ésimo: shaders, alvos, texturas |
| `NX_TRACE_TEXTURE_SIZES=1` | tamanhos de armazenamento de texturas ≥ 1 MB |
| `NX_DUMP_SHADER_MISSES=1` | grava shaders que não foram traduzidos (`private/acnh/shader_misses/`) |
| arquivo `private/acnh/dump_threads` | despeja o estado de todas as threads no log |
| `NX_TRACE_SUBMITS=<quadro>` | esboço das listas de comandos submetidas (alvos, clears, scissor, desenhos) |
| `NX_TRACE_RECORD=<quadro>` | `SetRenderTargets`/`ClearColor`/`SetScissor` como o jogo grava, com o chamador |
| `NX_SKIP_CLEAR_AT=<offset do main>` | ignora os clears de cor chamados daquele ponto |
| `NX_SKIP_DEPTH_CLEAR_AT=<offset>` / `NX_CLEAR_ONCE_AT=<offsets>` | ignora os clears de profundidade dali / mantém só o primeiro por alvo e quadro |
| `NX_TRACE_FRAME=<quadro>` | com `NX_TRACE_DRAWS`, só desenhos a partir daquele quadro |
| `NX_PRESENT_INTERVAL=<n>` | força o intervalo de apresentação (1 = 60 fps) |
| `NX_SNAP_FOLLOW=<hash do fs>@<desenho>` | captura o alvo depois de cada desenho seguinte |
| `NX_INPUT=quadro:Botão,...` | aperta botões (ou `LUp`/`LDown`/`LLeft`/`LRight`: analógico) em quadros definidos |
| `NX_TIME_OFFSET=<segundos>` | desloca o relógio do jogo (viagem no tempo) |
| `NX_USER_NAME=<nome>` | nome do usuário do sistema (padrão: o do computador) |
| `NX_MOVIE_TRACE=1` / `NX_MOVIE_STATS=1` | cada chamada ao decodificador do filme / estado dos buffers a cada 2 s |
| `NX_AUDIO_OUT_STATS=1` | buffers do `AudioOut` (anexados, tocados, devolvidos) a cada 2 s |
