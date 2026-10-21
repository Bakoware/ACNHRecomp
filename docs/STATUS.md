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
| `nn::init::Start`, `nnosInitialize` | ✅ (precisou de `svcGetInfo` 24-28, firmware 18+) |
| `sm:` e serviços do SDK | 🔧 em andamento |
| Primeira chamada NVN | ⏳ |
| Primeiro quadro / logo | ⏳ |
| Tela título | ⏳ |
| Criação de personagem / ilha | ⏳ |
| Gameplay, áudio, saves | ⏳ |

## Problemas encontrados e resolvidos

1. `hactool` não lê o BKTR das NCAs do SDK 20 ("Invalid BKTR layout"): usamos o importer próprio.
2. `prod.keys` com entradas de tamanho ímpar quebrava o hactool: o setup filtra as linhas malformadas.
3. Tabelas de salto em funções com cauda compartilhada apontando para antes da entrada: viram entradas.
4. libdecor/GTK aborta o processo em alguns GNOME (schema sem `antialiasing`): preferimos X11/XWayland.
5. `svcGetInfo` tipo 28 (AliasRegionExtraSize) desconhecido: adicionado (com 24, 26, 27).
6. **DT_RELR** não era lido: ponteiros do `sdk` ficavam sem a base (crash em `nn::lm`). Agora é decodificado.
