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
| **Imagem final na tela** | 🔧 ainda preta: a cena principal (HDR) sai preta; investigando |
| Tela título / criação de personagem / ilha | ⏳ |
| Saves, relógio local (fuso horário), teclado do jogo | ⏳ |

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
