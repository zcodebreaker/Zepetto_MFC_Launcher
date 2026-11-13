# Zepetto Launcher

Launcher para Windows desenvolvido em **C++17 e MFC**, com cliente de atualização, servidor de versões TCP e distribuição de patches por HTTP. O projeto implementa o fluxo de atualização do Point Blank/Piercing Blow, incluindo leitura de catálogos, verificação de arquivos e atualização do próprio launcher.

## Recursos

- Interface com imagens e textos configuráveis, progresso de atualização e pausa/retomada.
- Verificação e reparo de arquivos com base no catálogo local, incluindo tamanho, metadados e MD5 conforme as flags de cada entrada.
- Download de pacotes ZIP por HTTP/HTTPS e suporte aos formatos de catálogo 1 a 4.
- Atualização dos componentes do launcher e substituição do executável por meio do `ResourceCheck.exe`.
- Painel de notícias com Microsoft Edge WebView2.
- Servidor TCP com conexões persistentes, registro de eventos e servidor HTTP integrado.
- Publicador de patches e testes de integração em Python.

## Requisitos

Para compilar, use Windows x64 com:

- Visual Studio 2022 com a carga de trabalho **Desenvolvimento para desktop com C++**.
- MSVC, bibliotecas **MFC para x86/x64**, Windows SDK e ferramentas CMake para C++ instalados.
- CMake **3.24 ou superior** e Git disponíveis no terminal.
- Acesso à internet na primeira configuração para baixar as dependências.

O CMake baixa as versões fixadas no [CMakeLists.txt](CMakeLists.txt): WebView2 SDK `1.0.4314.35`, cpp-httplib `0.20.0` e miniz `3.0.2`.

Para exibir notícias, a máquina que executa o cliente precisa do **Microsoft Edge WebView2 Runtime**. Para publicar patches e executar os testes, use **Python 3.12 ou superior**; os scripts usam a biblioteca padrão. O teste de paridade também utiliza o compilador C# do .NET Framework em `%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe`.

## Compilação

No **Developer PowerShell for VS 2022**:

```powershell
git clone https://github.com/zcodebreaker/Zepetto_MFC_Launcher.git
cd Zepetto_MFC_Launcher
cmake --preset vs2022
cmake --build --preset release
```

O preset gera uma solução Visual Studio para **x64** em `build/`. Depois da configuração, também é possível abrir `LauncherMFC.slnf` para trabalhar com os projetos principais no Visual Studio.

| Saída | Função |
| --- | --- |
| `build/client/Release/PBLauncher.exe` | Cliente compilado |
| `build/client/Release/ResourceCheck.exe` | Auxiliar de substituição e reinício do cliente |
| `build/server/Release/UpdateServerss.exe` | Servidor de versões e patches |

Os arquivos de configuração são copiados para as pastas de saída **apenas quando ainda não existem**, preservando ajustes locais. Após alterar os modelos em `engine/` ou `server/`, atualize também as cópias usadas na execução.

O executável original em `engine/PBLauncher.exe` é um arquivo de referência. Para executar esta implementação, use o binário gerado em `build/client/Release/`.

## Configuração e execução

### Servidor

O servidor lê `Config.ini` e `Version.ini` da pasta do próprio executável. A configuração padrão de [server/Config.ini](server/Config.ini) atende somente à máquina local:

```ini
[Address]
Ip0=127.0.0.1
Port0=49100

[HTTP]
Enabled=1
Port=49101
Directory=patch
```

`Port0` é a porta TCP de negociação de versões. O servidor HTTP disponibiliza o diretório configurado na rota `/patch`. Para usar um servidor HTTP externo, defina `Enabled=0` e ajuste a URL em `Version.ini`.

Apesar da extensão `.ini`, [server/Version.ini](server/Version.ini) contém três linhas de dados, sem seções ou chaves:

```text
20171214
2016-04-27_01-25-54
http://127.0.0.1:49101/patch
```

As linhas representam, respectivamente, a versão do conjunto do launcher, a versão do catálogo do jogo no formato `AAAA-MM-DD_HH-MM-SS` e a URL base dos patches. O servidor relê esse arquivo a cada consulta de versão.

Após configurar e publicar os pacotes, inicie o servidor:

```powershell
& .\build\server\Release\UpdateServerss.exe
```

Para atender outros computadores, ajuste o endereço de escuta em `Config.ini`, o endereço TCP no cliente e a URL de patches para um endereço acessível aos clientes.

### Cliente

Mantenha estes arquivos junto ao executável do jogo:

```text
PBLauncher.exe
ResourceCheck.exe
config.zpt
Launcher.svl
Messages.xml
UI.xml
```

Copie-os de `build/client/Release/`. O cliente usa a pasta do próprio executável como raiz da instalação. A configuração fornecida espera o jogo em `PiercingBlow.exe`, ao lado do launcher.

| Campo de `config.zpt` | Uso |
| --- | --- |
| `server_address` | Host do servidor TCP; padrão fornecido: `127.0.0.1` |
| `server_port` | Porta TCP; padrão fornecido: `49100` |
| `web_url` | Endereço do painel de notícias |
| `executable_file` | Nome do executável do jogo, sem subdiretórios |
| `patcherror_retry_count` | Quantidade de novas tentativas após falha; padrão: `3` |
| `server_ack_timeout_sec` | Tempo limite da comunicação TCP em segundos; padrão: `30` |
| `notice_ui` | Campo opcional; `hide` oculta o painel de notícias |

`config.zpt` e `Launcher.svl` armazenam XML codificado por rotação de bits. Para editá-los, decodifique o conteúdo, altere o XML e recodifique-o antes de salvar. As funções `rotated_xml` e `encode_xml` em [tests/paridade.py](tests/paridade.py) exemplificam as duas operações. `Messages.xml` e `UI.xml` são XML em texto e contêm textos e versões de componentes.

O repositório contém o código e os recursos do launcher. Para atualizar ou iniciar uma instalação real, forneça os arquivos do jogo, um catálogo compatível e os pacotes correspondentes no servidor.

## Publicação de patches

[server/PublishPatch.py](server/PublishPatch.py) recebe um **catálogo existente**, a pasta com os arquivos correspondentes e o diretório de saída. Ele verifica tamanhos e MD5, cria os ZIPs e pode atualizar a versão do catálogo anunciada pelo servidor.

Exemplo no PowerShell, a partir da raiz do projeto; substitua os caminhos de `D:\Jogos\PointBlank` pelos da sua instalação:

```powershell
py -3.12 .\server\PublishPatch.py `
  "D:\Jogos\PointBlank\UserFileList.dat" `
  "D:\Jogos\PointBlank" `
  ".\build\server\Release\patch" `
  --version-ini ".\build\server\Release\Version.ini"
```

O publicador organiza a saída assim:

```text
patch/
  <versao-do-catalogo>_FileList.dat.zip
  <versao-do-arquivo>/
    <caminho-do-arquivo>.zip
```

Cada pacote de arquivo contém uma entrada com o nome base do arquivo. A versão na segunda linha de `Version.ini` só é atualizada após a verificação dos pacotes. Sem `--version-ini`, o script prepara os arquivos sem alterar a versão anunciada.

Esse comando publica o catálogo e os arquivos do jogo. Os pacotes dos componentes do launcher ficam separadamente em `_LauncherPatchFiles/`, sob a mesma URL base, e precisam seguir os nomes e versões de `Launcher.svl`; o fluxo está implementado em [src/Engine.cpp](src/Engine.cpp).

## Testes

Após compilar em **Release**, execute os testes em uma sessão Windows com ambiente gráfico:

```powershell
py -3.12 .\tests\paridade.py
py -3.12 .\tests\servidor.py
py -3.12 .\tests\noticias.py
```

| Script | Cobertura |
| --- | --- |
| `paridade.py` | Interface, catálogos, atualização, reparo, pausa/retomada, erros e início do jogo com um executável de teste |
| `servidor.py` | Conexões simultâneas, sessões persistentes, reconexão e registros do servidor |
| `noticias.py` | Carregamento progressivo, respostas lentas, painel oculto e página indisponível; requer WebView2 Runtime |

Os testes criam arquivos temporários e servidores locais. Para incluir a substituição e o reinício do launcher no teste de paridade, gere uma segunda compilação com a versão `20171215`, mantendo a compilação principal na versão padrão `20171214`:

```powershell
cmake -S . -B out/next-client -G "Visual Studio 17 2022" -A x64 -DLAUNCHER_NATIVE_VERSION=20171215
cmake --build out/next-client --config Release --target PBLauncher
py -3.12 .\tests\paridade.py --next-client .\out\next-client\client\Release\PBLauncher.exe
```

## Estrutura do projeto

| Caminho | Conteúdo |
| --- | --- |
| [src/](src/) | Interface MFC, atualização, catálogos, protocolo TCP e auxiliar de manutenção |
| [projects/](projects/) | Definições CMake dos executáveis e bibliotecas |
| [assets/](assets/) | Imagens e ícones da interface |
| [engine/](engine/) | Configurações iniciais, textos e executável original de referência |
| [server/](server/) | Servidor C++, configurações e publicador Python |
| [tests/](tests/) | Testes de integração |
| `build/` e `out/` | Saídas de compilação, ignoradas pelo Git |

Para investigar falhas do cliente, consulte `PBLauncher.log` na pasta da instalação. O servidor registra conexões, pacotes e erros no console. Se o painel indicar WebView2 indisponível, confira a instalação do runtime; se os patches retornarem HTTP 404, confira a URL base, os nomes dos ZIPs e as versões anunciadas.
