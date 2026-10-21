# ACNH Recomp

Recompilação estática de **Animal Crossing: New Horizons** (Nintendo Switch, **3.0.2**, title ID
`01006F8002326000`) para código nativo **Linux x86-64** e **macOS (Apple Silicon)**. Não há emulador: o código
de máquina AArch64 do jogo vira C++ compilado para o host. As chamadas de sistema e de gráficos (NVN, a API da
Nintendo) são implementadas nativamente sobre **Vulkan** (MoltenVK no Mac).

> [!NOTE]
> **Nenhum arquivo do jogo está incluído.** Este repositório não contém código, assets, chaves ou firmware do jogo,
> nem builds feitas a partir deles. Tudo o que é derivado do jogo fica em `private/`, que está no gitignore. Você
> precisa da sua própria cópia legalmente obtida do jogo base e do update 3.0.2, e das suas chaves. Não fornecemos
> instruções de como obtê-los. Nunca distribua o executável gerado: ele contém código do jogo recompilado a partir
> do seu dump.

> [!IMPORTANT]
> **Feito com IA.** Quase todo o código novo, as ferramentas e a documentação foram escritos por IA (Claude, da
> Anthropic, via Claude Code), dirigida e testada por uma pessoa. A base é o framework do
> [SuperMarioWonderRecomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp) (MIT), também feito com IA.

## Status

Veja [docs/STATUS.md](docs/STATUS.md) para o estado atual, medido, de cada etapa (o que funciona e o que falta).
Resumo:

| Etapa | Estado |
|---|---|
| Importação (NSP base + update 3.0.2, BKTR aplicado) | ✅ 103.400 arquivos, 10,5 GB |
| Recompilação da CPU (`main`, `sdk`, `subsdk0`) | ✅ 16,4 M + 1,7 M + 0,9 M instruções, 0 não tratadas |
| Shaders Maxwell → SPIR-V | ✅ 6.200 blocos, 0 inválidos |
| Build nativo Linux | ✅ |
| Boot, serviços do sistema, áudio, relógio, controle | ✅ |
| UI: carregamento, diálogos, criação de personagem com teclado | ✅ |
| Cena 3D | 🔧 em andamento |
| Gameplay na ilha | ⏳ ver [docs/STATUS.md](docs/STATUS.md) |

## Como funciona

```
 seu dump (NSP base + update)         tools/importer (nximport): NCA, BKTR, RomFS
          │
          ├── ExeFS: main, sdk, subsdk0 ──► recomp/ (Python + Capstone): AArch64 → C++  ──┐
          │                                                                               ├─► clang ─► private/acnh/build/acnh
          ├── RomFS: shaders Maxwell ─────► tools/maxwell + Ryujinx (MIT, offline) → SPIR-V │
          │                                                                               │
          └── RomFS: modelos, texturas, áudio … lidos em tempo de execução               runtime/ (C++): kernel Horizon,
                                                                                           IPC/serviços, nn::* HLE,
                                                                                           NVN → Vulkan, SDL3
```

1. **CPU.** Cada função AArch64 vira uma função C++ (`recomp/`). A memória do convidado é a do host: os módulos
   são mapeados em endereços fixos (`0x40_0000_0000` no Linux, `0x3_0000_0000` no macOS), então um ponteiro do
   jogo é um ponteiro do host, sem tradução e sem troca de endianness. Desvios dentro da função viram `goto`,
   tabelas de salto viram `switch`, chamadas indiretas passam por uma tabela (`nx::call`).
2. **SDK.** O `sdk` da Nintendo (musl, libc++, nn::os, nn::fs, ...) também é recompilado, porque o `main` embute os
   layouts dos objetos dele. O runtime faz o papel do `rtld` (relocações, símbolos) e de um kernel Horizon mínimo
   (threads, sincronização, memória, SVCs), e substitui funções `nn::*` por nome (HLE), como `nn::fs` (saves e
   RomFS em pastas do host), `nn::hid` (teclado/controle via SDL3), `nn::time` (relógio do computador) e áudio.
3. **Gráficos.** O jogo pede todas as funções NVN por nome a `nvnBootstrapLoader`. O runtime devolve as suas, que
   gravam os comandos e os reproduzem em Vulkan. Os shaders Maxwell do jogo são traduzidos **antes** para SPIR-V
   (Ryujinx, offline). As texturas são "destileadas" do layout block-linear, e ASTC/BCn são decodificados.

A explicação completa do framework está em [docs/upstream/](docs/upstream/) (documentos do SuperMarioWonderRecomp)
e o que é específico do ACNH está em [docs/ACNH.md](docs/ACNH.md).

## O que foi preciso adaptar para o ACNH

Comparado ao Super Mario Bros. Wonder (SDK 16), o ACNH 3.0.2 usa o **SDK 20.5** e um motor diferente ("Park"):

- **Terceiro módulo:** `subsdk0` é a `nn::movie` e é recompilada e carregada junto (`NX_HAS_SUBSDK0`).
- **Instruções novas** no recompilador (`recomp/emit_sdk20.py`): `sqshl`, `sqrshl`, `uqshl`, `sqabs`, `sqneg`,
  `saddlv`, `sli`, `sri`, `pmull` de bytes, `tbx`, `bfc`, `umnegl`, `smnegl`, `csdb`, `stg`. Todas validadas
  contra o Unicorn com `tests/difftest.py` (0 divergências).
- **Correção na análise** (vale para qualquer jogo): tabelas de salto em funções com cauda compartilhada que
  apontam para antes da entrada atual agora viram entradas próprias. Sem isso o C++ gerado não compilava.
- **Yaz0** (`.szs`): o ACNH comprime com Yaz0 além de zstd, inclusive Yaz0 dentro de SARC (`tools/maxwell/yaz0.py`).
- **Containers de shader do ACNH:** VFXB com `GRSN`/`GRSC` contendo FSHA, shaders de compute **sem cabeçalho NVN**
  (SDK 20) e alguns estágios de geometria (`tools/maxwell/containers.py`, `spirv_harness`).
- **Importação** com update: o importer aplica o patch BKTR (inclusive o formato novo, que o `hactool` não lê).

## Requisitos

| | Linux (x86-64) | macOS (Apple Silicon) |
|---|---|---|
| Compilador | clang 18+ (testado com 21) | Xcode command line tools |
| Pacotes | `build-essential clang lld cmake ninja-build patch curl python3-venv spirv-tools pkg-config libvulkan-dev libsdl3-dev mesa-vulkan-drivers` | Homebrew: `cmake ninja sdl3 vulkan-headers vulkan-loader molten-vk spirv-tools` |
| GPU | Vulkan 1.3 (testado: Intel Iris Xe, Mesa ANV) | MoltenVK |
| RAM | 16 GB (32 GB recomendado para compilar com mais jobs) | 16 GB |
| Disco | ~30 GB livres (RomFS 10,5 GB + código gerado 1,6 GB + objetos) | idem |

O script baixa sozinho, para `private/third_party/`, o resto: um venv Python (capstone, lz4, zstandard, fonttools,
cmake, ninja, unicorn), os Vulkan-Headers 1.4.357, o .NET SDK 10.0.401 e o código-fonte do tradutor de shaders do
Ryujinx (fork Ryubing, MIT).

Ubuntu/Debian, uma vez:

```sh
sudo apt install build-essential clang lld cmake ninja-build patch curl python3-venv spirv-tools pkg-config \
     libvulkan-dev libsdl3-dev mesa-vulkan-drivers vulkan-tools
```

## Instalação

Você precisa de: `prod.keys` (e `title.keys` se tiver), o NSP do **jogo base** e o NSP do **update 3.0.2**.

```sh
git clone <este repositório> ACNHRecomp && cd ACNHRecomp
tools/setup_acnh.sh --keys ~/switch/prod.keys --title-keys ~/switch/title.keys \
                    --base ~/switch/base.nsp --update ~/switch/update.nsp
```

O script roda em etapas e pula o que já está pronto. Se algo falhar, corrija e rode de novo (sem argumentos,
depois da importação):

| Etapa | Saída | Tempo (12 threads, 30 GB RAM) |
|---|---|---|
| Ferramentas | `private/venv`, `private/third_party/*`, `private/build/{importer,spirv_harness}` | ~5 min |
| Importação | `private/acnh/{analysis,exefs,extract/romfs}` | ~5 min |
| Recompilação | `private/acnh/gen/{main,sdk,subsdk0}` (1,6 GB de C++) | ~8 min |
| Shaders | `private/acnh/shaders_extracted`, `private/acnh/spirv` (6.200 `.spv`) | ~2 min |
| Build | `private/acnh/build/acnh` | 60-110 min na primeira vez |

`JOBS=<n>` controla os jobs do compilador (padrão: RAM em GB ÷ 3). `FORCE=<etapa>` (`import`, `recomp`,
`shaders`, `harness`) refaz uma etapa.

Para conferir o dump: o importer imprime `import complete: 103400 RomFS files, 10.50 GB` e o recompilador termina
com `unhandled 0` nos três módulos.

### Passo a passo manual

O que o script faz, comando por comando:

```sh
P=private/venv/bin/python
# 1. importar (ExeFS + RomFS com o update aplicado)
private/build/importer/nximport --exefs --title 01006F8002326000 --out private/acnh \
    private/keys/prod.keys title.keys base.nsp update.nsp
# 2. recompilar
$P -m recomp private/acnh/exefs/main    private/acnh/gen/main    --name main    --base 0x4000000000
$P -m recomp private/acnh/exefs/sdk     private/acnh/gen/sdk     --name sdk     --base 0x4010000000
$P -m recomp private/acnh/exefs/subsdk0 private/acnh/gen/subsdk0 --name subsdk0 --base 0x4018000000
# 3. shaders
tools/extract_shaders.sh
dotnet private/build/spirv_harness/bin/Release/net10.0/spirv_harness.dll \
    --batch private/acnh/shaders_extracted --out private/acnh/spirv --jobs 8 --spirv-val /usr/bin/spirv-val
# 4. build
CC=clang CXX=clang++ cmake -S . -B private/acnh/build -G Ninja -DNX_GAME=acnh -DCMAKE_BUILD_TYPE=Release \
    -DNX_VULKAN_INCLUDE=$PWD/private/third_party/Vulkan-Headers-1.4.357/include
ninja -C private/acnh/build acnh
```

## Jogando

```sh
tools/acnh_linux.sh            # Linux, 1080p (modo dock)
tools/acnh_linux.sh intel      # GPU integrada: 720p (modo portátil), submits em pipeline
tools/acnh_linux.sh balanced   # 720p + FSR 1
tools/acnh_mac.sh              # macOS (padrão: balanced)
```

| Variável | O que faz |
|---|---|
| `NX_LOG=<arquivo>` | log do runtime (padrão `private/acnh/nx_play.log`) |
| `NX_FULLSCREEN=1`, `NX_RESOLUTION=WxH` | tela cheia, tamanho da janela (F11 ou Alt+Enter alterna) |
| `NX_HANDHELD=1` | modo portátil (720p) |
| `NX_MUTE=1` | sem som |
| `NX_HEADLESS=1` | sem janela (testes automáticos) |
| `NX_MEM_LIMIT_GB=<n>` | encerra o processo acima desse uso de memória |

As demais opções de depuração estão em [docs/upstream/nvn/SWITCHES.md](docs/upstream/nvn/SWITCHES.md).

**Controles:** qualquer controle reconhecido pelo SDL3 (Xbox, PlayStation, Pro Controller) ou o teclado:

| Switch | Teclado | No jogo |
|---|---|---|
| Analógico esquerdo | W A S D | andar |
| Analógico direito | I J K L | câmera |
| D-pad | setas | atalhos de ferramentas |
| A / B | Espaço / Shift | interagir / correr, cancelar |
| X / Y | X / F | inventário / pegar |
| L / R | Q / E | |
| ZL / ZR | Z / C | NookPhone / |
| + / − | Enter / Esc ou Backspace | |
| | F11 ou Alt+Enter | tela cheia |

**Saves** ficam em `private/acnh/user/`. **Relógio:** a ilha segue a data e a hora do computador, como no console.
**Online** (visitas, Nook Link, Dream Suite) não existe: o jogo vê o console sem internet.

## Estrutura do repositório

| Caminho | O quê |
|---|---|
| `recomp/` | recompilador AArch64 → C++ (Python) |
| `runtime/` | runtime compartilhado: kernel, IPC, HLE `nn::*`, NVN → Vulkan, plataformas (Linux, macOS, Windows) |
| `games/acnh/` | o que é só do ACNH (nome, program id, ganchos) |
| `tools/importer/` | importação de NSP/XCI: chaves, NCA, BKTR, RomFS |
| `tools/maxwell/` | extração dos containers de shader e tradutor Maxwell → SPIR-V |
| `tools/setup_acnh.sh`, `tools/acnh_*.sh` | instalação e execução |
| `tests/` | teste diferencial do recompilador (Unicorn) e testes de unidade do runtime |
| `docs/` | `ACNH.md` (o jogo), `STATUS.md` (estado), `REFERENCES.md` (o que aprendemos de cada projeto), `upstream/` |

## Projetos de referência

Este projeto foi construído depois de estudar quatro projetos (resumo em [docs/REFERENCES.md](docs/REFERENCES.md)):

- [SuperMarioWonderRecomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp) (MIT): **a base**. Recompilador,
  runtime, NVN → Vulkan, importer, tradução de shaders.
- [LASRecomp](https://github.com/peiikai/LASRecomp) (MIT): Link's Awakening. Referência para relógio, saves,
  fontes compartilhadas, teclado e recompilação na primeira execução.
- [SwitchRecomp](https://github.com/bgyss/SwitchRecomp): especificações (HIPC/CMIF/TIPC, validação).
- [uDPR](https://github.com/shatter9652/uDPR): Pokémon BD reconstruído como projeto Unity (outra abordagem).

## Licença

MIT (veja [LICENSE](LICENSE)); o código herdado do SuperMarioWonderRecomp mantém o aviso de copyright original.
Animal Crossing é marca registrada da Nintendo. Este projeto não é afiliado à Nintendo.
